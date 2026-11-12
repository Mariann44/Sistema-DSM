/* dsm.c - API pública: inicialización/cierre, asignación, consultas. */
#include "internal.h"
#include <signal.h>

dsm_ctx_t G;

int dsm_init(const char *config_path) {
    if (G.inited) return DSM_E_ALREADY_INIT;
    if (!config_path) return DSM_E_INVAL;
    int rc = config_load(config_path, &G.cfg);
    if (rc) return rc;
    pthread_mutex_init(&G.nodes_mx, NULL); pthread_mutex_init(&G.waiters_mx, NULL); pthread_mutex_init(&G.alloc_mx, NULL);
    pthread_rwlock_init(&G.allocs_rw, NULL);
    for (int i = 0; i < MAX_NODES_HARD; i++) {
        G.nodes[i].data_fd = -1; G.nodes[i].hb_fd = -1; pthread_mutex_init(&G.nodes[i].send_lock, NULL);
    }
    G.listen_fd = G.hb_listen_fd = -1;
    for (int i = 0; i < MAX_WAITERS; i++) sem_init(&G.waiters[i].sem, 0, 0);
    signal(SIGPIPE, SIG_IGN);
    G.max_pages = ((size_t)G.cfg.max_global_mb << 20) / G.cfg.page_size;
    if ((rc = pager_init())) return rc;
    if ((rc = membership_start())) return rc;
    if ((rc = net_start())) return rc;
    if ((rc = membership_join())) { net_stop(); return rc; }
    if ((rc = heartbeat_start())) { net_stop(); return rc; }
    if ((rc = ctl_start())) { heartbeat_stop(); net_stop(); return rc; }
    G.inited = 1;
    return DSM_OK;
}

int dsm_finalize(void) {
    if (!G.inited) return DSM_E_NOT_INIT;
    /* TODO: PAGE_WRITEBACK de las páginas remotas en M antes de salir. */
    membership_broadcast(MSG_NODE_LEAVE);
    usleep(100000);
    ctl_stop(); heartbeat_stop(); net_stop(); pager_shutdown();
    G.inited = 0;
    return DSM_OK;
}

int dsm_alloc(size_t size, void **out) {
    if (!G.inited) return DSM_E_NOT_INIT;
    if (!out || !size) return DSM_E_INVAL;
    return pager_alloc(size, out);
}
int dsm_free(void *ptr) { (void)ptr; return DSM_E_NOT_IMPL; }
void *dsm_root(void) { return G.inited ? (void *)G.app : NULL; }

int dsm_prefetch(void *ptr, size_t len, int for_write) {
    if (!G.inited) return DSM_E_NOT_INIT;
    uintptr_t a = (uintptr_t)ptr, base = (uintptr_t)G.app;
    if (!len || a < base || a + len > base + G.span) return DSM_E_INVAL;
    uintptr_t ps = G.cfg.page_size;
    for (uintptr_t q = a & ~(ps - 1); q < a + len; q += ps) { /* provoca el fallo en este hilo, no en una syscall */
        volatile uint8_t *c = (volatile uint8_t *)q;
        if (for_write) *c = *c; else (void)*c;
    }
    return DSM_OK;
}

dsm_node_t dsm_node_id(void) { return G.me; }
int dsm_num_nodes(void) {
    int n = 0;
    pthread_mutex_lock(&G.nodes_mx);
    for (int i = 0; i < G.n_nodes; i++) if (G.nodes[i].state == N_ALIVE || G.nodes[i].state == N_SUSPECT) n++;
    pthread_mutex_unlock(&G.nodes_mx);
    return n;
}
size_t dsm_page_size(void) { return G.cfg.page_size; }

/* dsm_lock / dsm_unlock: ver sync.c */
int dsm_barrier(dsm_barrier_t *b, uint32_t c) { (void)b; (void)c; return DSM_E_NOT_IMPL; }

/* Llena las estadísticas locales; no es static para que ctl.c u otros módulos puedan usarla si se requiere */
void fill_local_stats(dsm_stats_t *o) {
    memset(o, 0, sizeof *o);
    o->read_faults = G.st.read_faults;
    o->write_faults = G.st.write_faults;
    o->pages_sent = G.st.pages_sent;
    o->pages_received = G.st.pages_received;
    o->invalidations_sent = G.st.inv_sent;
    o->invalidations_received = G.st.inv_received;
    o->bytes_sent = G.st.bytes_sent;
    o->bytes_received = G.st.bytes_received;
    o->fault_time_total_us = G.st.fault_us;
    o->messages_sent = G.st.msgs_sent;
    o->pages_owned = G.slice_num;
    o->pages_allocated = G.slice_num - pager_free_pages();
    for (uint32_t p = 0; p < G.max_pages; p++) {
        if ((p < G.slice_first || p >= G.slice_first + G.slice_num) &&
            (G.pages[p].state == PG_S || G.pages[p].state == PG_M)) {
            o->pages_cached++;
        }
    }
}

static void stats_to_wire(const dsm_stats_t *s, uint8_t buf[96]) {
    put64(buf + 0,  s->read_faults);
    put64(buf + 8,  s->write_faults);
    put64(buf + 16, s->pages_sent);
    put64(buf + 24, s->pages_received);
    put64(buf + 32, s->invalidations_sent);
    put64(buf + 40, s->invalidations_received);
    put64(buf + 48, s->bytes_sent);
    put64(buf + 56, s->bytes_received);
    put64(buf + 64, s->fault_time_total_us);
    put64(buf + 72, s->messages_sent);
    put32(buf + 80, s->pages_owned);
    put32(buf + 84, s->pages_allocated);
    put32(buf + 88, s->pages_cached);
    put32(buf + 92, s->reserved);
}

