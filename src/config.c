/* config.c - lectura y validación de dsm.conf (formato CLAVE=valor). */
#include "internal.h"
#include <ctype.h>
#include <limits.h>

typedef enum { T_U32, T_U16, T_U8, T_HEX64, T_STR, T_NODE } ktype;
typedef struct { const char *name; ktype t; void *dst; uint64_t lo, hi; } key_t_;

static uint64_t fnv1a(uint64_t h, const char *s) {
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 1099511628211ULL; }
    return h;
}

static int fail(const char *path, int line, const char *msg) {
    fprintf(stderr, "[dsm] %s:%d: %s\n", path, line, msg);
    return DSM_E_CONFIG;
}

int config_load(const char *path, dsm_config_t *c) {
    memset(c, 0, sizeof *c);
    c->page_size = 4096; c->node_mem_mb = 64; c->max_global_mb = 1024; c->max_nodes = 64;
    c->base_addr = 0x600000000000ULL; c->port_base = 50100; c->port_range = 100;
    c->slots_per_host = 8; c->cache_max_pages = 16384; c->service_threads = 4;
    c->hb_interval_ms = 1000; c->suspect_after_ms = 3000; c->hb_timeout_ms = 10000;
    c->join_timeout_ms = 15000; c->request_timeout_ms = 2000; c->max_retries = 5;
    strcpy(c->log_level, "info");

    key_t_ keys[] = {
        {"PAGE_SIZE", T_U32, &c->page_size, 4096, 65536},
        {"NODE_MEM_MB", T_U32, &c->node_mem_mb, 1, 4096},
        {"MAX_GLOBAL_MB", T_U32, &c->max_global_mb, 1, 65536},
        {"MAX_NODES", T_U16, &c->max_nodes, 2, 64},
        {"GLOBAL_BASE_ADDR", T_HEX64, &c->base_addr, 0, 0x7F0000000000ULL},
        {"NODE", T_NODE, NULL, 0, 0},
        {"PORT_BASE", T_U16, &c->port_base, 1024, 65000},
        {"PORT_RANGE", T_U16, &c->port_range, 4, 65535},
        {"SLOTS_PER_HOST", T_U16, &c->slots_per_host, 1, 25},
        {"CACHE_MAX_PAGES", T_U32, &c->cache_max_pages, 16, 1048576},
        {"SERVICE_THREADS", T_U16, &c->service_threads, 2, 32},
        {"HEARTBEAT_INTERVAL_MS", T_U32, &c->hb_interval_ms, 100, 10000},
        {"SUSPECT_AFTER_MS", T_U32, &c->suspect_after_ms, 200, UINT_MAX},
        {"HEARTBEAT_TIMEOUT_MS", T_U32, &c->hb_timeout_ms, 300, UINT_MAX},
        {"JOIN_TIMEOUT_MS", T_U32, &c->join_timeout_ms, 1000, 120000},
        {"REQUEST_TIMEOUT_MS", T_U32, &c->request_timeout_ms, 100, 60000},
        {"MAX_RETRIES", T_U32, &c->max_retries, 1, 20},
        {"HTTP_ENABLED", T_U8, &c->http_enabled, 0, 1},
        {"LOG_LEVEL", T_STR, c->log_level, 0, 0},
    };
    const int nk = sizeof keys / sizeof keys[0];
    int seen[sizeof keys / sizeof keys[0]] = {0};

    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[dsm] no se pudo abrir %s: %s\n", path, strerror(errno)); return DSM_E_CONFIG; }
    char line[256]; int ln = 0, rc = DSM_OK;
    while (fgets(line, sizeof line, f)) {
        ln++;
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq || eq == line) { rc = fail(path, ln, "se esperaba CLAVE=valor"); break; }
        *eq = 0; const char *k = line, *v = eq + 1;
        int i; for (i = 0; i < nk && strcmp(k, keys[i].name); i++) ;
        if (i == nk) { rc = fail(path, ln, "clave desconocida"); break; }
        if (seen[i] && keys[i].t != T_NODE) { rc = fail(path, ln, "clave repetida"); break; }
        seen[i] = 1;
        if (keys[i].t == T_NODE) {
            if (c->n_hosts >= MAX_HOSTS || !v[0] || strlen(v) >= 64) { rc = fail(path, ln, "NODE inválido"); break; }
            strcpy(c->hosts[c->n_hosts++], v); continue;
        }
        if (keys[i].t == T_STR) {
            if (strlen(v) >= 8) { rc = fail(path, ln, "valor demasiado largo"); break; }
            strcpy((char *)keys[i].dst, v); continue;
        }
        char *end; errno = 0;
        unsigned long long x = strtoull(v, &end, keys[i].t == T_HEX64 ? 0 : 10);
        if (errno || end == v || *end || x < keys[i].lo || x > keys[i].hi) { rc = fail(path, ln, "valor fuera de rango"); break; }
        switch (keys[i].t) {
            case T_U32: *(uint32_t *)keys[i].dst = x; break;
            case T_U16: *(uint16_t *)keys[i].dst = x; break;
            case T_U8: *(uint8_t *)keys[i].dst = x; break;
            default: *(uint64_t *)keys[i].dst = x; break;
        }
    }
    fclose(f);
    if (rc) return rc;

    long sys_ps = sysconf(_SC_PAGESIZE);
    if (c->n_hosts < 1) return fail(path, 0, "falta al menos un NODE");
    if (c->page_size & (c->page_size - 1)) return fail(path, 0, "PAGE_SIZE debe ser potencia de 2");
    if (c->page_size % sys_ps) return fail(path, 0, "PAGE_SIZE debe ser múltiplo de la página del sistema");
    if (c->node_mem_mb > c->max_global_mb) return fail(path, 0, "MAX_GLOBAL_MB < NODE_MEM_MB");
    if (c->base_addr % c->page_size) return fail(path, 0, "GLOBAL_BASE_ADDR no alineada a PAGE_SIZE");
    if (c->port_range < 4u * c->slots_per_host) return fail(path, 0, "PORT_RANGE < 4 x SLOTS_PER_HOST");
    if ((uint32_t)c->port_base + c->port_range > 65535) return fail(path, 0, "rango de puertos excede 65535");
    if (c->suspect_after_ms < 2 * c->hb_interval_ms) return fail(path, 0, "SUSPECT_AFTER_MS < 2 x HEARTBEAT_INTERVAL_MS");
    if (c->hb_timeout_ms <= c->suspect_after_ms) return fail(path, 0, "HEARTBEAT_TIMEOUT_MS <= SUSPECT_AFTER_MS");
    if (!strcmp(c->log_level, "error")) c->log_lvl = LOG_ERROR;
    else if (!strcmp(c->log_level, "warn")) c->log_lvl = LOG_WARN;
    else if (!strcmp(c->log_level, "info")) c->log_lvl = LOG_INFO;
    else if (!strcmp(c->log_level, "debug")) c->log_lvl = LOG_DEBUG;
    else return fail(path, 0, "LOG_LEVEL inválido");

    /* config_hash: FNV-1a 64 de las claves marcadas "Sí", ordenadas alfabéticamente. */
    char buf[2048], t[96]; buf[0] = 0;
#define ADD(k, fmt, val) do { snprintf(t, sizeof t, k "=" fmt "\n", val); strcat(buf, t); } while (0)
    ADD("GLOBAL_BASE_ADDR", "%llu", (unsigned long long)c->base_addr);
    ADD("HEARTBEAT_INTERVAL_MS", "%u", c->hb_interval_ms);
    ADD("HEARTBEAT_TIMEOUT_MS", "%u", c->hb_timeout_ms);
    ADD("MAX_GLOBAL_MB", "%u", c->max_global_mb);
    ADD("MAX_NODES", "%u", c->max_nodes);
    for (int i = 0; i < c->n_hosts; i++) ADD("NODE", "%s", c->hosts[i]);
    ADD("PAGE_SIZE", "%u", c->page_size);
    ADD("PORT_BASE", "%u", c->port_base);
    ADD("PORT_RANGE", "%u", c->port_range);
    ADD("SLOTS_PER_HOST", "%u", c->slots_per_host);
    ADD("SUSPECT_AFTER_MS", "%u", c->suspect_after_ms);
    ADD("PROTO", "%d", DSM_PROTO_VERSION);
    c->config_hash = fnv1a(14695981039346656037ULL, buf);
    return DSM_OK;
}
