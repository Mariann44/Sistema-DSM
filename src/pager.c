/* pager.c - doble mapeo (memfd), manejador de SIGSEGV, tabla de páginas local,
 * máquina de estados MSI y directorio del home. */
#include "internal.h"
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#define NONE_OWNER 0xFFFF

static inline int prot_of(int st) { return st == PG_M ? (PROT_READ | PROT_WRITE) : st == PG_S ? PROT_READ : PROT_NONE; }

/* Diagnóstico sin malloc/printf: el manejador no puede devolver un error a la aplicación. */
static void fatal_fault(uint32_t p, int home, const char *cause) {
    char b[256];
    int n = snprintf(b, sizeof b, "[dsm n%u] FALLO FATAL: addr=%p page_id=%u home=%d causa=%s\n",
                     (unsigned)G.me, (void *)app_addr(p), p, home, cause);
    if (write(2, b, n) < 0) { }
    abort();
}

static void local_set(uint32_t p, int st) {
    sem_wait(plock(p));
    mprotect(app_addr(p), G.cfg.page_size, prot_of(st));
    G.pages[p].state = st;
    sem_post(plock(p));
}

/* ---------- home: operaciones del directorio (con dir_locks[p] tomado) ---------- */
static int fetch_from(uint32_t p, int owner, int inval) {
    if (owner == G.me) { local_set(p, inval ? PG_I : PG_S); return DSM_OK; }
    uint32_t ps = G.cfg.page_size; uint8_t *tmp = malloc(4 + ps); int rc = DSM_E_TIMEOUT;
    if (!tmp) return DSM_E_NOMEM;
    for (uint32_t t = 0; t < G.cfg.max_retries && rc == DSM_E_TIMEOUT; t++) {
        waiter_t *w = waiter_get(W_FETCH, owner, p, tmp, 4 + ps, 1);
        if (!w) { rc = DSM_E_NOMEM; break; }
        rc = send_msg_node(owner, MSG_PAGE_FETCH, inval ? F_FETCH_INVAL : 0, w->request_id, p, 0, NULL, 0, NULL, 0);
        if (rc == DSM_OK) {
            rc = waiter_wait(w, G.cfg.request_timeout_ms);
            if (rc == DSM_OK) { rc = w->status; if (rc == DSM_OK && w->reply_len != 4 + ps) rc = DSM_E_BAD_MSG; }
        }
        waiter_put(w);
    }
    if (rc == DSM_OK) memcpy(sys_addr(p), tmp + 4, ps);
    free(tmp);
    return rc;
}

static int invalidate_set(uint32_t p, uint64_t mask) {
    if (mask & (1ULL << G.me)) { local_set(p, PG_I); mask &= ~(1ULL << G.me); }
    int count = __builtin_popcountll(mask);
    if (!count) return DSM_OK;
    waiter_t *w = waiter_get(W_INVAL, NODE_UNKNOWN, p, NULL, 0, count);
    if (!w) return DSM_E_NOMEM;
    for (int i = 0; i < MAX_NODES_HARD; i++) {
        if (!(mask & (1ULL << i))) continue;
        atomic_fetch_add(&G.st.inv_sent, 1);
        if (send_msg_node(i, MSG_INVALIDATE, 0, w->request_id, p, 0, NULL, 0, NULL, 0) != DSM_OK) {
            msg_header_t fake = { .type = MSG_INVALIDATE_ACK, .request_id = w->request_id }; /* nodo caído: no hay quien invalidar */
            waiter_complete(&fake, NULL, 0);
        }
    }
    int rc = waiter_wait(w, G.cfg.request_timeout_ms * 2);
    if (rc == DSM_OK) rc = w->status;
    waiter_put(w);
    return rc;
}

