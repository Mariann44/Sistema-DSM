/* internal.h - tipos y estado global compartidos entre módulos de libdsm. */
#ifndef INTERNAL_H
#define INTERNAL_H
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdatomic.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include "dsm.h"
#include "protocol.h"

#define MAX_NODES_HARD 64
#define MAX_WAITERS    256
#define N_LOCKS        1024
#define MAX_HOSTS      32

enum { LOG_ERROR = 0, LOG_WARN, LOG_INFO, LOG_DEBUG };
enum { N_UNUSED = 0, N_JOINING, N_ALIVE, N_SUSPECT, N_DOWN, N_LEFT };
enum { PG_I = 0, PG_S, PG_M, PG_PR, PG_PW };
enum { W_PAGE = 1, W_INVAL, W_FETCH, W_JOIN };
#define DF_ALLOCATED 0x02

typedef struct {
    uint32_t page_size, node_mem_mb, max_global_mb;
    uint16_t max_nodes;
    uint64_t base_addr;
    char hosts[MAX_HOSTS][64]; int n_hosts;
    uint16_t port_base, port_range, slots_per_host;
    uint32_t cache_max_pages;
    uint16_t service_threads;
    uint32_t hb_interval_ms, suspect_after_ms, hb_timeout_ms;
    uint32_t join_timeout_ms, request_timeout_ms, max_retries;
    uint8_t http_enabled;
    char log_level[8]; int log_lvl;
    uint64_t config_hash;
} dsm_config_t;

typedef struct { /* 20 bytes */
    uint8_t state, flags; uint16_t reserved;
    uint32_t version, waiter_idx, lru_prev, lru_next;
} local_page_t;

typedef struct { /* 16 bytes, solo en el home */
    uint64_t copyset; uint32_t version; uint16_t owner; uint8_t flags, reserved;
} dir_entry_t;

typedef struct {
    uint16_t node_id; uint8_t state, slot;
    uint32_t ipv4; /* orden de host */
    uint16_t data_port, hb_port, ctl_port, http_port;
    uint32_t first_page, num_pages, free_pages;
    int data_fd;
    uint64_t last_hb_ms;
    pthread_mutex_t send_lock;
} node_entry_t;

typedef struct {
    sem_t sem; uint32_t request_id, page_id;
    uint16_t dst_node; uint8_t kind, in_use;
    int16_t status; uint16_t pending_acks;
    void *reply_buf; uint32_t reply_cap, reply_len;
} waiter_t;

typedef struct {
    _Atomic uint64_t read_faults, write_faults, pages_sent, pages_received,
        inv_sent, inv_received, bytes_sent, bytes_received, fault_us, msgs_sent;
} stats_t;

typedef struct {
    dsm_config_t cfg;
    int inited; volatile int running;
    uint16_t me; int slot, is_founder, listen_fd;
    uint32_t my_ip;
    int memfd; uint8_t *app, *sys; size_t span; uint32_t max_pages;
    local_page_t *pages; dir_entry_t *dir;
    sem_t page_locks[N_LOCKS], dir_locks[N_LOCKS];
    node_entry_t nodes[MAX_NODES_HARD]; int n_nodes; pthread_mutex_t nodes_mx;
    waiter_t waiters[MAX_WAITERS]; pthread_mutex_t waiters_mx; _Atomic uint32_t next_rid;
    pthread_t listener_th, svc_th[32];
    uint32_t slice_first, slice_num, alloc_next;
    pthread_mutex_t alloc_mx;
    stats_t st;
} dsm_ctx_t;
extern dsm_ctx_t G;

#define LOG(lvl, ...) do { if ((lvl) <= G.cfg.log_lvl) { \
    fprintf(stderr, "[dsm n%u] ", (unsigned)G.me); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr);} } while (0)

static inline uint8_t *app_addr(uint32_t p) { return G.app + (size_t)p * G.cfg.page_size; }
static inline uint8_t *sys_addr(uint32_t p) { return G.sys + (size_t)p * G.cfg.page_size; }
uint64_t now_ms(void);

/* config.c */
int config_load(const char *path, dsm_config_t *c);
/* net.c */
int net_start(void); void net_stop(void);
int net_connect(uint32_t ipv4, uint16_t port, int timeout_ms);
void net_add_fd(int fd);
int send_msg_fd(int fd, pthread_mutex_t *mx, int type, int flags, uint32_t rid, uint32_t page,
                int status, const void *a, uint32_t alen, const void *b, uint32_t blen);
int send_msg_node(int node, int type, int flags, uint32_t rid, uint32_t page, int status,
                  const void *a, uint32_t alen, const void *b, uint32_t blen);
waiter_t *waiter_get(int kind, int dst, uint32_t page, void *buf, uint32_t cap, uint16_t acks);
void waiter_put(waiter_t *w);
void waiter_complete(const msg_header_t *h, const uint8_t *pl, uint32_t len);
void waiter_fail_node(int node);
int waiter_wait(waiter_t *w, uint32_t ms);
/* membership.c */
int membership_start(void); int membership_join(void);
int home_of(uint32_t page);
void membership_on_join_ack(int fd, const msg_header_t *h, const uint8_t *pl, uint32_t len);
void membership_on_node_joined(const uint8_t *pl, uint32_t len);
void membership_on_leave(int src); void membership_on_conn_closed(int fd);
void membership_serve_join(int fd, const msg_header_t *h, const uint8_t *pl);
void membership_broadcast(int type);
/* pager.c */
int pager_init(void); void pager_shutdown(void);
void pager_init_slice(uint32_t first, uint32_t num);
void pager_on_page_data(const msg_header_t *h, const uint8_t *pl, uint32_t len);
void pager_on_fetch(const msg_header_t *h);
void pager_on_invalidate(const msg_header_t *h);
void pager_serve_request(const msg_header_t *h);
int pager_alloc(size_t size, void **out);
#endif
