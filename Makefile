CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -pthread -Iinclude -Isrc
SRC := src/config.c src/net.c src/membership.c src/pager.c src/dsm.c
OBJ := $(SRC:.c=.o)
APPS := apps/hello_dsm

all: libdsm.a $(APPS)
libdsm.a: $(OBJ)
	ar rcs $@ $^
src/%.o: src/%.c src/internal.h src/protocol.h include/dsm.h
	$(CC) $(CFLAGS) -c $< -o $@
apps/%: apps/%.c libdsm.a
	$(CC) $(CFLAGS) $< libdsm.a -o $@ -pthread
test: all
	./tests/run_local.sh 3
clean:
	rm -f src/*.o libdsm.a $(APPS)
.PHONY: all test clean