/* Atiende una solicitud (lectura o escritura) de R sobre la página p. */
static int home_serve(uint32_t p, uint16_t R, int write, int has_copy, int *has_data, int *grant_w, uint32_t *ver) {
    dir_entry_t *d = &G.dir[p]; uint64_t bit = 1ULL << R; int rc = DSM_OK;
    *has_data = 0; *grant_w = write;
    if (!(d->flags & DF_ALLOCATED)) return DSM_E_INVAL;
    if (!write) {
        if (d->owner == R) { *grant_w = 1; goto out; } /* reintento tardío: ya es dueño */
        if (d->owner != NONE_OWNER) {
            if ((rc = fetch_from(p, d->owner, 0))) return rc;
            d->copyset |= 1ULL << d->owner; d->owner = NONE_OWNER;
        }
        d->copyset |= bit; *has_data = 1;
        if (R == G.me) local_set(p, PG_S);
    } else {
        uint16_t prev = d->owner;
        if (prev != NONE_OWNER && prev != R) { if ((rc = fetch_from(p, prev, 1))) return rc; d->owner = NONE_OWNER; }
        int had = (d->copyset & bit) != 0;
        if ((rc = invalidate_set(p, d->copyset & ~bit))) return rc;
        d->copyset = 0; d->owner = R; d->version++;
        if (R == G.me) local_set(p, PG_M);
        *has_data = !((has_copy && had) || prev == R);
    }
out:
    *ver = d->version;
    return DSM_OK;
}

void pager_serve_request(const msg_header_t *h) {
    uint32_t p = h->page_id; uint16_t R = h->src_node;
    int has_data = 0, gw = 0, rc; uint32_t ver = 0; uint8_t vb[4];
    if (home_of(p) != G.me) { send_msg_node(R, MSG_PAGE_DATA, 0, h->request_id, p, DSM_E_INVAL, NULL, 0, NULL, 0); return; }
    sem_wait(dlock(p)); /* se mantiene hasta enviar PAGE_DATA: así el orden en el socket es el del directorio */
    rc = home_serve(p, R, (h->flags & F_WRITE) != 0, (h->flags & F_HAS_COPY) != 0, &has_data, &gw, &ver);
    if (rc != DSM_OK) send_msg_node(R, MSG_PAGE_DATA, 0, h->request_id, p, rc, NULL, 0, NULL, 0);
    else {
        put32(vb, ver);
        send_msg_node(R, MSG_PAGE_DATA, (gw ? F_WRITE : 0) | (has_data ? F_HAS_DATA : 0), h->request_id, p, DSM_OK,
                      vb, 4, has_data ? sys_addr(p) : NULL, has_data ? G.cfg.page_size : 0);
        if (has_data) atomic_fetch_add(&G.st.pages_sent, 1);
    }

    sem_post(dlock(p));
}

uint32_t pager_collect_pages(uint8_t *buf, uint32_t cap) {
    uint32_t count = 0;
    uint32_t first = G.slice_first;
    uint32_t end = first + G.slice_num;
    if (cap < PAGE_INFO_WIRE_LEN) return 0;

    for (uint32_t p = first; p < end; p++) {
        uint16_t owner;
        uint32_t version;
        uint8_t allocated;
        sem_wait(dlock(p));
        allocated = (G.dir[p].flags & DF_ALLOCATED) != 0;
        owner = G.dir[p].owner;
        version = G.dir[p].version;
        sem_post(dlock(p));
        if (!allocated) continue;
        if (count > (cap - PAGE_INFO_WIRE_LEN) / PAGE_INFO_WIRE_LEN) break;

        uint8_t *out = buf + count * PAGE_INFO_WIRE_LEN;
        put32(out, p);
        out[4] = G.pages[p].state;
        out[5] = allocated;
        put16(out + 6, owner);
        put32(out + 8, version);
        put16(out + 12, G.me);
        put16(out + 14, 0);
        count++;
    }
    return count * PAGE_INFO_WIRE_LEN;
}

void pager_serve_pages_request(const msg_header_t *h) {
    uint8_t *payload = malloc(MAX_PAYLOAD);
    if (!payload) {
        send_msg_node(h->src_node, MSG_PAGES_REPLY, 0, h->request_id, 0, DSM_E_NOMEM, NULL, 0, NULL, 0);
        return;
    }
    uint32_t len = pager_collect_pages(payload, MAX_PAYLOAD);
    send_msg_node(h->src_node, MSG_PAGES_REPLY, 0, h->request_id, 0, DSM_OK, payload, len, NULL, 0);
    free(payload);
}

