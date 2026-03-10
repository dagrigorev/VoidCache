/*
 * net/cluster.h  –  VoidCache cluster client-side routing layer.
 *
 * ── How it works ─────────────────────────────────────────────────────────────
 *
 *  VoidCache uses Redis Cluster slot protocol (16384 hash slots) so that
 *  any Redis Cluster-aware driver can connect and route correctly.
 *
 *  Server side:  Each vcserver_t responds to CLUSTER SLOTS / CLUSTER NODES
 *  so the driver builds its slot→node map.  In a real cluster multiple
 *  vcserver_t instances are launched; their slot ranges are configured
 *  via vcache.conf.
 *
 *  Client side (this module):  A thin routing library for non-cluster-aware
 *  clients (plain redis-cli, your own C code).  It:
 *    1. Connects to a seed node and fetches CLUSTER SLOTS.
 *    2. Builds a consistent hash ring: slot → (host, port, SSL).
 *    3. Routes every key to the correct node via CRC16 of the key.
 *    4. On MOVED / ASK redirections, updates the routing table.
 *    5. Maintains a connection pool per node.
 *    6. On node failure (ECONNREFUSED / timeout), retries on replica or
 *       next ring node, with exponential back-off.
 *
 *  Cluster topology:
 *
 *    Client
 *      │  CLUSTER SLOTS → slot map
 *      ▼
 *    ┌─────────┐   ┌─────────┐   ┌─────────┐
 *    │ Node 0  │   │ Node 1  │   │ Node 2  │
 *    │ 0-5460  │   │5461-10922│  │10923-16383│
 *    └─────────┘   └─────────┘   └─────────┘
 *
 *  Fault tolerance:
 *    - Health-check thread pings each node every 5 s.
 *    - Dead node → all its slots routed to a configured replica.
 *    - If no replica, returns VC_CLUSTER_ERR_NONODE.
 *    - Reconnect attempt with exponential back-off: 100ms → 200 → 400 → 3200ms.
 */
#pragma once
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include "vc_ssl_abi.h"

#define VC_CLUSTER_SLOTS      16384
#define VC_CLUSTER_MAX_NODES  64
#define VC_CLUSTER_MAX_CONNS  8       /* pool size per node */
#define VC_CLUSTER_TIMEOUT_MS 5000    /* connect/read timeout */
#define VC_CLUSTER_PING_INTVL 5       /* health check interval, seconds */
#define VC_CLUSTER_MAX_RETRIES 3

typedef enum {
    VC_CLUSTER_OK          =  0,
    VC_CLUSTER_ERR_NONODE  = -1,
    VC_CLUSTER_ERR_NET     = -2,
    VC_CLUSTER_ERR_PROTO   = -3,
    VC_CLUSTER_ERR_AUTH    = -4,
    VC_CLUSTER_ERR_MOVED   = -5,
} vc_cluster_err_t;

/* ── Node connection pool entry ─────────────────────────────────────────── */
typedef struct {
    int      fd;
    SSL     *ssl;
    bool     in_use;
    int64_t  last_used_ns;
} vc_node_conn_t;

/* ── Cluster node ─────────────────────────────────────────────────────────── */
typedef struct {
    char      host[64];
    uint16_t  port;
    char      id[17];           /* node id from CLUSTER NODES */
    bool      alive;
    int64_t   last_seen_ns;
    int64_t   backoff_ns;       /* reconnect back-off */

    /* Connection pool */
    vc_node_conn_t conns[VC_CLUSTER_MAX_CONNS];
    pthread_mutex_t pool_lock;
} vc_cluster_node_t;

/* ── Cluster handle ──────────────────────────────────────────────────────── */
typedef struct {
    vc_cluster_node_t nodes[VC_CLUSTER_MAX_NODES];
    int               node_count;

    /* slot → node index */
    int8_t   slot_map[VC_CLUSTER_SLOTS];  /* -1 = unassigned */

    pthread_rwlock_t topology_lock;   /* readers: route; writers: update map */
    pthread_t        health_thread;
    volatile bool    running;

    /* TLS for all connections in this cluster */
    SSL_CTX  *ssl_ctx;     /* NULL = plain TCP */

    /* Auth credentials sent to every node */
    char      username[64];
    char      password[128];
} vc_cluster_t;

/* ── CRC16 for slot calculation (Redis-compatible) ───────────────────────── */
uint16_t vc_crc16(const char *key, size_t len);
uint16_t vc_key_slot(const char *key, size_t len);

/* ── Cluster API ──────────────────────────────────────────────────────────── */

/*
 * vc_cluster_connect – connect to a cluster via seed nodes.
 * seeds:  comma-separated "host:port" list (e.g. "127.0.0.1:6379,127.0.0.1:6380")
 * username/password: NULL = no auth
 * tls_ca:  path to CA cert for verifying nodes, or NULL for plain TCP
 */
vc_cluster_t *vc_cluster_connect(const char *seeds,
                                  const char *username,
                                  const char *password,
                                  const char *tls_ca);

void vc_cluster_destroy(vc_cluster_t *cl);

/*
 * vc_cluster_cmd – send a command to the correct node for key.
 * key may be NULL for commands without a key (INFO, PING).
 * reply is malloc'd; caller must free.
 * Returns VC_CLUSTER_OK or an error code.
 */
vc_cluster_err_t vc_cluster_cmd(vc_cluster_t *cl,
                                 const char *key, size_t klen,
                                 const char *cmd,  /* RESP3 encoded command */
                                 size_t cmd_len,
                                 char **reply, size_t *reply_len);

/* Update slot map from CLUSTER SLOTS response. */
int vc_cluster_refresh(vc_cluster_t *cl);

/* Get node index for a key slot. */
int vc_cluster_node_for_slot(vc_cluster_t *cl, uint16_t slot);

/* Format a RESP3 command into a buffer (for use with vc_cluster_cmd). */
int vc_resp_format(char *out, size_t outsz, int argc, ...);
