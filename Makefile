CC      := gcc
CFLAGS  := -O3 -march=native -funroll-loops -std=c11 \
           -Iinclude -Inet \
           -Wall -Wextra \
           -Wno-unused-function -Wno-unused-parameter
LDFLAGS := -lpthread -lm

# OpenSSL link flags.
# - With libssl-dev installed (Docker builder, dev machines): pkg-config resolves to -lssl -lcrypto
# - Without libssl-dev (runtime-only): fall back to direct .so.3 path
SSL_LIBS := $(shell pkg-config --libs openssl 2>/dev/null || echo "-lssl -lcrypto")
# Uncomment the line below only if pkg-config is unavailable AND libssl-dev is not installed:
# SSL_LIBS := /usr/lib/x86_64-linux-gnu/libssl.so.3 /usr/lib/x86_64-linux-gnu/libcrypto.so.3

CORE_SRC  := src/voidcache.c
NET_SRC   := net/proto.c net/auth.c net/commands.c net/server.c net/cluster.c
CLI_SRC   := cli/vcli.c
TEST_SRC  := tests/test_voidcache.c
BENCH_SRC := bench/benchmark.c

.PHONY: all vcli test bench smoke clean

all: vcli voidcache_test voidcache_bench

# Main binary: server + CLI combined
vcli: $(CORE_SRC) $(NET_SRC) $(CLI_SRC)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(SSL_LIBS)

# Unit + stress tests (no network layer)
voidcache_test: $(CORE_SRC) $(TEST_SRC)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Benchmark suite
voidcache_bench: $(CORE_SRC) $(BENCH_SRC)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

test: voidcache_test
	./voidcache_test

bench: voidcache_bench
	./voidcache_bench

# Quick smoke test: start server, run commands, stop
smoke: vcli
	@./vcli server --port 16399 & \
	  SERVER_PID=$$!; sleep 0.4; \
	  printf "PING\nSET x 1\nGET x\nVCSET n int 42\nVCGET n\nQUIT\n" \
	    | ./vcli -p 16399 --no-color --pipe; \
	  kill $$SERVER_PID 2>/dev/null; wait $$SERVER_PID 2>/dev/null

clean:
	rm -f vcli voidcache_test voidcache_bench
