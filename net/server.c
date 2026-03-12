/*
 * net/server.c  –  VoidCache network server implementation.
 *
 * Event model: epoll edge-triggered (Linux) / wepoll (Windows/MSYS2).
 * Threading: worker threads, each with own epoll fd.
 * TLS: OpenSSL 3.x, non-blocking handshake driven by event loop.
 * Connection distribution: SO_REUSEPORT (Linux) / SO_REUSEADDR (Windows).
 */
#ifndef _WIN32
# define _POSIX_C_SOURCE 200809L
# define _GNU_SOURCE
#endif

#include "server.h"
#include "commands.h"
#include "proto.h"
#include "auth.h"
#include "vc_ssl_abi.h"

#ifdef _MSC_VER
# include "../compat/msvc.h"
# include "../compat/pthread_win32.h"
# include "../compat/wepoll.h"
#elif defined(_WIN32)
# include "../compat/windows.h"
# include "../compat/wepoll.h"
#else
# include <sys/epoll.h>
# include <signal.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

/* ══════════════════════════════════════════════════════════════
 * UTILITIES
 * ══════════════════════════════════════════════════════════════ */

static int64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int set_tcp_nodelay(int fd) {
    int yes = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
}

static int set_reuseport(int fd) {
    int yes = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
}

static int set_reuseaddr(int fd) {
    int yes = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
}

/* ══════════════════════════════════════════════════════════════
 * TLS  (libssl.so.3)
 * ══════════════════════════════════════════════════════════════ */

static SSL_CTX *tls_ctx_create(const char *cert, const char *key) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        fprintf(stderr, "[TLS] SSL_CTX_new failed\n"); return NULL;
    }
    /* TLS 1.2+ only */
    SSL_CTX_set_options(ctx,
        SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 |
        SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1);
    SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);

    if (SSL_CTX_use_certificate_file(ctx, cert, SSL_FILETYPE_PEM) <= 0 ||
        SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) <= 0 ||
        SSL_CTX_check_private_key(ctx) <= 0) {
        char errbuf[256];
        ERR_error_string(ERR_get_error(), errbuf);
        fprintf(stderr, "[TLS] cert/key error: %s\n", errbuf);
        SSL_CTX_free(ctx);
        return NULL;
    }
    fprintf(stderr, "[TLS] context created, cert=%s\n", cert);
    return ctx;
}

/* ══════════════════════════════════════════════════════════════
 * CONNECTION MANAGEMENT
 * ══════════════════════════════════════════════════════════════ */

static vc_conn_t *conn_alloc(vcserver_t *srv) {
    pthread_mutex_lock(&srv->conns_lock);
    for (int i = 0; i < VC_MAX_CONNS; i++) {
        if (srv->conns[i].state == VC_CONN_FREE) {
            vc_conn_t *c = &srv->conns[i];
            c->state = VC_CONN_CONNECTED;
            c->rbuf  = vc_buf_new(VC_RBUF_INIT);
            c->wbuf  = vc_buf_new(VC_WBUF_INIT);
            c->user  = NULL;
            c->resp3 = false;
            c->ssl   = NULL;
            c->db_idx = 0;
            c->last_active = mono_ns();
            pthread_mutex_unlock(&srv->conns_lock);
            return c;
        }
    }
    pthread_mutex_unlock(&srv->conns_lock);
    return NULL;  /* pool exhausted */
}

static void conn_close(vcserver_t *srv, vc_conn_t *conn, int epoll_fd) {
    if (conn->state == VC_CONN_FREE) return;
    if (epoll_fd >= 0)
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
    if (conn->ssl) {
        SSL_shutdown(conn->ssl);
        SSL_free(conn->ssl);
        conn->ssl = NULL;
    }
    close(conn->fd);
    conn->fd = -1;
    vc_buf_free(conn->rbuf); conn->rbuf = NULL;
    vc_buf_free(conn->wbuf); conn->wbuf = NULL;
    conn->state = VC_CONN_FREE;
    (void)srv;
}

