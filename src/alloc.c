/* alloc.c - asignación distribuida en dos fases (Entregable 1, "Asignación de memoria").
 *
 * Una asignación es siempre un rango contiguo de páginas globales [start, start + n), para que
 * dsm_alloc devuelva un solo puntero. Como los slices de los nodos son consecutivos, el rango
 * puede cruzar el límite entre dos o más slices; cada tramo vive en un home distinto.
 *
 * Fase 1 (reserva): el nodo que asigna elige un rango libre con su copia de la tabla de
 * asignaciones y envía ALLOC_REQUEST a cada home involucrado, en orden ascendente de página.
 * El home revisa que ninguna página del tramo esté asignada o reservada y la marca DF_RESERVED.
 * Si un home rechaza (otro nodo reservó antes), se envía ALLOC_ABORT a los que ya aceptaron y se
 * reintenta con otro rango, evitando el tramo ocupado que informó el home.
 *
 * Fase 2 (confirmación): ALLOC_COMMIT se difunde a todos los nodos. Los homes pasan sus páginas
 * de DF_RESERVED a DF_ALLOCATED y todos registran la asignación en la tabla replicada.
 *
 * ALLOC_REQUEST, ALLOC_REPLY, ALLOC_COMMIT y ALLOC_ABORT son de hoja: el home solo toma
 * alloc_mx (nunca espera a la red ni a un semáforo de directorio), así que el hilo de red los
 * atiende en línea sin riesgo de interbloqueo. */
#include "internal.h"

#define MAX_RESV     256   /* reservas pendientes en este home */
#define MAX_ATTEMPTS 32    /* rangos distintos que se prueban antes de rendirse */
#define MAX_BLOCKED  64    /* tramos ocupados aprendidos durante una misma dsm_alloc */

typedef struct { uint64_t id; uint32_t first, num; uint16_t node; uint8_t used; } resv_t;
static resv_t resv[MAX_RESV];   /* protegido por G.alloc_mx */

static inline uint8_t dflags(uint32_t p) { return __atomic_load_n(&G.dir[p].flags, __ATOMIC_ACQUIRE); }

/* ---------- lado home ---------- */

/* Reserva [first, first + num) del slice propio para alloc_id. Si hay páginas ocupadas, devuelve
 * DSM_E_NOMEM y el primer tramo ocupado en [*cf, *ce). */
static int home_reserve(uint64_t id, uint32_t first, uint32_t num, uint16_t node, uint32_t *cf, uint32_t *ce) {
    int rc = DSM_OK, slot = -1;
    pthread_mutex_lock(&G.alloc_mx);
    if (!num || first < G.slice_first || first + num > G.slice_first + G.slice_num) { rc = DSM_E_INVAL; goto out; }
    for (uint32_t p = first; p < first + num; p++)
        if (dflags(p) & (DF_ALLOCATED | DF_RESERVED)) {
            uint32_t e = p;
            while (e < first + num && (dflags(e) & (DF_ALLOCATED | DF_RESERVED))) e++;
            *cf = p; *ce = e; rc = DSM_E_NOMEM; goto out;
        }
    for (int i = 0; i < MAX_RESV; i++) if (!resv[i].used) { slot = i; break; }
    if (slot < 0) { rc = DSM_E_NOMEM; *cf = first; *ce = first; goto out; }   /* tabla de reservas llena */
    for (uint32_t p = first; p < first + num; p++) __atomic_fetch_or(&G.dir[p].flags, DF_RESERVED, __ATOMIC_RELEASE);
    resv[slot] = (resv_t){ id, first, num, node, 1 };
    G.slice_free -= num;
out:
    pthread_mutex_unlock(&G.alloc_mx);
    if (rc == DSM_OK) pager_update_self_free();
    return rc;
}

