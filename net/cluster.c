/*
 * net/cluster.c  –  VoidCache cluster client routing.
 */
#ifndef _WIN32
# define _POSIX_C_SOURCE 200809L
# define _GNU_SOURCE
#endif

#include "cluster.h"
#include "vc_ssl_abi.h"

#ifdef _MSC_VER
# include "../compat/msvc.h"
# include "../compat/pthread_win32.h"
#elif defined(_WIN32)
# include "../compat/windows.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <time.h>
#include <inttypes.h>

#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

/* ══════════════════════════════════════════════════════════════
 * CRC16 (Redis slot hash — CCITT polynomial)
 * ══════════════════════════════════════════════════════════════ */

static const uint16_t crc16_tab[256] = {
    0x0000,0x1021,0x2042,0x3063,0x4084,0x50a5,0x60c6,0x70e7,
    0x8108,0x9129,0xa14a,0xb16b,0xc18c,0xd1ad,0xe1ce,0xf1ef,
    0x1231,0x0210,0x3273,0x2252,0x52b5,0x4294,0x72f7,0x62d6,
    0x9339,0x8318,0xb37b,0xa35a,0xd3bd,0xc39c,0xf3ff,0xe3de,
    0x2462,0x3443,0x0420,0x1401,0x64e6,0x74c7,0x44a4,0x5485,
    0xa56a,0xb54b,0x8528,0x9509,0xe5ee,0xf5cf,0xc5ac,0xd58d,
    0x3653,0x2672,0x1611,0x0630,0x76d7,0x66f6,0x5695,0x46b4,
    0xb75b,0xa77a,0x9719,0x8738,0xf7df,0xe7fe,0xd79d,0xc7bc,
    0x4864,0x5845,0x6826,0x7807,0x08e0,0x18c1,0x28a2,0x38c3,
    0xc92c,0xd90d,0xe96e,0xf94f,0x89a8,0x99f9,0xa9ba,0xb99b,
    0x5a65,0x4a44,0x7a27,0x6a06,0x1ae1,0x0ac0,0x3aa3,0x2a82,
    0xdb6d,0xcb4c,0xfb2f,0xeb0e,0x9be9,0x8bc8,0xbbab,0xabda,
    0x6c76,0x7c57,0x4c34,0x5c15,0x2cf2,0x3cd3,0x0cb0,0x1c91,
    0xed7e,0xfd5f,0xcd3c,0xdd1d,0xadfa,0xbddb,0x8db8,0x9d99,
    0x7e67,0x6e46,0x5e25,0x4e04,0x3ee3,0x2ec2,0x1ea1,0x0e80,
    0xff6f,0xef4e,0xdf2d,0xcf0c,0xbfeb,0xafca,0x9fa9,0x8f88,
    0x9188,0x81a9,0xb1ca,0xa1eb,0xd10c,0xc12d,0xf14e,0xe16f,
    0x1080,0x00a1,0x30c2,0x20e3,0x5004,0x4025,0x7046,0x6067,
    0x83b9,0x9398,0xa3fb,0xb3da,0xc33d,0xd31c,0xe37f,0xf35e,
    0x02b1,0x1290,0x22f3,0x32d2,0x4235,0x5214,0x6277,0x7256,
    0xb5ea,0xa5cb,0x95a8,0x8589,0xf56e,0xe54f,0xd52c,0xc50d,
    0x34e2,0x24c3,0x14a0,0x0481,0x7466,0x6447,0x5424,0x4405,
    0xa7db,0xb7fa,0x8799,0x97b8,0xe75f,0xf77e,0xc71d,0xd73c,
    0x26d3,0x36f2,0x0691,0x16b0,0x6657,0x7676,0x4615,0x5634,
    0xd94c,0xc96d,0xf90e,0xe92f,0x99c8,0x89e9,0xb98a,0xa9ab,
    0x5844,0x4865,0x7806,0x6827,0x18c0,0x08e1,0x38a2,0x28a3,
    0xcb7d,0xdb5c,0xeb3f,0xfb1e,0x8bf9,0x9bd8,0xabbb,0xbb9a,
    0x4a75,0x5a54,0x6a37,0x7a16,0x0af1,0x1ad0,0x2ab3,0x3a92,
    0xfd2e,0xed0f,0xdd6c,0xcd4d,0xbdaa,0xad8b,0x9de8,0x8dc9,
    0x7c26,0x6c07,0x5c64,0x4c45,0x3ca2,0x2c83,0x1ce0,0x0cc1,
    0xef1f,0xff3e,0xcf5d,0xdf7c,0xaf9b,0xbfba,0x8fd9,0x9ff8,
    0x6e17,0x7e36,0x4e55,0x5e74,0x2e93,0x3eb2,0x0ed1,0x1ef0,
};

