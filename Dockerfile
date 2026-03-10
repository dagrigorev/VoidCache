# ─────────────────────────────────────────────────────────────────────────────
# VoidCache  —  Multi-stage Dockerfile
#
# Stage 1 (builder): compiles vcli from source on Ubuntu 24.04
# Stage 2 (runtime): minimal image with only the binary + libssl
#
# The final image is ~12 MB.
# ─────────────────────────────────────────────────────────────────────────────

# ── Stage 1: build ────────────────────────────────────────────────────────────
FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc make ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# Build with native-march replaced by generic x86-64 for portability inside Docker
RUN sed -i 's/-march=native/-march=x86-64/' Makefile && make vcli

# ── Stage 2: runtime ──────────────────────────────────────────────────────────
FROM ubuntu:24.04 AS runtime

# Only runtime deps: libssl3, libcrypto (already in ubuntu base), ca-certs
RUN apt-get update && apt-get install -y --no-install-recommends \
        libssl3 ca-certificates openssl \
    && rm -rf /var/lib/apt/lists/*

# Non-root user for security
RUN groupadd -r vcache && useradd -r -g vcache -d /vcache -s /sbin/nologin vcache

WORKDIR /vcache
COPY --from=builder /src/vcli /usr/local/bin/vcli

# Data directories
RUN mkdir -p /vcache/data /vcache/tls /vcache/config \
    && chown -R vcache:vcache /vcache

# Default config files (overridable via bind-mount)
COPY docker/config/vcache.acl   /vcache/config/vcache.acl
COPY docker/config/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

USER vcache

EXPOSE 6379
EXPOSE 6380

# Health check: PING via vcli
HEALTHCHECK --interval=10s --timeout=3s --start-period=5s --retries=3 \
    CMD vcli -h 127.0.0.1 -p 6379 --no-color PING 2>/dev/null | grep -q PONG

ENTRYPOINT ["/entrypoint.sh"]
