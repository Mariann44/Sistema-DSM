/* net.c - sockets TCP, hilo de red (listener), hilos de servicio y waiters. */
#include "internal.h"
#include <poll.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/eventfd.h>
#include <sys/socket.h>

#define MAX_CONNS 160
#define QCAP 256

/* ---------- envío ---------- */
int send_msg_fd(int fd, pthread_mutex_t *mx, int type, int flags, uint32_t rid, uint32_t page,
                int status, const void *a, uint32_t alen, const void *b, uint32_t blen) {
    size_t total = sizeof(msg_header_t) + alen + blen;
    uint8_t *buf = malloc(total);
    if (!buf) return DSM_E_NOMEM;
    msg_header_t h = { htons(DSM_MAGIC), DSM_PROTO_VERSION, (uint8_t)type, htons(G.me), htons(flags),
                       htonl(rid), htonl(page), (int16_t)htons((uint16_t)status), 0, htonl(alen + blen) };
    memcpy(buf, &h, sizeof h);
    if (alen) memcpy(buf + sizeof h, a, alen);
    if (blen) memcpy(buf + sizeof h + alen, b, blen);
    int rc = DSM_OK;
    if (mx) pthread_mutex_lock(mx);
    for (size_t off = 0; off < total && rc == DSM_OK;) {
        ssize_t n = send(fd, buf + off, total - off, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) rc = DSM_E_NET; else off += n;
    }
    if (mx) pthread_mutex_unlock(mx);
    free(buf);
    if (rc == DSM_OK) {
        atomic_fetch_add(&G.st.msgs_sent, 1); atomic_fetch_add(&G.st.bytes_sent, total);
    }
    return rc;
}

int send_msg_node(int node, int type, int flags, uint32_t rid, uint32_t page, int status,
                  const void *a, uint32_t alen, const void *b, uint32_t blen) {
    if (node < 0 || node >= MAX_NODES_HARD) return DSM_E_INVAL;
    pthread_mutex_lock(&G.nodes_mx);
    int fd = G.nodes[node].data_fd, st = G.nodes[node].state;
    pthread_mutex_unlock(&G.nodes_mx);
    if (fd < 0 || st == N_DOWN || st == N_LEFT) return DSM_E_NODE_DOWN;
    return send_msg_fd(fd, &G.nodes[node].send_lock, type, flags, rid, page, status, a, alen, b, blen);
}

static int recv_all(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) {
        ssize_t r = recv(fd, p, n, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r; n -= r;
    }
    return 0;
}

/* ---------- waiters ---------- */
waiter_t *waiter_get(int kind, int dst, uint32_t page, void *buf, uint32_t cap, uint16_t acks) {
    pthread_mutex_lock(&G.waiters_mx);
    for (int i = 0; i < MAX_WAITERS; i++) {
        waiter_t *w = &G.waiters[i];
        if (w->in_use) continue;
        while (sem_trywait(&w->sem) == 0) ;
        w->in_use = 1; w->kind = kind; w->dst_node = dst; w->page_id = page;
        w->request_id = atomic_fetch_add(&G.next_rid, 1) + 1;
        w->status = DSM_OK; w->pending_acks = acks; w->reply_buf = buf; w->reply_cap = cap; w->reply_len = 0;
        pthread_mutex_unlock(&G.waiters_mx);
        return w;
    }
    pthread_mutex_unlock(&G.waiters_mx);
    return NULL;
}
void waiter_put(waiter_t *w) {
    pthread_mutex_lock(&G.waiters_mx); w->in_use = 0; pthread_mutex_unlock(&G.waiters_mx);
}
int waiter_wait(waiter_t *w, uint32_t ms) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000; ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    int r;
    while ((r = sem_timedwait(&w->sem, &ts)) < 0 && errno == EINTR) ;
    return r == 0 ? DSM_OK : DSM_E_TIMEOUT;
}
/* Una respuesta cuyo request_id no corresponde a ningún waiter se descarta (respuesta tardía). */
void waiter_complete(const msg_header_t *h, const uint8_t *pl, uint32_t len) {
    pthread_mutex_lock(&G.waiters_mx);
    for (int i = 0; i < MAX_WAITERS; i++) {
        waiter_t *w = &G.waiters[i];
        if (!w->in_use || w->request_id != h->request_id) continue;
        if (!w->pending_acks) break;                 /* ya se completó (o falló): se ignora */
        if (h->status != DSM_OK) w->status = h->status;
        if (w->reply_buf && len) { w->reply_len = len < w->reply_cap ? len : w->reply_cap; memcpy(w->reply_buf, pl, w->reply_len); }
        if (h->type == MSG_INVALIDATE_ACK && w->pending_acks > 1) w->pending_acks--;
        else { w->pending_acks = 0; sem_post(&w->sem); }
        break;
    }
    pthread_mutex_unlock(&G.waiters_mx);
}
void waiter_fail_node(int node) {
    pthread_mutex_lock(&G.waiters_mx);
    for (int i = 0; i < MAX_WAITERS; i++) {
        waiter_t *w = &G.waiters[i];
        /* pending_acks == 0: la respuesta ya llegó; no se sobrescribe con NODE_DOWN aunque el
         * nodo salga justo después (p. ej. BARRIER_RELEASE seguido de NODE_LEAVE). */
        if (w->in_use && w->dst_node == node && w->pending_acks) { w->status = DSM_E_NODE_DOWN; w->pending_acks = 0; sem_post(&w->sem); }
    }
    pthread_mutex_unlock(&G.waiters_mx);
}

