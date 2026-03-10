# VoidCache — Docker Cluster Guide

## Architecture

```
                     ┌─────────────────────────────────┐
  your app           │      Docker network (bridge)     │
  redis-py / ioredis │                                  │
       │             │  ┌───────────────────────────┐   │
       └─────────────┼──►  HAProxy  :6379            │   │
                     │  │  (leastconn, health-check) │   │
                     │  └──────┬──────────┬──────────┘   │
                     │         │          │          │    │
                     │  ┌──────▼───┐ ┌───▼────┐ ┌───▼──┐ │
                     │  │vcache-1  │ │vcache-2│ │vcache-3│ │
                     │  │:6379     │ │:6379   │ │:6379   │ │
                     │  │slot 0-   │ │5461-   │ │10923-  │ │
                     │  │5460      │ │10922   │ │16383   │ │
                     │  └──────────┘ └────────┘ └───────┘ │
                     └─────────────────────────────────────┘

  External ports (host):
    6379 → HAProxy (load-balanced client endpoint)
    6381 → vcache-1 (direct node access)
    6382 → vcache-2 (direct node access)
    6383 → vcache-3 (direct node access)
    8404 → HAProxy stats UI
```

HAProxy speaks plain TCP (RESP3 passes through).  
Each VoidCache node runs as a non-root user, with a persistent WAL volume.

---

## Quick Start

```bash
cd VoidCache/docker

# 1. Build the image (once)
make build
# or: docker compose build

# 2. Start the 3-node cluster
make up
# or: docker compose up -d

# 3. Test it
make ping
# → vcache-1: PONG
# → vcache-2: PONG
# → vcache-3: PONG
# → haproxy:  PONG

# 4. Open interactive CLI (connects through HAProxy)
make cli
# vcli > SET hello world
# vcli > GET hello
# vcli > VCSET score int 42
# vcli > VCGET score
```

---

## Detailed Setup

### Prerequisites

| Tool | Minimum version |
|---|---|
| Docker Engine | 24.0+ |
| Docker Compose | v2.20+ (plugin, not standalone) |
| Available ports | 6379, 6381–6383, 8404 |

### 1. Build the image

```bash
cd VoidCache/docker
make build
```

This runs a **two-stage build**:
- Stage 1 (`builder`): Ubuntu 24.04 + gcc, compiles `vcli` from source
- Stage 2 (`runtime`): Ubuntu 24.04 minimal + libssl3 only (~12 MB final image)

No external dependencies are downloaded at runtime.

### 2. Configuration

Edit `docker/.env` before starting:

```env
VC_PORT=6379        # HAProxy host port
VC_MAXMEMORY=512m   # Memory cap per node (k/m/g)
VC_THREADS=4        # Worker threads per node
VC_PASSWORD=        # Leave blank to disable auth
VC_NO_WAL=0         # 1 = no persistence (faster for dev)
```

To enable **authentication**, set a password:

```env
VC_PASSWORD=my_strong_password
```

Then connect with:
```bash
vcli -h localhost -p 6379 -a my_strong_password PING
```

For **per-user ACL**, edit `docker/config/vcache.acl`:

```
# username  sha256_of_password  flags(r/w/a/*)
admin   <sha256>  *
app     <sha256>  rw
reader  <sha256>  r
```

Generate password hashes:
```bash
echo -n "mypassword" | sha256sum | cut -d' ' -f1
```

### 3. Start the cluster

```bash
make up
```

Watch logs while it starts:
```bash
make logs
# or: docker compose logs -f
```

Expected output per node:
```
[server] VoidCache 2.0.0  node=a1b2c3d4e5f6...
[server] Listening on 0.0.0.0:6379
[server] Auth: disabled  Cluster: enabled
```

### 4. Verify health

```bash
# Check all container statuses
make status

# HAProxy stats dashboard
open http://localhost:8404/stats
# (login: admin / admin)
```

The stats page shows each node's health, connection count, and bytes in/out in real time.

---

## Connecting with Redis Clients

VoidCache is **wire-compatible with Redis**. Any Redis client works:

### redis-cli (no extra config)
```bash
redis-cli -h localhost -p 6379 PING
redis-cli -h localhost -p 6379 SET foo bar
redis-cli -h localhost -p 6379 GET foo
```

### Python (redis-py)
```python
import redis

r = redis.Redis(host='localhost', port=6379, decode_responses=True)
r.set('key', 'value')
print(r.get('key'))           # → "value"

# VoidCache extended commands
r.execute_command('VCSET', 'counter', 'int', '0')
r.execute_command('VCGET', 'counter')
# → {'type': 'int', 'value': 0}
```

### Python cluster client (redis-py cluster)
```python
from redis.cluster import RedisCluster

rc = RedisCluster(
    host='localhost', port=6379,
    skip_full_coverage_check=True,   # single-region cluster
    decode_responses=True
)
rc.set('foo', 'bar')
print(rc.get('foo'))
```

### Node.js (ioredis)
```javascript
const Redis = require('ioredis')

// Single node through HAProxy
const r = new Redis({ host: 'localhost', port: 6379 })
await r.set('key', 'value')
await r.get('key')

// Cluster client (connects directly to nodes)
const cluster = new Redis.Cluster([
  { host: 'localhost', port: 6381 },
  { host: 'localhost', port: 6382 },
  { host: 'localhost', port: 6383 },
])
await cluster.set('key', 'value')
```

