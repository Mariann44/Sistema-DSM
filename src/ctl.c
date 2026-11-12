/* ctl.c - hilo de control (CTL_PORT).
 * Atiende comandos de texto, uno por línea:
 *   - nodes
 *   - stats [local|global]
 *   - pages [global]
 *   - procs [detail]
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

/* Copia de la tabla de nodos para no mantener nodes_mx mientras se escribe al socket. */
static int nodes_snapshot(node_entry_t *out) {
    pthread_mutex_lock(&G.nodes_mx);
    int n = G.n_nodes;
    memcpy(out, G.nodes, sizeof(node_entry_t) * (size_t)n);
    pthread_mutex_unlock(&G.nodes_mx);
    return n;
}

static int snap_home_of(const node_entry_t *nt, int n, uint32_t page) {
    for (int i = 0; i < n; i++)
        if (nt[i].state != N_UNUSED && page >= nt[i].first_page && page < nt[i].first_page + nt[i].num_pages) return i;
    return -1;
}

static void fmt_alloc_id(char *b, size_t cap, uint64_t id) {
    snprintf(b, cap, "%u:%llu", (unsigned)(id >> 48), (unsigned long long)(id & 0xFFFFFFFFFFFFULL));
}

static int cmp_alloc_by_proc(const void *x, const void *y) {
    const alloc_entry_t *a = x, *b = y;
    if (a->dsm_pid != b->dsm_pid) return a->dsm_pid < b->dsm_pid ? -1 : 1;
    if (a->alloc_id != b->alloc_id) return a->alloc_id < b->alloc_id ? -1 : 1;
    return 0;
}

static void ctl_cmd_nodes(FILE *fp) {
    fprintf(fp, "%-6s %-16s %-8s %-8s %-12s %-12s %-10s\n",
            "NODE", "IP", "STATE", "SLOT", "FIRST_PAGE", "NUM_PAGES", "FREE_PAGES");
    fprintf(fp, "----------------------------------------------------------------------------\n");

    pager_update_self_free();
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

    fprintf(fp, "%-10s %-18s %-6s %-6s %-6s %-8s %-10s %-8s %-8s\n",
            "PAGE_ID", "ADDRESS", "STATE", "HOME", "OWNER", "VERSION", "ALLOC_ID", "PID", "SEQ");
    fprintf(fp, "------------------------------------------------------------------------------------------\n");

    uint8_t *wire = malloc(MAX_PAYLOAD);
    if (!wire) {
        fprintf(fp, "ERROR: %s\n\n", dsm_strerror(DSM_E_NOMEM));
        return;
    }

    int targets[MAX_NODES_HARD], n_targets = 0;
    if (global) {
        pthread_mutex_lock(&G.nodes_mx);
        for (int i = 0; i < G.n_nodes; i++) {
            if (G.nodes[i].state == N_ALIVE || G.nodes[i].state == N_SUSPECT)
                targets[n_targets++] = i;
        }
        pthread_mutex_unlock(&G.nodes_mx);
    } else {
        targets[n_targets++] = G.me;
    }

    for (int t = 0; t < n_targets; t++) {
        int node = targets[t];
        uint32_t len = 0;
        int rc = DSM_OK;
        if (node == G.me) {
            len = pager_collect_pages(wire, MAX_PAYLOAD);
        } else {
            waiter_t *w = waiter_get(W_PAGES, node, 0, wire, MAX_PAYLOAD, 1);
            if (!w) { rc = DSM_E_NOMEM; }
            else {
                rc = send_msg_node(node, MSG_PAGES_REQUEST, 0, w->request_id, 0, 0, NULL, 0, NULL, 0);
                if (rc == DSM_OK) rc = waiter_wait(w, G.cfg.request_timeout_ms);
                if (rc == DSM_OK) rc = w->status;
                if (rc == DSM_OK) len = w->reply_len;
                waiter_put(w);
            }
        }
        if (rc != DSM_OK) {
            fprintf(fp, "ERROR: no se pudo consultar páginas del nodo %d: %s\n", node, dsm_strerror(rc));
            continue;
        }
        if (len % PAGE_INFO_WIRE_LEN != 0) {
            fprintf(fp, "ERROR: respuesta de páginas inválida del nodo %d\n", node);
            continue;
        }
        for (uint32_t off = 0; off < len; off += PAGE_INFO_WIRE_LEN) {
            const uint8_t *in = wire + off;
            uint32_t p = get32(in);
            int st = in[4];
            int home = (int)get16(in + 12);
            int owner_id = get16(in + 6) == NODE_UNKNOWN ? -1 : (int)get16(in + 6);
            uint32_t ver = get32(in + 8);
            char owner_str[16];
            if (owner_id >= 0) snprintf(owner_str, sizeof owner_str, "%d", owner_id);
            else snprintf(owner_str, sizeof owner_str, "-");
            /* Proceso dueño y posición de la página dentro de su asignación (tabla replicada). */
            char aid[24] = "-", pid_str[16] = "-", seq_str[16] = "-";
            alloc_entry_t a;
            if (alloc_table_find(p, &a)) {
                fmt_alloc_id(aid, sizeof aid, a.alloc_id);
                uint16_t an = (uint16_t)(a.dsm_pid >> 16);
                pthread_mutex_lock(&G.nodes_mx);
                uint32_t os_pid = an < MAX_NODES_HARD ? G.nodes[an].os_pid : 0;
                pthread_mutex_unlock(&G.nodes_mx);
                snprintf(pid_str, sizeof pid_str, "%u", os_pid ? os_pid : (a.dsm_pid & 0xFFFF));
                snprintf(seq_str, sizeof seq_str, "%u", p - a.first_page);
            }
            fprintf(fp, "%-10u %-18p %-6s %-6d %-6s %-8u %-10s %-8s %-8s\n",
                    p, (void *)app_addr(p), page_state_name(st), home,
                    owner_str, ver, aid, pid_str, seq_str);
        }
    }
    free(wire);
    fprintf(fp, "\n");
}

