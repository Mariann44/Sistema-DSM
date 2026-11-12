/* sync.c - locks (LOCK_ACQUIRE / GRANT / RELEASE) y barreras (BARRIER_ENTER / RELEASE).
 *
 * Un dsm_lock_t o dsm_barrier_t vive dentro de la memoria DSM, pero su estado no: lo guarda
 * el "administrador", que es el home de la página donde está guardado. Así tomar un lock o
 * entrar a una barrera no mueve la página entre nodos.
 *
 * Los mensajes del administrador son de hoja: el hilo de red los atiende en línea y nunca
 * espera a otro nodo, solo encola, cuenta o responde. El que pide sí espera (en su waiter_t).
 * Con MSI, lo que el dueño escribió antes de dsm_unlock ya es visible para el siguiente,
 * porque cada escritura invalida las demás copias antes de completarse.
 */
#include "internal.h"

#define MAX_LOCKS    256
#define LOCK_QUEUE   64
#define MAX_BARRIERS 64
#define BARRIER_MAX  64   /* participantes por barrera */
#define NO_NODE    0xFFFF

typedef struct { uint16_t node, reserved; uint32_t request_id; } lock_req_t;   /* 8 bytes */
typedef struct {
    uint32_t page_id, offset;
    lock_req_t holder;              /* node = NO_NODE si está libre */
    uint16_t head, count;           /* cola circular */
    uint32_t in_use;
    lock_req_t queue[LOCK_QUEUE];
} lock_entry_t;                     /* 536 bytes */

/* Lado administrador: tabla hash abierta por (page_id, offset). */
static lock_entry_t locks[MAX_LOCKS];
static pthread_mutex_t locks_mx = PTHREAD_MUTEX_INITIALIZER;

/* Lado solicitante: locks que este nodo tiene concedidos (para validar dsm_unlock). */
typedef struct { uint32_t page_id, offset; int in_use; } held_t;
static held_t held[MAX_LOCKS];
static pthread_mutex_t held_mx = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    uint32_t page_id, offset;
    uint32_t count;                 /* participantes esperados; lo fija la primera llegada */
    uint32_t arrived;
    uint32_t generation;            /* sube en cada liberación; permite reutilizar la barrera */
    uint32_t in_use;
    lock_req_t waiting[BARRIER_MAX];
} barrier_entry_t;                  /* 536 bytes */

static barrier_entry_t barriers[MAX_BARRIERS];

/* Respuesta pendiente de enviar: se arma con locks_mx tomado y se envía después de soltarlo. */
typedef struct { lock_req_t to; uint32_t page_id, offset; int status; } grant_t;

static lock_entry_t *find_entry(uint32_t page, uint32_t off, int create) {
    uint32_t h = (page * 2654435761u ^ off * 40503u) % MAX_LOCKS;
    for (int i = 0; i < MAX_LOCKS; i++) {
        lock_entry_t *e = &locks[(h + i) % MAX_LOCKS];
        if (e->in_use && e->page_id == page && e->offset == off) return e;
        if (!e->in_use) {
            if (!create) return NULL;
            memset(e, 0, sizeof *e);
            e->in_use = 1; e->page_id = page; e->offset = off; e->holder.node = NO_NODE;
            return e;
        }
    }
    return NULL;   /* tabla llena */
}

/* Envía una respuesta (LOCK_GRANT o BARRIER_RELEASE) a quien la pidió. */
static void send_reply(int type, lock_req_t to, uint32_t page, const uint8_t *pl, uint32_t len, int status) {
    if (to.node == G.me) {   /* el solicitante es este mismo nodo: sin red */
        msg_header_t fake = { .type = (uint8_t)type, .src_node = G.me, .request_id = to.request_id,
                              .page_id = page, .status = (int16_t)status, .payload_len = len };
        waiter_complete(&fake, pl, len);
    } else {
        send_msg_node(to.node, type, 0, to.request_id, page, status, pl, len, NULL, 0);
    }
}

