/* hello_dsm: prueba de humo de coherencia. Uso: hello_dsm dsm.conf <nodos_esperados>
 * Nodo 0 crea un arreglo y lo publica en la raíz; todos lo leen (copias S); cada nodo k
 * escribe en su propia página del arreglo (invalida copias S / pide M); el nodo 0 verifica.
 * Las fases se separan con dsm_barrier, que también espera a que se unan todos los nodos. */
#include <stdio.h>
#include <stdlib.h>
#include "dsm.h"
#define STRIDE 1024                 /* ints por página de 4 KB: cada nodo escribe en su página */
typedef struct { int *arr; int n; dsm_barrier_t bar; } root_t;

static void barrier(root_t *root, int expected, int me) {
    int rc = dsm_barrier(&root->bar, (uint32_t)expected);
    if (rc) { fprintf(stderr, "[nodo %d] dsm_barrier: %s\n", me, dsm_strerror(rc)); exit(1); }
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "uso: %s dsm.conf nodos\n", argv[0]); return 2; }
    int expected = atoi(argv[2]), rc = dsm_init(argv[1]);
    if (rc) { fprintf(stderr, "dsm_init: %s\n", dsm_strerror(rc)); return 1; }
    root_t *root = dsm_root(); int me = dsm_node_id(), ok = 1;

    if (me == 0) {                                   /* una página del arreglo por nodo (mínimo 4) */
        int n = STRIDE * (expected > 4 ? expected : 4), *v;
        if ((rc = dsm_alloc(n * sizeof(int), (void **)&v))) { fprintf(stderr, "dsm_alloc: %s\n", dsm_strerror(rc)); return 1; }
        for (int i = 0; i < n; i++) v[i] = i;
        root->arr = v; root->n = n;
        printf("[nodo 0] arreglo de %d ints en %p publicado\n", n, (void *)v);
    }
    barrier(root, expected, me);                     /* 1: el arreglo existe y están todos */

    int n = root->n; int *arr = root->arr;
    if (me != 0) {
        long sum = 0, exp = 0;
        for (int i = 0; i < n; i++) if (i % STRIDE) { sum += arr[i]; exp += i; }   /* excluye lo que escriben otros nodos */
        printf("[nodo %d] suma=%ld esperada=%ld %s\n", me, sum, exp, sum == exp ? "OK" : "ERROR");
        ok = sum == exp;
        arr[me * STRIDE] = 1000000 + me;             /* fallo de escritura sobre página en S */
    }
    barrier(root, expected, me);                     /* 2: todos escribieron */

    if (me == 0) {
        for (int k = 1; k < expected; k++) if (arr[k * STRIDE] != 1000000 + k) { printf("[nodo 0] ERROR arr[%d]\n", k * STRIDE); ok = 0; }
        for (int i = 0; i < n; i++) if (i % STRIDE != 0 || i / STRIDE >= expected) if (arr[i] != i) { printf("[nodo 0] ERROR arr[%d]=%d\n", i, arr[i]); ok = 0; break; }
        printf("[nodo 0] verificacion final: %s\n", ok ? "PASS" : "FAIL");
    }
    barrier(root, expected, me);                     /* 3: nadie sale antes de que el nodo 0 verifique */

    dsm_stats_t s; dsm_get_stats(DSM_LOCAL, &s);
    printf("[nodo %d] fallos lectura=%lu escritura=%lu inval_enviadas=%lu inval_recibidas=%lu paginas_recibidas=%lu\n", me,
           s.read_faults, s.write_faults, s.invalidations_sent, s.invalidations_received, s.pages_received);
    dsm_finalize();
    return ok ? 0 : 1;
}
