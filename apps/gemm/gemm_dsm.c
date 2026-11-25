/* gemm_dsm.c: PolyBench/C gemm (C := alpha*A*B + beta*C) adaptado al API del DSM.
 *
 * Origen: PolyBench/C 4.2.1, linear-algebra/blas/gemm/gemm.c (ver LICENSE.txt en esta carpeta).
 * La versión original, sin cambios, está en gemm_serial.c y sirve como referencia.
 *
 * Cambios respecto al original:
 *   - Las matrices A, B y C se reservan con dsm_alloc (no malloc) y el nodo 0 publica los
 *     punteros en la página raíz (dsm_root). El puntero vale lo mismo en todos los nodos.
 *   - Cada fila se rellena hasta un múltiplo del tamaño de página, de modo que el bloque de filas
 *     de cada nodo ocupa páginas completas y dos nodos nunca escriben en la misma página.
 *   - Las filas de C (y de A) se reparten entre los nodos; dentro de cada nodo se reparten entre
 *     T pthreads (worker threads). La inicialización también se reparte igual.
 *   - Las fases se separan con dsm_barrier.
 *   - El nodo 0 verifica el resultado recalculando una muestra de elementos de C desde A y B, con
 *     el mismo orden de operaciones que el kernel original, y calcula un checksum de toda C.
 *   - Con -d <archivo>, el nodo 0 escribe C en el mismo formato que el volcado de PolyBench
 *     (POLYBENCH_DUMP_ARRAYS), para compararlo con gemm_serial usando diff.
 *
 * Uso: gemm_dsm <dsm.conf> <nodos> [-t hilos] [-n ni nj nk] [-d archivo] [-s muestras]
 *   nodos  cantidad de nodos que deben participar (uno por terminal o por máquina)
 *   -t     worker threads por nodo (4 por defecto)
 *   -n     dimensiones; por defecto las del dataset de gemm.h (LARGE: 1000 1100 1200)
 *   -d     archivo donde el nodo 0 vuelca C (formato PolyBench)
 *   -s     elementos de C que el nodo 0 verifica (2000 por defecto; 0 = todos) */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include "dsm.h"
#include "gemm.h"

#define MAXN       64      /* MAX_NODES del DSM */
#define MAXT       64      /* hilos por nodo */

/* Vive en la página raíz (página 0). Todo lo que deben ver los demás nodos va aquí. */
typedef struct {
    dsm_barrier_t bar;
    uint64_t A, B, C;                  /* punteros publicados por el nodo 0 */
    uint32_t ni, nj, nk, nodes;
    uint64_t lda, ldb, ldc;            /* elementos por fila, ya rellenados a página */
    DATA_TYPE alpha, beta;
    double t_init[MAXN], t_comp[MAXN]; /* segundos, uno por nodo */
} root_t;

typedef struct {
    int phase;                         /* 0 = inicializar, 1 = kernel */
    size_t r_lo, r_hi;                 /* filas de A y C */
    size_t k_lo, k_hi;                 /* filas de B (solo inicialización) */
} work_t;

static root_t *R;
static DATA_TYPE *A, *B, *C;
static size_t ni, nj, nk, lda, ldb, ldc;
static int NODES;              /* nodos esperados; cuenta de cada dsm_barrier */