uint16_t vc_crc16(const char *key, size_t len) {
    uint16_t crc = 0;
    for (size_t i = 0; i < len; i++)
        crc = (crc << 8) ^ crc16_tab[((crc >> 8) ^ (uint8_t)key[i]) & 0xFF];
    return crc;
}

uint16_t vc_key_slot(const char *key, size_t len) {
    /* Redis hash tags: {tag} portion only */
    const char *s = memchr(key, '{', len);
    if (s) {
        const char *e = memchr(s + 1, '}', len - (size_t)(s - key) - 1);
        if (e && e > s + 1) {
            key = s + 1;
            len = (size_t)(e - s - 1);
        }
    }
    return vc_crc16(key, len) & (VC_CLUSTER_SLOTS - 1);
}

/* ══════════════════════════════════════════════════════════════
 * LOW-LEVEL SOCKET  (blocking with timeout)
 * ══════════════════════════════════════════════════════════════ */

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int tcp_connect(const char *host, uint16_t port, int timeout_ms) {
    struct addrinfo hints = {0}, *res, *rp;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[8]; snprintf(portstr, sizeof(portstr), "%u", port);

    if (getaddrinfo(host, portstr, &hints, &res) != 0) return -1;

    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
        if (fd < 0) continue;

        /* Set non-blocking for connect with timeout */
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);

        int r = connect(fd, rp->ai_addr, rp->ai_addrlen);
        if (r == 0) { fcntl(fd, F_SETFL, fl); break; }
        if (errno != EINPROGRESS) { close(fd); fd = -1; continue; }

        /* Wait for connect with timeout */
        fd_set wset;
        FD_ZERO(&wset); FD_SET(fd, &wset);
        struct timeval tv = { .tv_sec  = timeout_ms / 1000,
                              .tv_usec = (timeout_ms % 1000) * 1000 };
        r = select(fd + 1, NULL, &wset, NULL, &tv);
        if (r <= 0) { close(fd); fd = -1; continue; }

        int err = 0; socklen_t elen = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen);
        if (err) { close(fd); fd = -1; continue; }

        fcntl(fd, F_SETFL, fl);  /* restore blocking */
        int yes = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
        break;
    }
    freeaddrinfo(res);
    return fd;
}

/* ── Send all bytes with timeout ─────────────────────────────────────────── */
static int sock_sendall(int fd, SSL *ssl, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = ssl ? (ssize_t)SSL_write(ssl, buf, (int)len)
                        : send(fd, buf, len, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        buf += n; len -= (size_t)n;
    }
    return 0;
}

/* ── Read until \r\n or len bytes ─────────────────────────────────────────── */
static ssize_t sock_readline(int fd, SSL *ssl, char *buf, size_t maxlen) {
    size_t pos = 0;
    while (pos < maxlen - 1) {
        char c;
        ssize_t n = ssl ? (ssize_t)SSL_read(ssl, &c, 1)
                        : recv(fd, &c, 1, 0);
        if (n <= 0) return -1;
        buf[pos++] = c;
        if (pos >= 2 && buf[pos-2] == '\r' && buf[pos-1] == '\n') {
            buf[pos-2] = '\0';
            return (ssize_t)pos;
        }
    }
    return -1;
}

