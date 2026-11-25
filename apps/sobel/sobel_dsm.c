/* sobel_dsm.c: filtro Sobel sobre un video completo guardado en la memoria DSM.
 *
 * Origen: DanyEle/Sobel_Filter, Native_Sobel (Daniele Gadler), con partes de petermlm/SobelFilter.
 * Las funciones del filtro (rgb_to_gray, itConv, contour en image_operations.c) se usan sin
 * modificar. sobel_serial.c es la versión de referencia sin DSM.
 *
 * Cambios respecto al original:
 *   - En lugar de una imagen, la entrada es un video RGB crudo (rgb24, cuadros seguidos), que
 *     produce ffmpeg (tools/video_to_rgb.sh).
 *   - El nodo 0 carga el archivo COMPLETO en memoria DSM reservada con dsm_alloc. La lectura se
 *     hace con fread a un búfer local y memcpy a la memoria DSM, porque un read() o fread() directo
 *     sobre una página ausente no genera SIGSEGV sino EFAULT. Así el video queda repartido en los
 *     slices de los nodos (el caso de "almacenar un archivo grande en memoria distribuida").
 *   - El resultado también vive en memoria DSM. Cada cuadro (de entrada y de salida) empieza en una
 *     página nueva, de modo que dos nodos nunca escriben en la misma página.
 *   - Los cuadros se reparten entre los nodos y, dentro de cada nodo, entre T pthreads.
 *   - Las fases se separan con dsm_barrier.
 *   - El nodo 0 vuelve a calcular algunos cuadros y los compara con lo que produjeron los demás
 *     nodos, calcula un checksum FNV-1a de toda la salida (el mismo que imprime sobel_serial) y,
 *     con -o, escribe la salida en escala de grises cruda.
 *
 * Uso: sobel_dsm <dsm.conf> <nodos> <entrada.rgb> <ancho> <alto> [-t hilos] [-o salida.gray]
 *                [-f max_cuadros] [-v cuadros_a_verificar]
 *   -t  worker threads por nodo (4 por defecto)
 *   -o  archivo de salida (escala de grises, 1 byte por píxel); tools/gray_to_video.sh lo pasa a mp4
 *   -f  procesar solo los primeros max_cuadros cuadros
 *   -v  cuadros que el nodo 0 recalcula para verificar (8 por defecto; 0 = todos) */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include "dsm.h"
#include "image_operations.h"

#define MAXN 64
#define MAXT 64

/* Vive en la página raíz. */
typedef struct {
    dsm_barrier_t bar;
    uint64_t rgb, out;                 /* video de entrada y de salida, publicados por el nodo 0 */
    uint64_t rgb_stride, out_stride;   /* bytes por cuadro, rellenados a página */
    uint32_t width, height, frames, pad;
    double t_load, t_write;
    double t_proc[MAXN];
    uint32_t done[MAXN];               /* cuadros procesados por cada nodo */
} root_t;

typedef struct { uint32_t lo, hi, done; } work_t;

static root_t *R;
static int NODES;
static int sobel_h[] = { -1, 0, 1, -2, 0, 2, -1, 0, 1 };   /* kernels del main.c original */
static int sobel_v[] = { 1, 2, 1, 0, 0, 0, -1, -2, -1 };