static double now(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void barrier(int me) {
    int rc = dsm_barrier(&R->bar, (uint32_t)NODES);
    if (rc) { fprintf(stderr, "[nodo %d] dsm_barrier: %s\n", me, dsm_strerror(rc)); exit(1); }
}

/* Valores iniciales: las mismas fórmulas que init_array del original. */
static inline DATA_TYPE c0(size_t i, size_t j) { return (DATA_TYPE)((i * j + 1) % ni) / ni; }
static inline DATA_TYPE a0(size_t i, size_t j) { return (DATA_TYPE)(i * (j + 1) % nk) / nk; }
static inline DATA_TYPE b0(size_t i, size_t j) { return (DATA_TYPE)(i * (j + 2) % nj) / nj; }

/* Reparto en bloques contiguos: la parte p de q del rango [lo, hi). */
static void split(size_t lo, size_t hi, int p, int q, size_t *a, size_t *b) {
    size_t n = hi - lo, base = n / q, extra = n % q;
    *a = lo + p * base + (p < (int)extra ? (size_t)p : extra);
    *b = *a + base + (p < (int)extra);
}

static void *worker(void *arg) {
    work_t *w = arg;
    DATA_TYPE alpha = R->alpha, beta = R->beta;
    if (w->phase == 0) {
        for (size_t i = w->r_lo; i < w->r_hi; i++) {
            for (size_t j = 0; j < nj; j++) C[i * ldc + j] = c0(i, j);
            for (size_t j = 0; j < nk; j++) A[i * lda + j] = a0(i, j);
        }
        for (size_t i = w->k_lo; i < w->k_hi; i++)
            for (size_t j = 0; j < nj; j++) B[i * ldb + j] = b0(i, j);
    } else {
        /* kernel_gemm original, limitado a las filas de este hilo */
        for (size_t i = w->r_lo; i < w->r_hi; i++) {
            DATA_TYPE *Ci = &C[i * ldc];
            const DATA_TYPE *Ai = &A[i * lda];
            for (size_t j = 0; j < nj; j++)
                Ci[j] *= beta;
            for (size_t k = 0; k < nk; k++) {
                const DATA_TYPE *Bk = &B[k * ldb];
                for (size_t j = 0; j < nj; j++)
                    Ci[j] += alpha * Ai[k] * Bk[j];
            }
        }
    }
    return NULL;
}

/* Corre una fase con T hilos sobre las filas que le tocan a este nodo. */
static double run_phase(int phase, int me, int nodes, int T) {
    pthread_t th[MAXT]; work_t w[MAXT];
    size_t rlo, rhi, klo, khi;
    split(0, ni, me, nodes, &rlo, &rhi);
    split(0, nk, me, nodes, &klo, &khi);
    double t = now();
    for (int i = 0; i < T; i++) {
        w[i].phase = phase;
        split(rlo, rhi, i, T, &w[i].r_lo, &w[i].r_hi);
        split(klo, khi, i, T, &w[i].k_lo, &w[i].k_hi);
        if (pthread_create(&th[i], NULL, worker, &w[i])) { perror("pthread_create"); exit(1); }
    }
    for (int i = 0; i < T; i++) pthread_join(th[i], NULL);
    return now() - t;
}

/* Mismo formato que print_array del original (incluido el salto cada 20 con i*ni+j). */
static int dump(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return -1; }
    fprintf(f, "==BEGIN DUMP_ARRAYS==\n");
    fprintf(f, "begin dump: %s", "C");
    for (size_t i = 0; i < ni; i++)
        for (size_t j = 0; j < nj; j++) {
            if ((i * ni + j) % 20 == 0) fprintf(f, "\n");
            fprintf(f, DATA_PRINTF_MODIFIER, C[i * ldc + j]);
        }
    fprintf(f, "\nend   dump: %s\n", "C");
    fprintf(f, "==END   DUMP_ARRAYS==\n");
    fclose(f);
    return 0;
}

