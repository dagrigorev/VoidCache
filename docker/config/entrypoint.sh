#!/bin/sh
# /entrypoint.sh  –  VoidCache container entrypoint
#
# Environment variables (all optional):
#
#   VC_PORT            TCP port to listen on           (default: 6379)
#   VC_BIND            Bind address                    (default: 0.0.0.0)
#   VC_PASSWORD        requirepass (simple auth)       (default: none)
#   VC_ACL_FILE        Path to ACL file                (default: /vcache/config/vcache.acl if exists)
#   VC_MAXMEMORY       Memory cap, e.g. 512m or 2g    (default: 256m)
#   VC_WAL             WAL file path                   (default: /vcache/data/vcache.wal)
#   VC_THREADS         Worker threads                  (default: 4)
#   VC_CLUSTER         Enable cluster mode: yes/no     (default: no)
#   VC_ANNOUNCE_ADDR   Cluster announce address        (default: auto-detect)
#   VC_ANNOUNCE_PORT   Cluster announce port           (default: VC_PORT)
#   VC_TLS_CERT        TLS certificate path            (default: /vcache/tls/server.crt if exists)
#   VC_TLS_KEY         TLS key path                    (default: /vcache/tls/server.key if exists)
#   VC_NO_WAL          Set to "1" to disable WAL       (default: WAL enabled)
#
set -e

PORT="${VC_PORT:-6379}"
BIND="${VC_BIND:-0.0.0.0}"
MAXMEMORY="${VC_MAXMEMORY:-256m}"
THREADS="${VC_THREADS:-4}"

ARGS="server"
ARGS="$ARGS --port $PORT"
ARGS="$ARGS --bind $BIND"
ARGS="$ARGS --maxmemory $MAXMEMORY"
ARGS="$ARGS --threads $THREADS"

# Auth
if [ -n "$VC_PASSWORD" ]; then
    ARGS="$ARGS --requirepass $VC_PASSWORD"
fi

# ACL file
if [ -n "$VC_ACL_FILE" ] && [ -f "$VC_ACL_FILE" ]; then
    ARGS="$ARGS --acl-file $VC_ACL_FILE"
elif [ -f "/vcache/config/vcache.acl" ]; then
    ARGS="$ARGS --acl-file /vcache/config/vcache.acl"
fi

# WAL persistence
if [ "${VC_NO_WAL:-0}" != "1" ]; then
    WAL="${VC_WAL:-/vcache/data/vcache.wal}"
    ARGS="$ARGS --wal $WAL"
fi

# TLS
TLS_CERT="${VC_TLS_CERT:-/vcache/tls/server.crt}"
TLS_KEY="${VC_TLS_KEY:-/vcache/tls/server.key}"
if [ -f "$TLS_CERT" ] && [ -f "$TLS_KEY" ]; then
    echo "[entrypoint] TLS enabled: cert=$TLS_CERT"
    ARGS="$ARGS --tls-cert $TLS_CERT --tls-key $TLS_KEY"
fi

# Cluster mode
if [ "${VC_CLUSTER:-no}" = "yes" ]; then
    ARGS="$ARGS --cluster"

    # Auto-detect announce address from hostname if not set
    ANNOUNCE_ADDR="${VC_ANNOUNCE_ADDR}"
    if [ -z "$ANNOUNCE_ADDR" ]; then
        # Try to resolve container hostname to IP
        ANNOUNCE_ADDR=$(getent hosts "$(hostname)" 2>/dev/null | awk '{print $1; exit}')
        if [ -z "$ANNOUNCE_ADDR" ]; then
            ANNOUNCE_ADDR=$(hostname -i 2>/dev/null | awk '{print $1}')
        fi
    fi

    ANNOUNCE_PORT="${VC_ANNOUNCE_PORT:-$PORT}"

    if [ -n "$ANNOUNCE_ADDR" ]; then
        echo "[entrypoint] Cluster announce: $ANNOUNCE_ADDR:$ANNOUNCE_PORT"
        ARGS="$ARGS --announce-addr $ANNOUNCE_ADDR --announce-port $ANNOUNCE_PORT"
    fi
fi

echo "[entrypoint] Starting VoidCache: vcli $ARGS"
exec vcli $ARGS