static double now(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void barrier(int me) {
    int rc = dsm_barrier(&R->bar, (uint32_t)NODES);
    if (rc) { fprintf(stderr, "[nodo %d] dsm_barrier: %s\n", me, dsm_strerror(rc)); exit(1); }
}

static void split(uint32_t n, int p, int q, uint32_t *a, uint32_t *b) {
    uint32_t base = n / q, extra = n % q;
    *a = p * base + ((uint32_t)p < extra ? (uint32_t)p : extra);
    *b = *a + base + ((uint32_t)p < extra);
}

static uint64_t fnv1a(uint64_t h, const byte *p, size_t n) {
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

/* Pasos del main.c original para un cuadro. Lee el cuadro RGB directamente de la memoria DSM;
 * las funciones originales reservan sus búferes con malloc (memoria local del nodo). */
static byte *sobel_frame(uint32_t f) {
    int w = R->width, rgb_size = w * R->height * 3;
    byte *rgb = (byte *)(uintptr_t)(R->rgb + f * R->rgb_stride), *gray, *gh, *gv, *res;
    int gray_size = rgb_to_gray(rgb, &gray, rgb_size);
    itConv(gray, gray_size, w, sobel_h, &gh);
    itConv(gray, gray_size, w, sobel_v, &gv);
    contour(gh, gv, gray_size, &res);
    free(gray); free(gh); free(gv);
    return res;
}

static void *worker(void *arg) {
    work_t *w = arg;
    size_t gray_size = (size_t)R->width * R->height;
    for (uint32_t f = w->lo; f < w->hi; f++) {
        byte *res = sobel_frame(f);
        memcpy((byte *)(uintptr_t)(R->out + f * R->out_stride), res, gray_size);
        free(res);
        w->done++;
    }
    return NULL;
}

static size_t pad(size_t n, size_t ps) { return (n + ps - 1) / ps * ps; }

/* Nodo 0: lee el archivo cuadro por cuadro y lo copia a la memoria DSM. */
static int load_video(const char *path, uint32_t frames) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    size_t rgb_size = (size_t)R->width * R->height * 3;
    byte *buf = malloc(rgb_size);
    for (uint32_t i = 0; i < frames; i++) {
        if (fread(buf, 1, rgb_size, f) != rgb_size) { fprintf(stderr, "[nodo 0] %s: cuadro %u incompleto\n", path, i); fclose(f); free(buf); return -1; }
        memcpy((byte *)(uintptr_t)(R->rgb + i * R->rgb_stride), buf, rgb_size);
    }
    fclose(f); free(buf);
    return 0;
}

/* Nodo 0: escribe la salida y calcula el checksum. fwrite tampoco puede leer de una página
 * ausente, así que cada cuadro se copia primero a un búfer local. */
static uint64_t write_output(const char *path) {
    size_t gray_size = (size_t)R->width * R->height;
    byte *buf = malloc(gray_size);
    FILE *f = path ? fopen(path, "wb") : NULL;
    if (path && !f) perror(path);
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < R->frames; i++) {
        memcpy(buf, (byte *)(uintptr_t)(R->out + i * R->out_stride), gray_size);
        h = fnv1a(h, buf, gray_size);
        if (f) fwrite(buf, 1, gray_size, f);
    }
    if (f) fclose(f);
    free(buf);
    return h;
}

/* Nodo 0: recalcula cuadros (primero, último y otros repartidos) y los compara con la salida. */
static int verify(uint32_t count) {
    uint32_t n = R->frames, k = (count == 0 || count > n) ? n : count, bad = 0;
    size_t gray_size = (size_t)R->width * R->height;
    for (uint32_t s = 0; s < k; s++) {
        uint32_t f = (k == n) ? s : (k == 1 ? 0 : (uint32_t)((uint64_t)s * (n - 1) / (k - 1)));
        byte *ref = sobel_frame(f);
        if (memcmp(ref, (byte *)(uintptr_t)(R->out + f * R->out_stride), gray_size)) {
            if (bad < 5) printf("[nodo 0] cuadro %u no coincide\n", f);
            bad++;
        }
        free(ref);
    }
    printf("[nodo 0] verificacion: %u de %u cuadros recalculados, %u distintos\n", k, n, bad);
    return bad == 0;
}

static void usage(const char *p) {
    fprintf(stderr, "uso: %s dsm.conf nodos entrada.rgb ancho alto [-t hilos] [-o salida.gray] "
                    "[-f max_cuadros] [-v cuadros]\n", p);
    exit(2);
}