/* Recalcula C[i][j] desde A y B con el mismo orden de operaciones del kernel. */
static int check(size_t samples) {
    size_t total = ni * nj, n = (samples == 0 || samples > total) ? total : samples, bad = 0;
    uint64_t seed = 0x9E3779B97F4A7C15ull;
    for (size_t s = 0; s < n; s++) {
        size_t i, j;
        if (n == total) { i = s / nj; j = s % nj; }
        else if (s < 4) { i = (s & 1) ? ni - 1 : 0; j = (s & 2) ? nj - 1 : 0; }  /* esquinas */
        else { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; i = seed % ni; j = (seed >> 32) % nj; }
        DATA_TYPE c = c0(i, j) * R->beta;
        for (size_t k = 0; k < nk; k++) c += R->alpha * A[i * lda + k] * B[k * ldb + j];
        DATA_TYPE got = C[i * ldc + j];
        if (fabs((double)(got - c)) > 1e-9 * (fabs((double)c) + 1.0)) {
            if (bad < 5) printf("[nodo 0] C[%zu][%zu] = %.10g, esperado %.10g\n", i, j, (double)got, (double)c);
            bad++;
        }
    }
    printf("[nodo 0] verificacion: %zu de %zu elementos revisados, %zu incorrectos\n", n, total, bad);
    return bad == 0;
}

static size_t pad(size_t n, size_t ps) {
    size_t per = ps / sizeof(DATA_TYPE);
    return (n + per - 1) / per * per;
}

static void usage(const char *p) {
    fprintf(stderr, "uso: %s dsm.conf nodos [-t hilos] [-n ni nj nk] [-d archivo] [-s muestras]\n", p);
    exit(2);
}

