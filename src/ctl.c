/* ctl.c - hilo de control (CTL_PORT).
 * Atiende comandos de texto, uno por línea:
 *   - nodes
 *   - stats [local|global]
 *   - pages [global]
 *   - procs
 * Responde con tablas formateadas con encabezados y columnas, y termina con una línea vacía.
 */
#include "internal.h"
#include <ctype.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <poll.h>

static const char *page_state_name(int st) {
    switch (st) {
    case PG_M:  return "M";
    case PG_S:  return "S";
    case PG_I:  return "I";
    case PG_PR: return "PR";
    case PG_PW: return "PW";
    default:    return "?";
    }
}

static void ctl_cmd_nodes(FILE *fp) {
    fprintf(fp, "%-6s %-16s %-8s %-8s %-12s %-12s %-10s\n",
            "NODE", "IP", "STATE", "SLOT", "FIRST_PAGE", "NUM_PAGES", "FREE_PAGES");
    fprintf(fp, "----------------------------------------------------------------------------\n");

    pthread_mutex_lock(&G.nodes_mx);
    for (int i = 0; i < G.n_nodes; i++) {
        node_entry_t *n = &G.nodes[i];
        if (n->state == N_UNUSED) continue;
        char ip_str[32];
        uint32_t ip = n->ipv4;
        snprintf(ip_str, sizeof ip_str, "%u.%u.%u.%u",
                 (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
        fprintf(fp, "%-6u %-16s %-8s %-8u %-12u %-12u %-10u\n",
                n->node_id, ip_str, node_state_name(n->state), n->slot,
                n->first_page, n->num_pages, n->free_pages);
    }
    pthread_mutex_unlock(&G.nodes_mx);
    fprintf(fp, "\n");
}

static void ctl_cmd_stats(FILE *fp, const char *arg) {
    dsm_scope_t scope = DSM_LOCAL;
    if (arg && strcasecmp(arg, "global") == 0) scope = DSM_GLOBAL;

    dsm_stats_t s;
    int rc = dsm_get_stats(scope, &s);
    if (rc != DSM_OK) {
        fprintf(fp, "ERROR: %s\n\n", dsm_strerror(rc));
        return;
    }

    uint64_t total_faults = s.read_faults + s.write_faults;
    double avg_latency_us = total_faults > 0 ? (double)s.fault_time_total_us / total_faults : 0.0;

    fprintf(fp, "%-26s %-18s\n", "METRIC", "VALUE");
    fprintf(fp, "--------------------------------------------\n");
    fprintf(fp, "%-26s %s\n", "Scope", (scope == DSM_GLOBAL) ? "GLOBAL" : "LOCAL");
    fprintf(fp, "%-26s %lu\n", "Read Faults", s.read_faults);
    fprintf(fp, "%-26s %lu\n", "Write Faults", s.write_faults);
    fprintf(fp, "%-26s %lu\n", "Total Faults", total_faults);
    fprintf(fp, "%-26s %.2f us\n", "Avg Fault Latency", avg_latency_us);
    fprintf(fp, "%-26s %lu us\n", "Total Fault Time", s.fault_time_total_us);
    fprintf(fp, "%-26s %lu\n", "Pages Sent", s.pages_sent);
    fprintf(fp, "%-26s %lu\n", "Pages Received", s.pages_received);
    fprintf(fp, "%-26s %lu\n", "Invalidations Sent", s.invalidations_sent);
    fprintf(fp, "%-26s %lu\n", "Invalidations Received", s.invalidations_received);
    fprintf(fp, "%-26s %lu\n", "Bytes Sent", s.bytes_sent);
    fprintf(fp, "%-26s %lu\n", "Bytes Received", s.bytes_received);
    fprintf(fp, "%-26s %lu\n", "Messages Sent", s.messages_sent);
    fprintf(fp, "%-26s %u\n", "Pages Owned", s.pages_owned);
    fprintf(fp, "%-26s %u\n", "Pages Allocated", s.pages_allocated);
    fprintf(fp, "%-26s %u\n", "Pages Cached", s.pages_cached);
    fprintf(fp, "\n");
}

static void ctl_cmd_pages(FILE *fp, const char *arg) {
    int global = 0;
    if (arg && strcasecmp(arg, "global") == 0) global = 1;

    uint32_t start_p = 0;
    uint32_t end_p = G.max_pages;

    if (!global) {
        start_p = G.slice_first;
        end_p = G.slice_first + G.slice_num;
    }

    fprintf(fp, "%-10s %-18s %-6s %-6s %-6s %-8s %-12s\n",
            "PAGE_ID", "ADDRESS", "STATE", "HOME", "OWNER", "VERSION", "ALLOCATED");
    fprintf(fp, "----------------------------------------------------------------------------\n");

    for (uint32_t p = start_p; p < end_p; p++) {
        int home = home_of(p);
        if (home < 0) continue;

        int st = G.pages[p].state;
        uint32_t ver = G.pages[p].version;
        int owner_id = -1;
        int is_alloc = 0;

        if (home == G.me && G.dir) {
            sem_wait(dlock(p));
            dir_entry_t *d = &G.dir[p];
            owner_id = (d->owner == 0xFFFF) ? -1 : (int)d->owner;
            if (d->flags & DF_ALLOCATED) is_alloc = 1;
            ver = d->version;
            sem_post(dlock(p));
        } else {
            if (st == PG_M) owner_id = G.me;
        }

        /* Si es local y la página no está asignada ni en cache, omitir vacías para no saturar si hay muchas */
        if (!global && !is_alloc && st == PG_I) continue;

        char owner_str[16];
        if (owner_id >= 0) snprintf(owner_str, sizeof owner_str, "%d", owner_id);
        else snprintf(owner_str, sizeof owner_str, "-");

        fprintf(fp, "%-10u %-18p %-6s %-6d %-6s %-8u %-12s\n",
                p, (void *)app_addr(p), page_state_name(st), home, owner_str, ver,
                is_alloc ? "YES" : "NO");
    }
    fprintf(fp, "\n");
}

static void ctl_cmd_procs(FILE *fp) {
    fprintf(fp, "%-8s %-10s %-12s %-12s %-12s\n",
            "NODE", "PID", "FIRST_PAGE", "PAGES_ALLOC", "MEM_BYTES");
    fprintf(fp, "------------------------------------------------------------\n");

    pthread_mutex_lock(&G.nodes_mx);
    for (int i = 0; i < G.n_nodes; i++) {
        node_entry_t *n = &G.nodes[i];
        if (n->state == N_UNUSED) continue;
        uint32_t allocated = n->num_pages - n->free_pages;
        size_t bytes = (size_t)allocated * G.cfg.page_size;
        fprintf(fp, "%-8u %-10u %-12u %-12u %-12zu\n",
                n->node_id, (unsigned)getpid(), n->first_page, allocated, bytes);
    }
    pthread_mutex_unlock(&G.nodes_mx);
    fprintf(fp, "\n");
}

static void handle_client(int cfd) {
    int one = 1;
    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    FILE *rfp = fdopen(dup(cfd), "r");
    FILE *wfp = fdopen(cfd, "w");
    if (!rfp || !wfp) {
        if (rfp) fclose(rfp);
        if (wfp) fclose(wfp);
        else close(cfd);
        return;
    }

    char line[256];
    while (G.ctl_running && fgets(line, sizeof line, rfp)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        char *end = p + strlen(p) - 1;
        while (end >= p && isspace((unsigned char)*end)) *end-- = '\0';
        if (!*p) continue;

        char cmd[64] = {0}, arg[64] = {0};
        sscanf(p, "%63s %63s", cmd, arg);

        if (strcasecmp(cmd, "nodes") == 0) {
            ctl_cmd_nodes(wfp);
        } else if (strcasecmp(cmd, "stats") == 0) {
            ctl_cmd_stats(wfp, arg);
        } else if (strcasecmp(cmd, "pages") == 0) {
            ctl_cmd_pages(wfp, arg);
        } else if (strcasecmp(cmd, "procs") == 0) {
            ctl_cmd_procs(wfp);
        } else if (strcasecmp(cmd, "quit") == 0 || strcasecmp(cmd, "exit") == 0) {
            break;
        } else {
            fprintf(wfp, "ERROR: comando desconocido '%s' (comandos: nodes, stats [local|global], pages [global], procs, quit)\n\n", cmd);
        }
        fflush(wfp);
    }

    fclose(rfp);
    fclose(wfp);
}

static void *ctl_main(void *arg) {
    (void)arg;
    while (G.ctl_running) {
        struct pollfd pfd = { G.ctl_listen_fd, POLLIN, 0 };
        int r = poll(&pfd, 1, 300);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r > 0 && (pfd.revents & POLLIN)) {
            int cfd = accept(G.ctl_listen_fd, NULL, NULL);
            if (cfd >= 0) {
                handle_client(cfd);
            }
        }
    }
    return NULL;
}

int ctl_start(void) {
    uint16_t port = G.cfg.port_base + 4 * G.slot + 2;
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    if (fd < 0) return DSM_E_NET;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(fd, 16) != 0) {
        close(fd);
        LOG(LOG_ERROR, "no se pudo abrir CTL_PORT %u", port);
        return DSM_E_NET;
    }

    G.ctl_listen_fd = fd;
    G.ctl_running = 1;
    if (pthread_create(&G.ctl_th, NULL, ctl_main, NULL)) {
        close(fd);
        G.ctl_running = 0;
        return DSM_E_NET;
    }
    LOG(LOG_INFO, "hilo de control escuchando en puerto %u", port);
    return DSM_OK;
}

void ctl_stop(void) {
    if (!G.ctl_running) return;
    G.ctl_running = 0;
    pthread_join(G.ctl_th, NULL);
    if (G.ctl_listen_fd >= 0) {
        close(G.ctl_listen_fd);
        G.ctl_listen_fd = -1;
    }
}