/* ---------- nodo con copia: mensajes del hilo de red (hoja, nunca esperan a otro nodo) ---------- */
void pager_on_fetch(const msg_header_t *h) {
    uint32_t p = h->page_id; int inval = (h->flags & F_FETCH_INVAL) != 0; uint8_t vb[4];
    sem_wait(plock(p));
    mprotect(app_addr(p), G.cfg.page_size, inval ? PROT_NONE : PROT_READ); /* primero bloquea escrituras, luego copia */
    G.pages[p].state = inval ? PG_I : PG_S;
    put32(vb, G.pages[p].version);
    send_msg_node(h->src_node, MSG_PAGE_FETCH_REPLY, 0, h->request_id, p, DSM_OK, vb, 4, sys_addr(p), G.cfg.page_size);
    sem_post(plock(p));
    atomic_fetch_add(&G.st.pages_sent, 1);
}

void pager_on_invalidate(const msg_header_t *h) {
    uint32_t p = h->page_id;
    sem_wait(plock(p));
    int st = G.pages[p].state;
    if (st == PG_S) { mprotect(app_addr(p), G.cfg.page_size, PROT_NONE); G.pages[p].state = PG_I; }
    else if (st == PG_PW) mprotect(app_addr(p), G.cfg.page_size, PROT_NONE); /* copia S vieja; el home enviará datos */
    /* PR: la invalidación es anterior a la concesión (FIFO en el socket del home). M: no debería ocurrir. */
    sem_post(plock(p));
    atomic_fetch_add(&G.st.inv_received, 1);
    send_msg_node(h->src_node, MSG_INVALIDATE_ACK, 0, h->request_id, p, DSM_OK, NULL, 0, NULL, 0);
}

static int waiter_pending(uint32_t rid) {
    int r = 0;
    pthread_mutex_lock(&G.waiters_mx);
    for (int i = 0; i < MAX_WAITERS; i++) if (G.waiters[i].in_use && G.waiters[i].request_id == rid && G.waiters[i].kind == W_PAGE) r = 1;
    pthread_mutex_unlock(&G.waiters_mx);
    return r;
}

void pager_on_page_data(const msg_header_t *h, const uint8_t *pl, uint32_t len) {
    uint32_t p = h->page_id, ps = G.cfg.page_size;
    if (!waiter_pending(h->request_id)) return; /* respuesta tardía: se descarta */
    sem_wait(plock(p));
    local_page_t *lp = &G.pages[p];
    if (h->status != DSM_OK) { mprotect(app_addr(p), ps, PROT_NONE); lp->state = PG_I; }
    else if (lp->state == PG_PR || lp->state == PG_PW) {
        if ((h->flags & F_HAS_DATA) && len >= 4 + ps) memcpy(sys_addr(p), pl + 4, ps);
        if (len >= 4) lp->version = get32(pl);
        lp->state = (h->flags & F_WRITE) ? PG_M : PG_S;
        mprotect(app_addr(p), ps, prot_of(lp->state));
        if (h->flags & F_HAS_DATA) atomic_fetch_add(&G.st.pages_received, 1);
    }
    sem_post(plock(p));
    waiter_complete(h, NULL, 0); /* despierta al hilo que falló */
}

/* ---------- lado que falla ---------- */
static int remote_request(uint32_t p, int home, int write, int has_copy) {
    int rc = DSM_E_TIMEOUT;
    for (uint32_t t = 0; t < G.cfg.max_retries && rc == DSM_E_TIMEOUT; t++) {
        waiter_t *w = waiter_get(W_PAGE, home, p, NULL, 0, 1);
        if (!w) return DSM_E_NOMEM;
        rc = send_msg_node(home, MSG_PAGE_REQUEST, (write ? F_WRITE : 0) | (has_copy ? F_HAS_COPY : 0), w->request_id, p, 0, NULL, 0, NULL, 0);
        if (rc == DSM_OK) { rc = waiter_wait(w, G.cfg.request_timeout_ms); if (rc == DSM_OK) rc = w->status; }
        waiter_put(w); /* cada reintento usa un request_id nuevo */
    }
    return rc;
}

