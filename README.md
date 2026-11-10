# DSM - base del proyecto (IC-6600)

Base de `libdsm`: `mmap`+`memfd` con doble vista, `SIGSEGV` + `mprotect`,
MSI con directorio en el home, protocolo binario (header de 24 bytes) sobre TCP, unión de nodos
con `config_hash`, hilo de red con `poll` + hilos de servicio.

## Uso
    make                      # libdsm.a + apps/hello_dsm
    make test                 # 3 nodos locales, uniones secuenciales
    ./apps/hello_dsm conf/dsm_local.conf 3     # una terminal por nodo (el primero es el fundador)

## Mapa de módulos
| Archivo | Contenido |
|---|---|
| `include/dsm.h` | API pública |
| `src/protocol.h` | encabezado, tipos de mensaje, flags, put/get en orden de red |
| `src/config.c` | parseo, validación de rangos, `config_hash` (FNV-1a) |
| `src/net.c` | listener (`poll`+eventfd), cola y pool de servicio, envío, waiters |
| `src/membership.c` | slot/puertos, JOIN_REQUEST/ACK, NODE_JOINED, `home_of()` |
| `src/pager.c` | memfd + 2 vistas, handler SIGSEGV, MSI, directorio, PAGE_*/INVALIDATE |
| `src/dsm.c` | `dsm_init/finalize/alloc/prefetch/get_stats` |

## Implementado
- Fallos de lectura/escritura entre nodos (S, M, I, PR, PW), invalidación, fetch al dueño en M.
- Cada nodo aporta su slice; el home sirve las páginas de su slice; la página 0 es la raíz.
- `dsm_alloc` dentro del propio slice (publicar el puntero en `dsm_root()`).

## Pendiente (siguiente trabajo)
1. `sync.c`: `dsm_lock` / `dsm_barrier` (hoy devuelven `DSM_E_NOT_IMPL`; la prueba usa flags en la raíz).
2. Heartbeat y detección SUSPECT/DOWN (hoy solo se detecta el cierre del socket).
3. `dsm_alloc` distribuido (ALLOC_REQUEST/REPLY/COMMIT/ABORT) y `dsm_free`.
4. PAGE_WRITEBACK en `dsm_finalize` y reemplazo por `CACHE_MAX_PAGES` (las páginas M remotas se pierden al salir).
5. `dsmctl` / puerto de control, estadísticas globales, HTTP.
6. Uniones concurrentes en contactos distintos pueden asignar el mismo rango (hoy: arrancar nodos de a uno).
7. El listener lee mensajes con `recv` bloqueante; un par lento podría retrasarlo (pasar a buffers por conexión).
8. Decodificación de lectura/escritura del fallo solo en x86-64; en otras arquitecturas lee y luego escala.
