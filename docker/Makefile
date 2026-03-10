# docker/Makefile  –  Docker cluster management shortcuts
#
# Usage (from the docker/ directory):
#   make build    — build the VoidCache Docker image
#   make up       — start 3-node cluster + HAProxy
#   make down     — stop and remove containers
#   make ps       — show running containers + health
#   make logs     — tail all container logs
#   make cli      — open interactive vcli shell against the cluster
#   make ping     — PING all 3 nodes directly
#   make bench    — run a quick throughput test
#   make tls-up   — start cluster with TLS
#   make clean    — remove containers, volumes, image

COMPOSE := docker compose
IMAGE   := voidcache

.PHONY: build up down restart ps logs cli ping bench \
        tls-certs tls-up clean prune status

# ── Build ─────────────────────────────────────────────────────────────────────
build:
	$(COMPOSE) build --no-cache

# ── Cluster lifecycle ─────────────────────────────────────────────────────────
up:
	$(COMPOSE) up -d
	@echo ""
	@echo "VoidCache cluster started."
	@echo "  HAProxy :  localhost:6379  (client endpoint)"
	@echo "  Stats UI:  http://localhost:8404/stats  (admin/admin)"
	@echo "  Node 1  :  localhost:6381  (direct)"
	@echo "  Node 2  :  localhost:6382  (direct)"
	@echo "  Node 3  :  localhost:6383  (direct)"
	@echo ""
	@echo "Try:  make cli"

down:
	$(COMPOSE) down

restart:
	$(COMPOSE) restart

ps:
	$(COMPOSE) ps

status:
	@echo "=== Container health ==="
	$(COMPOSE) ps --format "table {{.Name}}\t{{.Status}}\t{{.Ports}}"
	@echo ""
	@echo "=== Node info ==="
	@for node in 1 2 3; do \
	  echo "--- vcache-$$node ---"; \
	  docker exec vcache-$$node vcli -h 127.0.0.1 -p 6379 --no-color VCINFO 2>/dev/null || echo "  (not ready)"; \
	done

logs:
	$(COMPOSE) logs -f --tail=50

logs-node1:
	$(COMPOSE) logs -f vcache-1

logs-node2:
	$(COMPOSE) logs -f vcache-2

logs-node3:
	$(COMPOSE) logs -f vcache-3

# ── Interactive CLI ───────────────────────────────────────────────────────────
# Connect to HAProxy (load-balanced entry point)
cli:
	docker run --rm -it --network voidcache_vcache-net \
	  voidcache vcli -h haproxy -p 6379

# Connect directly to a specific node (make cli-1, cli-2, cli-3)
cli-1:
	docker exec -it vcache-1 vcli -h 127.0.0.1 -p 6379

cli-2:
	docker exec -it vcache-2 vcli -h 127.0.0.1 -p 6379

cli-3:
	docker exec -it vcache-3 vcli -h 127.0.0.1 -p 6379

# ── Health checks ─────────────────────────────────────────────────────────────
ping:
	@echo "Pinging all nodes..."
	@for node in 1 2 3; do \
	  printf "  vcache-$$node: "; \
	  docker exec vcache-$$node vcli -h 127.0.0.1 -p 6379 --no-color PING 2>/dev/null || echo "FAIL"; \
	done
	@printf "  haproxy:   "; \
	docker run --rm --network voidcache_vcache-net voidcache \
	  vcli -h haproxy -p 6379 --no-color PING 2>/dev/null || echo "FAIL"

# ── Quick benchmark ───────────────────────────────────────────────────────────
bench:
	@echo "Running quick bench against HAProxy:6379 ..."
	docker run --rm --network voidcache_vcache-net voidcache sh -c \
	  'for i in $$(seq 1 1000); do \
	     printf "SET bench:$$i val$$i\n"; \
	   done | vcli -h haproxy -p 6379 --no-color --pipe 2>/dev/null | \
	   grep -c "^OK" | xargs -I{} echo "{} / 1000 SETs OK"'

# ── TLS ───────────────────────────────────────────────────────────────────────
tls-certs:
	$(COMPOSE) --profile tls run --rm cert-gen
	@# HAProxy needs cert+key in a single PEM
	@cat tls/server.crt tls/server.key > tls/haproxy.pem 2>/dev/null || true
	@echo "TLS certs ready in docker/tls/"

tls-up: tls-certs
	$(COMPOSE) -f docker-compose.yml -f docker-compose.tls.yml up -d
	@echo "TLS cluster started on :6379 (plain) and :6380 (TLS)"

# ── Cleanup ───────────────────────────────────────────────────────────────────
clean:
	$(COMPOSE) down -v --remove-orphans

prune: clean
	docker rmi voidcache 2>/dev/null || true
	docker volume prune -f