static void fault_page(uint32_t p, int write) {
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t deadline = now_ms() + (uint64_t)G.cfg.request_timeout_ms * G.cfg.max_retries;
    atomic_fetch_add(write ? &G.st.write_faults : &G.st.read_faults, 1);
    for (;;) {
        sem_wait(plock(p));
        int st = G.pages[p].state;
        if (st == PG_M || (st == PG_S && !write)) { sem_post(plock(p)); break; }
        int home = home_of(p);
        if (home < 0) { sem_post(plock(p)); fatal_fault(p, home, "pagina fuera del espacio global"); }
        if (home == G.me) { /* el solicitante es el propio home: mismo procedimiento sin red */
            sem_post(plock(p));
            int hd, gw, rc; uint32_t ver;
            sem_wait(dlock(p)); rc = home_serve(p, G.me, write, 0, &hd, &gw, &ver); sem_post(dlock(p));
            if (rc) fatal_fault(p, home, rc == DSM_E_INVAL ? "pagina no asignada" : "error en el directorio local");
            continue;
        }
        if (st == PG_PR || st == PG_PW) { /* otro hilo ya la pidió: esperar el resultado */
            sem_post(plock(p));
            if (now_ms() > deadline) fatal_fault(p, home, "timeout esperando otra solicitud en curso");
            usleep(100); continue;
        }
        int has_copy = (st == PG_S);
        G.pages[p].state = write ? PG_PW : PG_PR;
        sem_post(plock(p));
        int rc = remote_request(p, home, write, has_copy);
        if (rc) fatal_fault(p, home, rc == DSM_E_NODE_DOWN ? "nodo home caido" : rc == DSM_E_INVAL ? "pagina no asignada" :
                                      rc == DSM_E_TIMEOUT ? "timeout (MAX_RETRIES agotados)" : "error de red");
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    atomic_fetch_add(&G.st.fault_us, (t1.tv_sec - t0.tv_sec) * 1000000ULL + (t1.tv_nsec - t0.tv_nsec) / 1000);
}

static void segv_handler(int sig, siginfo_t *si, void *uc) {
    (void)sig;
    uintptr_t a = (uintptr_t)si->si_addr, base = (uintptr_t)G.app;
    if (a < base || a >= base + G.span) { signal(SIGSEGV, SIG_DFL); return; } /* error real de la aplicación */
    uint32_t p = (a - base) / G.cfg.page_size; int write;
#if defined(__x86_64__)
    write = (((ucontext_t *)uc)->uc_mcontext.gregs[REG_ERR] & 2) != 0;
#else
    (void)uc; write = (G.pages[p].state == PG_S); /* sin decodificar el contexto: lee y luego escala a escritura */
#endif
    fault_page(p, write);
}

/* ---------- inicialización ---------- */
int pager_init(void) {
    dsm_config_t *c = &G.cfg;
    G.span = (size_t)c->max_global_mb << 20; G.max_pages = G.span / c->page_size;
    G.memfd = memfd_create("dsm", 0);
    if (G.memfd < 0 || ftruncate(G.memfd, G.span) < 0) return DSM_E_MMAP;
    G.sys = mmap(NULL, G.span, PROT_READ | PROT_WRITE, MAP_SHARED, G.memfd, 0);
    G.app = mmap((void *)c->base_addr, G.span, PROT_NONE, MAP_SHARED | MAP_FIXED_NOREPLACE, G.memfd, 0);
    if (G.sys == MAP_FAILED || G.app == MAP_FAILED) return DSM_E_MMAP;
    if ((uint64_t)G.app != c->base_addr) { munmap(G.app, G.span); G.app = MAP_FAILED; LOG(LOG_ERROR, "GLOBAL_BASE_ADDR ocupada"); return DSM_E_MMAP; }
    G.pages = calloc(G.max_pages, sizeof(local_page_t)); G.dir = calloc(G.max_pages, sizeof(dir_entry_t));
    if (!G.pages || !G.dir) return DSM_E_NOMEM;
    for (uint32_t i = 0; i < N_LOCKS; i++) { sem_init(&G.page_locks[i], 0, 1); sem_init(&G.dir_locks[i], 0, 1); }
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = segv_handler; sa.sa_flags = SA_SIGINFO; sigemptyset(&sa.sa_mask);
    return sigaction(SIGSEGV, &sa, NULL) == 0 ? DSM_OK : DSM_E_MMAP;
}

void pager_init_slice(uint32_t first, uint32_t num) {
    pthread_mutex_lock(&G.alloc_mx);
    G.slice_first = first; G.slice_num = num; G.alloc_next = first;
    pthread_mutex_unlock(&G.alloc_mx);
    for (uint32_t p = first; p < first + num; p++) { G.pages[p].state = PG_M; G.dir[p].owner = G.me; G.dir[p].copyset = 0; }
    if (first == 0) { /* página 0 = raíz: se registra como la asignación 0 del fundador */
        G.dir[0].flags |= DF_ALLOCATED;
        pthread_mutex_lock(&G.alloc_mx); G.alloc_next = 1; pthread_mutex_unlock(&G.alloc_mx);
        alloc_entry_t root = { .alloc_id = (uint64_t)G.me << 48, .size_bytes = G.cfg.page_size,
                               .dsm_pid = make_dsm_pid(G.me, (uint32_t)getpid()), .first_page = 0, .num_pages = 1 };
        alloc_table_add(&root);
    }
    mprotect(app_addr(first), (size_t)num * G.cfg.page_size, PROT_READ | PROT_WRITE);
    pager_update_self_free();
}

void pager_shutdown(void) {
    munmap(G.app, G.span); munmap(G.sys, G.span); close(G.memfd);
    free(G.pages); free(G.dir); G.pages = NULL; G.dir = NULL;
}

/* ---------- páginas libres del slice propio ---------- */
uint32_t pager_free_pages(void) {
    pthread_mutex_lock(&G.alloc_mx);
    uint32_t f = G.slice_num - (G.alloc_next - G.slice_first);
    pthread_mutex_unlock(&G.alloc_mx);
    return f;
}

/* Mantiene al día la fila propia de la tabla de nodos */
void pager_update_self_free(void) {
    uint32_t f = pager_free_pages();          /* alloc_mx y nodes_mx nunca se toman anidados */
    pthread_mutex_lock(&G.nodes_mx);
    if (G.me < MAX_NODES_HARD) G.nodes[G.me].free_pages = f;
    pthread_mutex_unlock(&G.nodes_mx);
}

/* ---------- tabla de asignaciones replicada (orden por first_page) ---------- */
void alloc_wire(uint8_t *b, const alloc_entry_t *a) {
    put64(b, a->alloc_id); put64(b + 8, a->size_bytes); put32(b + 16, a->dsm_pid);
    put32(b + 20, a->first_page); put32(b + 24, a->num_pages); put32(b + 28, 0);
}
void alloc_unwire(const uint8_t *b, alloc_entry_t *a) {
    a->alloc_id = get64(b); a->size_bytes = get64(b + 8); a->dsm_pid = get32(b + 16);
    a->first_page = get32(b + 20); a->num_pages = get32(b + 24); a->reserved = 0;
}

/* Inserta en orden; si el alloc_id ya existe (JOIN_ACK + ALLOC_COMMIT repetido) no hace nada. */
int alloc_table_add(const alloc_entry_t *a) {
    int rc = DSM_OK;
    pthread_rwlock_wrlock(&G.allocs_rw);
    for (int i = 0; i < G.n_allocs; i++) if (G.allocs[i].alloc_id == a->alloc_id) goto out;
    if (G.n_allocs >= MAX_ALLOCS) { rc = DSM_E_NOMEM; goto out; }
    int pos = G.n_allocs;
    while (pos > 0 && G.allocs[pos - 1].first_page > a->first_page) { G.allocs[pos] = G.allocs[pos - 1]; pos--; }
    G.allocs[pos] = *a; G.n_allocs++;
out:
    pthread_rwlock_unlock(&G.allocs_rw);
    return rc;
}

/* Búsqueda binaria de la asignación que contiene la página. 1 si la encontró. */
int alloc_table_find(uint32_t page, alloc_entry_t *out) {
    int found = 0, lo = 0, hi;
    pthread_rwlock_rdlock(&G.allocs_rw);
    hi = G.n_allocs - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2; const alloc_entry_t *a = &G.allocs[mid];
        if (page < a->first_page) hi = mid - 1;
        else if (page >= a->first_page + a->num_pages) lo = mid + 1;
        else { *out = *a; found = 1; break; }
    }
    pthread_rwlock_unlock(&G.allocs_rw);
    return found;
}

