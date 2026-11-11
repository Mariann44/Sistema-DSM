/* counter: prueba de dsm_lock / dsm_unlock. Uso: counter dsm.conf <nodos> <iteraciones> [sinlock]
 * Cada nodo incrementa un contador compartido <iteraciones> veces dentro de un lock.
 * El nodo 0 verifica que el total sea nodos x iteraciones. Con "sinlock" se hace sin el
 * lock para mostrar que se pierden incrementos (lectura y escritura no son atómicas). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "dsm.h"

typedef struct {
    dsm_lock_t lock;          /* su administrador es el home de la página raíz (nodo 0) */
    volatile long counter;
    volatile int ready, finish;
    volatile int done[64];
} root_t;

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "uso: %s dsm.conf nodos iteraciones [sinlock]\n", argv[0]); return 2; }
    int expected = atoi(argv[2]), iters = atoi(argv[3]), use_lock = !(argc > 4 && !strcmp(argv[4], "sinlock"));
    int rc = dsm_init(argv[1]);
    if (rc) { fprintf(stderr, "dsm_init: %s\n", dsm_strerror(rc)); return 1; }
    root_t *root = dsm_root(); int me = dsm_node_id();

    if (me == 0) {                       /* espera a que estén todos y da la salida */
        while (dsm_num_nodes() < expected) usleep(10000);
        root->ready = 1;
    } else while (!root->ready) usleep(1000);

    for (int i = 0; i < iters; i++) {
        if (use_lock && (rc = dsm_lock(&root->lock))) { fprintf(stderr, "[nodo %d] dsm_lock: %s\n", me, dsm_strerror(rc)); return 1; }
        long v = root->counter;          /* lectura y escritura separadas a propósito */
        root->counter = v + 1;
        if (use_lock && (rc = dsm_unlock(&root->lock))) { fprintf(stderr, "[nodo %d] dsm_unlock: %s\n", me, dsm_strerror(rc)); return 1; }
    }
    root->done[me] = 1;
    printf("[nodo %d] %d incrementos %s lock\n", me, iters, use_lock ? "con" : "sin");

    int ok = 1;
    if (me == 0) {
        for (int k = 0; k < expected; k++) while (!root->done[k]) usleep(1000);
        long total = root->counter, want = (long)expected * iters;
        ok = (total == want);
        printf("[nodo 0] contador=%ld esperado=%ld %s\n", total, want, ok ? "PASS" : "FAIL");
        root->finish = 1; sleep(1);
    } else while (!root->finish) usleep(1000);
    dsm_finalize();
    return use_lock ? !ok : 0;
}
