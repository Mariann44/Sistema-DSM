CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -pthread -Iinclude -Isrc
SRC := src/config.c src/net.c src/membership.c src/heartbeat.c src/sync.c src/pager.c src/alloc.c src/ctl.c src/dsm.c
OBJ := $(SRC:.c=.o)
APPS := apps/hello_dsm apps/idle_dsm apps/counter apps/alloc_test apps/cache_test
GEMM := apps/gemm/gemm_dsm apps/gemm/gemm_serial
# Dataset de la versión serial de referencia (MINI, SMALL, MEDIUM, LARGE o EXTRALARGE)
GEMM_DATASET ?= MEDIUM_DATASET
SOBEL := apps/sobel/sobel_dsm apps/sobel/sobel_serial
SOBEL_LIB := apps/sobel/image_operations.o apps/sobel/file_operations.o
TOOLS := tools/dsmctl

all: libdsm.a $(APPS) $(GEMM) $(SOBEL) $(TOOLS)
libdsm.a: $(OBJ)
	ar rcs $@ $^
src/%.o: src/%.c src/internal.h src/protocol.h include/dsm.h
	$(CC) $(CFLAGS) -c $< -o $@
apps/%: apps/%.c libdsm.a
	$(CC) $(CFLAGS) $< libdsm.a -o $@ -pthread
# Aplicación 1: gemm de PolyBench/C. gemm_dsm usa el DSM; gemm_serial es el original sin cambios,
# compilado con volcado de C para compararlo con la salida de gemm_dsm.
apps/gemm/gemm_dsm: apps/gemm/gemm_dsm.c apps/gemm/gemm.h libdsm.a
	$(CC) $(CFLAGS) -Iapps/gemm $< libdsm.a -o $@ -pthread -lm
apps/gemm/gemm_serial: apps/gemm/gemm_serial.c apps/gemm/polybench.c apps/gemm/gemm.h
	$(CC) $(CFLAGS) -Iapps/gemm -D$(GEMM_DATASET) -DPOLYBENCH_DUMP_ARRAYS -DPOLYBENCH_TIME -w apps/gemm/gemm_serial.c apps/gemm/polybench.c -o $@ -lm
# Aplicación 2: filtro Sobel de DanyEle/Sobel_Filter sobre video. image_operations.c y
# file_operations.c son los originales sin cambios (-w porque traen advertencias propias).
apps/sobel/%.o: apps/sobel/%.c apps/sobel/image_operations.h apps/sobel/file_operations.h
	$(CC) $(CFLAGS) -w -c $< -o $@
apps/sobel/sobel_dsm: apps/sobel/sobel_dsm.c $(SOBEL_LIB) libdsm.a
	$(CC) $(CFLAGS) -Iapps/sobel $< $(SOBEL_LIB) libdsm.a -o $@ -pthread -lm
apps/sobel/sobel_serial: apps/sobel/sobel_serial.c $(SOBEL_LIB)
	$(CC) $(CFLAGS) -Iapps/sobel $< $(SOBEL_LIB) -o $@ -lm
tools/%: tools/%.c
	$(CC) $(CFLAGS) $< -o $@
test: all
	./tests/run_local.sh 3
test-hb: all
	./tests/run_heartbeat.sh
test-lock: all
	./tests/run_counter.sh 3 300
test-alloc: all
	./tests/run_alloc.sh 3
test-cache: all
	./tests/run_cache.sh
test-gemm: all
	./tests/run_gemm.sh 3 2
test-sobel: all
	./tests/run_sobel.sh 3 2
clean:
	rm -f src/*.o apps/sobel/*.o libdsm.a $(APPS) $(GEMM) $(SOBEL) $(TOOLS)
.PHONY: all test test-hb test-lock test-alloc test-cache test-gemm test-sobel clean