/* Termina una reserva: commit = 1 la convierte en asignación, commit = 0 la deshace. */
static int home_finish(int i, int commit) {
    resv_t *r = &resv[i];
    for (uint32_t p = r->first; p < r->first + r->num; p++) {
        if (commit) __atomic_fetch_or(&G.dir[p].flags, DF_ALLOCATED, __ATOMIC_RELEASE);
        __atomic_fetch_and(&G.dir[p].flags, (uint8_t)~DF_RESERVED, __ATOMIC_RELEASE);
    }
    if (!commit) G.slice_free += r->num;
    r->used = 0;
    return !commit;
}

static void home_end(uint64_t id, int commit) {
    int changed = 0;
    pthread_mutex_lock(&G.alloc_mx);
    for (int i = 0; i < MAX_RESV; i++) if (resv[i].used && resv[i].id == id) changed |= home_finish(i, commit);
    pthread_mutex_unlock(&G.alloc_mx);
    if (changed) pager_update_self_free();
}

void alloc_home_commit(uint64_t alloc_id) { home_end(alloc_id, 1); }

/* El nodo que reservó cayó o salió antes de confirmar: sus reservas se deshacen. */
void alloc_node_down(int node) {
    int changed = 0;
    pthread_mutex_lock(&G.alloc_mx);
    for (int i = 0; i < MAX_RESV; i++) if (resv[i].used && resv[i].node == node) changed |= home_finish(i, 0);
    pthread_mutex_unlock(&G.alloc_mx);
    if (changed) { pager_update_self_free(); LOG(LOG_INFO, "reservas del nodo %d deshechas", node); }
}

/* ALLOC_REQUEST (hoja). */
void alloc_on_request(const msg_header_t *h, const uint8_t *pl, uint32_t len) {
    uint32_t cf = 0, ce = 0; uint8_t b[ALLOC_CONFLICT_LEN]; int rc;
    if (len < ALLOC_REQ_LEN) rc = DSM_E_BAD_MSG;
    else rc = home_reserve(get64(pl), get32(pl + 8), get32(pl + 12), h->src_node, &cf, &ce);
    put32(b, cf); put32(b + 4, ce);
    send_msg_node(h->src_node, MSG_ALLOC_REPLY, 0, h->request_id, h->page_id, rc,
                  rc == DSM_E_NOMEM ? b : NULL, rc == DSM_E_NOMEM ? sizeof b : 0, NULL, 0);
}

/* ALLOC_ABORT (hoja). */
void alloc_on_abort(const uint8_t *pl, uint32_t len) {
    if (len >= ALLOC_ABORT_LEN) home_end(get64(pl), 0);
}

/* ---------- lado que asigna ---------- */

typedef struct { uint32_t first, end; } range_t;
typedef struct { int home; uint32_t first, num; int reserved; } seg_t;

static int snap_home(const node_entry_t *nt, int nn, uint32_t p) {
    for (int i = 0; i < nn; i++)
        if (nt[i].state != N_UNUSED && p >= nt[i].first_page && p < nt[i].first_page + nt[i].num_pages) return i;
    return -1;
}

/* Elige el inicio de un rango libre de n páginas. Prefiere el que tenga más páginas en el slice
 * propio (así una asignación que cabe localmente no usa la red) y, a igualdad, la dirección más
 * baja. Devuelve -1 si no hay ningún rango libre de ese tamaño. */