static void send_grant(const grant_t *g) {
    uint8_t ob[4]; put32(ob, g->offset);
    send_reply(MSG_LOCK_GRANT, g->to, g->page_id, ob, 4, g->status);
}

/* Pasa el lock al siguiente de la cola (o lo deja libre). Requiere locks_mx. */
static int pass_next(lock_entry_t *e, grant_t *g, int status) {
    if (!e->count) { e->holder.node = NO_NODE; return 0; }
    e->holder = e->queue[e->head];
    e->head = (e->head + 1) % LOCK_QUEUE; e->count--;
    *g = (grant_t){ e->holder, e->page_id, e->offset, status };
    return 1;
}

/* ---------- administrador ---------- */
static void manager_acquire(uint16_t node, uint32_t rid, uint32_t page, uint32_t off) {
    grant_t g = { { node, 0, rid }, page, off, DSM_OK }; int send = 0;
    pthread_mutex_lock(&locks_mx);
    lock_entry_t *e = find_entry(page, off, 1);
    if (!e) { g.status = DSM_E_NOMEM; send = 1; LOG(LOG_ERROR, "tabla de locks llena (MAX_LOCKS=%d)", MAX_LOCKS); }
    else if (e->holder.node == NO_NODE) { e->holder = g.to; send = 1; }
    else if (e->count == LOCK_QUEUE) { g.status = DSM_E_NOMEM; send = 1; LOG(LOG_WARN, "cola del lock %u+%u llena", page, off); }
    else e->queue[(e->head + e->count++) % LOCK_QUEUE] = g.to;   /* se concede al soltarlo el dueño */
    pthread_mutex_unlock(&locks_mx);
    if (send) send_grant(&g);
}

static int manager_release(uint16_t node, uint32_t page, uint32_t off) {
    grant_t g; int send = 0, rc = DSM_OK;
    pthread_mutex_lock(&locks_mx);
    lock_entry_t *e = find_entry(page, off, 0);
    if (!e || e->holder.node != node) rc = DSM_E_PERM;
    else send = pass_next(e, &g, DSM_OK);
    pthread_mutex_unlock(&locks_mx);
    if (rc) LOG(LOG_WARN, "LOCK_RELEASE de n%u sobre el lock %u+%u que no tiene", node, page, off);
    if (send) send_grant(&g);
    return rc;
}

void sync_on_acquire(const msg_header_t *h, const uint8_t *pl, uint32_t len) {
    if (len < 4) return;
    uint32_t off = get32(pl);
    if (home_of(h->page_id) != G.me || off >= G.cfg.page_size) {   /* no soy el administrador */
        grant_t g = { { h->src_node, 0, h->request_id }, h->page_id, off, DSM_E_INVAL };
        send_grant(&g); return;
    }
    manager_acquire(h->src_node, h->request_id, h->page_id, off);
}

void sync_on_release(const msg_header_t *h, const uint8_t *pl, uint32_t len) {
    if (len < 4) return;
    manager_release(h->src_node, h->page_id, get32(pl));
}

/* Un nodo cayó: se saca de las colas; si tenía un lock, pasa al siguiente con DSM_E_NODE_DOWN
 * para avisarle que los datos protegidos pueden haber quedado a medias. */
