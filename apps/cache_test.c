/* cache_test: prueba del reemplazo por CACHE_MAX_PAGES y de PAGE_WRITEBACK.
 * Uso: cache_test conf/dsm_cache.conf 3   (CACHE_MAX_PAGES pequeño; ver tests/run_cache.sh)
 *
 * Fase 1: el nodo 0 llena un bloque de 8 MB (2048 páginas, muchas más que la caché) y los nodos
 *         1 y 2 lo leen completo dos veces. Las copias S más antiguas se descartan y se vuelven a
 *         pedir; el contenido debe ser siempre el correcto.
 * Fase 2: el nodo 1 escribe todo el bloque. Sus páginas M más antiguas se devuelven al nodo 0
 *         con PAGE_WRITEBACK; después los nodos 0 y 2 verifican lo que escribió.
 * Fase 3: el nodo 2 escribe 32 páginas (caben en su caché, así que quedan en M) y llama a
 *         dsm_finalize sin barrera final. El writeback de salida las devuelve al nodo 0, que las
 *         lee después de que el nodo 2 ya salió. Sin writeback, ese dato se perdería. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include "dsm.h"

#define MB8 (8u << 20)
#define TAIL 32

typedef struct { dsm_barrier_t bar; uint64_t blk; } root_t;

static size_t check(const uint32_t *w, size_t words, uint32_t key) {
    size_t bad = 0;
    for (size_t i = 0; i < words; i++) if (w[i] != ((uint32_t)i ^ key)) bad++;
    return bad;
}
static void fill(uint32_t *w, size_t words, uint32_t key) { for (size_t i = 0; i < words; i++) w[i] = (uint32_t)i ^ key; }

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "uso: %s dsm.conf 3\n", argv[0]); return 2; }
    int n = atoi(argv[2]), rc = dsm_init(argv[1]), fails = 0;
    if (rc) { fprintf(stderr, "dsm_init: %s\n", dsm_strerror(rc)); return 1; }
    if (n != 3) { fprintf(stderr, "esta prueba usa exactamente 3 nodos\n"); return 2; }
    int me = dsm_node_id(); size_t ps = dsm_page_size(), words = MB8 / 4;
    root_t *r = dsm_root();
    dsm_barrier(&r->bar, 3);

    /* ---- fase 1: lectura de un bloque remoto mucho más grande que la caché ---- */
    if (me == 0) {
        void *p;
        if ((rc = dsm_alloc(MB8, &p))) { printf("[nodo 0] dsm_alloc: %s\n", dsm_strerror(rc)); return 1; }
        fill(p, words, 0x11111111); r->blk = (uint64_t)(uintptr_t)p;
    }
    dsm_barrier(&r->bar, 3);
    uint32_t *w = (uint32_t *)(uintptr_t)r->blk;
    if (me != 0) {
        size_t bad = check(w, words, 0x11111111) + check(w, words, 0x11111111);
        printf("[nodo %d] fase 1: leyo 2 x %zu paginas remotas, %zu palabras incorrectas %s\n",
               me, MB8 / ps, bad, bad ? "FALLO" : "OK");
        if (bad) fails++;
    }
    dsm_barrier(&r->bar, 3);

    /* ---- fase 2: escrituras remotas que no caben en la caché ---- */
    if (me == 1) fill(w, words, 0x22222222);
    dsm_barrier(&r->bar, 3);
    if (me != 1) {
        size_t bad = check(w, words, 0x22222222);
        printf("[nodo %d] fase 2: escritura del nodo 1 verificada, %zu palabras incorrectas %s\n", me, bad, bad ? "FALLO" : "OK");
        if (bad) fails++;
    }
    dsm_barrier(&r->bar, 3);

    /* ---- fase 3: writeback al salir ---- */
    uint32_t *tail = w + (MB8 - TAIL * ps) / 4; size_t tw = TAIL * ps / 4;
    if (me == 2) {
        fill(tail, tw, 0x33333333);
        dsm_barrier(&r->bar, 3);   /* los demás saben que ya escribió */
        printf("[nodo 2] fase 3: escribio %d paginas y sale sin barrera final\n", TAIL);
        printf("[nodo 2] %s\n", fails ? "RESULTADO: FALLO" : "RESULTADO: PASS");
        fflush(stdout);
        dsm_finalize();
        return fails ? 1 : 0;
    }
    dsm_barrier(&r->bar, 3);
    for (int t = 0; t < 100 && dsm_num_nodes() > 2; t++) usleep(100000);   /* espera a que el nodo 2 salga */
    if (me == 0) {
        size_t bad = check(tail, tw, 0x33333333);
        printf("[nodo 0] fase 3: nodos vivos=%d, %zu palabras incorrectas en lo que escribio el nodo 2 %s\n",
               dsm_num_nodes(), bad, (bad || dsm_num_nodes() != 2) ? "FALLO" : "OK");
        if (bad || dsm_num_nodes() != 2) fails++;
    }
    dsm_barrier(&r->bar, 2);
    printf("[nodo %d] %s\n", me, fails ? "RESULTADO: FALLO" : "RESULTADO: PASS");
    fflush(stdout);
    dsm_barrier(&r->bar, 2);
    dsm_finalize();
    return fails ? 1 : 0;
}