int main(int argc, char **argv) {
    if (argc < 3) usage(argv[0]);
    int nodes = atoi(argv[2]), T = 4;
    size_t a_ni = NI, a_nj = NJ, a_nk = NK, samples = 2000;
    const char *dump_path = NULL;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i + 1 < argc) T = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 3 < argc) {
            a_ni = strtoul(argv[++i], NULL, 10); a_nj = strtoul(argv[++i], NULL, 10); a_nk = strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "-d") && i + 1 < argc) dump_path = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) samples = strtoul(argv[++i], NULL, 10);
        else usage(argv[0]);
    }
    if (nodes < 1 || nodes > MAXN || T < 1 || T > MAXT || !a_ni || !a_nj || !a_nk) usage(argv[0]);

    NODES = nodes;
    int rc = dsm_init(argv[1]);
    if (rc) { fprintf(stderr, "dsm_init: %s\n", dsm_strerror(rc)); return 1; }
    int me = dsm_node_id();
    size_t ps = dsm_page_size();
    R = dsm_root();
    if (me >= nodes) { fprintf(stderr, "[nodo %d] se esperaban solo %d nodos\n", me, nodes); return 1; }

    barrier(me);                                   /* 0: todos unidos; antes no existen sus slices */

    /* ---- 1. el nodo 0 reserva las matrices y las publica en la raíz ---- */
    if (me == 0) {
        R->nodes = nodes;
        R->ni = a_ni; R->nj = a_nj; R->nk = a_nk;
        R->lda = pad(a_nk, ps); R->ldb = pad(a_nj, ps); R->ldc = pad(a_nj, ps);
        R->alpha = 1.5; R->beta = 1.2;          /* los mismos valores de init_array */
        size_t sa = a_ni * R->lda * sizeof(DATA_TYPE), sb = a_nk * R->ldb * sizeof(DATA_TYPE),
               sc = a_ni * R->ldc * sizeof(DATA_TYPE);
        void *p;
        if ((rc = dsm_alloc(sa, &p))) { fprintf(stderr, "[nodo 0] dsm_alloc(A, %zu MB): %s\n", sa >> 20, dsm_strerror(rc)); return 1; }
        R->A = (uint64_t)(uintptr_t)p;
        if ((rc = dsm_alloc(sb, &p))) { fprintf(stderr, "[nodo 0] dsm_alloc(B, %zu MB): %s\n", sb >> 20, dsm_strerror(rc)); return 1; }
        R->B = (uint64_t)(uintptr_t)p;
        if ((rc = dsm_alloc(sc, &p))) { fprintf(stderr, "[nodo 0] dsm_alloc(C, %zu MB): %s\n", sc >> 20, dsm_strerror(rc)); return 1; }
        R->C = (uint64_t)(uintptr_t)p;
        printf("[nodo 0] gemm %ux%ux%u, %d nodos x %d hilos\n", R->ni, R->nj, R->nk, nodes, T);
        printf("[nodo 0] A %.1f MB, B %.1f MB, C %.1f MB (total %.1f MB en memoria DSM)\n",
               sa / 1048576.0, sb / 1048576.0, sc / 1048576.0, (sa + sb + sc) / 1048576.0);
    }
    barrier(me);                                   /* 1: matrices reservadas y publicadas */

    ni = R->ni; nj = R->nj; nk = R->nk; lda = R->lda; ldb = R->ldb; ldc = R->ldc;
    A = (DATA_TYPE *)(uintptr_t)R->A; B = (DATA_TYPE *)(uintptr_t)R->B; C = (DATA_TYPE *)(uintptr_t)R->C;

    /* ---- 2. inicialización repartida ---- */
    R->t_init[me] = run_phase(0, me, nodes, T);
    barrier(me);                                   /* 2: A, B y C inicializadas */

    /* ---- 3. kernel ---- */
    double t0 = now();
    R->t_comp[me] = run_phase(1, me, nodes, T);
    barrier(me);                                   /* 3: C calculada */
    double wall = now() - t0;

    /* ---- 4. verificación en el nodo 0 ---- */
    int ok = 1;
    if (me == 0) {
        for (int k = 0; k < nodes; k++) {
            size_t lo, hi; split(0, ni, k, nodes, &lo, &hi);
            printf("[nodo 0] nodo %d: filas %zu..%zu, init %.3f s, kernel %.3f s\n", k, lo, hi ? hi - 1 : 0,
                   R->t_init[k], R->t_comp[k]);
        }
        double flops = 2.0 * ni * nj * nk;
        printf("[nodo 0] tiempo del kernel (entre barreras): %.3f s, %.2f GFLOP/s\n", wall, flops / wall * 1e-9);
        double sum = 0;
        for (size_t i = 0; i < ni; i++) for (size_t j = 0; j < nj; j++) sum += C[i * ldc + j];
        printf("[nodo 0] checksum de C: %.10e\n", sum);
        ok = check(samples);
        if (dump_path && dump(dump_path) == 0) printf("[nodo 0] C volcada en %s\n", dump_path);
    }

    dsm_stats_t s; dsm_get_stats(DSM_LOCAL, &s);
    unsigned long long nf = s.read_faults + s.write_faults;
    printf("[nodo %d] fallos lectura=%llu escritura=%llu (latencia prom %.1f us) paginas recibidas=%llu enviadas=%llu "
           "inval recibidas=%llu MB recibidos=%.1f\n", me, (unsigned long long)s.read_faults,
           (unsigned long long)s.write_faults, nf ? (double)s.fault_time_total_us / nf : 0.0,
           (unsigned long long)s.pages_received, (unsigned long long)s.pages_sent,
           (unsigned long long)s.invalidations_received, s.bytes_received / 1048576.0);
    if (me == 0) {
        dsm_stats_t g;
        if (dsm_get_stats(DSM_GLOBAL, &g) == DSM_OK)
            printf("[nodo 0] global: fallos=%llu paginas transferidas=%llu invalidaciones=%llu MB enviados=%.1f mensajes=%llu\n",
                   (unsigned long long)(g.read_faults + g.write_faults), (unsigned long long)g.pages_received,
                   (unsigned long long)g.invalidations_sent, g.bytes_sent / 1048576.0, (unsigned long long)g.messages_sent);
        printf("[nodo 0] RESULTADO: %s\n", ok ? "PASS" : "FALLO");
    }
    fflush(stdout);
    dsm_flush();                                   /* las páginas en M vuelven a su home mientras todos siguen vivos */
    barrier(me);                                   /* 4: nadie sale antes de que el nodo 0 termine y todos devolvieron sus páginas */
    dsm_finalize();
    return ok ? 0 : 1;
}