int alloc_table_snapshot(alloc_entry_t *out, int cap) {
    pthread_rwlock_rdlock(&G.allocs_rw);
    int n = G.n_allocs < cap ? G.n_allocs : cap;
    memcpy(out, G.allocs, (size_t)n * sizeof *out);
    pthread_rwlock_unlock(&G.allocs_rw);
    return n;
}

/* NODE_LEAVE: dsm_finalize libera las asignaciones del proceso que sale. */
void alloc_table_remove_node(uint16_t node) {
    pthread_rwlock_wrlock(&G.allocs_rw);
    int j = 0;
    for (int i = 0; i < G.n_allocs; i++)
        if ((uint16_t)(G.allocs[i].alloc_id >> 48) != node) G.allocs[j++] = G.allocs[i];
    G.n_allocs = j;
    pthread_rwlock_unlock(&G.allocs_rw);
}

/* ALLOC_COMMIT (hoja): registra la asignación difundida por el nodo que asignó. */
void pager_on_alloc_commit(const uint8_t *pl, uint32_t len) {
    if (len < ALLOC_WIRE_LEN) return;
    alloc_entry_t a; alloc_unwire(pl, &a);
    if (alloc_table_add(&a) != DSM_OK) LOG(LOG_WARN, "tabla de asignaciones llena (MAX_ALLOCS)");
}

