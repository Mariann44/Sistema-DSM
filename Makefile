CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -pthread -Iinclude -Isrc
SRC := src/config.c src/net.c src/membership.c src/heartbeat.c src/sync.c src/pager.c src/ctl.c src/dsm.c
OBJ := $(SRC:.c=.o)
APPS := apps/hello_dsm apps/idle_dsm apps/counter
TOOLS := tools/dsmctl

all: libdsm.a $(APPS) $(TOOLS)
libdsm.a: $(OBJ)
	ar rcs $@ $^
src/%.o: src/%.c src/internal.h src/protocol.h include/dsm.h
	$(CC) $(CFLAGS) -c $< -o $@
apps/%: apps/%.c libdsm.a
	$(CC) $(CFLAGS) $< libdsm.a -o $@ -pthread
tools/%: tools/%.c
	$(CC) $(CFLAGS) $< -o $@
test: all
	./tests/run_local.sh 3
test-hb: all
	./tests/run_heartbeat.sh
test-lock: all
	./tests/run_counter.sh 3 300
clean:
	rm -f src/*.o libdsm.a $(APPS) $(TOOLS)
.PHONY: all test test-hb test-lock clean