/* procs [detail]
 * 1) Un renglón por proceso (cada nodo es un proceso que enlaza libdsm)
 * 2) Las asignaciones de cada proceso en orden de creación, con el rango de seq y el rango de page_id global que le corresponde, partido por home.
 * 3) Con "detail": una fila por página, en orden de seq. */
static void ctl_cmd_procs(FILE *fp, const char *arg) {
    int detail = arg && (strcasecmp(arg, "detail") == 0 || strcasecmp(arg, "detalle") == 0);
    node_entry_t *nt = malloc(sizeof(node_entry_t) * MAX_NODES_HARD);
    alloc_entry_t *al = malloc(sizeof(alloc_entry_t) * MAX_ALLOCS);
    if (!nt || !al) { free(nt); free(al); fprintf(fp, "ERROR: %s\n\n", dsm_strerror(DSM_E_NOMEM)); return; }
    int nn = nodes_snapshot(nt);
    int na = alloc_table_snapshot(al, MAX_ALLOCS);
    qsort(al, (size_t)na, sizeof *al, cmp_alloc_by_proc);

    fprintf(fp, "PROCESOS DEL SISTEMA\n");
    fprintf(fp, "%-6s %-6s %-10s %-12s %-8s %-8s %-8s %-12s\n",
            "NODE", "SLOT", "PID", "DSM_PID", "STATE", "ALLOCS", "PAGES", "BYTES");
    fprintf(fp, "------------------------------------------------------------------------------\n");
    for (int i = 0; i < nn; i++) {
        if (nt[i].state == N_UNUSED) continue;
        uint32_t dpid = make_dsm_pid((uint16_t)i, nt[i].os_pid);
        uint32_t cnt = 0, pages = 0; uint64_t bytes = 0;
        for (int k = 0; k < na; k++) if ((al[k].dsm_pid >> 16) == (uint32_t)i) { cnt++; pages += al[k].num_pages; bytes += al[k].size_bytes; }
        fprintf(fp, "%-6u %-6u %-10u 0x%08x   %-8s %-8u %-8u %-12llu\n",
                nt[i].node_id, nt[i].slot, nt[i].os_pid, dpid, node_state_name(nt[i].state),
                cnt, pages, (unsigned long long)bytes);
    }

    fprintf(fp, "==============================================================================\nASIGNACIONES POR PROCESO (orden de páginas: SEQ -> PAGE_ID)\n");
    fprintf(fp, "%-6s %-10s %-10s %-14s %-20s %-6s %-12s\n",
            "NODE", "PID", "ALLOC_ID", "SEQ", "PAGE_ID", "HOME", "BYTES");
    fprintf(fp, "------------------------------------------------------------------------------\n");
    if (na == 0) fprintf(fp, "(sin asignaciones)\n");
    for (int k = 0; k < na; k++) {
        const alloc_entry_t *a = &al[k];
        uint16_t an = (uint16_t)(a->dsm_pid >> 16);
        uint32_t os_pid = an < nn && nt[an].os_pid ? nt[an].os_pid : (a->dsm_pid & 0xFFFF);
        char aid[24]; fmt_alloc_id(aid, sizeof aid, a->alloc_id);
        if (a->first_page == 0) strncat(aid, " raiz", sizeof aid - strlen(aid) - 1);
        /* Una fila por tramo de la asignación que vive en el mismo home (siempre un tramo;
         * con dsm_alloc distribuido una asignación podrá abarcar varios slices). */
        uint32_t end = a->first_page + a->num_pages, p = a->first_page; int first_row = 1;
        while (p < end) {
            int h = snap_home_of(nt, nn, p);
            uint32_t seg_end = end;
            if (h >= 0 && nt[h].first_page + nt[h].num_pages < seg_end) seg_end = nt[h].first_page + nt[h].num_pages;
            if (h < 0) seg_end = p + 1;
            char seq[32], pr[32], hs[16];
            snprintf(seq, sizeof seq, "%u..%u", p - a->first_page, seg_end - 1 - a->first_page);
            snprintf(pr, sizeof pr, "%u..%u", p, seg_end - 1);
            if (h >= 0) snprintf(hs, sizeof hs, "%d", h); else snprintf(hs, sizeof hs, "?");
            if (first_row)
                fprintf(fp, "%-6u %-10u %-10s %-14s %-20s %-6s %-12llu\n",
                        an, os_pid, aid, seq, pr, hs, (unsigned long long)a->size_bytes);
            else
                fprintf(fp, "%-6s %-10s %-10s %-14s %-20s %-6s\n", "", "", "", seq, pr, hs);
            first_row = 0; p = seg_end;
        }
    }

    if (detail) {
        fprintf(fp, "==============================================================================\nDETALLE POR PÁGINA\n");
        fprintf(fp, "%-6s %-10s %-10s %-8s %-10s %-18s %-6s\n",
                "NODE", "PID", "ALLOC_ID", "SEQ", "PAGE_ID", "ADDRESS", "HOME");
        fprintf(fp, "------------------------------------------------------------------------------\n");
        for (int k = 0; k < na; k++) {
            const alloc_entry_t *a = &al[k];
            uint16_t an = (uint16_t)(a->dsm_pid >> 16);
            uint32_t os_pid = an < nn && nt[an].os_pid ? nt[an].os_pid : (a->dsm_pid & 0xFFFF);
            char aid[24]; fmt_alloc_id(aid, sizeof aid, a->alloc_id);
            for (uint32_t s = 0; s < a->num_pages; s++) {
                uint32_t p = a->first_page + s;
                fprintf(fp, "%-6u %-10u %-10s %-8u %-10u %-18p %-6d\n",
                        an, os_pid, aid, s, p, (void *)app_addr(p), snap_home_of(nt, nn, p));
            }
        }
    }
    free(nt); free(al);
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
            ctl_cmd_procs(wfp, arg);
        } else if (strcasecmp(cmd, "quit") == 0 || strcasecmp(cmd, "exit") == 0) {
            break;
        } else {
            fprintf(wfp, "ERROR: comando desconocido '%s' (comandos: nodes, stats [local|global], pages [global], procs [detail], quit)\n\n", cmd);
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