/* ══════════════════════════════════════════════════════════════
 * I/O HELPERS (TLS-aware)
 * ══════════════════════════════════════════════════════════════ */

static ssize_t conn_read(vc_conn_t *conn, void *buf, size_t len) {
    if (conn->ssl)
        return (ssize_t)SSL_read(conn->ssl, buf, (int)len);
    return read(conn->fd, buf, len);
}

static ssize_t conn_write(vc_conn_t *conn, const void *buf, size_t len) {
    if (conn->ssl)
        return (ssize_t)SSL_write(conn->ssl, buf, (int)len);
    return write(conn->fd, buf, len);
}

/* Flush wbuf to socket.  Returns 0 ok, -1 error, 1 would-block. */
static int conn_flush(vc_conn_t *conn) {
    vc_buf_t *b = conn->wbuf;
    while (b->rpos < b->len) {
        ssize_t n = conn_write(conn, b->data + b->rpos, b->len - b->rpos);
        if (n > 0) { b->rpos += (size_t)n; continue; }
        if (n == 0) return -1;
        int err = conn->ssl ? SSL_get_error(conn->ssl, (int)n) : errno;
        if (err == EAGAIN || err == EWOULDBLOCK ||
            err == SSL_ERROR_WANT_WRITE) return 1;
        return -1;
    }
    vc_buf_reset(b);
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 * REQUEST PROCESSING
 * ══════════════════════════════════════════════════════════════ */

static void process_conn(vcserver_t *srv, vc_conn_t *conn, int epoll_fd) {
    /* Read available bytes */
    while (1) {
        if (vc_buf_grow(conn->rbuf, 4096) < 0) { conn_close(srv, conn, epoll_fd); return; }
        ssize_t n = conn_read(conn,
                              conn->rbuf->data + conn->rbuf->len,
                              conn->rbuf->cap  - conn->rbuf->len);
        if (n > 0) {
            conn->rbuf->len += (size_t)n;
            conn->last_active = mono_ns();
        } else if (n == 0) {
            conn_close(srv, conn, epoll_fd); return;
        } else {
            int err = conn->ssl ? SSL_get_error(conn->ssl, (int)n) : errno;
            if (err == EAGAIN || err == EWOULDBLOCK ||
                err == SSL_ERROR_WANT_READ) break;
            conn_close(srv, conn, epoll_fd); return;
        }
    }

    /* Parse and dispatch all complete commands in rbuf */
    while (conn->rbuf->len > 0) {
        vc_cmd_t cmd = {0};
        size_t consumed = 0;
        resp_parse_result_t pr = resp_parse_command(
            conn->rbuf->data, conn->rbuf->len, &cmd, &consumed);

        if (pr == RESP_PARSE_MORE) break;
        if (pr == RESP_PARSE_ERR) {
            resp_write_error(conn->wbuf, "ERR", "protocol error");
            conn_flush(conn);
            conn_close(srv, conn, epoll_fd);
            return;
        }

        vc_dispatch(srv, conn, &cmd);
        vc_cmd_free(&cmd);
        vc_buf_consume(conn->rbuf, consumed);

        if (conn->state == VC_CONN_CLOSING) {
            conn_flush(conn);
            conn_close(srv, conn, epoll_fd);
            return;
        }
    }

    /* Flush write buffer */
    if (conn->wbuf->len > 0) {
        int r = conn_flush(conn);
        if (r == -1) { conn_close(srv, conn, epoll_fd); return; }
        if (r == 1) {
            /* Re-arm for EPOLLOUT */
            struct epoll_event ev = {
                .events  = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP,
                .data.ptr = conn
            };
            epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);
        }
    }
}

/* ── TLS handshake (non-blocking, driven by epoll) ─────────────────────── */
static void tls_do_handshake(vcserver_t *srv, vc_conn_t *conn, int epoll_fd) {
    int r = SSL_accept(conn->ssl);
    if (r == 1) {
        /* Handshake complete */
        conn->state = srv->auth.require_auth ? VC_CONN_CONNECTED : VC_CONN_AUTHED;
        return;
    }
    int err = SSL_get_error(conn->ssl, r);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) return;
    fprintf(stderr, "[TLS] handshake failed on fd %d\n", conn->fd);
    conn_close(srv, conn, epoll_fd);
}