static int64_t choose_range(uint32_t n, const node_entry_t *nt, int nn, const range_t *blk, int nb) {
    uint32_t total = 0;
    for (int i = 0; i < nn; i++)
        if (nt[i].state != N_UNUSED && nt[i].first_page + nt[i].num_pages > total) total = nt[i].first_page + nt[i].num_pages;
    if (total > G.max_pages) total = G.max_pages;
    uint8_t *ok = calloc(total ? total : 1, 1);
    alloc_entry_t *al = malloc(sizeof(alloc_entry_t) * MAX_ALLOCS);
    if (!ok || !al) { free(ok); free(al); return -1; }
    for (int i = 0; i < nn; i++) {                       /* solo homes vivos pueden recibir páginas */
        if (nt[i].state != N_ALIVE) continue;
        uint32_t e = nt[i].first_page + nt[i].num_pages; if (e > total) e = total;
        memset(ok + nt[i].first_page, 1, e - nt[i].first_page);
    }
    int na = alloc_table_snapshot(al, MAX_ALLOCS);       /* lo ya asignado en todo el clúster */
    for (int k = 0; k < na; k++)
        for (uint32_t p = al[k].first_page; p < al[k].first_page + al[k].num_pages && p < total; p++) ok[p] = 0;
    for (uint32_t p = G.slice_first; p < G.slice_first + G.slice_num && p < total; p++)   /* reservas en curso aquí */
        if (dflags(p) & (DF_ALLOCATED | DF_RESERVED)) ok[p] = 0;
    for (int k = 0; k < nb; k++)
        for (uint32_t p = blk[k].first; p < blk[k].end && p < total; p++) ok[p] = 0;

    uint32_t of = G.slice_first, oe = G.slice_first + G.slice_num;
    int64_t best = -1; uint32_t best_ov = 0;
    for (uint32_t s = 0; s < total;) {                   /* recorre los tramos libres maximales [s, e) */
        if (!ok[s]) { s++; continue; }
        uint32_t e = s; while (e < total && ok[e]) e++;
        if (e - s >= n) {
            uint32_t lo = s, hi = e - n, cand[4] = { lo, hi, of, oe >= n ? oe - n : 0 };
            for (int c = 0; c < 4; c++) {
                uint32_t st = cand[c] < lo ? lo : cand[c] > hi ? hi : cand[c];
                uint32_t a = st > of ? st : of, b = st + n < oe ? st + n : oe, ov = b > a ? b - a : 0;
                if (best < 0 || ov > best_ov || (ov == best_ov && st < (uint32_t)best)) { best = st; best_ov = ov; }
            }
        }
        s = e;
    }
    free(ok); free(al);
    return best;
}

static void abort_segs(uint64_t id, seg_t *sg, int ns) {
    uint8_t b[ALLOC_ABORT_LEN]; put64(b, id);
    for (int i = 0; i < ns; i++) {
        if (!sg[i].reserved) continue;
        if (sg[i].home == G.me) home_end(id, 0);
        else send_msg_node(sg[i].home, MSG_ALLOC_ABORT, 0, 0, sg[i].first, DSM_OK, b, sizeof b, NULL, 0);
        sg[i].reserved = 0;
    }
}

/* Reserva un tramo en su home. Devuelve DSM_OK o el error; con DSM_E_NOMEM llena el tramo ocupado. */
static int reserve_seg(uint64_t id, seg_t *s, range_t *busy) {
    uint32_t cf = s->first, ce = s->first + s->num; int rc;
    if (s->home == G.me) rc = home_reserve(id, s->first, s->num, G.me, &cf, &ce);
    else {
        uint8_t req[ALLOC_REQ_LEN], rep[ALLOC_CONFLICT_LEN];
        put64(req, id); put32(req + 8, s->first); put32(req + 12, s->num);
        waiter_t *w = waiter_get(W_ALLOC, s->home, s->first, rep, sizeof rep, 1);
        if (!w) return DSM_E_NOMEM;
        rc = send_msg_node(s->home, MSG_ALLOC_REQUEST, 0, w->request_id, s->first, 0, req, sizeof req, NULL, 0);
        if (rc == DSM_OK) rc = waiter_wait(w, G.cfg.request_timeout_ms);
        if (rc == DSM_OK) {
            rc = w->status;
            if (rc == DSM_E_NOMEM && w->reply_len == sizeof rep) { cf = get32(rep); ce = get32(rep + 4); }
        }
        waiter_put(w);
        /* Sin respuesta, el home pudo haber reservado: el ABORT de abort_segs lo deshace. */
        if (rc == DSM_E_TIMEOUT) s->reserved = 1;
    }
    if (rc == DSM_E_NOMEM && ce <= cf) { cf = s->first; ce = s->first + s->num; }
    busy->first = cf; busy->end = ce;
    return rc;
}

