# Sistema DSM — Memoria Compartida Distribuida (IC-6600)

`libdsm` es una biblioteca en C para Linux que emula un espacio de direcciones compartido sobre
varios procesos o computadoras. Cada proceso que la enlaza es un nodo del clúster:

- Cada nodo aporta un bloque de memoria (`memfd` + `mmap` con doble vista).
- Las páginas que el nodo no tiene se protegen con `mprotect`. Un manejador de `SIGSEGV` las trae
  por TCP de forma transparente.
- La coherencia usa el protocolo MSI (más dos estados transitorios) con un directorio en el nodo
  home de cada página.

Un puntero vale lo mismo y apunta al mismo contenido en todos los nodos.

Proyecto del curso IC-6600 Principios de Sistemas Operativos, TEC, II semestre 2026.

## Compilar y probar

    make                 # libdsm.a, aplicaciones, pruebas y dsmctl
    make test            # coherencia: 3 nodos locales, fases separadas con dsm_barrier
    make test-lock       # 3 nodos x 300 incrementos de un contador compartido con dsm_lock
    make test-hb         # heartbeat: pausa un nodo y verifica SUSPECT, recuperación y DOWN (~35 s)
    make test-alloc      # asignación que cruza slices, choque de reservas y DSM_E_NOMEM
    make test-cache      # reemplazo con una caché de 64 páginas y writeback al salir
    make test-gemm       # aplicación 1: multiplicación de matrices, comparada con PolyBench serial
    make test-sobel      # aplicación 2: filtro Sobel sobre video, comparado con la versión serial
    make clean

Requisitos: Linux x86-64, gcc, make y pthreads. `ffmpeg` solo hace falta para la aplicación de
video.

## Ejecución

Cada nodo es un proceso: se abre una terminal por nodo (o una por máquina). El primer nodo que
arranca en la primera máquina de la lista `NODE` funda el clúster y los demás se unen a él. Todos
los programas reciben el archivo de configuración y la cantidad de nodos que deben participar.

    ./apps/hello_dsm conf/dsm_local.conf 3            # coherencia; repetir en 3 terminales
    ./apps/counter   conf/dsm_local.conf 3 1000       # contador con lock ("sinlock" para ver la pérdida)
    ./apps/idle_dsm  conf/dsm_local.conf 60 64,32     # nodo que solo se mantiene vivo, con asignaciones
    ./tools/dsmctl 127.0.0.1 0 stats global           # consultas al nodo del slot 0

### Configuración

| Archivo | Uso |
|---|---|
| `conf/dsm_local.conf` | Pruebas en una sola máquina: 16 MB por nodo, 256 MB de espacio global |
| `conf/dsm_gemm.conf` | Escenarios grandes de las dos aplicaciones: 256 MB por nodo, 4 GB de espacio global |
| `conf/dsm_cache.conf` | Igual que `dsm_local.conf`, con `CACHE_MAX_PAGES=64` para forzar el reemplazo |
| `conf/dsm_lab.conf` | Plantilla para varias computadoras: 256 MB por nodo, una línea `NODE` por máquina |

Las claves y sus rangos están documentados en el Entregable 1.

### Varias computadoras

1. Copiar el repositorio y compilar con `make` en cada máquina.
2. Editar `conf/dsm_lab.conf`: una línea `NODE=<ip>` por máquina, en el mismo orden en todas. La
   primera es la fundadora. Todas las máquinas deben usar exactamente el mismo archivo, porque al
   unirse se compara su `config_hash`.
3. Abrir en el firewall los puertos TCP `PORT_BASE` a `PORT_BASE + 4 * SLOTS_PER_HOST - 1`
   (27100 a 27131 con los valores de la plantilla).
4. Arrancar primero el nodo de la máquina fundadora y después los demás, de a uno. En cada máquina
   puede haber varios nodos (cada uno toma el siguiente slot libre):

       ./apps/gemm/gemm_dsm conf/dsm_lab.conf 4 -t 2 -n 2048 2048 2048     # en cada nodo
       ./tools/dsmctl -c conf/dsm_lab.conf 192.168.0.101 0 nodes            # desde cualquier máquina