/* ══════════════════════════════════════════════════════════════
 * WORKER THREAD
 * ══════════════════════════════════════════════════════════════ */

typedef struct {
    vcserver_t *srv;
    int         epoll_fd;
    int         listen_fd;  /* SO_REUSEPORT: each worker has own listen copy */
} worker_arg_t;

#define MAX_EVENTS 256

static void *worker_thread(void *arg) {
    worker_arg_t *wa = (worker_arg_t *)arg;
    vcserver_t   *srv = wa->srv;
    int efd  = wa->epoll_fd;
    int lfd  = wa->listen_fd;

    /* Register listen fd on this worker's epoll */
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = lfd };
    epoll_ctl(efd, EPOLL_CTL_ADD, lfd, &ev);

    struct epoll_event events[MAX_EVENTS];

    while (srv->running) {
        int n = epoll_wait(efd, events, MAX_EVENTS, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < n; i++) {
            uint32_t evmask = events[i].events;

            /* New connection on listen fd */
            if (events[i].data.fd == lfd) {
                while (1) {
                    struct sockaddr_storage sa;
                    socklen_t salen = sizeof(sa);
#ifdef _WIN32
                    int cfd = accept(lfd, (struct sockaddr *)&sa, &salen);
                    if (cfd < 0) break;
                    /* Set non-blocking on Windows via ioctlsocket */
                    { u_long _nb = 1; ioctlsocket((SOCKET)cfd, FIONBIO, &_nb); }
#else
                    int cfd = accept4(lfd, (struct sockaddr *)&sa, &salen,
                                      SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (cfd < 0) break;
#endif

                    set_tcp_nodelay(cfd);

                    vc_conn_t *conn = conn_alloc(srv);
                    if (!conn) { close(cfd); continue; }

                    conn->fd = cfd;
                    /* Extract peer addr */
                    if (sa.ss_family == AF_INET) {
                        struct sockaddr_in *s4 = (struct sockaddr_in *)&sa;
                        inet_ntop(AF_INET, &s4->sin_addr,
                                  conn->peer_addr, sizeof(conn->peer_addr));
                        conn->peer_port = ntohs(s4->sin_port);
                    } else if (sa.ss_family == AF_INET6) {
                        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&sa;
                        inet_ntop(AF_INET6, &s6->sin6_addr,
                                  conn->peer_addr, sizeof(conn->peer_addr));
                        conn->peer_port = ntohs(s6->sin6_port);
                    }

                    /* Wrap in TLS if configured */
                    if (srv->ssl_ctx) {
                        conn->ssl = SSL_new(srv->ssl_ctx);
                        SSL_set_fd(conn->ssl, cfd);
                        conn->state = VC_CONN_TLS_HAND;
                    }

                    /* Add to epoll */
                    struct epoll_event cev = {
                        .events   = EPOLLIN | EPOLLET | EPOLLRDHUP,
                        .data.ptr = conn
                    };
                    epoll_ctl(efd, EPOLL_CTL_ADD, cfd, &cev);
                    atomic_fetch_add(&srv->total_conns, 1);
                }
                continue;
            }

            /* Existing connection */
            vc_conn_t *conn = (vc_conn_t *)events[i].data.ptr;
            if (!conn || conn->state == VC_CONN_FREE) continue;

            if (evmask & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                conn_close(srv, conn, efd);
                continue;
            }

            if (conn->state == VC_CONN_TLS_HAND) {
                tls_do_handshake(srv, conn, efd);
                continue;
            }

            if (evmask & EPOLLOUT) {
                /* Resume pending write */
                int r = conn_flush(conn);
                if (r == -1) { conn_close(srv, conn, efd); continue; }
                if (r == 0) {
                    /* Re-arm read-only */
                    struct epoll_event nev = {
                        .events  = EPOLLIN | EPOLLET | EPOLLRDHUP,
                        .data.ptr = conn
                    };
                    epoll_ctl(efd, EPOLL_CTL_MOD, conn->fd, &nev);
                }
            }

            if (evmask & EPOLLIN) {
                process_conn(srv, conn, efd);
            }
        }

        /* Idle timeout sweep (every ~10 s of wall time) */
        static __thread int sweep_count = 0;
        if (++sweep_count >= 10) {
            sweep_count = 0;
            int64_t now = mono_ns();
            int64_t timeout_ns = (int64_t)VC_CONN_TIMEOUT_S * 1000000000LL;
            for (int j = 0; j < VC_MAX_CONNS; j++) {
                vc_conn_t *c = &srv->conns[j];
                if (c->state == VC_CONN_FREE || c->fd < 0) continue;
                if (now - c->last_active > timeout_ns)
                    conn_close(srv, c, efd);
            }
        }
    }

    free(wa);
    return NULL;
}

