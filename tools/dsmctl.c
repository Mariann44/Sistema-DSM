/* tools/dsmctl.c - herramienta de línea de comandos para consultar un nodo DSM.
 * Uso:
 *   dsmctl <ip> <slot> <comando> [argumento]
 *   dsmctl <ip> <slot> [conf_path] <comando>
 *
 * Ejemplo:
 *   dsmctl 127.0.0.1 0 nodes
 *   dsmctl 127.0.0.1 0 stats
 *   dsmctl 127.0.0.1 0 stats global
 *   dsmctl 127.0.0.1 0 pages
 *   dsmctl 127.0.0.1 0 procs
 *   dsmctl 127.0.0.1 0 procs detail
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define DEFAULT_PORT_BASE 27100

static int parse_port_base_from_conf(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return DEFAULT_PORT_BASE;
    char line[256];
    int base = DEFAULT_PORT_BASE;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        if (strncmp(line, "PORT_BASE=", 10) == 0) {
            int p = atoi(line + 10);
            if (p > 0 && p < 65535) base = p;
            break;
        }
    }
    fclose(f);
    return base;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Uso: %s <ip> <slot> <comando> [argumentos...]\n", argv[0]);
        fprintf(stderr, "Comandos disponibles:\n");
        fprintf(stderr, "  nodes\n");
        fprintf(stderr, "  stats [local|global]\n");
        fprintf(stderr, "  pages [global]\n");
        fprintf(stderr, "  procs [detail]\n");
        return 1;
    }

    const char *ip_str = argv[1];
    int slot = atoi(argv[2]);
    if (slot < 0 || slot > 64) {
        fprintf(stderr, "Error: slot inválido (%d)\n", slot);
        return 1;
    }

    /* Revisar si existe conf/dsm_local.conf o dsm.conf para tomar PORT_BASE si está configurado */
    int port_base = DEFAULT_PORT_BASE;
    if (access("conf/dsm_local.conf", R_OK) == 0) {
        port_base = parse_port_base_from_conf("conf/dsm_local.conf");
    } else if (access("dsm.conf", R_OK) == 0) {
        port_base = parse_port_base_from_conf("dsm.conf");
    }

    int ctl_port = port_base + 4 * slot + 2;

    /* Resolver dirección */
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    char port_str[16];
    snprintf(port_str, sizeof port_str, "%d", ctl_port);

    if (getaddrinfo(ip_str, port_str, &hints, &res) != 0 || !res) {
        fprintf(stderr, "Error: no se pudo resolver la IP/host '%s'\n", ip_str);
        return 1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        perror("socket");
        freeaddrinfo(res);
        return 1;
    }

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        fprintf(stderr, "Error: no se pudo conectar al CTL_PORT %d de %s (slot %d). ¿Está el nodo activo?\n",
                ctl_port, ip_str, slot);
        close(fd);
        freeaddrinfo(res);
        return 1;
    }
    freeaddrinfo(res);

    /* Construir comando completo a partir de argv[3...] */
    char cmd_buf[512] = {0};
    for (int i = 3; i < argc; i++) {
        if (i > 3) strncat(cmd_buf, " ", sizeof(cmd_buf) - strlen(cmd_buf) - 1);
        strncat(cmd_buf, argv[i], sizeof(cmd_buf) - strlen(cmd_buf) - 1);
    }
    strncat(cmd_buf, "\n", sizeof(cmd_buf) - strlen(cmd_buf) - 1);

    /* Enviar comando */
    size_t cmd_len = strlen(cmd_buf);
    if (write(fd, cmd_buf, cmd_len) != (ssize_t)cmd_len) {
        perror("write");
        close(fd);
        return 1;
    }

    /* Leer respuesta hasta EOF o línea vacía final */
    FILE *fp = fdopen(fd, "r");
    if (!fp) {
        perror("fdopen");
        close(fd);
        return 1;
    }

    char line[1024];
    while (fgets(line, sizeof line, fp)) {
        /* Si encontramos la línea vacía de cierre de respuesta, imprimimos y terminamos */
        if (strcmp(line, "\n") == 0 || strcmp(line, "\r\n") == 0) {
            break;
        }
        fputs(line, stdout);
    }

    fclose(fp);
    return 0;
}

