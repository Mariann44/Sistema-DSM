/* heartbeat.c - latidos por HB_PORT y detección de fallas (ALIVE -> SUSPECT -> DOWN).
 *
 * Cada nodo abre una conexión de salida hacia el HB_PORT de cada vecino y le envía un
 * HEARTBEAT cada HEARTBEAT_INTERVAL_MS. Los latidos que llegan se leen en este mismo
 * hilo, con su propio poll(), separado del hilo de red: así la transferencia de páginas
 * por DATA_PORT no retrasa los latidos ni provoca falsos positivos.
 *
 * Sin latidos durante SUSPECT_AFTER_MS el vecino pasa a SUSPECT; si vuelve a latir regresa
 * a ALIVE. Sin latidos durante HEARTBEAT_TIMEOUT_MS pasa a DOWN, de forma definitiva: sus
 * páginas quedan no disponibles y las esperas pendientes sobre él terminan con
 * DSM_E_NODE_DOWN. Un cierre de la conexión de datos también lo marca DOWN (membership.c).
 */
#include "internal.h"
#include <poll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#define HB_MAX_IN (MAX_NODES_HARD * 2)

static int in_fds[HB_MAX_IN], n_in;   /* conexiones entrantes (solo las lee este hilo) */

const char *node_state_name(int st) {
    switch (st) {
    case N_JOINING: return "JOINING"; case N_ALIVE: return "ALIVE"; case N_SUSPECT: return "SUSPECT";
    case N_DOWN: return "DOWN"; case N_LEFT: return "LEFT"; default: return "UNUSED";
    }
}