/* ── Read exactly n bytes ─────────────────────────────────────────────────── */
static int sock_readn(int fd, SSL *ssl, char *buf, size_t n) {
    while (n > 0) {
        ssize_t r = ssl ? (ssize_t)SSL_read(ssl, buf, (int)n)
                        : recv(fd, buf, n, MSG_WAITALL);
        if (r <= 0) return -1;
        buf += r; n -= (size_t)r;
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 * CONNECTION POOL
 * ══════════════════════════════════════════════════════════════ */

static vc_node_conn_t *node_conn_get(vc_cluster_node_t *node,
                                      vc_cluster_t *cl) {
    pthread_mutex_lock(&node->pool_lock);
    for (int i = 0; i < VC_CLUSTER_MAX_CONNS; i++) {
        if (!node->conns[i].in_use && node->conns[i].fd >= 0) {
            node->conns[i].in_use = true;
            node->conns[i].last_used_ns = now_ns();
            pthread_mutex_unlock(&node->pool_lock);
            return &node->conns[i];
        }
    }
    /* No idle conn — open a new one */
    for (int i = 0; i < VC_CLUSTER_MAX_CONNS; i++) {
        if (node->conns[i].fd < 0) {
            int fd = tcp_connect(node->host, node->port,
                                  VC_CLUSTER_TIMEOUT_MS);
            if (fd < 0) break;

            SSL *ssl_conn = NULL;
            if (cl->ssl_ctx) {
                ssl_conn = SSL_new(cl->ssl_ctx);
                SSL_set_fd(ssl_conn, fd);
                if (SSL_connect(ssl_conn) <= 0) {
                    SSL_free(ssl_conn); close(fd); break;
                }
            }

            node->conns[i].fd       = fd;
            node->conns[i].ssl      = ssl_conn;
            node->conns[i].in_use   = true;
            node->conns[i].last_used_ns = now_ns();

            /* Authenticate if needed */
            if (cl->password[0]) {
                char auth_cmd[256];
                int alen;
                if (cl->username[0])
                    alen = snprintf(auth_cmd, sizeof(auth_cmd),
                        "*3\r\n$4\r\nAUTH\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                        strlen(cl->username), cl->username,
                        strlen(cl->password), cl->password);
                else
                    alen = snprintf(auth_cmd, sizeof(auth_cmd),
                        "*2\r\n$4\r\nAUTH\r\n$%zu\r\n%s\r\n",
                        strlen(cl->password), cl->password);
                sock_sendall(fd, ssl_conn, auth_cmd, (size_t)alen);
                char resp[64]; sock_readline(fd, ssl_conn, resp, sizeof(resp));
                /* +OK expected */
            }

            pthread_mutex_unlock(&node->pool_lock);
            return &node->conns[i];
        }
    }
    pthread_mutex_unlock(&node->pool_lock);
    return NULL;
}

static void node_conn_release(vc_node_conn_t *nc) {
    nc->in_use = false;
}

static void node_conn_close(vc_node_conn_t *nc) {
    if (nc->ssl) { SSL_shutdown(nc->ssl); SSL_free(nc->ssl); nc->ssl = NULL; }
    if (nc->fd >= 0) { close(nc->fd); nc->fd = -1; }
    nc->in_use = false;
}

/* ══════════════════════════════════════════════════════════════
 * RESP3 SIMPLE READER  (synchronous, for cluster commands)
 * ══════════════════════════════════════════════════════════════ */

static char *resp_read_reply(int fd, SSL *ssl, size_t *out_len) {
    char line[1024];
    if (sock_readline(fd, ssl, line, sizeof(line)) <= 0) return NULL;

    char type = line[0];
    const char *rest = line + 1;

    /* Simple string or error */
    if (type == '+' || type == '-') {
        *out_len = strlen(rest);
        return strdup(rest);
    }
    /* Integer */
    if (type == ':') {
        *out_len = strlen(rest);
        return strdup(rest);
    }
    /* Bulk string */
    if (type == '$') {
        int64_t blen = atoll(rest);
        if (blen < 0) { *out_len = 0; return strdup(""); }
        char *buf = malloc((size_t)blen + 3);
        if (!buf) return NULL;
        if (sock_readn(fd, ssl, buf, (size_t)blen + 2) < 0) { free(buf); return NULL; }
        buf[blen] = '\0';
        *out_len = (size_t)blen;
        return buf;
    }
    /* Array — return raw for CLUSTER SLOTS parsing */
    if (type == '*') {
        int64_t count = atoll(rest);
        /* For now, read and discard; return element count as string */
        /* A full RESP3 parser is in proto.c — here we just need PING and AUTH */
        char tmp[32]; snprintf(tmp, sizeof(tmp), "*%" PRId64, count);
        *out_len = strlen(tmp);
        return strdup(tmp);
    }
    return NULL;
}

/* ══════════════════════════════════════════════════════════════
 * CLUSTER TOPOLOGY
 * ══════════════════════════════════════════════════════════════ */

/* Parse "host:port" string */
static bool parse_hostport(const char *s, char *host, size_t hsz,
                            uint16_t *port) {
    const char *col = strrchr(s, ':');
    if (!col) return false;
    size_t hlen = (size_t)(col - s);
    if (hlen >= hsz) return false;
    memcpy(host, s, hlen); host[hlen] = '\0';
    *port = (uint16_t)atoi(col + 1);
    return *port > 0;
}

static int node_find_or_add(vc_cluster_t *cl,
                             const char *host, uint16_t port) {
    for (int i = 0; i < cl->node_count; i++) {
        if (cl->nodes[i].port == port &&
            strcmp(cl->nodes[i].host, host) == 0)
            return i;
    }
    if (cl->node_count >= VC_CLUSTER_MAX_NODES) return -1;
    int idx = cl->node_count++;
    vc_cluster_node_t *n = &cl->nodes[idx];
    strncpy(n->host, host, sizeof(n->host)-1);
    n->port  = port;
    n->alive = true;
    n->backoff_ns = 100000000LL; /* 100ms */
    pthread_mutex_init(&n->pool_lock, NULL);
    for (int i = 0; i < VC_CLUSTER_MAX_CONNS; i++) n->conns[i].fd = -1;
    return idx;
}

/* Refresh slot map via CLUSTER SLOTS (simplified parser) */
int vc_cluster_refresh(vc_cluster_t *cl) {
    /* Find any alive node */
    for (int ni = 0; ni < cl->node_count; ni++) {
        vc_cluster_node_t *node = &cl->nodes[ni];
        if (!node->alive) continue;

        vc_node_conn_t *nc = node_conn_get(node, cl);
        if (!nc) continue;

        const char *cmd = "*2\r\n$7\r\nCLUSTER\r\n$5\r\nSLOTS\r\n";
        if (sock_sendall(nc->fd, nc->ssl, cmd, strlen(cmd)) < 0) {
            node_conn_close(nc); node->alive = false; continue;
        }

        /* Read "*N\r\n" — N slot ranges */
        char line[256];
        if (sock_readline(nc->fd, nc->ssl, line, sizeof(line)) <= 0) {
            node_conn_close(nc); continue;
        }
        if (line[0] != '*') { node_conn_release(nc); continue; }
        int nranges = atoi(line + 1);

        pthread_rwlock_wrlock(&cl->topology_lock);
        memset(cl->slot_map, -1, sizeof(cl->slot_map));

        for (int r = 0; r < nranges; r++) {
            /* Each range: *3 / :start / :end / *2 host port ... */
            char l[256];
            if (sock_readline(nc->fd, nc->ssl, l, sizeof(l)) <= 0) break; /* *3 */
            if (sock_readline(nc->fd, nc->ssl, l, sizeof(l)) <= 0) break;
            int slot_start = atoi(l + 1);
            if (sock_readline(nc->fd, nc->ssl, l, sizeof(l)) <= 0) break;
            int slot_end   = atoi(l + 1);
            if (sock_readline(nc->fd, nc->ssl, l, sizeof(l)) <= 0) break; /* *2 */
            int naddrs = atoi(l + 1);

            int owner = -1;
            for (int a = 0; a < naddrs; a++) {
                char host[64]; uint16_t port;
                if (sock_readline(nc->fd, nc->ssl, l, sizeof(l)) <= 0) break;
                /* *2 or host string */
                char hbuf[256]; size_t hlen = 0;
                if (l[0] == '*') {
                    /* Sub-array [host, port] */
                    char tmp[256];
                    sock_readline(nc->fd, nc->ssl, tmp, sizeof(tmp));
                    hlen = atoll(tmp+1);
                    sock_readn(nc->fd, nc->ssl, hbuf, hlen + 2);
                    hbuf[hlen] = '\0';
                    sock_readline(nc->fd, nc->ssl, tmp, sizeof(tmp));
                    port = (uint16_t)atoll(tmp + 1);
                    snprintf(host, sizeof(host), "%.*s", (int)(sizeof(host)-1), hbuf);
                } else {
                    snprintf(host, sizeof(host), "%.*s", (int)(sizeof(host)-1), l+1);
                    sock_readline(nc->fd, nc->ssl, l, sizeof(l));
                    port = (uint16_t)atoll(l+1);
                }
                int idx = node_find_or_add(cl, host, port);
                if (idx >= 0 && a == 0) owner = idx;
            }

            if (owner >= 0) {
                int s = slot_start > 0 ? slot_start : 0;
                int e = slot_end < VC_CLUSTER_SLOTS ? slot_end : VC_CLUSTER_SLOTS-1;
                for (int s2 = s; s2 <= e; s2++)
                    cl->slot_map[s2] = (int8_t)owner;
            }
        }
        pthread_rwlock_unlock(&cl->topology_lock);
        node_conn_release(nc);
        return 0;
    }
    return -1;
}

/* ══════════════════════════════════════════════════════════════
 * HEALTH CHECK THREAD
 * ══════════════════════════════════════════════════════════════ */

static void *health_thread(void *arg) {
    vc_cluster_t *cl = (vc_cluster_t *)arg;
    while (cl->running) {
        sleep(VC_CLUSTER_PING_INTVL);
        for (int i = 0; i < cl->node_count; i++) {
            vc_cluster_node_t *node = &cl->nodes[i];

            /* Respect backoff */
            if (!node->alive) {
                int64_t now = now_ns();
                if (now - node->last_seen_ns < node->backoff_ns) continue;
                /* Try to reconnect */
            }

            vc_node_conn_t *nc = node_conn_get(node, cl);
            if (!nc) {
                node->alive = false;
                node->backoff_ns = node->backoff_ns < 3200000000LL
                                 ? node->backoff_ns * 2 : 3200000000LL;
                node->last_seen_ns = now_ns();
                fprintf(stderr, "[cluster] node %s:%u DEAD\n",
                        node->host, node->port);
                vc_cluster_refresh(cl);
                continue;
            }

            const char *ping = "*1\r\n$4\r\nPING\r\n";
            if (sock_sendall(nc->fd, nc->ssl, ping, strlen(ping)) < 0) {
                node_conn_close(nc);
                node->alive = false;
                node->backoff_ns = node->backoff_ns < 3200000000LL
                                 ? node->backoff_ns * 2 : 3200000000LL;
                vc_cluster_refresh(cl);
            } else {
                size_t rl = 0;
                char *r = resp_read_reply(nc->fd, nc->ssl, &rl);
                if (r) { free(r); } (void)rl;
                node->alive        = true;
                node->last_seen_ns = now_ns();
                node->backoff_ns   = 100000000LL; /* reset */
                node_conn_release(nc);
            }
        }
    }
    return NULL;
}

/* ══════════════════════════════════════════════════════════════
 * PUBLIC API
 * ══════════════════════════════════════════════════════════════ */

vc_cluster_t *vc_cluster_connect(const char *seeds,
                                  const char *username,
                                  const char *password,
                                  const char *tls_ca) {
    vc_cluster_t *cl = calloc(1, sizeof(vc_cluster_t));
    if (!cl) return NULL;

    pthread_rwlock_init(&cl->topology_lock, NULL);
    memset(cl->slot_map, -1, sizeof(cl->slot_map));
    cl->running = true;

    if (username) strncpy(cl->username, username, sizeof(cl->username)-1);
    if (password) strncpy(cl->password, password, sizeof(cl->password)-1);

    /* TLS client context */
    if (tls_ca) {
        cl->ssl_ctx = SSL_CTX_new(TLS_client_method());
        if (!cl->ssl_ctx) { free(cl); return NULL; }
        SSL_CTX_set_verify(cl->ssl_ctx, SSL_VERIFY_PEER, NULL);
        /* In production: SSL_CTX_load_verify_locations(cl->ssl_ctx, tls_ca, NULL) */
    }

    /* Parse seed list and connect to first available */
    char seeds_copy[1024]; strncpy(seeds_copy, seeds, sizeof(seeds_copy)-1);
    char *tok = strtok(seeds_copy, ",");
    while (tok) {
        char host[64]; uint16_t port;
        if (parse_hostport(tok, host, sizeof(host), &port)) {
            node_find_or_add(cl, host, port);
        }
        tok = strtok(NULL, ",");
    }

    if (cl->node_count == 0) { free(cl); return NULL; }

    /* Initial topology fetch */
    if (vc_cluster_refresh(cl) < 0) {
        /* If only 1 node and no cluster, map all slots to it */
        if (cl->node_count == 1) {
            pthread_rwlock_wrlock(&cl->topology_lock);
            for (int i = 0; i < VC_CLUSTER_SLOTS; i++) cl->slot_map[i] = 0;
            pthread_rwlock_unlock(&cl->topology_lock);
        }
    }

    /* Start health-check thread */
    pthread_create(&cl->health_thread, NULL, health_thread, cl);

    fprintf(stderr, "[cluster] Connected to %d node(s)\n", cl->node_count);
    return cl;
}

void vc_cluster_destroy(vc_cluster_t *cl) {
    if (!cl) return;
    cl->running = false;
    pthread_join(cl->health_thread, NULL);
    for (int i = 0; i < cl->node_count; i++) {
        vc_cluster_node_t *n = &cl->nodes[i];
        for (int j = 0; j < VC_CLUSTER_MAX_CONNS; j++)
            node_conn_close(&n->conns[j]);
        pthread_mutex_destroy(&n->pool_lock);
    }
    if (cl->ssl_ctx) SSL_CTX_free(cl->ssl_ctx);
    pthread_rwlock_destroy(&cl->topology_lock);
    free(cl);
}

int vc_cluster_node_for_slot(vc_cluster_t *cl, uint16_t slot) {
    pthread_rwlock_rdlock(&cl->topology_lock);
    int ni = (int)cl->slot_map[slot];
    pthread_rwlock_unlock(&cl->topology_lock);
    return ni;
}

vc_cluster_err_t vc_cluster_cmd(vc_cluster_t *cl,
                                 const char *key, size_t klen,
                                 const char *cmd, size_t cmd_len,
                                 char **reply, size_t *reply_len) {
    uint16_t slot = key ? vc_key_slot(key, klen) : 0;
    int ni = vc_cluster_node_for_slot(cl, slot);
    if (ni < 0) return VC_CLUSTER_ERR_NONODE;

    for (int attempt = 0; attempt < VC_CLUSTER_MAX_RETRIES; attempt++) {
        if (ni < 0 || ni >= cl->node_count) return VC_CLUSTER_ERR_NONODE;
        vc_cluster_node_t *node = &cl->nodes[ni];
        if (!node->alive) {
            /* Try another node */
            for (int j = 0; j < cl->node_count; j++) {
                if (cl->nodes[j].alive) { ni = j; node = &cl->nodes[j]; break; }
            }
            if (!node->alive) return VC_CLUSTER_ERR_NONODE;
        }

        vc_node_conn_t *nc = node_conn_get(node, cl);
        if (!nc) { node->alive = false; continue; }

        if (sock_sendall(nc->fd, nc->ssl, cmd, cmd_len) < 0) {
            node_conn_close(nc); node->alive = false; continue;
        }

        char *rep = resp_read_reply(nc->fd, nc->ssl, reply_len);
        node_conn_release(nc);

        if (!rep) { node->alive = false; continue; }

        /* Check for MOVED redirection */
        if (strncmp(rep, "MOVED ", 6) == 0) {
            /* MOVED <slot> <host>:<port> */
            char *sp = strchr(rep + 6, ' ');
            if (sp) {
                char host[64]; uint16_t rport;
                if (parse_hostport(sp + 1, host, sizeof(host), &rport)) {
                    ni = node_find_or_add(cl, host, rport);
                    vc_cluster_refresh(cl);
                }
            }
            free(rep);
            continue;
        }

        *reply = rep;
        return VC_CLUSTER_OK;
    }

    return VC_CLUSTER_ERR_NET;
}

int vc_resp_format(char *out, size_t outsz, int argc, ...) {
    va_list ap; va_start(ap, argc);
    int pos = snprintf(out, outsz, "*%d\r\n", argc);
    for (int i = 0; i < argc && pos < (int)outsz; i++) {
        const char *arg = va_arg(ap, const char *);
        size_t      alen = strlen(arg);
        pos += snprintf(out + pos, outsz - (size_t)pos,
                        "$%zu\r\n%s\r\n", alen, arg);
    }
    va_end(ap);
    return pos;
}
