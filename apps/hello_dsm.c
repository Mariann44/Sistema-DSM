/* hello_dsm: prueba de humo de coherencia. Uso: hello_dsm dsm.conf <nodos_esperados>
 * Nodo 0 crea un arreglo y lo publica en la raíz; todos lo leen (copias S); cada nodo k
 * escribe una posición (invalida copias S / pide M); el nodo 0 verifica los valores. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "dsm.h"
#define N 4096
typedef struct { int *arr; volatile int ready, finish; volatile int done[64]; } root_t;

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "uso: %s dsm.conf nodos\n", argv[0]); return 2; }
    int expected = atoi(argv[2]), rc = dsm_init(argv[1]);
    if (rc) { fprintf(stderr, "dsm_init: %s\n", dsm_strerror(rc)); return 1; }
    root_t *root = dsm_root(); int me = dsm_node_id(), ok = 1;
    if (me == 0) {
        int *v; if (dsm_alloc(N * sizeof(int), (void **)&v)) return 1;
        for (int i = 0; i < N; i++) v[i] = i;
        root->arr = v; __sync_synchronize(); root->ready = 1;
        printf("[nodo 0] arreglo en %p publicado\n", (void *)v);
    } else {
        while (!root->ready) usleep(1000);
        long sum = 0, exp = 0; for (int i = 0; i < N; i++) if (i % 1024) { sum += root->arr[i]; exp += i; } /* excluye lo que escriben otros nodos */
        printf("[nodo %d] suma=%ld esperada=%ld %s\n", me, sum, exp, sum == exp ? "OK" : "ERROR");
        ok = sum == exp;
        root->arr[me * 1024] = 1000000 + me;   /* fallo de escritura sobre página en S */
        __sync_synchronize(); root->done[me] = 1;
    }
    if (me == 0) {
        for (int k = 1; k < expected; k++) while (!root->done[k]) usleep(1000);
        for (int k = 1; k < expected; k++) if (root->arr[k * 1024] != 1000000 + k) { printf("[nodo 0] ERROR arr[%d]\n", k * 1024); ok = 0; }
        for (int i = 0; i < N; i++) if (i % 1024 != 0 || i / 1024 >= expected) if (root->arr[i] != i) { printf("[nodo 0] ERROR arr[%d]=%d\n", i, root->arr[i]); ok = 0; break; }
        printf("[nodo 0] verificacion final: %s\n", ok ? "PASS" : "FAIL");
        root->finish = 1; sleep(1);
    } else while (!root->finish) usleep(1000);
    dsm_stats_t s; dsm_get_stats(DSM_LOCAL, &s);
    printf("[nodo %d] fallos lectura=%lu escritura=%lu inval_enviadas=%lu inval_recibidas=%lu paginas_recibidas=%lu\n", me,
           s.read_faults, s.write_faults, s.invalidations_sent, s.invalidations_received, s.pages_received);
    dsm_finalize();
    return ok ? 0 : 1;
}
