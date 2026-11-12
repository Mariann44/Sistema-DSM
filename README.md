# DSM - base del proyecto (IC-6600)

Base de `libdsm`: `mmap`+`memfd` con doble vista, `SIGSEGV` + `mprotect`,
MSI con directorio en el home, protocolo binario (header de 24 bytes) sobre TCP, unión de nodos
con `config_hash`, hilo de red con `poll` + hilos de servicio.

## Uso
    make                      # libdsm.a + apps/hello_dsm
    make test                 # 3 nodos locales: coherencia, fases separadas con dsm_barrier
    make test-hb              # heartbeat: pausa un nodo y verifica SUSPECT, recuperación y DOWN (~35 s)
    make test-lock            # 3 nodos x 300 incrementos de un contador compartido con dsm_lock
    ./apps/hello_dsm conf/dsm_local.conf 3     # una terminal por nodo (el primero es el fundador)
    ./apps/idle_dsm conf/dsm_local.conf 60     # nodo que solo se queda vivo 60 s (para probar el heartbeat)
    ./apps/counter conf/dsm_local.conf 3 1000          # contador con lock (agregar "sinlock" para ver la pérdida)
    ./tools/dsmctl 127.0.0.1 0 stats global            # consultar estadísticas o nodos desde terminal
    ./tools/dsmctl 127.0.0.1 0 pages global            # consultar páginas asignadas en todos los nodos

## Mapa de módulos
| Archivo | Contenido |
|---|---|
| `include/dsm.h` | API pública |
| `src/protocol.h` | encabezado, tipos de mensaje, flags, put/get en orden de red |
| `src/config.c` | parseo, validación de rangos, `config_hash` (FNV-1a) |
| `src/net.c` | listener (`poll`+eventfd), cola y pool de servicio, envío, waiters |
| `src/membership.c` | slot/puertos, JOIN_REQUEST/ACK, NODE_JOINED, `home_of()` |
| `src/heartbeat.c` | latidos por `HB_PORT`, estados ALIVE/SUSPECT/DOWN |
| `src/sync.c` | locks (LOCK_ACQUIRE/GRANT/RELEASE) y barreras (BARRIER_ENTER/RELEASE) |
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
- Locks: `dsm_lock`/`dsm_unlock` sobre un `dsm_lock_t` guardado en memoria DSM. El administrador
  es el home de la página donde está el lock; guarda el dueño y una cola FIFO (hasta 64 en espera)
  y nunca espera a otro nodo. Si el dueño cae, el lock pasa al siguiente y `dsm_lock` le retorna
  `DSM_E_NODE_DOWN` (tiene el lock, pero los datos pueden haber quedado a medias).
- Barreras: `dsm_barrier(b, count)` con el mismo esquema de administrador. La primera llegada fija
  `count` (con 0, el número de nodos vivos en ese momento); una llegada con otro `count` recibe
  `DSM_E_MISMATCH`. Se pueden reutilizar (cada liberación sube la generación). Si un nodo cae con
  llegadas pendientes, todos salen con `DSM_E_NODE_DOWN`. Las pruebas usan barreras en vez de banderas.

## Pendiente (siguiente trabajo)
1. `dsm_alloc` distribuido (ALLOC_REQUEST/REPLY/COMMIT/ABORT) y `dsm_free`.
2. PAGE_WRITEBACK en `dsm_finalize` y reemplazo por `CACHE_MAX_PAGES` (las páginas M remotas se pierden al salir).
3. `dsmctl` / puerto de control, estadísticas globales, HTTP.
4. Uniones concurrentes en contactos distintos pueden asignar el mismo rango (hoy: arrancar nodos de a uno).
5. El listener lee mensajes con `recv` bloqueante; un par lento podría retrasarlo (pasar a buffers por conexión).
6. Decodificación de lectura/escritura del fallo solo en x86-64; en otras arquitecturas lee y luego escala.
