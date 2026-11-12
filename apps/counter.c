/* counter: prueba de dsm_lock / dsm_unlock. Uso: counter dsm.conf <nodos> <iteraciones> [sinlock]
 * Cada nodo incrementa un contador compartido <iteraciones> veces dentro de un lock.
 * El nodo 0 verifica que el total sea nodos x iteraciones. Con "sinlock" se hace sin el
 * lock para mostrar que se pierden incrementos (lectura y escritura no son atómicas).
 * Las fases se separan con la misma dsm_barrier, reutilizada tres veces. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsm.h"

typedef struct {
    dsm_lock_t lock;          /* su administrador es el home de la página raíz (nodo 0) */
    dsm_barrier_t bar;
    volatile long counter;
} root_t;

static void barrier(root_t *root, int expected, int me) {
    int rc = dsm_barrier(&root->bar, (uint32_t)expected);
    if (rc) { fprintf(stderr, "[nodo %d] dsm_barrier: %s\n", me, dsm_strerror(rc)); exit(1); }
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "uso: %s dsm.conf nodos iteraciones [sinlock]\n", argv[0]); return 2; }
    int expected = atoi(argv[2]), iters = atoi(argv[3]), use_lock = !(argc > 4 && !strcmp(argv[4], "sinlock"));
    int rc = dsm_init(argv[1]);
    if (rc) { fprintf(stderr, "dsm_init: %s\n", dsm_strerror(rc)); return 1; }
    root_t *root = dsm_root(); int me = dsm_node_id();

    barrier(root, expected, me);         /* 1: salida al mismo tiempo, cuando estén todos */

    for (int i = 0; i < iters; i++) {
        if (use_lock && (rc = dsm_lock(&root->lock))) { fprintf(stderr, "[nodo %d] dsm_lock: %s\n", me, dsm_strerror(rc)); return 1; }
        long v = root->counter;          /* lectura y escritura separadas a propósito */
        root->counter = v + 1;
        if (use_lock && (rc = dsm_unlock(&root->lock))) { fprintf(stderr, "[nodo %d] dsm_unlock: %s\n", me, dsm_strerror(rc)); return 1; }
    }
    printf("[nodo %d] %d incrementos %s lock\n", me, iters, use_lock ? "con" : "sin");
    barrier(root, expected, me);         /* 2: todos terminaron */

    int ok = 1;
    if (me == 0) {
        long total = root->counter, want = (long)expected * iters;
        ok = (total == want);
        printf("[nodo 0] contador=%ld esperado=%ld %s\n", total, want, ok ? "PASS" : "FAIL");
    }
    barrier(root, expected, me);         /* 3: nadie sale antes de que el nodo 0 lea el total */
    dsm_finalize();
    return use_lock ? !ok : 0;
}
