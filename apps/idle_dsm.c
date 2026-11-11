/* idle_dsm: nodo que solo se une al clúster y se queda vivo un tiempo, para probar el
 * heartbeat y la detección de fallas. Uso: idle_dsm dsm.conf <segundos>
 * Cada segundo imprime cuántos nodos ve vivos (ALIVE o SUSPECT); las transiciones
 * ALIVE -> SUSPECT -> DOWN las registra la biblioteca en stderr. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "dsm.h"

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "uso: %s dsm.conf segundos\n", argv[0]); return 2; }
    int secs = atoi(argv[2]), rc = dsm_init(argv[1]);
    if (rc) { fprintf(stderr, "dsm_init: %s\n", dsm_strerror(rc)); return 1; }
    int me = dsm_node_id();
    for (int t = 1; t <= secs; t++) {
        sleep(1);
        printf("[nodo %d] t=%2ds nodos vivos=%d\n", me, t, dsm_num_nodes());
        fflush(stdout);
    }
    dsm_finalize();
    return 0;
}
