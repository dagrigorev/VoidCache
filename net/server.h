/*
 * net/server.h  –  VoidCache network server.
 *
 * ── Architecture ─────────────────────────────────────────────────────────────
 *
 *  ┌─────────────────────────────────────────────────────────────────┐
 *  │  vcserver_t                                                     │
 *  │                                                                 │
 *  │  listen fd (TLS or plain TCP)                                   │
 *  │  epoll fd                                                       │
 *  │                                                                 │
 *  │  ┌───────────────────────────────────────────────────────┐     │
 *  │  │  conn pool  [VC_MAX_CONNS connections]                │     │
 *  │  │  each conn: fd, SSL*, rbuf, wbuf, auth state, user*  │     │
 *  │  └───────────────────────────────────────────────────────┘     │
 *  │                                                                 │
 *  │  Multi-threaded: N worker threads each run their own epoll     │
 *  │  loop (SO_REUSEPORT sharding or a single accept loop that      │
 *  │  distributes fd via a ring).                                    │
 *  └─────────────────────────────────────────────────────────────────┘
 *
 *  TLS: optional.  If cert+key paths are configured, every connection
 *  is wrapped in TLS 1.2/1.3 via OpenSSL (libssl.so.3).  Plain TCP
 *  works for loopback / stunnel termination.
 *
 *  Protocol: RESP3.  HELLO 3 triggers RESP3 mode; HELLO 2 stays RESP2.
 *  All connections start in RESP2 for backward compatibility.
 *
 *  Auth: every command checks the connection's ACL bitmask.  Unauthenticated
 *  connections can only issue PING, HELLO, and AUTH.
 */
#pragma once
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include "../include/voidcache.h"
#include "proto.h"
#include "auth.h"
#include "vc_ssl_abi.h"

#define VC_SERVER_VERSION   "2.0.0"
#define VC_MAX_CONNS        16384
#define VC_WORKER_THREADS   4
#define VC_BACKLOG          512
#define VC_CONN_TIMEOUT_S   300    /* idle connection timeout, seconds */
#define VC_RBUF_INIT        (16 * 1024)
#define VC_WBUF_INIT        (16 * 1024)

/* ── Connection state ─────────────────────────────────────────────────────── */
typedef enum {
    VC_CONN_FREE      = 0,
    VC_CONN_CONNECTED = 1,
    VC_CONN_TLS_HAND  = 2,   /* TLS handshake in progress */
    VC_CONN_AUTHED    = 3,
    VC_CONN_CLOSING   = 4,
} vc_conn_state_t;

typedef struct {
    int              fd;
    vc_conn_state_t  state;
    SSL             *ssl;          /* NULL if plain TCP                  */
    bool             resp3;        /* HELLO 3 negotiated?                */

    vc_buf_t        *rbuf;         /* read buffer                        */
    vc_buf_t        *wbuf;         /* write buffer                       */

    const vc_user_t *user;         /* NULL = not authenticated           */
    char             token[65];    /* session token (hex)                */

    int64_t          last_active;  /* monotonic ns                       */
    int              db_idx;       /* selected DB index (always 0 for VC)*/

    /* cluster: which node owns this connection (for proxy routing) */
    char             peer_addr[46]; /* IPv4/IPv6 string                  */
    uint16_t         peer_port;
} vc_conn_t;

/* ── Server config ───────────────────────────────────────────────────────────*/
typedef struct {
    const char *bind_addr;         /* default "0.0.0.0"                 */
    uint16_t    port;              /* default 6379                       */
    const char *tls_cert;          /* NULL = plain TCP                  */
    const char *tls_key;
    const char *acl_file;          /* NULL = no auth                    */
    int         worker_threads;    /* default VC_WORKER_THREADS          */
    size_t      max_memory;        /* passed to vc_create               */
    const char *wal_path;
    uint32_t    shard_slots;

    /* Cluster */
    bool        cluster_enabled;
    const char *cluster_announce_addr;  /* address other nodes see      */
    uint16_t    cluster_announce_port;

    /* Simple password (Redis compat, maps to "default" user rwa) */
    const char *requirepass;
} vc_server_cfg_t;

/* ── Server handle ───────────────────────────────────────────────────────────*/
typedef struct {
    vc_server_cfg_t  cfg;
    vc_cache_t      *cache;
    vc_auth_db_t     auth;
    SSL_CTX         *ssl_ctx;      /* NULL = no TLS                      */

    int              listen_fd;
    int              epoll_fd;

    vc_conn_t        conns[VC_MAX_CONNS];
    pthread_mutex_t  conns_lock;   /* for conn slot alloc only           */

    pthread_t        workers[VC_WORKER_THREADS];
    volatile bool    running;

    /* Stats */
    _Atomic uint64_t total_conns;
    _Atomic uint64_t total_commands;
    _Atomic uint64_t rejected_auth;

    char             node_id[65];  /* token hex + NUL, first 16 used     */
} vcserver_t;

/* ── Public API ──────────────────────────────────────────────────────────── */

/* Create and bind server.  Returns NULL on error. */
vcserver_t *vcserver_create(const vc_server_cfg_t *cfg);

/* Start worker threads (non-blocking, returns immediately). */
int vcserver_start(vcserver_t *srv);

/* Block until SIGINT/SIGTERM. */
void vcserver_run(vcserver_t *srv);

/* Graceful shutdown. */
void vcserver_stop(vcserver_t *srv);
void vcserver_destroy(vcserver_t *srv);

/* Print server info (for INFO command). */
void vcserver_info(vcserver_t *srv, vc_buf_t *out);
