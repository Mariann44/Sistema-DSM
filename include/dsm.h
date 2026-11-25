/* dsm.h - API pública del DSM. */
#ifndef DSM_H
#define DSM_H
#include <stddef.h>
#include <stdint.h>

typedef uint16_t dsm_node_t;
typedef uint32_t dsm_pid_t;
typedef uint32_t dsm_page_t;
typedef uint64_t dsm_alloc_id_t;
typedef struct { uint64_t opaque; } dsm_lock_t;
typedef struct { uint64_t opaque; } dsm_barrier_t;
typedef enum { DSM_LOCAL = 0, DSM_GLOBAL = 1 } dsm_scope_t;

typedef struct {
    uint64_t read_faults, write_faults, pages_sent, pages_received;
    uint64_t invalidations_sent, invalidations_received;
    uint64_t bytes_sent, bytes_received, fault_time_total_us, messages_sent;
    uint32_t pages_owned, pages_allocated, pages_cached, reserved;
} dsm_stats_t; /* 96 bytes */

#define DSM_OK                0
#define DSM_E_INVAL          -1
#define DSM_E_NOT_INIT       -2
#define DSM_E_ALREADY_INIT   -3
#define DSM_E_CONFIG         -4
#define DSM_E_MMAP           -5
#define DSM_E_NET            -6
#define DSM_E_JOIN_TIMEOUT   -7
#define DSM_E_CLUSTER_FULL   -8
#define DSM_E_NOMEM          -9
#define DSM_E_TIMEOUT       -10
#define DSM_E_NODE_DOWN     -11
#define DSM_E_PERM          -12
#define DSM_E_MISMATCH      -13
#define DSM_E_BAD_MSG       -14
#define DSM_E_NOT_IMPL      -15  /* extensión de la base: función aún sin implementar */

int dsm_init(const char *config_path);
int dsm_finalize(void);
int dsm_flush(void);   /* devuelve a su home las páginas remotas en M; llamar antes de la barrera final */
int dsm_alloc(size_t size, void **out_ptr);
int dsm_free(void *ptr);
void *dsm_root(void);
int dsm_prefetch(void *ptr, size_t len, int for_write);
dsm_node_t dsm_node_id(void);
int dsm_num_nodes(void);
size_t dsm_page_size(void);
int dsm_lock(dsm_lock_t *lock);
int dsm_unlock(dsm_lock_t *lock);
int dsm_barrier(dsm_barrier_t *barrier, uint32_t count);
int dsm_get_stats(dsm_scope_t scope, dsm_stats_t *out);
const char *dsm_strerror(int err);
#endif