int alloc_distributed(size_t size, void **out) {
    uint32_t n = (uint32_t)((size + G.cfg.page_size - 1) / G.cfg.page_size);
    if ((uint64_t)n > G.max_pages) return DSM_E_NOMEM;
    node_entry_t *nt = malloc(sizeof(node_entry_t) * MAX_NODES_HARD);
    if (!nt) return DSM_E_NOMEM;
    range_t blk[MAX_BLOCKED]; int nb = 0, rc = DSM_E_NOMEM;
    seg_t sg[MAX_NODES_HARD];

    for (int attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
        pthread_mutex_lock(&G.nodes_mx);
        int nn = G.n_nodes; memcpy(nt, G.nodes, sizeof(node_entry_t) * nn);
        pthread_mutex_unlock(&G.nodes_mx);
        int64_t start = choose_range(n, nt, nn, blk, nb);
        if (start < 0) { rc = DSM_E_NOMEM; break; }

        int ns = 0;                                          /* parte el rango por home, en orden ascendente */
        for (uint32_t p = (uint32_t)start, end = (uint32_t)start + n; p < end;) {
            int h = snap_home(nt, nn, p);
            uint32_t he = nt[h].first_page + nt[h].num_pages, e = he < end ? he : end;
            sg[ns++] = (seg_t){ h, p, e - p, 0 };
            p = e;
        }
        pthread_mutex_lock(&G.alloc_mx);
        uint64_t id = ((uint64_t)G.me << 48) | (++G.alloc_counter & 0xFFFFFFFFFFFFULL);
        pthread_mutex_unlock(&G.alloc_mx);

        rc = DSM_OK;
        for (int i = 0; i < ns && rc == DSM_OK; i++) {
            range_t busy;
            rc = reserve_seg(id, &sg[i], &busy);
            if (rc == DSM_OK) sg[i].reserved = 1;
            else if (nb < MAX_BLOCKED) blk[nb++] = busy;   /* no volver a intentar ese tramo */
        }
        if (rc != DSM_OK) {
            abort_segs(id, sg, ns);
            LOG(LOG_INFO, "dsm_alloc: reserva de %u..%u rechazada (%s), se reintenta en otro rango",
                (uint32_t)start, (uint32_t)start + n - 1, dsm_strerror(rc));
            if (rc != DSM_E_NOMEM && rc != DSM_E_TIMEOUT && rc != DSM_E_NODE_DOWN) break;
            rc = DSM_E_NOMEM;
            continue;
        }

        /* Fase 2: confirmar en los homes y difundir a todos. */
        alloc_entry_t a = { .alloc_id = id, .size_bytes = size, .dsm_pid = make_dsm_pid(G.me, (uint32_t)getpid()),
                            .first_page = (uint32_t)start, .num_pages = n };
        if (alloc_table_add(&a) != DSM_OK) LOG(LOG_WARN, "tabla de asignaciones llena (MAX_ALLOCS)");
        home_end(id, 1);
        uint8_t wire[ALLOC_WIRE_LEN]; alloc_wire(wire, &a);
        for (int i = 0; i < nn; i++)
            if (i != G.me && (nt[i].state == N_ALIVE || nt[i].state == N_SUSPECT))
                send_msg_node(i, MSG_ALLOC_COMMIT, 0, 0, (uint32_t)start, DSM_OK, wire, sizeof wire, NULL, 0);
        if (ns > 1) LOG(LOG_INFO, "dsm_alloc: %u páginas (%u..%u) repartidas en %d homes",
                        n, (uint32_t)start, (uint32_t)start + n - 1, ns);
        *out = app_addr((uint32_t)start);
        break;
    }
    free(nt);
    return rc;
}