### Dos máquinas simuladas en un solo Linux

Para probar el tráfico por una interfaz de red real sin tener dos computadoras, `tests/netns_setup.sh`
crea dos espacios de nombres de red (`hostA` = 10.10.0.1 y `hostB` = 10.10.0.2) unidos por un par
veth, y `tests/run_twohost.sh` reparte los nodos entre los dos y corre hello, counter, alloc_test,
cache_test, gemm y sobel. Necesita root:

    sudo tests/netns_setup.sh          # opcional: sudo tests/netns_setup.sh 2ms  (agrega latencia)
    sudo tests/run_twohost.sh
    sudo tests/netns_setup.sh borrar

### Consultas (`dsmctl`)

    ./tools/dsmctl [-c dsm.conf] <ip> <slot> <comando> [argumento]

El puerto de control es `PORT_BASE + 4 * slot + 2`. `PORT_BASE` se toma del archivo indicado con
`-c` (o de la variable `DSM_CONF`); si no se indica, de `conf/dsm_local.conf` o `dsm.conf` del
directorio actual.

| Comando | Muestra |
|---|---|
| `nodes` | Tabla de nodos: id, IP, estado, slot, rango de páginas, páginas libres |
| `stats [local\|global]` | Fallos, latencia promedio, páginas enviadas/recibidas, invalidaciones, bytes, mensajes |
| `pages [global]` | Tabla de páginas: estado MSI, home, owner, versión, asignación, PID y orden (seq) |
| `procs [detail]` | Procesos, sus asignaciones en orden y, con `detail`, una fila por página |

## Aplicaciones de validación

Las dos aplicaciones son implementaciones existentes adaptadas al API del DSM. Cada una tiene una
versión serial de referencia y una versión DSM. Las dos versiones deben dar exactamente el mismo
resultado.

### Aplicación 1: multiplicación de matrices (`apps/gemm/`)

Kernel `gemm` de PolyBench/C 4.2.1 (C := α·A·B + β·C). `gemm_serial.c` es el original sin cambios
y `gemm_dsm.c` es la adaptación; su encabezado explica cada cambio. Licencia en
`apps/gemm/LICENSE.txt`.

- El nodo 0 reserva A, B y C con `dsm_alloc` y publica los punteros en `dsm_root()`.
- Las filas de C se reparten entre los nodos y, dentro de cada nodo, entre pthreads. Las filas se
  rellenan a página para evitar falso compartimiento.
- El nodo 0 verifica una muestra (o todos los elementos con `-s 0`) recalculándolos desde A y B.

Uso:

    ./apps/gemm/gemm_dsm <conf> <nodos> [-t hilos] [-n ni nj nk] [-d volcado] [-s muestras]
    ./apps/gemm/gemm_dsm conf/dsm_gemm.conf 3 -t 2 -n 4096 4096 4096   # 384 MB en memoria DSM

### Aplicación 2: filtro Sobel sobre video (`apps/sobel/`)

Detector de bordes Sobel de DanyEle/Sobel_Filter (`Native_Sobel`, Daniele Gadler, con partes de
petermlm/SobelFilter), aplicado a todos los cuadros de un video.

- `image_operations.c` y `file_operations.c` son los originales sin cambios.
- `main.c` es el programa original, que procesa una imagen. Se conserva como referencia y no se
  compila.
- `sobel_serial.c` es la versión serial de referencia y `sobel_dsm.c` la adaptación.

Funcionamiento:

- El nodo 0 carga el video completo (RGB crudo) en memoria DSM. Lee con `fread` a un búfer local y
  copia con `memcpy`, porque `fread` directo sobre una página ausente da `EFAULT`.
- Los cuadros se reparten entre los nodos y, dentro de cada nodo, entre pthreads. Cada cuadro de
  entrada y de salida empieza en una página nueva.