### Go (go-redis)
```go
import "github.com/redis/go-redis/v9"

rdb := redis.NewClient(&redis.Options{
    Addr: "localhost:6379",
})

// Or cluster mode
rdb = redis.NewClusterClient(&redis.ClusterOptions{
    Addrs: []string{
        "localhost:6381",
        "localhost:6382",
        "localhost:6383",
    },
})
```

---

## TLS Setup

### Step 1: Generate certificates

```bash
make tls-certs
```

This generates a self-signed certificate valid for all node hostnames into `docker/tls/`.  
For production, replace with your own certs:

```bash
# Copy your real certs
cp /etc/letsencrypt/live/example.com/fullchain.pem docker/tls/server.crt
cp /etc/letsencrypt/live/example.com/privkey.pem   docker/tls/server.key
cat docker/tls/server.crt docker/tls/server.key > docker/tls/haproxy.pem
```

### Step 2: Start with TLS

```bash
make tls-up
```

### Step 3: Connect

```bash
# Through HAProxy TLS termination (port 6380)
vcli --tls -h localhost -p 6380 PING

# Direct node with TLS (nodes also have certs mounted)
vcli --tls -h localhost -p 6381 PING

# Python
r = redis.Redis(host='localhost', port=6380, ssl=True, ssl_cert_reqs=None)
```

---

## Operations

### Live cluster management

```bash
# Tail logs for all containers
make logs

# Tail a specific node
make logs-node1

# Restart a single node (zero-downtime — HAProxy reroutes)
docker compose restart vcache-2

# Check memory/key stats on each node
make status
```

### Scale horizontally

Add a 4th node:

```bash
# 1. Add vcache-4 to docker-compose.yml (copy vcache-3 block, change names/ports)
# 2. Start the new node
docker compose up -d vcache-4

# 3. Add it to HAProxy (edit haproxy.cfg, add server line)
#    server vcache-4 vcache-4:6379 check inter 5s fall 3 rise 2 weight 10

# 4. Reload HAProxy config without dropping connections
docker kill -s HUP vcache-haproxy
```

### Rolling restart (zero downtime)

```bash
for node in vcache-1 vcache-2 vcache-3; do
  echo "Restarting $node..."
  docker compose restart $node
  # Wait for health check to pass before continuing
  until docker inspect --format='{{.State.Health.Status}}' $node | grep -q healthy; do
    sleep 2
  done
  echo "$node healthy"
done
```

### Inspect data on a node

```bash
# Direct CLI into node 1
make cli-1

# vcli> DBSIZE
# vcli> KEYS *
# vcli> VCINFO
# vcli> INFO
```

### Backup WAL data

```bash
# Volumes are in Docker-managed locations
docker run --rm \
  -v voidcache_vcache-1-data:/data \
  -v $(pwd)/backup:/backup \
  alpine tar czf /backup/vcache-1-$(date +%Y%m%d).tar.gz /data
```

### Wipe and restart fresh

```bash
make clean     # stops containers + removes volumes
make up        # fresh cluster
```

---

## Environment Variables Reference

| Variable | Default | Description |
|---|---|---|
| `VC_PORT` | `6379` | HAProxy host port |
| `VC_MAXMEMORY` | `512m` | Memory cap per node |
| `VC_THREADS` | `4` | Worker threads per node |
| `VC_PASSWORD` | _(none)_ | requirepass (simple auth) |
| `VC_ACL_FILE` | auto | Path to ACL file |
| `VC_NO_WAL` | `0` | `1` = disable WAL (dev mode) |
| `VC_CLUSTER` | `yes` | Enable cluster protocol |
| `VC_ANNOUNCE_ADDR` | auto | Cluster announce IP |
| `VC_ANNOUNCE_PORT` | `6379` | Cluster announce port |
| `VC_TLS_CERT` | auto | TLS cert path |
| `VC_TLS_KEY` | auto | TLS key path |

---

## Troubleshooting

### "Connection refused" on port 6379

HAProxy only starts after all 3 nodes pass health checks. Wait ~15s then:

```bash
make ps      # check container states
make logs    # look for startup errors
```

### Node stuck in "starting" health status

```bash
docker logs vcache-1
```

Common causes:
- Port already in use: change `VC_PORT` in `.env`
- Permissions on WAL volume: `docker compose down -v && make up`

### "NOAUTH Authentication required"

You have `VC_PASSWORD` set. Pass it:

```bash
vcli -h localhost -p 6379 -a $VC_PASSWORD PING
```

### TLS handshake failed

```bash
# Regenerate certs
rm -rf docker/tls
make tls-certs tls-up
```

### HAProxy shows nodes as DOWN

HAProxy sends `PING\r\n` and expects `+PONG`. Verify directly:

```bash
docker exec vcache-1 vcli -h 127.0.0.1 -p 6379 --no-color PING
```

---

## File Layout

```
docker/
├── Makefile                  Cluster management shortcuts
├── docker-compose.yml        3-node cluster + HAProxy
├── docker-compose.tls.yml    TLS overlay
├── .env                      Tunable defaults
├── config/
│   ├── vcache.acl            User ACL file (edit for your users)
│   └── entrypoint.sh         Container entrypoint
├── haproxy/
│   ├── haproxy.cfg           Plain TCP load balancer config
│   └── haproxy-tls.cfg       TLS-terminating config
└── tls/                      Generated by make tls-certs
    ├── server.crt
    ├── server.key
    └── haproxy.pem           (cert + key concatenated for HAProxy)
```