static void wire_to_stats(const uint8_t buf[96], dsm_stats_t *s) {
    s->read_faults           = get64(buf + 0);
    s->write_faults          = get64(buf + 8);
    s->pages_sent            = get64(buf + 16);
    s->pages_received        = get64(buf + 24);
    s->invalidations_sent    = get64(buf + 32);
    s->invalidations_received= get64(buf + 40);
    s->bytes_sent            = get64(buf + 48);
    s->bytes_received        = get64(buf + 56);
    s->fault_time_total_us   = get64(buf + 64);
    s->messages_sent         = get64(buf + 72);
    s->pages_owned           = get32(buf + 80);
    s->pages_allocated       = get32(buf + 84);
    s->pages_cached          = get32(buf + 88);
    s->reserved              = get32(buf + 92);
}

/* Manejador de mensaje de hoja MSG_STATS_REQUEST: se atiende en línea por el hilo de red sin bloquear */
void stats_on_request(const msg_header_t *h) {
    dsm_stats_t st;
    fill_local_stats(&st);
    uint8_t payload[96];
    stats_to_wire(&st, payload);
    send_msg_node(h->src_node, MSG_STATS_REPLY, 0, h->request_id, 0, DSM_OK, payload, sizeof(payload), NULL, 0);
}

int dsm_get_stats(dsm_scope_t scope, dsm_stats_t *o) {
    if (!G.inited) return DSM_E_NOT_INIT;
    if (!o) return DSM_E_INVAL;

    if (scope == DSM_LOCAL) {
        fill_local_stats(o);
        return DSM_OK;
    }

    if (scope != DSM_GLOBAL) return DSM_E_INVAL;

    /* DSM_GLOBAL: recolectar estadísticas de todos los nodos activos en el clúster */
    dsm_stats_t sum;
    fill_local_stats(&sum);

    /* Obtener lista de nodos remotos a consultar */
    int targets[MAX_NODES_HARD];
    int n_targets = 0;

    pthread_mutex_lock(&G.nodes_mx);
    for (int i = 0; i < G.n_nodes; i++) {
        if (i == G.me) continue;
        int st = G.nodes[i].state;
        if (st == N_ALIVE || st == N_SUSPECT) {
            targets[n_targets++] = i;
        }
    }
    pthread_mutex_unlock(&G.nodes_mx);

    /* Consultar cada nodo remoto con un waiter_t y reply_buf */
    for (int t = 0; t < n_targets; t++) {
        int node = targets[t];
        uint8_t wire_buf[96];
        waiter_t *w = waiter_get(W_STATS, node, 0, wire_buf, sizeof(wire_buf), 1);
        if (!w) return DSM_E_NOMEM;

        int rc = send_msg_node(node, MSG_STATS_REQUEST, 0, w->request_id, 0, 0, NULL, 0, NULL, 0);
        if (rc != DSM_OK) {
            waiter_put(w);
            return (rc == DSM_E_NODE_DOWN) ? DSM_E_NODE_DOWN : DSM_E_NET;
        }

        rc = waiter_wait(w, G.cfg.request_timeout_ms);
        if (rc != DSM_OK) {
            waiter_put(w);
            return DSM_E_TIMEOUT;
        }

        if (w->status != DSM_OK) {
            int err = w->status;
            waiter_put(w);
            return err;
        }

        if (w->reply_len != sizeof(wire_buf)) {
            waiter_put(w);
            return DSM_E_BAD_MSG;
        }

        dsm_stats_t node_st;
        wire_to_stats(wire_buf, &node_st);
        waiter_put(w);

        /* Sumar los resultados */
        sum.read_faults           += node_st.read_faults;
        sum.write_faults          += node_st.write_faults;
        sum.pages_sent            += node_st.pages_sent;
        sum.pages_received        += node_st.pages_received;
        sum.invalidations_sent    += node_st.invalidations_sent;
        sum.invalidations_received+= node_st.invalidations_received;
        sum.bytes_sent            += node_st.bytes_sent;
        sum.bytes_received        += node_st.bytes_received;
        sum.fault_time_total_us   += node_st.fault_time_total_us;
        sum.messages_sent         += node_st.messages_sent;
        sum.pages_owned           += node_st.pages_owned;
        sum.pages_allocated       += node_st.pages_allocated;
        sum.pages_cached          += node_st.pages_cached;
    }

    *o = sum;
    return DSM_OK;
}

const char *dsm_strerror(int e) {
    switch (e) {
    case DSM_OK: return "exito"; case DSM_E_INVAL: return "parametro invalido";
    case DSM_E_NOT_INIT: return "dsm_init no se ha llamado"; case DSM_E_ALREADY_INIT: return "dsm_init ya se llamo";
    case DSM_E_CONFIG: return "configuracion invalida o distinta a la del cluster"; case DSM_E_MMAP: return "fallo de memfd/mmap";
    case DSM_E_NET: return "error de red"; case DSM_E_JOIN_TIMEOUT: return "ningun nodo respondio al unirse";
    case DSM_E_CLUSTER_FULL: return "cluster lleno"; case DSM_E_NOMEM: return "sin memoria contigua libre";
    case DSM_E_TIMEOUT: return "tiempo de espera agotado"; case DSM_E_NODE_DOWN: return "nodo caido";
    case DSM_E_PERM: return "operacion no permitida"; case DSM_E_MISMATCH: return "barrera con count distinto";
    case DSM_E_BAD_MSG: return "mensaje mal formado"; case DSM_E_NOT_IMPL: return "no implementado aun";
    default: return "error desconocido";
    }
}