/* ---------- dsm_free: FREE_REQUEST / FREE_REPLY / FREE_COMMIT ----------
 * Solo el nodo que hizo la asignación puede liberarla, y ptr debe ser su inicio. El nodo pide a
 * cada home involucrado (en orden ascendente) que recupere su tramo; el home revoca las copias,
 * pone las páginas en cero y las vuelve a contar como libres. Al final, FREE_COMMIT quita la
 * asignación de la tabla replicada en todos los nodos. Un acceso posterior a esa memoria es un
 * error de la aplicación (uso después de liberar) y termina en un fallo fatal "pagina no asignada". */

/* FREE_REQUEST (hilo de servicio: pager_reclaim espera a la red). */
void alloc_serve_free(const msg_header_t *h, const uint8_t *pl, uint32_t len) {
    int rc = DSM_OK;
    if (len < FREE_REQ_LEN) rc = DSM_E_BAD_MSG;
    else {
        uint32_t first = get32(pl + 8), num = get32(pl + 12);
        if (!num || first < G.slice_first || first + num > G.slice_first + G.slice_num) rc = DSM_E_INVAL;
        else pager_reclaim(first, num);
    }
    send_msg_node(h->src_node, MSG_FREE_REPLY, 0, h->request_id, h->page_id, rc, NULL, 0, NULL, 0);
}

/* FREE_COMMIT (hoja). */
void alloc_on_free_commit(const uint8_t *pl, uint32_t len) {
    if (len >= FREE_COMMIT_LEN) alloc_table_remove(get64(pl));
}

int alloc_free(void *ptr) {
    uintptr_t a = (uintptr_t)ptr, base = (uintptr_t)G.app;
    if (a < base || a >= base + G.span || (a - base) % G.cfg.page_size) return DSM_E_INVAL;
    uint32_t p = (uint32_t)((a - base) / G.cfg.page_size);
    alloc_entry_t e;
    if (p == 0 || !alloc_table_find(p, &e) || e.first_page != p) return DSM_E_INVAL;   /* la raíz no se libera */
    if ((int)(e.alloc_id >> 48) != G.me) return DSM_E_PERM;
    int rc = DSM_OK;
    for (uint32_t q = e.first_page, end = e.first_page + e.num_pages; q < end;) {
        int home = home_of(q);
        uint32_t he = q, num;
        while (he < end && home_of(he) == home) he++;
        num = he - q;
        if (home == G.me) pager_reclaim(q, num);
        else {
            uint8_t req[FREE_REQ_LEN]; put64(req, e.alloc_id); put32(req + 8, q); put32(req + 12, num);
            waiter_t *w = waiter_get(W_FREE, home, q, NULL, 0, 1);
            if (!w) { rc = DSM_E_NOMEM; break; }
            int r = send_msg_node(home, MSG_FREE_REQUEST, 0, w->request_id, q, 0, req, sizeof req, NULL, 0);
            /* El home puede tener que revocar muchas páginas: el plazo crece con el tramo. */
            if (r == DSM_OK) r = waiter_wait(w, G.cfg.request_timeout_ms * G.cfg.max_retries + num);
            if (r == DSM_OK) r = w->status;
            waiter_put(w);
            if (r != DSM_OK) { LOG(LOG_WARN, "dsm_free: el home %d no recuperó %u..%u (%s)", home, q, he - 1, dsm_strerror(r)); rc = r; }
        }
        q = he;
    }
    /* Aunque un home haya caído, la asignación deja de existir para el resto del clúster. */
    alloc_table_remove(e.alloc_id);
    uint8_t b[FREE_COMMIT_LEN]; put64(b, e.alloc_id);
    pthread_mutex_lock(&G.nodes_mx); int nn = G.n_nodes; pthread_mutex_unlock(&G.nodes_mx);
    for (int i = 0; i < nn; i++) if (i != G.me) send_msg_node(i, MSG_FREE_COMMIT, 0, 0, e.first_page, DSM_OK, b, sizeof b, NULL, 0);
    return rc == DSM_E_NODE_DOWN ? DSM_OK : rc;
}