void sync_node_down(int node) {
    grant_t *gs = malloc(sizeof(grant_t) * (MAX_LOCKS + MAX_BARRIERS * BARRIER_MAX)); int ng = 0;
    if (!gs) return;
    pthread_mutex_lock(&locks_mx);
    for (int i = 0; i < MAX_LOCKS; i++) {
        lock_entry_t *e = &locks[i];
        if (!e->in_use) continue;
        int k = 0;
        for (int j = 0; j < e->count; j++) {
            lock_req_t r = e->queue[(e->head + j) % LOCK_QUEUE];
            if (r.node != node) e->queue[(e->head + k++) % LOCK_QUEUE] = r;
        }
        e->count = k;
        if (e->holder.node == node && pass_next(e, &gs[ng], DSM_E_NODE_DOWN)) ng++;
    }
    int first_barrier = ng;
    /* Barreras: no se sabe si el nodo caído iba a llegar, así que toda barrera con alguien
     * esperando se libera con DSM_E_NODE_DOWN en vez de quedar trabada para siempre. */
    for (int i = 0; i < MAX_BARRIERS; i++) {
        barrier_entry_t *b = &barriers[i];
        if (!b->in_use || !b->arrived) continue;
        for (uint32_t j = 0; j < b->arrived; j++)
            if (b->waiting[j].node != node) gs[ng++] = (grant_t){ b->waiting[j], b->page_id, b->offset, DSM_E_NODE_DOWN };
        b->arrived = 0; b->count = 0; b->generation++;
    }
    pthread_mutex_unlock(&locks_mx);
    for (int i = 0; i < first_barrier; i++) send_grant(&gs[i]);
    for (int i = first_barrier; i < ng; i++) {
        uint8_t pl[8]; put32(pl, gs[i].offset); put32(pl + 4, 0);
        send_reply(MSG_BARRIER_RELEASE, gs[i].to, gs[i].page_id, pl, 8, gs[i].status);
    }
    free(gs);
}

/* ---------- barreras: administrador ---------- */
static barrier_entry_t *find_barrier(uint32_t page, uint32_t off) {
    uint32_t h = (page * 2654435761u ^ off * 40503u) % MAX_BARRIERS;
    for (int i = 0; i < MAX_BARRIERS; i++) {
        barrier_entry_t *b = &barriers[(h + i) % MAX_BARRIERS];
        if (b->in_use && b->page_id == page && b->offset == off) return b;
        if (!b->in_use) {
            memset(b, 0, sizeof *b);
            b->in_use = 1; b->page_id = page; b->offset = off;
            return b;
        }
    }
    return NULL;
}

/* Registra una llegada. Si completa la barrera, libera a todos los que esperaban. */
static void manager_barrier_enter(lock_req_t who, uint32_t page, uint32_t off, uint32_t count) {
    lock_req_t out[BARRIER_MAX]; int nout = 0, status = DSM_OK; uint32_t gen = 0;
    pthread_mutex_lock(&locks_mx);
    barrier_entry_t *b = find_barrier(page, off);
    if (!b) { status = DSM_E_NOMEM; out[nout++] = who; LOG(LOG_ERROR, "tabla de barreras llena (MAX_BARRIERS=%d)", MAX_BARRIERS); }
    else {
        if (b->arrived == 0) b->count = count ? count : (uint32_t)dsm_num_nodes();   /* la primera llegada fija count */
        if (count && count != b->count) { status = DSM_E_MISMATCH; out[nout++] = who; }   /* solo a este nodo */
        else {
            b->waiting[b->arrived++] = who;
            if (b->arrived >= b->count) {      /* llegaron todos: se libera y se reinicia */
                for (uint32_t j = 0; j < b->arrived; j++) out[nout++] = b->waiting[j];
                gen = b->generation++; b->arrived = 0; b->count = 0;
            }
        }
    }
    pthread_mutex_unlock(&locks_mx);
    uint8_t pl[8]; put32(pl, off); put32(pl + 4, gen);
    for (int i = 0; i < nout; i++) send_reply(MSG_BARRIER_RELEASE, out[i], page, pl, 8, status);
}

void sync_on_barrier_enter(const msg_header_t *h, const uint8_t *pl, uint32_t len) {
    if (len < 8) return;
    uint32_t off = get32(pl), count = get32(pl + 4);
    lock_req_t who = { h->src_node, 0, h->request_id };
    if (home_of(h->page_id) != G.me || off >= G.cfg.page_size || count > BARRIER_MAX) {
        uint8_t r[8]; put32(r, off); put32(r + 4, 0);
        send_reply(MSG_BARRIER_RELEASE, who, h->page_id, r, 8, DSM_E_INVAL);
        return;
    }
    manager_barrier_enter(who, h->page_id, off, count);
}

