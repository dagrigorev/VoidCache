CC      := gcc
CFLAGS  := -O3 -march=native -funroll-loops -std=c11 -Iinclude \
           -Wall -Wextra -Wpedantic
LDFLAGS := -lpthread -lm

SRC     := src/voidcache.c
TESTS   := tests/test_voidcache.c
BENCH   := bench/benchmark.c

.PHONY: all test bench clean

all: voidcache_test voidcache_bench

voidcache_test: $(SRC) $(TESTS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

voidcache_bench: $(SRC) $(BENCH)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

test: voidcache_test
	./voidcache_test

bench: voidcache_bench
	./voidcache_bench

clean:
	rm -f voidcache_test voidcache_bench