/* ══════════════════════════════════════════════════════════════
 * PUBLIC API
 * ══════════════════════════════════════════════════════════════ */

vcserver_t *vcserver_create(const vc_server_cfg_t *cfg) {
    vcserver_t *srv = calloc(1, sizeof(vcserver_t));
    if (!srv) return NULL;
    srv->cfg = *cfg;

    /* Node ID: 16 random hex chars */
    vc_auth_gen_token(srv->node_id);
    srv->node_id[16] = '\0';

    /* Create cache */
    srv->cache = vc_create(
        cfg->max_memory ? cfg->max_memory : 256ULL*1024*1024,
        cfg->wal_path,
        cfg->shard_slots ? cfg->shard_slots : 4096);
    if (!srv->cache) { free(srv); return NULL; }

    /* Load ACL */
    if (vc_auth_load(&srv->auth, cfg->acl_file) < 0) {
        vc_destroy(srv->cache); free(srv); return NULL;
    }

    /* Add requirepass as "default" user if set */
    if (cfg->requirepass && *cfg->requirepass) {
        vc_auth_add_user(&srv->auth, "default", cfg->requirepass,
                         VC_ACL_READ | VC_ACL_WRITE | VC_ACL_ADMIN);
    }

    /* TLS context */
    if (cfg->tls_cert && cfg->tls_key) {
        srv->ssl_ctx = tls_ctx_create(cfg->tls_cert, cfg->tls_key);
        if (!srv->ssl_ctx) {
            vc_destroy(srv->cache); free(srv); return NULL;
        }
    }

    /* Create listen socket */
    const char *addr = cfg->bind_addr ? cfg->bind_addr : "0.0.0.0";
    uint16_t    port = cfg->port      ? cfg->port      : 6379;

    struct addrinfo hints = {0}, *res;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE | AI_NUMERICHOST;
    char portstr[8]; snprintf(portstr, sizeof(portstr), "%u", port);

    if (getaddrinfo(addr, portstr, &hints, &res) != 0) {
        fprintf(stderr, "[server] getaddrinfo failed\n");
        vcserver_destroy(srv); return NULL;
    }

#ifdef _WIN32
    int lfd = socket(res->ai_family, SOCK_STREAM, IPPROTO_TCP);
    { u_long _nb = 1; ioctlsocket((SOCKET)lfd, FIONBIO, &_nb); }
#else
    int lfd = socket(res->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                     IPPROTO_TCP);
#endif
    if (lfd < 0) { freeaddrinfo(res); vcserver_destroy(srv); return NULL; }

    set_reuseaddr(lfd);
    set_reuseport(lfd);

    if (bind(lfd, res->ai_addr, res->ai_addrlen) < 0 ||
        listen(lfd, VC_BACKLOG) < 0) {
        fprintf(stderr, "[server] bind/listen failed on %s:%u: %s\n",
                addr, port, strerror(errno));
        close(lfd); freeaddrinfo(res); vcserver_destroy(srv); return NULL;
    }
    freeaddrinfo(res);
    srv->listen_fd = lfd;

    /* Master epoll fd (used by accept + workers share via SO_REUSEPORT) */
    srv->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (srv->epoll_fd < 0) { vcserver_destroy(srv); return NULL; }

    pthread_mutex_init(&srv->conns_lock, NULL);
    srv->running = true;

    fprintf(stderr,
        "[server] VoidCache %s  node=%s\n"
        "[server] Listening on %s:%u%s\n"
        "[server] Auth: %s  Cluster: %s\n",
        VC_SERVER_VERSION, srv->node_id,
        addr, port,
        srv->ssl_ctx ? " [TLS]" : "",
        srv->auth.require_auth ? "required" : "disabled",
        cfg->cluster_enabled   ? "enabled"  : "disabled");

    return srv;
}