/* ---------- API (lado solicitante) ---------- */
static int lock_addr(const void *p, uint32_t *page, uint32_t *off) {   /* sirve para locks y barreras (8 B) */
    if (!G.inited) return DSM_E_NOT_INIT;
    uintptr_t a = (uintptr_t)p, base = (uintptr_t)G.app;
    if (!p || a < base || a + 8 > base + G.span || (a & 7)) return DSM_E_INVAL;
    *page = (a - base) / G.cfg.page_size; *off = (a - base) % G.cfg.page_size;
    return DSM_OK;
}

int dsm_lock(dsm_lock_t *l) {
    uint32_t page, off; int rc;
    if ((rc = lock_addr(l, &page, &off))) return rc;
    int mgr = home_of(page);
    if (mgr < 0) return DSM_E_INVAL;
    uint8_t ob[4], rb[4]; put32(ob, off);
    waiter_t *w = waiter_get(W_LOCK, mgr, page, rb, sizeof rb, 1);
    if (!w) return DSM_E_NOMEM;
    if (mgr == G.me) manager_acquire(G.me, w->request_id, page, off);
    else if ((rc = send_msg_node(mgr, MSG_LOCK_ACQUIRE, 0, w->request_id, page, 0, ob, 4, NULL, 0))) { waiter_put(w); return rc; }
    /* Sin límite de tiempo mientras el administrador siga vivo: si cae, waiter_fail_node despierta. */
    while ((rc = waiter_wait(w, 1000)) == DSM_E_TIMEOUT && G.inited) ;
    int granted = (rc == DSM_OK && w->reply_len == 4);   /* llegó un LOCK_GRANT (con o sin aviso) */
    if (rc == DSM_OK) rc = w->status;
    waiter_put(w);
    if (granted && (rc == DSM_OK || rc == DSM_E_NODE_DOWN)) {
        pthread_mutex_lock(&held_mx);
        for (int i = 0; i < MAX_LOCKS; i++) if (!held[i].in_use) { held[i] = (held_t){ page, off, 1 }; break; }
        pthread_mutex_unlock(&held_mx);
    }
    return rc;
}

int dsm_unlock(dsm_lock_t *l) {
    uint32_t page, off; int rc, found = 0;
    if ((rc = lock_addr(l, &page, &off))) return rc;
    pthread_mutex_lock(&held_mx);
    for (int i = 0; i < MAX_LOCKS && !found; i++)
        if (held[i].in_use && held[i].page_id == page && held[i].offset == off) { held[i].in_use = 0; found = 1; }
    pthread_mutex_unlock(&held_mx);
    if (!found) return DSM_E_PERM;
    int mgr = home_of(page);
    if (mgr == G.me) return manager_release(G.me, page, off);
    uint8_t ob[4]; put32(ob, off);
    return send_msg_node(mgr, MSG_LOCK_RELEASE, 0, 0, page, 0, ob, 4, NULL, 0);
}

int dsm_barrier(dsm_barrier_t *b, uint32_t count) {
    uint32_t page, off; int rc;
    if ((rc = lock_addr(b, &page, &off))) return rc;
    if (count > BARRIER_MAX) return DSM_E_INVAL;
    int mgr = home_of(page);
    if (mgr < 0) return DSM_E_INVAL;
    uint8_t pl[8], rb[8]; put32(pl, off); put32(pl + 4, count);
    waiter_t *w = waiter_get(W_BARRIER, mgr, page, rb, sizeof rb, 1);
    if (!w) return DSM_E_NOMEM;
    if (mgr == G.me) manager_barrier_enter((lock_req_t){ G.me, 0, w->request_id }, page, off, count);
    else if ((rc = send_msg_node(mgr, MSG_BARRIER_ENTER, 0, w->request_id, page, 0, pl, 8, NULL, 0))) { waiter_put(w); return rc; }
    while ((rc = waiter_wait(w, 1000)) == DSM_E_TIMEOUT && G.inited) ;   /* sin límite, como dsm_lock */
    if (rc == DSM_OK) rc = w->status;
    waiter_put(w);
    return rc;
}
