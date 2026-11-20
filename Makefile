CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -pthread -Iinclude -Isrc
SRC := src/config.c src/net.c src/membership.c src/heartbeat.c src/sync.c src/pager.c src/alloc.c src/ctl.c src/dsm.c
OBJ := $(SRC:.c=.o)
APPS := apps/hello_dsm apps/idle_dsm apps/counter apps/alloc_test
GEMM := apps/gemm/gemm_dsm apps/gemm/gemm_serial
# Dataset de la versión serial de referencia (MINI, SMALL, MEDIUM, LARGE o EXTRALARGE)
GEMM_DATASET ?= MEDIUM_DATASET
TOOLS := tools/dsmctl

all: libdsm.a $(APPS) $(GEMM) $(TOOLS)
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
test-gemm: all
	./tests/run_gemm.sh 3 2
clean:
	rm -f src/*.o libdsm.a $(APPS) $(GEMM) $(TOOLS)
.PHONY: all test test-hb test-lock test-alloc test-gemm clean
