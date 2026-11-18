/* alloc_test: prueba de la asignación distribuida (dsm_alloc en dos fases).
 * Uso: alloc_test dsm.conf <nodos>   (una terminal por nodo, o tests/run_alloc.sh)
 *
 * Fase 1: el nodo 1 pide 1,5 veces NODE_MEM_MB. No cabe en su slice, así que el rango
 *         tiene que cruzar al slice de otro nodo. Lo llena con un patrón y todos lo verifican.
 * Fase 2: los nodos 0 y 2 piden NODE_MEM_MB / 2 al mismo tiempo. Al nodo 0 ya no le cabe en su
 *         slice y ambos escogen el mismo rango (el inicio del slice del nodo 2), así que sus
 *         reservas chocan en el home: uno gana y el otro debe reintentar en otro rango.
 * Fase 3: todos los nodos piden 2 bloques de 1 MB al mismo tiempo.
 *         En las fases 2 y 3 el nodo 0 revisa que ningún bloque se traslape con otro y que cada
 *         página tenga la marca de quien la pidió.
 * Fase 4: una asignación más grande que toda la memoria libre debe fallar con DSM_E_NOMEM. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "dsm.h"

#define MAXN 16
#define K 2
#define BLK (1u << 20)

typedef struct {
    dsm_barrier_t bar;
    uint64_t big_ptr, big_size;
    uint64_t half[2], half_size;
    uint64_t blk[MAXN][K];
} root_t;

static int read_mem_mb(const char *conf) {
    FILE *f = fopen(conf, "r"); char line[256]; int mb = 0;
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) if (sscanf(line, "NODE_MEM_MB=%d", &mb) == 1) break;
    fclose(f);
    return mb;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "uso: %s dsm.conf nodos\n", argv[0]); return 2; }
    int n = atoi(argv[2]), rc = dsm_init(argv[1]), fails = 0;
    if (rc) { fprintf(stderr, "dsm_init: %s\n", dsm_strerror(rc)); return 1; }
    if (n < 3 || n > MAXN) { fprintf(stderr, "se necesitan entre 3 y %d nodos\n", MAXN); return 2; }
    int me = dsm_node_id(); size_t ps = dsm_page_size();
    root_t *r = dsm_root();
    dsm_barrier(&r->bar, n);

    /* ---- fase 1: una asignación que no cabe en un solo nodo ---- */
    if (me == 1) {
        size_t size = (size_t)read_mem_mb(argv[1]) * 3 / 2 << 20; void *p;
        rc = dsm_alloc(size, &p);
        if (rc) { printf("[nodo 1] dsm_alloc(%zu MB): %s\n", size >> 20, dsm_strerror(rc)); fails++; }
        else {
            uint32_t *w = p;
            for (size_t i = 0; i < size / 4; i++) w[i] = (uint32_t)i ^ 0x5a5a5a5a;
            r->big_ptr = (uint64_t)(uintptr_t)p; r->big_size = size;
            printf("[nodo 1] dsm_alloc(%zu MB) -> %p, paginas %zu..%zu\n", size >> 20, p,
                   ((uintptr_t)p - (uintptr_t)r) / ps, ((uintptr_t)p - (uintptr_t)r + size) / ps - 1);
        }
    }
    dsm_barrier(&r->bar, n);
    if (r->big_ptr) {
        uint32_t *w = (uint32_t *)(uintptr_t)r->big_ptr; size_t bad = 0;
        for (size_t i = 0; i < r->big_size / 4; i++) if (w[i] != ((uint32_t)i ^ 0x5a5a5a5a)) bad++;
        printf("[nodo %d] fase 1: %zu MB verificados, %zu palabras incorrectas %s\n", me, (size_t)(r->big_size >> 20), bad, bad ? "FALLO" : "OK");
        if (bad) fails++;
    } else if (me == 0) { printf("[nodo 0] fase 1: no hay asignacion grande FALLO\n"); fails++; }
    dsm_barrier(&r->bar, n);

    /* ---- fase 2: choque de reservas entre los nodos 0 y 2 ---- */
    size_t half = (size_t)read_mem_mb(argv[1]) << 19;
    if (me == 0 || me == 2) {
        void *p; int i = me / 2;
        if ((rc = dsm_alloc(half, &p))) { printf("[nodo %d] fase 2: dsm_alloc: %s\n", me, dsm_strerror(rc)); fails++; }
        else {
            for (size_t off = 0; off < half; off += ps) *(uint64_t *)((char *)p + off) = 0xC0DE0000u | (uint32_t)me;
            r->half[i] = (uint64_t)(uintptr_t)p;
            printf("[nodo %d] fase 2: dsm_alloc(%zu MB) -> paginas %zu..%zu\n", me, half >> 20,
                   ((uintptr_t)p - (uintptr_t)r) / ps, ((uintptr_t)p - (uintptr_t)r + half) / ps - 1);
        }
    }
    dsm_barrier(&r->bar, n);
    if (me == 0) {
        uint64_t a = r->half[0], b = r->half[1]; int wrong = 0;
        int ov = a && b && a < b + half && b < a + half;
        for (int i = 0; i < 2; i++) if (r->half[i])
            for (size_t off = 0; off < half; off += ps)
                if (*(uint64_t *)(uintptr_t)(r->half[i] + off) != (0xC0DE0000u | (uint32_t)(i * 2))) wrong++;
        int ok = a && b && !ov && !wrong;
        printf("[nodo 0] fase 2: traslape=%d, paginas con marca incorrecta=%d %s\n", ov, wrong, ok ? "OK" : "FALLO");
        if (!ok) fails++;
    }
    dsm_barrier(&r->bar, n);

    /* ---- fase 3: asignaciones simultáneas desde todos los nodos ---- */
    for (int k = 0; k < K; k++) {
        void *p;
        if ((rc = dsm_alloc(BLK, &p))) { printf("[nodo %d] dsm_alloc(1 MB) #%d: %s\n", me, k, dsm_strerror(rc)); fails++; continue; }
        for (size_t off = 0; off < BLK; off += ps) *(uint64_t *)((char *)p + off) = ((uint64_t)me << 32) | (uint32_t)k;
        r->blk[me][k] = (uint64_t)(uintptr_t)p;
    }
    dsm_barrier(&r->bar, n);
    if (me == 0) {
        int overlap = 0, wrong = 0, total = 0;
        for (int a = 0; a < n; a++) for (int i = 0; i < K; i++) {
            uint64_t pa = r->blk[a][i]; if (!pa) continue; total++;
            for (int b = 0; b < n; b++) for (int j = 0; j < K; j++) {
                uint64_t pb = r->blk[b][j];
                if (!pb || (a == b && i == j)) continue;
                if (pa < pb + BLK && pb < pa + BLK) overlap++;
            }
            for (size_t off = 0; off < BLK; off += ps)
                if (*(uint64_t *)(uintptr_t)(pa + off) != (((uint64_t)a << 32) | (uint32_t)i)) wrong++;
        }
        printf("[nodo 0] fase 3: %d bloques de 1 MB, traslapes=%d, paginas con marca incorrecta=%d %s\n",
               total, overlap / 2, wrong, (overlap || wrong || total != n * K) ? "FALLO" : "OK");
        if (overlap || wrong || total != n * K) fails++;
    }
    dsm_barrier(&r->bar, n);

    /* ---- fase 4: más memoria de la que hay ---- */
    if (me == 2) {
        void *p; size_t huge = (size_t)read_mem_mb(argv[1]) * (n + 1) << 20;
        rc = dsm_alloc(huge, &p);
        printf("[nodo 2] fase 4: dsm_alloc(%zu MB) -> %s %s\n", huge >> 20, dsm_strerror(rc), rc == DSM_E_NOMEM ? "OK" : "FALLO");
        if (rc != DSM_E_NOMEM) fails++;
    }
    dsm_barrier(&r->bar, n);

    dsm_stats_t st; dsm_get_stats(DSM_LOCAL, &st);
    printf("[nodo %d] paginas propias=%u asignadas=%u fallos lectura=%llu escritura=%llu\n", me, st.pages_owned,
           st.pages_allocated, (unsigned long long)st.read_faults, (unsigned long long)st.write_faults);
    printf("[nodo %d] %s\n", me, fails ? "RESULTADO: FALLO" : "RESULTADO: PASS");
    fflush(stdout);
    dsm_barrier(&r->bar, n);
    dsm_finalize();
    return fails ? 1 : 0;
}