static void set_timeouts(int fd, int ms) {
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

static void drop_in(int i) { close(in_fds[i]); in_fds[i] = in_fds[--n_in]; }

/* Lee un mensaje completo de una conexión entrante. -1 = conexión cerrada o inválida. */
static int read_one(int fd) {
    msg_header_t h; uint8_t pl[HEARTBEAT_LEN];
    if (recv(fd, &h, sizeof h, MSG_WAITALL) != (ssize_t)sizeof h) return -1;
    uint16_t src = ntohs(h.src_node); uint32_t len = ntohl(h.payload_len);
    if (ntohs(h.magic) != DSM_MAGIC || h.version != DSM_PROTO_VERSION || len > sizeof pl) return -1;
    if (len && recv(fd, pl, len, MSG_WAITALL) != (ssize_t)len) return -1;
    atomic_fetch_add(&G.st.bytes_received, sizeof h + len);
    if (h.type != MSG_HEARTBEAT || src >= MAX_NODES_HARD || src == G.me) return 0; /* HELLO u otro: se ignora */

    int recovered = 0;
    pthread_mutex_lock(&G.nodes_mx);
    node_entry_t *n = &G.nodes[src];
    if (n->state == N_ALIVE || n->state == N_SUSPECT) {
        n->last_hb_ms = now_ms();
        if (len >= HEARTBEAT_LEN) n->free_pages = get32(pl + 4);
        if (n->state == N_SUSPECT) { n->state = N_ALIVE; recovered = 1; }
    }
    pthread_mutex_unlock(&G.nodes_mx);
    if (recovered) LOG(LOG_INFO, "nodo %u volvió a latir: SUSPECT -> ALIVE", src);
    return 0;
}

/* Atiende lo que haya en el socket de escucha y en las conexiones entrantes. */
static void poll_incoming(int timeout_ms) {
    struct pollfd pfd[HB_MAX_IN + 1]; int n = 0;
    pfd[n++] = (struct pollfd){ G.hb_listen_fd, POLLIN, 0 };
    for (int i = 0; i < n_in; i++) pfd[n++] = (struct pollfd){ in_fds[i], POLLIN, 0 };
    if (poll(pfd, n, timeout_ms) <= 0) return;
    for (int i = n - 1; i >= 1; i--) {   /* de atrás hacia adelante: drop_in mueve el último */
        if (!(pfd[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        if (read_one(pfd[i].fd) < 0) drop_in(i - 1);
    }
    if (pfd[0].revents & POLLIN) {
        int c = accept(G.hb_listen_fd, NULL, NULL);
        if (c >= 0) {
            if (n_in < HB_MAX_IN) { set_timeouts(c, 500); in_fds[n_in++] = c; } else close(c);
        }
    }
}

/* Envía un latido a cada vecino vivo; abre la conexión de salida si aún no existe. */
static void send_beats(void) {
    uint8_t pl[HEARTBEAT_LEN] = {0};
    pl[0] = N_ALIVE;
    pthread_mutex_lock(&G.alloc_mx);
    put32(pl + 4, G.slice_num - (G.alloc_next - G.slice_first));
    pthread_mutex_unlock(&G.alloc_mx);
    put64(pl + 8, now_ms());

    for (int i = 0; i < MAX_NODES_HARD; i++) {
        pthread_mutex_lock(&G.nodes_mx);
        node_entry_t n = G.nodes[i];
        pthread_mutex_unlock(&G.nodes_mx);
        if (i == G.me || i >= G.n_nodes || (n.state != N_ALIVE && n.state != N_SUSPECT)) continue;
        int fd = n.hb_fd;
        if (fd < 0) {
            fd = net_connect(n.ipv4, n.hb_port, 100);
            if (fd < 0) continue;              /* se reintenta en el siguiente intervalo */
            set_timeouts(fd, 200);
            send_msg_fd(fd, NULL, MSG_HELLO, 0, 0, 0, 0, NULL, 0, NULL, 0);
            pthread_mutex_lock(&G.nodes_mx); G.nodes[i].hb_fd = fd; pthread_mutex_unlock(&G.nodes_mx);
        }
        if (send_msg_fd(fd, NULL, MSG_HEARTBEAT, 0, 0, 0, DSM_OK, pl, sizeof pl, NULL, 0) != DSM_OK) {
            close(fd);                         /* se vuelve a conectar en el siguiente intervalo */
            pthread_mutex_lock(&G.nodes_mx); G.nodes[i].hb_fd = -1; pthread_mutex_unlock(&G.nodes_mx);
        }
    }
}

/* Revisa cuánto tiempo lleva cada vecino sin latir y aplica las transiciones. */
static void check_timeouts(void) {
    uint64_t now = now_ms();
    for (int i = 0; i < MAX_NODES_HARD; i++) {
        int down = 0, suspect = 0; uint64_t since = 0;
        pthread_mutex_lock(&G.nodes_mx);
        node_entry_t *n = &G.nodes[i];
        if (i != G.me && i < G.n_nodes && (n->state == N_ALIVE || n->state == N_SUSPECT)) {
            since = now - n->last_hb_ms;
            if (since >= G.cfg.hb_timeout_ms) {
                n->state = N_DOWN; down = 1;
                if (n->hb_fd >= 0) { close(n->hb_fd); n->hb_fd = -1; }
            } else if (since >= G.cfg.suspect_after_ms && n->state == N_ALIVE) {
                n->state = N_SUSPECT; suspect = 1;
            }
        }
        pthread_mutex_unlock(&G.nodes_mx);
        if (suspect) LOG(LOG_WARN, "nodo %d sin latidos hace %llu ms: ALIVE -> SUSPECT", i, (unsigned long long)since);
        if (down) {
            LOG(LOG_WARN, "nodo %d sin latidos hace %llu ms: -> DOWN (sus páginas quedan no disponibles)", i, (unsigned long long)since);
            waiter_fail_node(i); sync_node_down(i);
        }
    }
}

static void *hb_main(void *arg) {
    (void)arg;
    uint64_t next = now_ms();
    while (G.hb_running) {
        uint64_t now = now_ms();
        if (now < next) { poll_incoming((int)(next - now)); continue; }
        poll_incoming(0);   /* primero los latidos ya recibidos: evita falsos DOWN tras una pausa larga */
        send_beats();
        check_timeouts();
        next += G.cfg.hb_interval_ms;
        if (next < now) next = now + G.cfg.hb_interval_ms;   /* el hilo estuvo detenido: no recuperar ticks */
    }
    return NULL;
}

int heartbeat_start(void) {
    uint64_t now = now_ms();
    pthread_mutex_lock(&G.nodes_mx);
    for (int i = 0; i < MAX_NODES_HARD; i++) G.nodes[i].last_hb_ms = now; /* plazo inicial para todos */
    pthread_mutex_unlock(&G.nodes_mx);
    G.hb_running = 1;
    return pthread_create(&G.hb_th, NULL, hb_main, NULL) == 0 ? DSM_OK : DSM_E_NET;
}

void heartbeat_stop(void) {
    if (!G.hb_running) return;
    G.hb_running = 0;
    pthread_join(G.hb_th, NULL);
    for (int i = 0; i < n_in; i++) close(in_fds[i]);
    n_in = 0;
    for (int i = 0; i < MAX_NODES_HARD; i++) if (G.nodes[i].hb_fd >= 0) { close(G.nodes[i].hb_fd); G.nodes[i].hb_fd = -1; }
    if (G.hb_listen_fd >= 0) { close(G.hb_listen_fd); G.hb_listen_fd = -1; }
}
