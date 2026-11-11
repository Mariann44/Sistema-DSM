# DSM - base del proyecto (IC-6600)

Base de `libdsm`: `mmap`+`memfd` con doble vista, `SIGSEGV` + `mprotect`,
MSI con directorio en el home, protocolo binario (header de 24 bytes) sobre TCP, unión de nodos
con `config_hash`, hilo de red con `poll` + hilos de servicio.

## Uso
    make                      # libdsm.a + apps/hello_dsm
    make test                 # 3 nodos locales, uniones secuenciales (hello_dsm admite hasta 4 nodos)
    make test-hb              # heartbeat: pausa un nodo y verifica SUSPECT, recuperación y DOWN (~35 s)
    ./apps/hello_dsm conf/dsm_local.conf 3     # una terminal por nodo (el primero es el fundador)
    ./apps/idle_dsm conf/dsm_local.conf 60     # nodo que solo se queda vivo 60 s (para probar el heartbeat)

## Mapa de módulos
| Archivo | Contenido |
|---|---|
| `include/dsm.h` | API pública |
| `src/protocol.h` | encabezado, tipos de mensaje, flags, put/get en orden de red |
| `src/config.c` | parseo, validación de rangos, `config_hash` (FNV-1a) |
| `src/net.c` | listener (`poll`+eventfd), cola y pool de servicio, envío, waiters |
| `src/membership.c` | slot/puertos, JOIN_REQUEST/ACK, NODE_JOINED, `home_of()` |
| `src/heartbeat.c` | latidos por `HB_PORT`, estados ALIVE/SUSPECT/DOWN |
| `src/pager.c` | memfd + 2 vistas, handler SIGSEGV, MSI, directorio, PAGE_*/INVALIDATE |
| `src/dsm.c` | `dsm_init/finalize/alloc/prefetch/get_stats` |

## Implementado
- Fallos de lectura/escritura entre nodos (S, M, I, PR, PW), invalidación, fetch al dueño en M.
- Cada nodo aporta su slice; el home sirve las páginas de su slice; la página 0 es la raíz.
- `dsm_alloc` dentro del propio slice (publicar el puntero en `dsm_root()`).
- Heartbeat: cada nodo envía un HEARTBEAT por segundo al `HB_PORT` de cada vecino, en una
  conexión aparte de la de datos y con su propio hilo. Sin latidos por `SUSPECT_AFTER_MS` el vecino
  pasa a SUSPECT (y vuelve a ALIVE si late de nuevo); por `HEARTBEAT_TIMEOUT_MS`, a DOWN de forma
  definitiva y las esperas sobre él terminan con `DSM_E_NODE_DOWN`. Un nodo declarado DOWN deja de
  recibir latidos, así que desde su lado también marca a los demás como DOWN.

## Pendiente (siguiente trabajo)
1. `sync.c`: `dsm_lock` / `dsm_barrier` (hoy devuelven `DSM_E_NOT_IMPL`; la prueba usa flags en la raíz).
2. `dsm_alloc` distribuido (ALLOC_REQUEST/REPLY/COMMIT/ABORT) y `dsm_free`.
3. PAGE_WRITEBACK en `dsm_finalize` y reemplazo por `CACHE_MAX_PAGES` (las páginas M remotas se pierden al salir).
4. `dsmctl` / puerto de control, estadísticas globales, HTTP.
5. Uniones concurrentes en contactos distintos pueden asignar el mismo rango (hoy: arrancar nodos de a uno).
6. El listener lee mensajes con `recv` bloqueante; un par lento podría retrasarlo (pasar a buffers por conexión).
7. Decodificación de lectura/escritura del fallo solo en x86-64; en otras arquitecturas lee y luego escala.