/* ---------- cola de servicio ---------- */
typedef struct { msg_header_t h; uint8_t *pl; int fd; } svc_item_t;
static svc_item_t queue[QCAP]; static int q_head, q_count;
static pthread_mutex_t q_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cv = PTHREAD_COND_INITIALIZER;

static void enqueue(const msg_header_t *h, uint8_t *pl, int fd) {
    pthread_mutex_lock(&q_mx);
    if (q_count == QCAP) { LOG(LOG_ERROR, "cola de servicio llena, se descarta mensaje 0x%02x", h->type); free(pl); }
    else { queue[(q_head + q_count++) % QCAP] = (svc_item_t){ *h, pl, fd }; pthread_cond_signal(&q_cv); }
    pthread_mutex_unlock(&q_mx);
}
static void *svc_main(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&q_mx);
        while (q_count == 0 && G.running) pthread_cond_wait(&q_cv, &q_mx);
        if (q_count == 0) { pthread_mutex_unlock(&q_mx); return NULL; }
        svc_item_t it = queue[q_head]; q_head = (q_head + 1) % QCAP; q_count--;
        pthread_mutex_unlock(&q_mx);
        if (it.h.type == MSG_PAGE_REQUEST) pager_serve_request(&it.h);
        else if (it.h.type == MSG_JOIN_REQUEST) membership_serve_join(it.fd, &it.h, it.pl);
        free(it.pl);
    }
}

/* ---------- hilo de red ---------- */
static int conns[MAX_CONNS], n_conns, wake_fd = -1;
static pthread_mutex_t conns_mx = PTHREAD_MUTEX_INITIALIZER;

void net_add_fd(int fd) {
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    pthread_mutex_lock(&conns_mx);
    if (n_conns < MAX_CONNS) conns[n_conns++] = fd; else close(fd);
    pthread_mutex_unlock(&conns_mx);
    uint64_t v = 1; if (write(wake_fd, &v, sizeof v) < 0) { }
}

static void dispatch(int fd, msg_header_t *h, uint8_t *pl) {
    int own = 1;
    uint32_t len = h->payload_len;
    atomic_fetch_add(&G.st.bytes_received, sizeof *h + len);
    switch (h->type) {
    case MSG_HELLO:
        if (h->src_node < MAX_NODES_HARD) {
            pthread_mutex_lock(&G.nodes_mx); G.nodes[h->src_node].data_fd = fd; pthread_mutex_unlock(&G.nodes_mx);
        }
        break;
    case MSG_HEARTBEAT: break; /* los latidos llegan por HB_PORT (heartbeat.c), no por esta conexión */
    case MSG_JOIN_REQUEST: case MSG_PAGE_REQUEST: enqueue(h, pl, fd); own = 0; break;
    case MSG_JOIN_ACK: membership_on_join_ack(fd, h, pl, len); waiter_complete(h, pl, len); break;
    case MSG_NODE_JOINED: membership_on_node_joined(pl, len); break;
    case MSG_NODE_LEAVE: membership_on_leave(h->src_node); break;
    case MSG_PAGE_DATA: pager_on_page_data(h, pl, len); break;
    case MSG_PAGE_FETCH: pager_on_fetch(h); break;
    case MSG_INVALIDATE: pager_on_invalidate(h); break;
    case MSG_PAGE_FETCH_REPLY: case MSG_INVALIDATE_ACK: waiter_complete(h, pl, len); break;
    case MSG_LOCK_ACQUIRE: sync_on_acquire(h, pl, len); break;   /* hoja: solo encola o concede */
    case MSG_LOCK_RELEASE: sync_on_release(h, pl, len); break;
    case MSG_LOCK_GRANT: case MSG_BARRIER_RELEASE: waiter_complete(h, pl, len); break;
    case MSG_BARRIER_ENTER: sync_on_barrier_enter(h, pl, len); break;   /* hoja: solo cuenta y libera */
    case MSG_ERROR: LOG(LOG_WARN, "ERROR de n%u: %.*s", h->src_node, (int)len, pl); break;
    default: LOG(LOG_WARN, "mensaje 0x%02x no soportado aún", h->type); break;
    }
    if (own) free(pl);
}