- El nodo 0 recalcula algunos cuadros (o todos con `-v 0`) y calcula un checksum FNV-1a de la
  salida, el mismo que imprime `sobel_serial`.

Uso:

    tools/video_to_rgb.sh                          # data/big_buck_bunny_720p_stereo.ogg -> data/video_1280x720.rgb (5 s, 120 cuadros, 316 MB)
    ./apps/sobel/sobel_dsm conf/dsm_gemm.conf 3 data/video_1280x720.rgb 1280 720 -o data/sobel.gray
    ./apps/sobel/sobel_serial data/video_1280x720.rgb 1280 720 data/sobel_serial.gray
    tools/gray_to_video.sh data/sobel.gray 1280 720   # -> data/sobel.mp4 para verlo

El video de entrada va descomprimido en `data/big_buck_bunny_720p_stereo.ogg` (de https://download.blender.org/peach/bigbuckbunny_movies/, archivo `big_buck_bunny_720p_stereo.ogg.zip`). Si no está, el script intenta descargarlo. También acepta un video propio como primer argumento; con 10 s en 720p se necesitan 4 nodos con `conf/dsm_gemm.conf`. Big Buck Bunny es
© Blender Foundation (peach.blender.org), con licencia CC BY 3.0. La carpeta `data/` no se sube al
repositorio.

### Resultados de referencia

Medidos con 3 nodos en una sola máquina virtual de 2 núcleos, con 2 hilos por nodo:

| Prueba | Memoria DSM | Tiempo | Resultado |
|---|---|---|---|
| `make test-gemm` (200×220×240) | 2,5 MB | 0,015 s | 44 000/44 000 elementos correctos, volcado idéntico a PolyBench |
| gemm 4096³ | 384 MB en varios slices | 26,5 s (5,2 GFLOP/s) | PASS, 131 071 páginas transferidas |
| `make test-sobel` (320×240, 48 cuadros) | 14 MB | 0,09 s | 48/48 cuadros iguales, salida idéntica a la serial |
| Sobel sobre Big Buck Bunny 1280×720, 120 cuadros | 422 MB en varios slices | 2,8 s (42,2 cuadros/s) | 120/120 cuadros iguales, salida idéntica byte a byte a `sobel_serial` |

## Mapa de módulos

| Archivo | Contenido |
|---|---|
| `include/dsm.h` | API pública: funciones, tipos y códigos de error |
| `src/protocol.h` | Encabezado de 24 bytes, tipos de mensaje, flags, serialización en orden de red |
| `src/internal.h` | Estado global `dsm_ctx_t`, tablas internas y prototipos entre módulos |
| `src/config.c` | Parseo de `dsm.conf`, validación de rangos, `config_hash` (FNV-1a) |
| `src/net.c` | Listener (`poll` + eventfd), cola y pool de hilos de servicio, envío, waiters |
| `src/membership.c` | Slot y puertos, JOIN_REQUEST/ACK, NODE_JOINED, `home_of()` |
| `src/heartbeat.c` | Latidos por `HB_PORT`, estados ALIVE/SUSPECT/DOWN |
| `src/sync.c` | Locks (LOCK_ACQUIRE/GRANT/RELEASE) y barreras (BARRIER_ENTER/RELEASE) |
| `src/pager.c` | memfd + 2 vistas, manejador de SIGSEGV, MSI, directorio, PAGE_* e INVALIDATE, caché y PAGE_WRITEBACK |
| `src/alloc.c` | `dsm_alloc` distribuida en dos fases (ALLOC_REQUEST/REPLY/COMMIT/ABORT) |
| `src/ctl.c` | Puerto de control y formato de las consultas |
| `src/dsm.c` | `dsm_init/finalize/alloc/prefetch/get_stats` y demás funciones del API |
| `tools/dsmctl.c` | Cliente de consultas por terminal |
| `tools/*.sh` | Conversión de video a RGB crudo y de la salida de Sobel a mp4 |
| `apps/` | Aplicaciones de prueba (`hello_dsm`, `counter`, `idle_dsm`, `alloc_test`) y de validación (`gemm/`, `sobel/`) |
| `tests/` | Guiones de las pruebas automáticas (`make test-*`) |

## Implementado

- **Fallos de página entre nodos:** lectura y escritura con estados S, M, I, PR y PW, invalidación
  de copias y fetch al dueño en M. Cada nodo aporta su slice y la página 0 es la raíz.
- **`dsm_alloc` distribuida:** el rango de páginas siempre es contiguo (un solo puntero) y puede
  cruzar los slices de varios nodos.
  - Se prefiere el rango con más páginas en el slice propio.
  - Si no cabe ahí, se reserva en cada home involucrado (ALLOC_REQUEST, en orden ascendente) y se
    confirma con ALLOC_COMMIT a todos.
  - Si un home rechaza porque otro nodo reservó antes, se envía ALLOC_ABORT y se reintenta en otro
    rango.
  - Si no hay memoria contigua libre en el clúster, retorna `DSM_E_NOMEM`.
  - Una aplicación debe esperar a que todos los nodos se unan (con una barrera) antes de hacer
    asignaciones grandes: antes de eso sus slices no existen.
- **Caché de páginas remotas (`CACHE_MAX_PAGES`):** las páginas que llegan de otros nodos entran
  a una cola FIFO. Cuando pasan del límite, se expulsan las más antiguas:
  - una copia S se descarta sin usar la red;
  - una página en M se devuelve a su home con PAGE_WRITEBACK y el home vuelve a ser su dueño.
- **`dsm_free`:** solo el nodo que hizo la asignación puede liberarla, y el puntero debe ser su
  inicio (si no, `DSM_E_PERM` o `DSM_E_INVAL`). Cada home involucrado recibe FREE_REQUEST, revoca
  las copias de sus páginas, las pone en cero y las vuelve a contar como libres. FREE_COMMIT quita
  la asignación de la tabla de todos los nodos. Cuando un nodo sale, cada home recupera de la
  misma forma las páginas de las asignaciones de ese nodo.
- **Writeback al salir:** `dsm_finalize` devuelve a su home todas las páginas remotas en M antes de
  avisar NODE_LEAVE, así que lo escrito por un nodo no se pierde cuando sale.
- **Heartbeat:** cada nodo envía un HEARTBEAT por segundo al `HB_PORT` de cada vecino, por una
  conexión aparte y con su propio hilo.
  - Sin latidos por `SUSPECT_AFTER_MS`, el vecino pasa a SUSPECT, y vuelve a ALIVE si late de nuevo.
  - Sin latidos por `HEARTBEAT_TIMEOUT_MS`, pasa a DOWN de forma definitiva y las esperas sobre él
    terminan con `DSM_E_NODE_DOWN`.
- **Locks:** el administrador de cada lock es el home de la página donde está guardado. Mantiene
  una cola FIFO de hasta 64 solicitudes y nunca espera a otro nodo.
- **Barreras:** reutilizables. Con `count` = 0 esperan a todos los nodos vivos. Una llegada con
  otro `count` recibe `DSM_E_MISMATCH`, y si un nodo cae, todos salen con `DSM_E_NODE_DOWN`.
- **Consultas:** puerto de control con `dsmctl` y estadísticas locales y globales.
- **Aplicaciones:** dos aplicaciones reales adaptadas al API, con worker threads, verificación y
  escenarios de cientos de MB.

## Pendiente

1. Las páginas del slice de un nodo que sale (o cae) dejan de estar disponibles. Las asignaciones
   de un nodo que cae sin `dsm_finalize` (DOWN) no se recuperan.
2. Consultas en HTML por `HTTP_PORT` (opcional según la especificación).
3. Uniones concurrentes en contactos distintos pueden asignar el mismo rango. Por eso los nodos
   se arrancan de a uno.
4. El listener lee con `recv` bloqueante, así que un par lento podría retrasarlo.
5. Pruebas en las computadoras del laboratorio. Ya se probó con dos máquinas simuladas
   (`tests/run_twohost.sh`), pero no en máquinas físicas distintas.
