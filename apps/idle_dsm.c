/* idle_dsm: nodo que solo se une al clúster y se queda vivo un tiempo, para probar el
 * heartbeat, la detección de fallas y las consultas de dsmctl.
 * Uso: idle_dsm dsm.conf <segundos> [kb1,kb2,...]
 * El tercer argumento (opcional) hace una dsm_alloc por cada tamaño en KB y escribe en ella,
 * para que `dsmctl ... procs` y `pages` muestren asignaciones de este proceso.
 * Cada segundo imprime cuántos nodos ve vivos (ALIVE o SUSPECT); las transiciones
 * ALIVE -> SUSPECT -> DOWN las registra la biblioteca en stderr. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "dsm.h"

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "uso: %s dsm.conf segundos [kb1,kb2,...]\n", argv[0]); return 2; }
    int secs = atoi(argv[2]), rc = dsm_init(argv[1]);
    if (rc) { fprintf(stderr, "dsm_init: %s\n", dsm_strerror(rc)); return 1; }
    int me = dsm_node_id();
    if (argc >= 4) {
        char *list = strdup(argv[3]), *save = NULL;
        for (char *tok = strtok_r(list, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            size_t kb = strtoul(tok, NULL, 10); void *p;
            if (!kb) continue;
            if ((rc = dsm_alloc(kb * 1024, &p)) != DSM_OK) { fprintf(stderr, "dsm_alloc(%zu KB): %s\n", kb, dsm_strerror(rc)); continue; }
            memset(p, me + 1, kb * 1024);
            printf("[nodo %d] dsm_alloc %zu KB -> %p\n", me, kb, p);
        }
        free(list);
    }
    for (int t = 1; t <= secs; t++) {
        sleep(1);
        printf("[nodo %d] t=%2ds nodos vivos=%d\n", me, t, dsm_num_nodes());
        fflush(stdout);
    }
    dsm_finalize();
    return 0;
}