/* Base: asigna páginas contiguas solo dentro del propio slice (sin ALLOC_REQUEST/REPLY todavía). */
int pager_alloc(size_t size, void **out) {
    uint32_t n = (size + G.cfg.page_size - 1) / G.cfg.page_size, start;
    pthread_mutex_lock(&G.alloc_mx);
    if (G.alloc_next + n > G.slice_first + G.slice_num) { pthread_mutex_unlock(&G.alloc_mx); return DSM_E_NOMEM; }
    start = G.alloc_next; G.alloc_next += n;
    uint64_t counter = ++G.alloc_counter;
    pthread_mutex_unlock(&G.alloc_mx);
    for (uint32_t p = start; p < start + n; p++) { sem_wait(dlock(p)); G.dir[p].flags |= DF_ALLOCATED; sem_post(dlock(p)); }

    alloc_entry_t a = { .alloc_id = ((uint64_t)G.me << 48) | (counter & 0xFFFFFFFFFFFFULL), .size_bytes = size,
                        .dsm_pid = make_dsm_pid(G.me, (uint32_t)getpid()), .first_page = start, .num_pages = n };
    if (alloc_table_add(&a) != DSM_OK) LOG(LOG_WARN, "tabla de asignaciones llena (MAX_ALLOCS)");
    uint8_t wire[ALLOC_WIRE_LEN]; alloc_wire(wire, &a);
    for (int i = 0; i < G.n_nodes; i++)
        if (i != G.me) send_msg_node(i, MSG_ALLOC_COMMIT, 0, 0, start, DSM_OK, wire, sizeof wire, NULL, 0);
    pager_update_self_free();
    *out = app_addr(start);
    return DSM_OK;
}
