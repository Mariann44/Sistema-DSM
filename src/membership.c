/* membership.c - slot/puertos, unión de nodos, tabla de nodos y home_of(). */
#include "internal.h"
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

static sem_t join_sem;
static int my_host_idx = -1;

uint64_t now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int resolve(const char *host, uint32_t *ip) {
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res;
    if (getaddrinfo(host, NULL, &hints, &res)) return -1;
    *ip = ntohl(((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr);
    freeaddrinfo(res);
    return 0;
}

static int is_local_ip(uint32_t ip) {
    if ((ip >> 24) == 127) return 1;
    struct ifaddrs *ifa, *p; int found = 0;
    if (getifaddrs(&ifa)) return 0;
    for (p = ifa; p; p = p->ifa_next)
        if (p->ifa_addr && p->ifa_addr->sa_family == AF_INET &&
            ntohl(((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr) == ip) found = 1;
    freeifaddrs(ifa);
    return found;
}

int home_of(uint32_t page) {
    int r = -1;
    pthread_mutex_lock(&G.nodes_mx);
    for (int i = 0; i < G.n_nodes; i++) {
        node_entry_t *n = &G.nodes[i];
        if (n->state != N_UNUSED && page >= n->first_page && page < n->first_page + n->num_pages) { r = i; break; }
    }
    pthread_mutex_unlock(&G.nodes_mx);
    return r;
}

static int listen_on(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) == 0 && listen(fd, 64) == 0) return fd;
    close(fd);
    return -1;
}

/* Reconoce la máquina propia, escoge el slot (primer DATA_PORT libre) y abre el puerto de escucha. */
int membership_start(void) {
    sem_init(&join_sem, 0, 1);
    for (int i = 0; i < G.cfg.n_hosts && my_host_idx < 0; i++) {
        uint32_t ip;
        if (resolve(G.cfg.hosts[i], &ip) == 0 && is_local_ip(ip)) { my_host_idx = i; G.my_ip = ip; }
    }
    if (my_host_idx < 0) { LOG(LOG_ERROR, "esta máquina no aparece en ninguna entrada NODE"); return DSM_E_CONFIG; }
    for (int s = 0; s < G.cfg.slots_per_host; s++) {
        uint16_t port = G.cfg.port_base + 4 * s;
        int fd = listen_on(port), hfd = fd >= 0 ? listen_on(port + 1) : -1;  /* DATA_PORT y HB_PORT del slot */
        if (fd >= 0 && hfd >= 0) {
            G.listen_fd = fd; G.hb_listen_fd = hfd; G.slot = s; G.is_founder = (my_host_idx == 0 && s == 0);
            return DSM_OK;
        }
        if (fd >= 0) close(fd);
    }
    LOG(LOG_ERROR, "no hay slot libre (todos los DATA_PORT/HB_PORT ocupados)");
    return DSM_E_NET;
}

static void fill_self(uint16_t id, uint32_t first, uint32_t num) {
    node_entry_t *n = &G.nodes[id];
    n->node_id = id; n->state = N_ALIVE; n->slot = G.slot; n->ipv4 = G.my_ip;
    n->data_port = G.cfg.port_base + 4 * G.slot; n->hb_port = n->data_port + 1;
    n->ctl_port = n->data_port + 2; n->http_port = n->data_port + 3;
    n->first_page = first; n->num_pages = num; n->free_pages = num;
}
static void wire_node(uint8_t *b, const node_entry_t *n) {
    put16(b, n->node_id); b[2] = n->state; b[3] = 0; put32(b + 4, n->ipv4);
    put16(b + 8, n->data_port); put16(b + 10, n->hb_port); put16(b + 12, n->ctl_port); put16(b + 14, n->http_port);
    put32(b + 16, n->first_page); put32(b + 20, n->num_pages); put32(b + 24, n->free_pages);
}
static void unwire_node(const uint8_t *b) { /* conserva data_fd, hb_fd y send_lock */
    uint16_t id = get16(b);
    if (id >= MAX_NODES_HARD) return;
    node_entry_t *n = &G.nodes[id];
    n->node_id = id; n->state = b[2]; n->ipv4 = get32(b + 4);
    n->data_port = get16(b + 8); n->hb_port = get16(b + 10); n->ctl_port = get16(b + 12); n->http_port = get16(b + 14);
    n->first_page = get32(b + 16); n->num_pages = get32(b + 20); n->free_pages = get32(b + 24);
    n->last_hb_ms = now_ms();                      /* plazo de gracia antes de exigir latidos */
    if (id >= G.n_nodes) G.n_nodes = id + 1;
}

/* ---- lado contacto ---- */
void membership_serve_join(int fd, const msg_header_t *h, const uint8_t *pl) {
    (void)h;
    sem_wait(&join_sem); /* serializa las uniones en este contacto */
    uint32_t mem_pages = 0; int st = DSM_OK; uint16_t id = 0;
    if (h->payload_len < JOIN_REQ_LEN) st = DSM_E_BAD_MSG;
    else if (get64(pl) != G.cfg.config_hash) st = DSM_E_CONFIG;
    if (st == DSM_OK) {
        mem_pages = get32(pl + 20);
        pthread_mutex_lock(&G.nodes_mx);
        uint32_t next = 0;
        for (int i = 0; i < G.n_nodes; i++) if (G.nodes[i].state != N_UNUSED && G.nodes[i].first_page + G.nodes[i].num_pages > next)
            next = G.nodes[i].first_page + G.nodes[i].num_pages;
        if (G.n_nodes >= G.cfg.max_nodes || (uint64_t)next + mem_pages > G.max_pages) st = DSM_E_CLUSTER_FULL;
        else {
            id = G.n_nodes;
            node_entry_t *n = &G.nodes[id];
            n->node_id = id; n->state = N_ALIVE; n->ipv4 = get32(pl + 8);
            n->data_port = get16(pl + 12); n->hb_port = get16(pl + 14); n->ctl_port = get16(pl + 16); n->http_port = get16(pl + 18);
            n->first_page = next; n->num_pages = mem_pages; n->free_pages = mem_pages; n->data_fd = fd;
            n->last_hb_ms = now_ms();
            G.n_nodes++;
        }
        pthread_mutex_unlock(&G.nodes_mx);
    }
    if (st != DSM_OK) {
        send_msg_fd(fd, NULL, MSG_JOIN_ACK, 0, h->request_id, 0, st, NULL, 0, NULL, 0);
        sem_post(&join_sem); return;
    }
    uint8_t *ack = malloc(12 + NODE_WIRE_LEN * MAX_NODES_HARD);
    pthread_mutex_lock(&G.nodes_mx);
    int n = G.n_nodes;
    put16(ack, id); put16(ack + 2, 0); put32(ack + 4, n); put32(ack + 8, 0 /* n_allocs: TODO */);
    for (int i = 0; i < n; i++) wire_node(ack + 12 + NODE_WIRE_LEN * i, &G.nodes[i]);
    uint8_t joined[NODE_WIRE_LEN]; wire_node(joined, &G.nodes[id]);
    pthread_mutex_unlock(&G.nodes_mx);
    send_msg_node(id, MSG_JOIN_ACK, 0, h->request_id, 0, DSM_OK, ack, 12 + NODE_WIRE_LEN * n, NULL, 0);
    free(ack);
    for (int i = 0; i < n; i++)               /* avisa a los demás (no al contacto ni al nuevo) */
        if (i != id && i != G.me) send_msg_node(i, MSG_NODE_JOINED, 0, 0, 0, DSM_OK, joined, NODE_WIRE_LEN, NULL, 0);
    LOG(LOG_INFO, "nodo %u se unió (páginas %u..%u)", id, G.nodes[id].first_page, G.nodes[id].first_page + mem_pages - 1);
    sem_post(&join_sem);
}

void membership_on_node_joined(const uint8_t *pl, uint32_t len) {
    if (len < NODE_WIRE_LEN) return;
    pthread_mutex_lock(&G.nodes_mx); unwire_node(pl); pthread_mutex_unlock(&G.nodes_mx);
}

/* ---- lado nuevo nodo (corre en el hilo de red) ---- */
void membership_on_join_ack(int fd, const msg_header_t *h, const uint8_t *pl, uint32_t len) {
    if (h->status != DSM_OK || len < 12) return;
    uint16_t id = get16(pl); uint32_t n = get32(pl + 4);
    if (len < 12 + n * NODE_WIRE_LEN) return;
    pthread_mutex_lock(&G.nodes_mx);
    for (uint32_t i = 0; i < n; i++) unwire_node(pl + 12 + NODE_WIRE_LEN * i);
    G.nodes[h->src_node].data_fd = fd;           /* el contacto */
    G.nodes[id].data_fd = -1;
    G.me = id;
    uint32_t first = G.nodes[id].first_page, num = G.nodes[id].num_pages;
    pthread_mutex_unlock(&G.nodes_mx);
    pager_init_slice(first, num);                /* antes de que nadie pueda pedirnos páginas */
}

int membership_join(void) {
    uint32_t mem_pages = (uint32_t)(((uint64_t)G.cfg.node_mem_mb << 20) / G.cfg.page_size);
    if (G.is_founder) {
        pthread_mutex_lock(&G.nodes_mx);
        G.me = 0; G.n_nodes = 1; fill_self(0, 0, mem_pages);
        pthread_mutex_unlock(&G.nodes_mx);
        pager_init_slice(0, mem_pages);
        LOG(LOG_INFO, "fundador del clúster (slot 0), %u páginas", mem_pages);
        return DSM_OK;
    }
    uint8_t req[JOIN_REQ_LEN];
    put64(req, G.cfg.config_hash); put32(req + 8, G.my_ip);
    put16(req + 12, G.cfg.port_base + 4 * G.slot); put16(req + 14, G.cfg.port_base + 4 * G.slot + 1);
    put16(req + 16, G.cfg.port_base + 4 * G.slot + 2); put16(req + 18, G.cfg.port_base + 4 * G.slot + 3);
    put32(req + 20, mem_pages); put32(req + 24, getpid());

    static uint8_t ackbuf[12 + NODE_WIRE_LEN * MAX_NODES_HARD];
    uint64_t t0 = now_ms(); int contact = -1, cfd = -1, rc = DSM_E_JOIN_TIMEOUT;
    G.me = NODE_UNKNOWN;
    while (now_ms() - t0 < G.cfg.join_timeout_ms && contact < 0) {
        for (int hI = 0; hI < G.cfg.n_hosts && contact < 0; hI++) {
            uint32_t ip; if (resolve(G.cfg.hosts[hI], &ip)) continue;
            for (int s = 0; s < G.cfg.slots_per_host && contact < 0; s++) {
                if (hI == my_host_idx && s == G.slot) continue;
                int fd = net_connect(ip, G.cfg.port_base + 4 * s, 200);
                if (fd < 0) continue;
                waiter_t *w = waiter_get(W_JOIN, NODE_UNKNOWN, 0, ackbuf, sizeof ackbuf, 1);
                net_add_fd(fd);
                send_msg_fd(fd, NULL, MSG_HELLO, 0, 0, 0, 0, NULL, 0, NULL, 0);
                send_msg_fd(fd, NULL, MSG_JOIN_REQUEST, 0, w->request_id, 0, 0, req, sizeof req, NULL, 0);
                if (waiter_wait(w, 5000) == DSM_OK) {
                    rc = w->status; waiter_put(w);
                    if (rc == DSM_OK) { contact = 1; cfd = fd; } else return rc;
                } else waiter_put(w);
            }
        }
        if (contact < 0) usleep(300000);
    }
    if (contact < 0) return rc;
    (void)cfd;
    /* Conexión directa con los demás nodos (el que se une después abre la conexión). */
    for (int i = 0; i < G.n_nodes; i++) {
        pthread_mutex_lock(&G.nodes_mx);
        node_entry_t n = G.nodes[i]; int has_fd = G.nodes[i].data_fd >= 0;
        pthread_mutex_unlock(&G.nodes_mx);
        if (i == G.me || n.state != N_ALIVE || has_fd) continue;
        int fd = net_connect(n.ipv4, n.data_port, 1000);
        if (fd < 0) { LOG(LOG_WARN, "no se pudo conectar con el nodo %d", i); continue; }
        pthread_mutex_lock(&G.nodes_mx); G.nodes[i].data_fd = fd; pthread_mutex_unlock(&G.nodes_mx);
        net_add_fd(fd);
        send_msg_fd(fd, &G.nodes[i].send_lock, MSG_HELLO, 0, 0, 0, 0, NULL, 0, NULL, 0);
    }
    LOG(LOG_INFO, "unido como nodo %u (slot %d), %d nodos en el clúster", G.me, G.slot, G.n_nodes);
    return DSM_OK;
}

void membership_on_leave(int src) {
    if (src >= MAX_NODES_HARD) return;
    pthread_mutex_lock(&G.nodes_mx); G.nodes[src].state = N_LEFT; pthread_mutex_unlock(&G.nodes_mx);
    waiter_fail_node(src); sync_node_down(src);
    LOG(LOG_INFO, "nodo %d salió del clúster", src);
}
void membership_on_conn_closed(int fd) {
    int dead = -1;
    pthread_mutex_lock(&G.nodes_mx);
    for (int i = 0; i < G.n_nodes; i++)
        if (G.nodes[i].data_fd == fd && i != G.me) {
            G.nodes[i].data_fd = -1;
            if (G.nodes[i].state == N_ALIVE) G.nodes[i].state = N_DOWN;
            dead = i; break;
        }
    pthread_mutex_unlock(&G.nodes_mx);
    if (dead >= 0) { LOG(LOG_WARN, "conexión con el nodo %d cerrada", dead); waiter_fail_node(dead); sync_node_down(dead); }
}
void membership_broadcast(int type) {
    for (int i = 0; i < G.n_nodes; i++) if (i != G.me) send_msg_node(i, type, 0, 0, 0, DSM_OK, NULL, 0, NULL, 0);
}