static int handle_readable(int fd) {
    msg_header_t h;
    if (recv_all(fd, &h, sizeof h) < 0) return -1;
    h.magic = ntohs(h.magic); h.src_node = ntohs(h.src_node); h.flags = ntohs(h.flags);
    h.request_id = ntohl(h.request_id); h.page_id = ntohl(h.page_id);
    h.status = (int16_t)ntohs((uint16_t)h.status); h.payload_len = ntohl(h.payload_len);
    if (h.magic != DSM_MAGIC || h.version != DSM_PROTO_VERSION || h.payload_len > MAX_PAYLOAD) {
        send_msg_fd(fd, NULL, MSG_ERROR, 0, 0, 0, DSM_E_BAD_MSG, "encabezado invalido", 19, NULL, 0);
        return -1;
    }
    uint8_t *pl = malloc(h.payload_len + 1);
    if (!pl) return -1;
    if (h.payload_len && recv_all(fd, pl, h.payload_len) < 0) { free(pl); return -1; }
    dispatch(fd, &h, pl);
    return 0;
}

static void *listener_main(void *arg) {
    (void)arg;
    while (G.running) {
        struct pollfd pfd[MAX_CONNS + 2]; int n = 0;
        pfd[n++] = (struct pollfd){ G.listen_fd, POLLIN, 0 };
        pfd[n++] = (struct pollfd){ wake_fd, POLLIN, 0 };
        pthread_mutex_lock(&conns_mx);
        for (int i = 0; i < n_conns; i++) pfd[n++] = (struct pollfd){ conns[i], POLLIN, 0 };
        pthread_mutex_unlock(&conns_mx);
        if (poll(pfd, n, 200) < 0) continue;
        if (pfd[1].revents) { uint64_t v; if (read(wake_fd, &v, sizeof v) < 0) { } }
        if (pfd[0].revents & POLLIN) {
            int c = accept(G.listen_fd, NULL, NULL);
            if (c >= 0) net_add_fd(c);
        }
        for (int i = 2; i < n; i++) {
            if (!(pfd[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            if (handle_readable(pfd[i].fd) < 0) {
                membership_on_conn_closed(pfd[i].fd);
                pthread_mutex_lock(&conns_mx);
                for (int j = 0; j < n_conns; j++) if (conns[j] == pfd[i].fd) { conns[j] = conns[--n_conns]; break; }
                pthread_mutex_unlock(&conns_mx);
                close(pfd[i].fd);
            }
        }
    }
    return NULL;
}

int net_connect(uint32_t ipv4, uint16_t port, int timeout_ms) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(ipv4) };
    int r = connect(fd, (struct sockaddr *)&sa, sizeof sa);
    if (r < 0 && errno == EINPROGRESS) {
        struct pollfd p = { fd, POLLOUT, 0 };
        if (poll(&p, 1, timeout_ms) == 1) {
            int err = 0; socklen_t l = sizeof err; getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l);
            r = err ? -1 : 0;
        } else r = -1;
    }
    if (r < 0) { close(fd); return -1; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    return fd;
}

int net_start(void) {
    wake_fd = eventfd(0, EFD_NONBLOCK);
    if (wake_fd < 0) return DSM_E_NET;
    G.running = 1;
    if (pthread_create(&G.listener_th, NULL, listener_main, NULL)) return DSM_E_NET;
    for (int i = 0; i < G.cfg.service_threads; i++)
        if (pthread_create(&G.svc_th[i], NULL, svc_main, NULL)) return DSM_E_NET;
    return DSM_OK;
}
void net_stop(void) {
    G.running = 0;
    pthread_mutex_lock(&q_mx); pthread_cond_broadcast(&q_cv); pthread_mutex_unlock(&q_mx);
    pthread_join(G.listener_th, NULL);
    for (int i = 0; i < G.cfg.service_threads; i++) pthread_join(G.svc_th[i], NULL);
    for (int i = 0; i < n_conns; i++) close(conns[i]);
    n_conns = 0; close(G.listen_fd);
}
