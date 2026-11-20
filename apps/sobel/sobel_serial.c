/* sobel_serial.c: versión serial de referencia del filtro Sobel aplicado a un video.
 *
 * Origen: DanyEle/Sobel_Filter, Native_Sobel (Daniele Gadler), que a su vez toma parte del código
 * de petermlm/SobelFilter. El main.c original procesa una sola imagen y queda en esta carpeta sin
 * cambios. image_operations.c y file_operations.c se usan sin modificar.
 *
 * Igual que el original, la entrada es RGB crudo (rgb24), pero con varios cuadros seguidos: es lo
 * que produce ffmpeg con -pix_fmt rgb24 -f rawvideo (ver tools/video_to_rgb.sh). Cada cuadro pasa
 * por los mismos pasos del original: rgb_to_gray, itConv con el kernel horizontal y el vertical,
 * y contour. La salida es escala de grises cruda, un byte por píxel y un cuadro tras otro.
 *
 * Uso: sobel_serial <entrada.rgb> <ancho> <alto> <salida.gray> [-f max_cuadros] */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "image_operations.h"

static uint64_t fnv1a(uint64_t h, const byte *p, size_t n) {
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "uso: %s entrada.rgb ancho alto salida.gray [-f max_cuadros]\n", argv[0]); return 2; }
    int width = atoi(argv[2]), height = atoi(argv[3]);
    long max_frames = (argc >= 7 && !strcmp(argv[5], "-f")) ? atol(argv[6]) : 0;
    if (width < 1 || height < 1) { fprintf(stderr, "dimensiones invalidas\n"); return 2; }
    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[4], "wb");
    if (!in) { perror(argv[1]); return 1; }
    if (!out) { perror(argv[4]); return 1; }

    int rgb_size = width * height * 3;
    int sobel_h[] = { -1, 0, 1, -2, 0, 2, -1, 0, 1 };   /* kernels del main.c original */
    int sobel_v[] = { 1, 2, 1, 0, 0, 0, -1, -2, -1 };
    byte *rgb = malloc(rgb_size);
    uint64_t h = 0xcbf29ce484222325ull;
    long frames = 0;
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);

    while ((!max_frames || frames < max_frames) && fread(rgb, 1, rgb_size, in) == (size_t)rgb_size) {
        byte *gray, *gh, *gv, *res;
        int gray_size = rgb_to_gray(rgb, &gray, rgb_size);
        itConv(gray, gray_size, width, sobel_h, &gh);
        itConv(gray, gray_size, width, sobel_v, &gv);
        contour(gh, gv, gray_size, &res);
        fwrite(res, 1, gray_size, out);
        h = fnv1a(h, res, gray_size);
        free(gray); free(gh); free(gv); free(res);
        frames++;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    fclose(in); fclose(out); free(rgb);
    double t = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
    printf("sobel_serial: %ld cuadros de %dx%d en %.3f s (%.1f cuadros/s)\n", frames, width, height, t, frames / t);
    printf("sobel_serial: checksum FNV-1a de la salida: %016llx\n", (unsigned long long)h);
    return frames ? 0 : 1;
}
