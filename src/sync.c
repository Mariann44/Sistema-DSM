/* sync.c - locks distribuidos (LOCK_ACQUIRE / LOCK_GRANT / LOCK_RELEASE).
 *
 * Un dsm_lock_t vive dentro de la memoria DSM, pero su estado no: el dueño y la cola de
 * espera los guarda el "administrador" del lock, que es el home de la página donde está
 * el dsm_lock_t. Así tomar o soltar un lock no mueve la página entre nodos.
 *
 * Los mensajes del administrador son de hoja: el hilo de red los atiende en línea y nunca
 * espera a otro nodo, solo encola o concede. El que pide sí espera (en su waiter_t).
 * Con MSI, lo que el dueño escribió antes de dsm_unlock ya es visible para el siguiente,
 * porque cada escritura invalida las demás copias antes de completarse.
 */
#include "internal.h"

#define MAX_LOCKS  256
#define LOCK_QUEUE 64
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

/* Concesión pendiente de enviar: se arma con locks_mx tomado y se envía después de soltarlo. */
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

static void send_grant(const grant_t *g) {
    uint8_t ob[4]; put32(ob, g->offset);
    if (g->to.node == G.me) {   /* el solicitante es este mismo nodo: sin red */
        msg_header_t fake = { .type = MSG_LOCK_GRANT, .src_node = G.me, .request_id = g->to.request_id,
                              .page_id = g->page_id, .status = (int16_t)g->status, .payload_len = 4 };
        waiter_complete(&fake, ob, 4);
    } else {
        send_msg_node(g->to.node, MSG_LOCK_GRANT, 0, g->to.request_id, g->page_id, g->status, ob, 4, NULL, 0);
    }
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
    grant_t gs[MAX_LOCKS]; int ng = 0;
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
    pthread_mutex_unlock(&locks_mx);
    for (int i = 0; i < ng; i++) send_grant(&gs[i]);
}

/* ---------- API (lado solicitante) ---------- */
static int lock_addr(const void *p, uint32_t *page, uint32_t *off) {
    if (!G.inited) return DSM_E_NOT_INIT;
    uintptr_t a = (uintptr_t)p, base = (uintptr_t)G.app;
    if (!p || a < base || a + sizeof(dsm_lock_t) > base + G.span || (a & 7)) return DSM_E_INVAL;
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