int main(int argc, char **argv) {
    if (argc < 6) usage(argv[0]);
    int nodes = atoi(argv[2]), width = atoi(argv[4]), height = atoi(argv[5]), T = 4;
    const char *in_path = argv[3], *out_path = NULL;
    long max_frames = 0; uint32_t nverify = 8;
    for (int i = 6; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i + 1 < argc) T = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) max_frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "-v") && i + 1 < argc) nverify = (uint32_t)atol(argv[++i]);
        else usage(argv[0]);
    }
    if (nodes < 1 || nodes > MAXN || T < 1 || T > MAXT || width < 1 || height < 1) usage(argv[0]);
    NODES = nodes;

    int rc = dsm_init(argv[1]);
    if (rc) { fprintf(stderr, "dsm_init: %s\n", dsm_strerror(rc)); return 1; }
    int me = dsm_node_id();
    size_t ps = dsm_page_size();
    R = dsm_root();
    if (me >= nodes) { fprintf(stderr, "[nodo %d] se esperaban solo %d nodos\n", me, nodes); return 1; }
    barrier(me);                                   /* 0: todos unidos; antes no existen sus slices */

    /* ---- 1. el nodo 0 reserva la memoria y carga el video completo ---- */
    if (me == 0) {
        struct stat st;
        if (stat(in_path, &st)) { perror(in_path); return 1; }
        size_t rgb_size = (size_t)width * height * 3;
        uint64_t frames = (uint64_t)st.st_size / rgb_size;
        if (max_frames > 0 && (uint64_t)max_frames < frames) frames = max_frames;
        if (frames == 0) { fprintf(stderr, "[nodo 0] %s no tiene ni un cuadro de %dx%d\n", in_path, width, height); return 1; }
        R->width = width; R->height = height; R->frames = (uint32_t)frames;
        R->rgb_stride = pad(rgb_size, ps); R->out_stride = pad((size_t)width * height, ps);
        size_t s_in = frames * R->rgb_stride, s_out = frames * R->out_stride;
        void *p;
        if ((rc = dsm_alloc(s_in, &p))) { fprintf(stderr, "[nodo 0] dsm_alloc(entrada, %zu MB): %s\n", s_in >> 20, dsm_strerror(rc)); return 1; }
        R->rgb = (uint64_t)(uintptr_t)p;
        if ((rc = dsm_alloc(s_out, &p))) { fprintf(stderr, "[nodo 0] dsm_alloc(salida, %zu MB): %s\n", s_out >> 20, dsm_strerror(rc)); return 1; }
        R->out = (uint64_t)(uintptr_t)p;
        printf("[nodo 0] video %ux%u, %u cuadros, %d nodos x %d hilos\n", R->width, R->height, R->frames, nodes, T);
        printf("[nodo 0] entrada %.1f MB, salida %.1f MB (total %.1f MB en memoria DSM)\n",
               s_in / 1048576.0, s_out / 1048576.0, (s_in + s_out) / 1048576.0);
        double t = now();
        if (load_video(in_path, R->frames)) return 1;
        R->t_load = now() - t;
        printf("[nodo 0] video cargado en memoria DSM en %.3f s (%.1f MB/s)\n", R->t_load, s_in / 1048576.0 / R->t_load);
    }
    barrier(me);                                   /* 1: video cargado y publicado */

    /* ---- 2. cada nodo procesa sus cuadros con T hilos ---- */
    uint32_t lo, hi; split(R->frames, me, nodes, &lo, &hi);
    pthread_t th[MAXT]; work_t w[MAXT];
    double t0 = now(), t = now();
    for (int i = 0; i < T; i++) {
        uint32_t a, b; split(hi - lo, i, T, &a, &b);
        w[i].lo = lo + a; w[i].hi = lo + b; w[i].done = 0;
        if (pthread_create(&th[i], NULL, worker, &w[i])) { perror("pthread_create"); exit(1); }
    }
    uint32_t done = 0;
    for (int i = 0; i < T; i++) { pthread_join(th[i], NULL); done += w[i].done; }
    R->t_proc[me] = now() - t; R->done[me] = done;
    barrier(me);                                   /* 2: todos los cuadros procesados */
    double wall = now() - t0;

    /* ---- 3. verificación y salida en el nodo 0 ---- */
    int ok = 1;
    if (me == 0) {
        uint32_t total = 0;
        for (int k = 0; k < nodes; k++) {
            uint32_t a, b; split(R->frames, k, nodes, &a, &b);
            printf("[nodo 0] nodo %d: cuadros %u..%u (%u procesados) en %.3f s\n", k, a, b ? b - 1 : 0, R->done[k], R->t_proc[k]);
            total += R->done[k];
        }
        printf("[nodo 0] procesamiento (entre barreras): %.3f s, %.1f cuadros/s\n", wall, R->frames / wall);
        if (total != R->frames) { printf("[nodo 0] se procesaron %u de %u cuadros\n", total, R->frames); ok = 0; }
        ok &= verify(nverify);
        double t = now();
        uint64_t h = write_output(out_path);
        R->t_write = now() - t;
        printf("[nodo 0] checksum FNV-1a de la salida: %016llx\n", (unsigned long long)h);
        if (out_path) printf("[nodo 0] salida escrita en %s en %.3f s\n", out_path, R->t_write);
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
    barrier(me);                                   /* 3: nadie sale antes de que el nodo 0 termine y todos devolvieron sus páginas */
    dsm_finalize();
    return ok ? 0 : 1;
}