int vcserver_start(vcserver_t *srv) {
    int nw = srv->cfg.worker_threads;
    if (nw <= 0) nw = VC_WORKER_THREADS;

    for (int i = 0; i < nw; i++) {
        int efd = epoll_create1(EPOLL_CLOEXEC);
        if (efd < 0) return -1;

        /* Each worker gets its own listen socket copy via SO_REUSEPORT */
        const char *addr = srv->cfg.bind_addr ? srv->cfg.bind_addr : "0.0.0.0";
        uint16_t    port = srv->cfg.port      ? srv->cfg.port      : 6379;

        struct addrinfo hints = {0}, *res;
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags    = AI_PASSIVE | AI_NUMERICHOST;
        char portstr[8]; snprintf(portstr, sizeof(portstr), "%u", port);
        getaddrinfo(addr, portstr, &hints, &res);

#ifdef _WIN32
        int wlfd = socket(res->ai_family, SOCK_STREAM, IPPROTO_TCP);
        { u_long _nb2 = 1; ioctlsocket((SOCKET)wlfd, FIONBIO, &_nb2); }
#else
        int wlfd = socket(res->ai_family,
                          SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                          IPPROTO_TCP);
#endif
        set_reuseaddr(wlfd);
        set_reuseport(wlfd);
        bind(wlfd, res->ai_addr, res->ai_addrlen);
        listen(wlfd, VC_BACKLOG);
        freeaddrinfo(res);

        worker_arg_t *wa = malloc(sizeof(worker_arg_t));
        wa->srv      = srv;
        wa->epoll_fd = efd;
        wa->listen_fd = wlfd;

        if (pthread_create(&srv->workers[i], NULL, worker_thread, wa) != 0) {
            free(wa); return -1;
        }
    }
    return 0;
}

void vcserver_run(vcserver_t *srv) {
    /* Block main thread until SIGINT/SIGTERM */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);

    int sig;
    sigwait(&mask, &sig);
    fprintf(stderr, "\n[server] Received signal %d, shutting down...\n", sig);
    vcserver_stop(srv);
}

void vcserver_stop(vcserver_t *srv) {
    srv->running = false;
    int nw = srv->cfg.worker_threads > 0
           ? srv->cfg.worker_threads : VC_WORKER_THREADS;
    for (int i = 0; i < nw; i++)
        pthread_join(srv->workers[i], NULL);
}

void vcserver_destroy(vcserver_t *srv) {
    if (!srv) return;
    if (srv->cache) vc_destroy(srv->cache);
    if (srv->ssl_ctx) SSL_CTX_free(srv->ssl_ctx);
    if (srv->listen_fd >= 0) close(srv->listen_fd);
    if (srv->epoll_fd  >= 0) close(srv->epoll_fd);
    pthread_mutex_destroy(&srv->conns_lock);
    free(srv);
}

void vcserver_info(vcserver_t *srv, vc_buf_t *out) {
    vc_global_stats_t st; vc_stats(srv->cache, &st);
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "VoidCache %s  node=%s  keys=%llu  hits=%llu  misses=%llu\n",
        VC_SERVER_VERSION, srv->node_id,
        (unsigned long long)st.total_keys,
        (unsigned long long)st.hits,
        (unsigned long long)st.misses);
    vc_buf_grow(out, strlen(buf) + 1);
    memcpy(out->data + out->len, buf, strlen(buf));
    out->len += strlen(buf);
}
