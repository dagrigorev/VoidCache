/*
 * net/commands.c  –  VoidCache command implementations.
 */
#define _POSIX_C_SOURCE 200809L
#ifdef _MSC_VER
# include "../compat/msvc.h"
#endif

#include "commands.h"
#include "proto.h"
#include "auth.h"
#include "../include/voidcache.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <inttypes.h>
#include <strings.h>
#include <math.h>
#include <time.h>

/* ── Convenience macros ──────────────────────────────────────────────────── */
#define WBUF(c)          ((c)->wbuf)
#define ERR(c, code, m)  resp_write_error(WBUF(c), code, m)
#define OK(c)            resp_write_ok(WBUF(c))
#define INT(c, n)        resp_write_integer(WBUF(c), n)
#define BULK(c, s, l)    resp_write_bulk(WBUF(c), s, l)
#define NULLREPLY(c)     resp_write_null_compat(WBUF(c), (c)->resp3)
#define ARGC_CHECK(c, mn, mx) \
    if ((cmd->argc-1) < (mn) || (cmd->argc-1) > (mx)) { \
        ERR(c, "ERR", "wrong number of arguments"); return; }

/* ── Auth guard ──────────────────────────────────────────────────────────── */
static bool need_auth(vcserver_t *srv, vc_conn_t *conn, uint32_t acl) {
    if (!srv->auth.require_auth) return false;
    if (!conn->user) {
        resp_write_error(WBUF(conn), "NOAUTH",
                         "Authentication required. Use AUTH command.");
        return true;
    }
    if (!vc_acl_check(conn->user, acl)) {
        resp_write_error(WBUF(conn), "NOPERM",
                         "Permission denied for this command.");
        return true;
    }
    return false;
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static int64_t mono_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static bool parse_int64(const char *s, int64_t *out) {
    char *end; errno = 0;
    *out = strtoll(s, &end, 10);
    return errno == 0 && end != s && *end == '\0';
}

static bool parse_double(const char *s, double *out) {
    char *end; errno = 0;
    *out = strtod(s, &end);
    return errno == 0 && end != s && *end == '\0';
}

/* ══════════════════════════════════════════════════════════════
 * CONNECTION COMMANDS
 * ══════════════════════════════════════════════════════════════ */

static void cmd_ping(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    (void)srv;
    if (cmd->argc > 1)
        BULK(conn, cmd->argv[1], cmd->argl[1]);
    else
        resp_write_simple(WBUF(conn), "PONG");
}

static void cmd_echo(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    (void)srv;
    ARGC_CHECK(conn, 1, 1);
    BULK(conn, cmd->argv[1], cmd->argl[1]);
}

static void cmd_hello(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    /* HELLO [protover [AUTH username password] [SETNAME name]] */
    int proto = 2;
    if (cmd->argc >= 2) {
        int64_t pv; parse_int64(cmd->argv[1], &pv);
        proto = (int)pv;
        if (proto != 2 && proto != 3) {
            ERR(conn, "NOPROTO", "unsupported protocol version");
            return;
        }
    }

    /* Handle AUTH in HELLO */
    for (int i = 2; i < cmd->argc - 1; i++) {
        if (strcasecmp(cmd->argv[i], "AUTH") == 0 && i + 2 < cmd->argc) {
            const char *uname = cmd->argv[i+1];
            const char *upass = cmd->argv[i+2];
            const vc_user_t *u = vc_auth_verify(&srv->auth,
                uname, strlen(uname), upass, strlen(upass));
            if (!u) {
                ERR(conn, "WRONGPASS", "Invalid username or password");
                return;
            }
            conn->user = u;
            vc_auth_gen_token(conn->token);
            i += 2;
        }

        if (strcasecmp(cmd->argv[i], "SETNAME") == 0 && i + 1 < cmd->argc) {
            snprintf(conn->client_name, sizeof(conn->client_name), "%s", cmd->argv[i+1]);
            i += 1;
            continue;
        }
    }

    conn->resp3 = (proto == 3);
    resp_write_hello(WBUF(conn), VC_SERVER_VERSION, srv->node_id, conn->resp3);
}

static void cmd_auth(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    if (cmd->argc < 2 || cmd->argc > 3) {
        ERR(conn, "ERR", "wrong number of arguments for AUTH"); return;
    }
    const char *uname, *upass;
    size_t uname_len, upass_len;
    if (cmd->argc == 2) {
        /* Legacy: AUTH <password> — use "default" user */
        uname = "default"; uname_len = 7;
        upass = cmd->argv[1]; upass_len = cmd->argl[1];
    } else {
        uname = cmd->argv[1]; uname_len = cmd->argl[1];
        upass = cmd->argv[2]; upass_len = cmd->argl[2];
    }

    if (!srv->auth.require_auth) { OK(conn); return; }

    const vc_user_t *u = vc_auth_verify(&srv->auth, uname, uname_len,
                                         upass, upass_len);
    if (!u) {
        ERR(conn, "WRONGPASS", "Invalid username or password");
        return;
    }
    conn->user = u;
    vc_auth_gen_token(conn->token);
    OK(conn);
}

static void cmd_select(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    (void)srv;
    ARGC_CHECK(conn, 1, 1);
    int64_t idx; parse_int64(cmd->argv[1], &idx);
    if (idx != 0) {
        ERR(conn, "ERR", "VoidCache supports only DB 0 (keyspace is global)");
        return;
    }
    OK(conn);
}

static void cmd_quit(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    (void)srv; (void)cmd;
    OK(conn);
    conn->state = VC_CONN_CLOSING;
}

static void cmd_client(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    (void)srv;

    if (cmd->argc < 2) {
        ERR(conn, "ERR", "wrong number of arguments for CLIENT");
        return;
    }

    const char *sub = cmd->argv[1];

    if (strcasecmp(sub, "SETNAME") == 0) {
        if (cmd->argc != 3) {
            ERR(conn, "ERR", "wrong number of arguments for CLIENT SETNAME");
            return;
        }
        snprintf(conn->client_name, sizeof(conn->client_name), "%s", cmd->argv[2]);
        OK(conn);
        return;
    }

    if (strcasecmp(sub, "GETNAME") == 0) {
        if (conn->client_name[0] == '\0') {
            NULLREPLY(conn);
        } else {
            BULK(conn, conn->client_name, strlen(conn->client_name));
        }
        return;
    }

    if (strcasecmp(sub, "SETINFO") == 0) {
        if (cmd->argc != 4) {
            ERR(conn, "ERR", "wrong number of arguments for CLIENT SETINFO");
            return;
        }

        if (strcasecmp(cmd->argv[2], "LIB-NAME") == 0) {
            snprintf(conn->lib_name, sizeof(conn->lib_name), "%s", cmd->argv[3]);
            OK(conn);
            return;
        }

        if (strcasecmp(cmd->argv[2], "LIB-VER") == 0) {
            snprintf(conn->lib_ver, sizeof(conn->lib_ver), "%s", cmd->argv[3]);
            OK(conn);
            return;
        }

        ERR(conn, "ERR", "unknown CLIENT SETINFO attribute");
        return;
    }

    if (strcasecmp(sub, "ID") == 0) {
        INT(conn, (int64_t)conn->fd);
        return;
    }

    if (strcasecmp(sub, "INFO") == 0) {
        char info[512];
        snprintf(info, sizeof(info),
            "id=%d addr=%s:%u laddr=127.0.0.1:%u name=%s age=0 idle=0 flags=N db=0 sub=0 psub=0 ssub=0 multi=-1 qbuf=0 qbuf-free=0 argv-mem=0 multi-mem=0 rbs=0 rbp=0 obl=0 oll=0 omem=0 tot-mem=0 events=r cmd=client|info user=%s lib-name=%s lib-ver=%s",
            conn->fd,
            conn->peer_addr[0] ? conn->peer_addr : "127.0.0.1",
            conn->peer_port,
            (unsigned)srv->cfg.port,
            conn->client_name[0] ? conn->client_name : "",
            conn->user ? conn->user->username : "default",
            conn->lib_name[0] ? conn->lib_name : "",
            conn->lib_ver[0] ? conn->lib_ver : "");
        BULK(conn, info, strlen(info));
        return;
    }

    ERR(conn, "ERR", "unknown CLIENT subcommand");
}

/* ══════════════════════════════════════════════════════════════
 * CORE KEY COMMANDS
 * ══════════════════════════════════════════════════════════════ */

static void cmd_set(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    if (need_auth(srv, conn, VC_ACL_WRITE)) return;
    /* SET key value [EX sec] [PX ms] [NX] [XX] */
    if (cmd->argc < 3) { ERR(conn, "ERR", "wrong arguments"); return; }

    const char *key = cmd->argv[1]; size_t klen = cmd->argl[1];
    const char *val = cmd->argv[2]; size_t vlen = cmd->argl[2];
    uint32_t ttl = 0;
    bool nx = false, xx = false;

    for (int i = 3; i < cmd->argc; i++) {
        if (strcasecmp(cmd->argv[i], "EX") == 0 && i+1 < cmd->argc) {
            int64_t v; parse_int64(cmd->argv[++i], &v); ttl = (uint32_t)v;
        } else if (strcasecmp(cmd->argv[i], "PX") == 0 && i+1 < cmd->argc) {
            int64_t v; parse_int64(cmd->argv[++i], &v);
            ttl = (uint32_t)(v / 1000);
        } else if (strcasecmp(cmd->argv[i], "NX") == 0) {
            nx = true;
        } else if (strcasecmp(cmd->argv[i], "XX") == 0) {
            xx = true;
        }
    }

    if (nx) {
        /* Only set if key doesn't exist */
        if (vc_get(srv->cache, key, klen, NULL, NULL) == VC_OK) {
            NULLREPLY(conn); return;
        }
    }
    if (xx) {
        /* Only set if key exists */
        if (vc_get(srv->cache, key, klen, NULL, NULL) != VC_OK) {
            NULLREPLY(conn); return;
        }
    }

    vc_err_t err = vc_set(srv->cache, key, klen, val, vlen > 0 ? vlen : 1, ttl);
    if (err != VC_OK) { ERR(conn, "ERR", "storage error"); return; }
    OK(conn);
}

static void cmd_get(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    if (need_auth(srv, conn, VC_ACL_READ)) return;
    ARGC_CHECK(conn, 1, 1);

    const void *val; size_t vlen;
    vc_err_t err = vc_get(srv->cache, cmd->argv[1], cmd->argl[1], &val, &vlen);
    if (err == VC_ERR_NOTFND || err == VC_ERR_EXPIRED) { NULLREPLY(conn); return; }
    if (err != VC_OK) { ERR(conn, "ERR", "get error"); return; }

    /* Check for VoidCache typed payload in stored value */
    const char *v = (const char *)val;
    if (vlen > VC_WIRE_MAGIC_LEN &&
        memcmp(v, VC_WIRE_MAGIC, VC_WIRE_MAGIC_LEN) == 0) {
        /* Pass through typed payload as-is so vcli can decode it */
        BULK(conn, v, vlen);
    } else {
        BULK(conn, v, vlen);
    }
}

static void cmd_del(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    if (need_auth(srv, conn, VC_ACL_WRITE)) return;
    if (cmd->argc < 2) { ERR(conn, "ERR", "wrong arguments"); return; }
    int64_t deleted = 0;
    for (int i = 1; i < cmd->argc; i++) {
        if (vc_del(srv->cache, cmd->argv[i], cmd->argl[i]) == VC_OK)
            deleted++;
    }
    INT(conn, deleted);
}

static void cmd_exists(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    if (need_auth(srv, conn, VC_ACL_READ)) return;
    if (cmd->argc < 2) { ERR(conn, "ERR", "wrong arguments"); return; }
    int64_t found = 0;
    for (int i = 1; i < cmd->argc; i++) {
        if (vc_get(srv->cache, cmd->argv[i], cmd->argl[i], NULL, NULL) == VC_OK)
            found++;
    }
    INT(conn, found);
}

static void cmd_expire(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    if (need_auth(srv, conn, VC_ACL_WRITE)) return;
    ARGC_CHECK(conn, 2, 2);
    /* Re-SET with new TTL: get existing value, set again */
    const void *val; size_t vlen;
    if (vc_get(srv->cache, cmd->argv[1], cmd->argl[1], &val, &vlen) != VC_OK) {
        INT(conn, 0); return;
    }
    int64_t ttl; parse_int64(cmd->argv[2], &ttl);
    /* Make a copy since val points into cache internals */
    void *vcopy = malloc(vlen);
    if (!vcopy) { ERR(conn, "ERR", "OOM"); return; }
    memcpy(vcopy, val, vlen);
    vc_set(srv->cache, cmd->argv[1], cmd->argl[1], vcopy, vlen, (uint32_t)ttl);
    free(vcopy);
    INT(conn, 1);
}

static void cmd_ttl(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    if (need_auth(srv, conn, VC_ACL_READ)) return;
    ARGC_CHECK(conn, 1, 1);
    /* VoidCache doesn't expose per-key TTL query directly; return -1 (no expiry) */
    if (vc_get(srv->cache, cmd->argv[1], cmd->argl[1], NULL, NULL) != VC_OK) {
        INT(conn, -2); /* key does not exist */
        return;
    }
    INT(conn, -1); /* exists, no TTL exposed */
}

static void cmd_pttl(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    cmd_ttl(srv, conn, cmd); /* same as TTL for now */
}

static void cmd_persist(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    if (need_auth(srv, conn, VC_ACL_WRITE)) return;
    ARGC_CHECK(conn, 1, 1);
    /* Re-set with ttl=0 */
    const void *val; size_t vlen;
    if (vc_get(srv->cache, cmd->argv[1], cmd->argl[1], &val, &vlen) != VC_OK) {
        INT(conn, 0); return;
    }
    void *vcopy = malloc(vlen);
    if (!vcopy) { ERR(conn, "ERR", "OOM"); return; }
    memcpy(vcopy, val, vlen);
    vc_set(srv->cache, cmd->argv[1], cmd->argl[1], vcopy, vlen, 0);
    free(vcopy);
    INT(conn, 1);
}

static void cmd_type(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    if (need_auth(srv, conn, VC_ACL_READ)) return;
    ARGC_CHECK(conn, 1, 1);
    const void *val; size_t vlen;
    if (vc_get(srv->cache, cmd->argv[1], cmd->argl[1], &val, &vlen) != VC_OK) {
        resp_write_simple(WBUF(conn), "none"); return;
    }
    const char *v = (const char *)val;
    if (vlen > VC_WIRE_MAGIC_LEN &&
        memcmp(v, VC_WIRE_MAGIC, VC_WIRE_MAGIC_LEN) == 0) {
        uint8_t t = (uint8_t)v[VC_WIRE_MAGIC_LEN];
        switch(t) {
        case VC_TYPE_INT64:   resp_write_simple(WBUF(conn), "integer"); return;
        case VC_TYPE_FLOAT64: resp_write_simple(WBUF(conn), "float"); return;
        case VC_TYPE_BOOL:    resp_write_simple(WBUF(conn), "boolean"); return;
        case VC_TYPE_JSON:    resp_write_simple(WBUF(conn), "json"); return;
        case VC_TYPE_LIST:    resp_write_simple(WBUF(conn), "list"); return;
        case VC_TYPE_HASH:    resp_write_simple(WBUF(conn), "hash"); return;
        case VC_TYPE_BINARY:  resp_write_simple(WBUF(conn), "binary"); return;
        }
    }
    resp_write_simple(WBUF(conn), "string");
}

/* ── MGET / MSET ────────────────────────────────────────────────────────── */
static void cmd_mget(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    if (need_auth(srv, conn, VC_ACL_READ)) return;
    if (cmd->argc < 2) { ERR(conn, "ERR", "wrong arguments"); return; }
    int nkeys = cmd->argc - 1;
    resp_write_array_header(WBUF(conn), nkeys);
    for (int i = 1; i <= nkeys; i++) {
        const void *val; size_t vlen;
        if (vc_get(srv->cache, cmd->argv[i], cmd->argl[i], &val, &vlen) == VC_OK)
            BULK(conn, (const char *)val, vlen);
        else
            NULLREPLY(conn);
    }
}

static void cmd_mset(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    if (need_auth(srv, conn, VC_ACL_WRITE)) return;
    if (cmd->argc < 3 || (cmd->argc % 2) == 0) {
        ERR(conn, "ERR", "wrong number of arguments"); return;
    }
    for (int i = 1; i < cmd->argc; i += 2) {
        vc_set(srv->cache, cmd->argv[i], cmd->argl[i],
               cmd->argv[i+1], cmd->argl[i+1], 0);
    }
    OK(conn);
}

/* ── INCR / DECR family ─────────────────────────────────────────────────── */
static void cmd_incrby(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd,
                       int64_t delta) {
    if (need_auth(srv, conn, VC_ACL_WRITE)) return;
    ARGC_CHECK(conn, 1, 2);
    if (cmd->argc == 3) parse_int64(cmd->argv[2], &delta);

    const void *val; size_t vlen;
    int64_t cur = 0;
    if (vc_get(srv->cache, cmd->argv[1], cmd->argl[1], &val, &vlen) == VC_OK) {
        /* Try to parse as VoidCache int64 first */
        const char *v = (const char *)val;
        if (vlen == VC_WIRE_MAGIC_LEN + 1 + 8 &&
            memcmp(v, VC_WIRE_MAGIC, VC_WIRE_MAGIC_LEN) == 0 &&
            v[VC_WIRE_MAGIC_LEN] == VC_TYPE_INT64) {
            uint8_t *b = (uint8_t *)(v + VC_WIRE_MAGIC_LEN + 1);
            for (int i = 0; i < 8; i++) cur |= ((int64_t)b[i] << (i*8));
        } else {
            char tmp[32]; size_t l = vlen < 31 ? vlen : 31;
            memcpy(tmp, val, l); tmp[l]='\0';
            parse_int64(tmp, &cur);
        }
    }
    cur += delta;
    resp_write_vc_int64(WBUF(conn), cur);
    /* Store back as VoidCache int64 type */
    uint8_t buf[VC_WIRE_MAGIC_LEN + 1 + 8];
    memcpy(buf, VC_WIRE_MAGIC, VC_WIRE_MAGIC_LEN);
    buf[VC_WIRE_MAGIC_LEN] = VC_TYPE_INT64;
    for (int i=0;i<8;i++) buf[VC_WIRE_MAGIC_LEN+1+i]=(uint8_t)(cur>>(i*8));
    vc_set(srv->cache, cmd->argv[1], cmd->argl[1], buf, sizeof(buf), 0);
    INT(conn, cur);
}

static void cmd_incr(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    cmd_incrby(srv, conn, cmd, 1);
}
static void cmd_decr(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    cmd_incrby(srv, conn, cmd, -1);
}
static void cmd_incrby2(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    int64_t d=0; if(cmd->argc==3) parse_int64(cmd->argv[2],&d);
    cmd_incrby(srv, conn, cmd, d);
}
static void cmd_decrby(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    int64_t d=0; if(cmd->argc==3) parse_int64(cmd->argv[2],&d);
    cmd_incrby(srv, conn, cmd, -d);
}

/* ── APPEND ─────────────────────────────────────────────────────────────── */
static void cmd_append(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    if (need_auth(srv, conn, VC_ACL_WRITE)) return;
    ARGC_CHECK(conn, 2, 2);
    const void *val; size_t vlen;
    size_t newlen;
    if (vc_get(srv->cache, cmd->argv[1], cmd->argl[1], &val, &vlen) == VC_OK) {
        size_t alen = cmd->argl[2];
        char *buf = malloc(vlen + alen);
        if (!buf) { ERR(conn, "ERR", "OOM"); return; }
        memcpy(buf, val, vlen);
        memcpy(buf + vlen, cmd->argv[2], alen);
        newlen = vlen + alen;
        vc_set(srv->cache, cmd->argv[1], cmd->argl[1], buf, newlen, 0);
        free(buf);
    } else {
        vc_set(srv->cache, cmd->argv[1], cmd->argl[1],
               cmd->argv[2], cmd->argl[2], 0);
        newlen = cmd->argl[2];
    }
    INT(conn, (int64_t)newlen);
}

/* ── RENAME ─────────────────────────────────────────────────────────────── */
static void cmd_rename(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    if (need_auth(srv, conn, VC_ACL_WRITE)) return;
    ARGC_CHECK(conn, 2, 2);
    const void *val; size_t vlen;
    if (vc_get(srv->cache, cmd->argv[1], cmd->argl[1], &val, &vlen) != VC_OK) {
        ERR(conn, "ERR", "no such key"); return;
    }
    void *vcopy = malloc(vlen);
    if (!vcopy) { ERR(conn, "ERR", "OOM"); return; }
    memcpy(vcopy, val, vlen);
    vc_del(srv->cache, cmd->argv[1], cmd->argl[1]);
    vc_set(srv->cache, cmd->argv[2], cmd->argl[2], vcopy, vlen, 0);
    free(vcopy);
    OK(conn);
}

/* ── KEYS / SCAN / DBSIZE ────────────────────────────────────────────────── */
static void cmd_dbsize(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    (void)cmd;
    if (need_auth(srv, conn, VC_ACL_READ)) return;
    vc_global_stats_t st;
    vc_stats(srv->cache, &st);
    INT(conn, (int64_t)st.total_keys);
}

static void cmd_keys(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    if (need_auth(srv, conn, VC_ACL_READ)) return;
    ARGC_CHECK(conn, 1, 1);
    /* Scan all shards for LIVE entries matching pattern.
     * Pattern: only "*" (match all) is implemented for simplicity. */
    const char *pat = cmd->argv[1];
    bool match_all = (strcmp(pat, "*") == 0);

    /* Two-pass: count then write */
    /* Pass 1: collect matching keys */
#define MAX_KEYS 65536
    char  **kptrs = malloc(MAX_KEYS * sizeof(char*));
    size_t *klens = malloc(MAX_KEYS * sizeof(size_t));
    int nk = 0;
    if (!kptrs || !klens) {
        free(kptrs); free(klens);
        ERR(conn, "ERR", "OOM"); return;
    }

    for (uint32_t si = 0; si < VC_NUM_SHARDS && nk < MAX_KEYS; si++) {
        vc_shard_t *s = &srv->cache->shards[si];
        uint32_t cap = s->cap;
        for (uint32_t j = 0; j < cap && nk < MAX_KEYS; j++) {
            vc_entry_t *e = &s->table[j];
            if (atomic_load(&e->state) != VC_STATE_LIVE) continue;
            /* Extract key */
            const char *k;
            if (e->flags & VC_EFLAG_KEY_LONG) {
                char *kp; memcpy(&kp, e->payload, sizeof(char*)); k = kp;
            } else {
                k = (const char *)e->payload;
            }
            size_t kl = e->key_len;
            if (!match_all) {
                /* Simple prefix/suffix wildcard: skip non-matching */
                /* For production, implement full glob matching */
                (void)k; (void)kl;
            }
            kptrs[nk] = strndup(k, kl);
            klens[nk]  = kl;
            nk++;
        }
    }

    resp_write_array_header(WBUF(conn), nk);
    for (int i = 0; i < nk; i++) {
        BULK(conn, kptrs[i], klens[i]);
        free(kptrs[i]);
    }
    free(kptrs); free(klens);
}

static void cmd_scan(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    if (need_auth(srv, conn, VC_ACL_READ)) return;
    /* SCAN cursor [MATCH pattern] [COUNT count] */
    /* We implement a simple cursor: cursor = shard<<16 | slot */
    int64_t cursor = 0;
    if (cmd->argc >= 2) parse_int64(cmd->argv[1], &cursor);
    int64_t count = 10;
    for (int i = 2; i < cmd->argc-1; i++) {
        if (strcasecmp(cmd->argv[i], "COUNT") == 0)
            parse_int64(cmd->argv[i+1], &count);
    }

    uint32_t shard = (uint32_t)((cursor >> 20) & 0x3F);
    uint32_t slot  = (uint32_t)(cursor & 0xFFFFF);

    char  **kptrs = malloc(count * sizeof(char*));
    size_t *klens = malloc(count * sizeof(size_t));
    int nk = 0;
    int64_t next_cursor = 0;

    for (; shard < VC_NUM_SHARDS && nk < (int)count; shard++) {
        vc_shard_t *s = &srv->cache->shards[shard];
        uint32_t cap = s->cap;
        for (; slot < cap && nk < (int)count; slot++) {
            vc_entry_t *e = &s->table[slot];
            if (atomic_load(&e->state) != VC_STATE_LIVE) continue;
            const char *k;
            if (e->flags & VC_EFLAG_KEY_LONG) {
                char *kp; memcpy(&kp, e->payload, sizeof(char*)); k = kp;
            } else {
                k = (const char *)e->payload;
            }
            kptrs[nk] = strndup(k, e->key_len);
            klens[nk]  = e->key_len;
            nk++;
        }
        if (slot >= cap) slot = 0;
        else { next_cursor = ((int64_t)shard << 20) | slot; break; }
    }
    if (shard >= VC_NUM_SHARDS) next_cursor = 0;

    /* SCAN returns [next_cursor, [keys]] */
    resp_write_array_header(WBUF(conn), 2);
    char cstr[32]; snprintf(cstr, sizeof(cstr), "%lld", (long long)next_cursor);
    BULK(conn, cstr, strlen(cstr));
    resp_write_array_header(WBUF(conn), nk);
    for (int i = 0; i < nk; i++) {
        BULK(conn, kptrs[i], klens[i]);
        free(kptrs[i]);
    }
    free(kptrs); free(klens);
}

/* ── FLUSHDB / FLUSHALL ──────────────────────────────────────────────────── */
static void cmd_flushdb(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    (void)cmd;
    if (need_auth(srv, conn, VC_ACL_ADMIN)) return;
    vc_flush(srv->cache);
    OK(conn);
}

/* ── INFO ────────────────────────────────────────────────────────────────── */
static void cmd_info(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    if (need_auth(srv, conn, VC_ACL_READ)) return;
    (void)cmd;

    vc_global_stats_t st;
    vc_stats(srv->cache, &st);

    char info[4096];
    int n = snprintf(info, sizeof(info),
        "# Server\r\n"
        "server:voidcache\r\n"
        "version:%s\r\n"
        "node_id:%s\r\n"
        "mode:%s\r\n"
        "os:Linux\r\n"
        "arch_bits:64\r\n"
        "\r\n"
        "# Clients\r\n"
        "connected_clients:%llu\r\n"
        "total_connections_received:%llu\r\n"
        "\r\n"
        "# Stats\r\n"
        "total_commands_processed:%llu\r\n"
        "keyspace_hits:%llu\r\n"
        "keyspace_misses:%llu\r\n"
        "evicted_keys:%llu\r\n"
        "expired_keys:%llu\r\n"
        "\r\n"
        "# Keyspace\r\n"
        "db0:keys=%llu\r\n"
        "\r\n"
        "# Memory\r\n"
        "used_memory:%llu\r\n"
        "maxmemory:%llu\r\n",
        VC_SERVER_VERSION,
        srv->node_id,
        srv->cfg.cluster_enabled ? "cluster" : "standalone",
        (unsigned long long)atomic_load(&srv->total_conns),
        (unsigned long long)atomic_load(&srv->total_conns),
        (unsigned long long)atomic_load(&srv->total_commands),
        (unsigned long long)st.hits,
        (unsigned long long)st.misses,
        (unsigned long long)st.evictions,
        (unsigned long long)st.expirations,
        (unsigned long long)st.total_keys,
        (unsigned long long)st.memory_bytes,
        (unsigned long long)srv->cache->max_memory
    );
    BULK(conn, info, (size_t)n);
}

/* ── CONFIG GET (stub for driver compat) ────────────────────────────────── */
static void cmd_config(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    (void)srv;
    if (cmd->argc < 2) { ERR(conn, "ERR", "wrong arguments"); return; }
    if (strcasecmp(cmd->argv[1], "GET") == 0 && cmd->argc >= 3) {
        /* Return empty array — no config params exposed */
        resp_write_array_header(WBUF(conn), 0);
        return;
    }
    if (strcasecmp(cmd->argv[1], "SET") == 0) { OK(conn); return; }
    if (strcasecmp(cmd->argv[1], "RESETSTAT") == 0) { OK(conn); return; }
    ERR(conn, "ERR", "unknown CONFIG subcommand");
}

static void cmd_command(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    (void)srv;
    /*
     * redis-cli and most drivers call COMMAND (or subcommands) on startup
     * to introspect available commands.  Return minimal valid responses so
     * they don't stall waiting for data that will never come.
     *
     * In particular, redis-cli 7+ sends:
     *   COMMAND DOCS <cmd>    → expects %map (RESP3) or *array (RESP2)
     *   COMMAND COUNT         → expects :integer
     *   COMMAND INFO <cmd>    → expects *array of per-command info
     */
    if (cmd->argc < 2) {
        resp_write_array_header(WBUF(conn), 0);
        return;
    }
    const char *sub = cmd->argv[1];
    if (strcasecmp(sub, "COUNT") == 0) {
        INT(conn, 0);
        return;
    }
    if (strcasecmp(sub, "DOCS") == 0) {
        /* RESP3 map or RESP2 empty array — both are zero-element */
        if (conn->resp3) resp_write_map_header(WBUF(conn), 0);
        else             resp_write_array_header(WBUF(conn), 0);
        return;
    }
    if (strcasecmp(sub, "INFO") == 0) {
        int n = cmd->argc - 2;
        if (n <= 0) { resp_write_array_header(WBUF(conn), 0); return; }
        resp_write_array_header(WBUF(conn), n);
        for (int i = 0; i < n; i++)
            resp_write_null_compat(WBUF(conn), conn->resp3);
        return;
    }
    /* LIST, GETKEYS, unknown subcommands */
    resp_write_array_header(WBUF(conn), 0);
}

static void cmd_debug(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    (void)srv; (void)cmd;
    if (need_auth(srv, conn, VC_ACL_ADMIN)) return;
    OK(conn);
}

/* ══════════════════════════════════════════════════════════════
 * VOIDCACHE EXTENDED COMMANDS
 * ══════════════════════════════════════════════════════════════ */

/* VCSET key type value  (type: int|float|bool|json|binary|string) */
static void cmd_vcset(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    if (need_auth(srv, conn, VC_ACL_WRITE)) return;
    if (cmd->argc < 4) { ERR(conn, "ERR", "Usage: VCSET key type value"); return; }

    const char *key   = cmd->argv[1]; size_t klen = cmd->argl[1];
    const char *tname = cmd->argv[2];
    const char *val   = cmd->argv[3]; size_t vlen = cmd->argl[3];

    uint8_t type_byte = VC_TYPE_STRING;
    if      (strcasecmp(tname, "int")    == 0) type_byte = VC_TYPE_INT64;
    else if (strcasecmp(tname, "float")  == 0) type_byte = VC_TYPE_FLOAT64;
    else if (strcasecmp(tname, "bool")   == 0) type_byte = VC_TYPE_BOOL;
    else if (strcasecmp(tname, "json")   == 0) type_byte = VC_TYPE_JSON;
    else if (strcasecmp(tname, "binary") == 0) type_byte = VC_TYPE_BINARY;
    else if (strcasecmp(tname, "string") == 0) type_byte = VC_TYPE_STRING;
    else { ERR(conn, "ERR", "unknown type; use int|float|bool|json|binary|string"); return; }

    /* Encode the payload */
    vc_buf_t *tb = vc_buf_new(VC_WIRE_MAGIC_LEN + 1 + vlen + 16);
    if (!tb) { ERR(conn, "ERR", "OOM"); return; }

    if (type_byte == VC_TYPE_INT64) {
        int64_t iv = 0; parse_int64(val, &iv);
        resp_write_vc_int64(tb, iv);
    } else if (type_byte == VC_TYPE_FLOAT64) {
        double dv = 0; parse_double(val, &dv);
        resp_write_vc_float64(tb, dv);
    } else if (type_byte == VC_TYPE_BOOL) {
        bool bv = (strcasecmp(val,"true")==0 || strcmp(val,"1")==0);
        resp_write_vc_bool(tb, bv);
    } else if (type_byte == VC_TYPE_JSON) {
        resp_write_vc_json(tb, val, vlen);
    } else {
        resp_write_vc_binary(tb, val, vlen);
    }

    /* Extract the bulk-string payload (skip $N\r\n prefix) */
    const char *wire = tb->data;
    size_t wlen = tb->len;
    /* Find start of payload after $N\r\n */
    const char *cr = memchr(wire, '\r', wlen);
    if (cr && (size_t)(cr - wire + 2) < wlen) {
        const char *payload = cr + 2;
        size_t plen = wlen - (size_t)(payload - wire) - 2; /* strip trailing \r\n */
        vc_set(srv->cache, key, klen, payload, plen, 0);
    }
    vc_buf_free(tb);
    OK(conn);
}

/* VCGET key — returns typed value with metadata */
static void cmd_vcget(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    if (need_auth(srv, conn, VC_ACL_READ)) return;
    ARGC_CHECK(conn, 1, 1);

    const void *val; size_t vlen;
    if (vc_get(srv->cache, cmd->argv[1], cmd->argl[1], &val, &vlen) != VC_OK) {
        NULLREPLY(conn); return;
    }

    const char *v = (const char *)val;
    if (vlen > VC_WIRE_MAGIC_LEN &&
        memcmp(v, VC_WIRE_MAGIC, VC_WIRE_MAGIC_LEN) == 0) {
        /* Return as RESP3 map: {type: ..., value: ...} */
        uint8_t t = (uint8_t)v[VC_WIRE_MAGIC_LEN];
        const uint8_t *payload = (const uint8_t *)v + VC_WIRE_MAGIC_LEN + 1;
        size_t plen = vlen - VC_WIRE_MAGIC_LEN - 1;

        resp_write_map_header(WBUF(conn), 2);
        BULK(conn, "type", 4);
        switch(t) {
        case VC_TYPE_INT64: {
            BULK(conn, "int", 3);
            BULK(conn, "value", 5);
            int64_t iv = 0;
            for (int i=0;i<8&&i<(int)plen;i++) iv|=((int64_t)payload[i]<<(i*8));
            INT(conn, iv);
            break;
        }
        case VC_TYPE_FLOAT64: {
            BULK(conn, "float", 5);
            BULK(conn, "value", 5);
            double dv; memcpy(&dv, payload, 8);
            resp_write_double(WBUF(conn), dv);
            break;
        }
        case VC_TYPE_BOOL: {
            BULK(conn, "bool", 4);
            BULK(conn, "value", 5);
            resp_write_bool(WBUF(conn), payload[0] != 0);
            break;
        }
        case VC_TYPE_JSON: {
            BULK(conn, "json", 4);
            BULK(conn, "value", 5);
            BULK(conn, (const char*)payload, plen);
            break;
        }
        default: {
            BULK(conn, "binary", 6);
            BULK(conn, "value", 5);
            BULK(conn, (const char*)payload, plen);
        }}
    } else {
        /* Plain string — return as map with type=string */
        resp_write_map_header(WBUF(conn), 2);
        BULK(conn, "type", 4); BULK(conn, "string", 6);
        BULK(conn, "value", 5); BULK(conn, v, vlen);
    }
}

/* VCINFO — extended server info as JSON */
static void cmd_vcinfo(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    (void)cmd;
    if (need_auth(srv, conn, VC_ACL_READ)) return;
    vc_global_stats_t st; vc_stats(srv->cache, &st);
    char json[2048];
    int n = snprintf(json, sizeof(json),
        "{\"server\":\"voidcache\",\"version\":\"%s\","
        "\"node_id\":\"%s\","
        "\"shards\":%d,\"max_memory\":%llu,"
        "\"keys\":%llu,\"hits\":%llu,\"misses\":%llu,"
        "\"evictions\":%llu,\"memory_bytes\":%llu}",
        VC_SERVER_VERSION, srv->node_id,
        VC_NUM_SHARDS,
        (unsigned long long)srv->cache->max_memory,
        (unsigned long long)st.total_keys,
        (unsigned long long)st.hits,
        (unsigned long long)st.misses,
        (unsigned long long)st.evictions,
        (unsigned long long)st.memory_bytes);
    resp_write_vc_json(WBUF(conn), json, (size_t)n);
}

/* ── CLUSTER commands ────────────────────────────────────────────────────── */
static void cmd_cluster(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    if (cmd->argc < 2) { ERR(conn, "ERR", "wrong arguments"); return; }
    const char *sub = cmd->argv[1];

    if (strcasecmp(sub, "INFO") == 0) {
        char info[512];
        snprintf(info, sizeof(info),
            "cluster_enabled:%d\r\n"
            "cluster_state:ok\r\n"
            "cluster_slots_assigned:16384\r\n"
            "cluster_slots_ok:16384\r\n"
            "cluster_known_nodes:1\r\n"
            "cluster_size:1\r\n",
            srv->cfg.cluster_enabled ? 1 : 0);
        BULK(conn, info, strlen(info));
        return;
    }
    if (strcasecmp(sub, "MYID") == 0) {
        BULK(conn, srv->node_id, strlen(srv->node_id));
        return;
    }
    if (strcasecmp(sub, "NODES") == 0) {
        /* Redis cluster NODES format: id addr flags master ping pong epoch connected slots */
        char line[256];
        const char *addr = srv->cfg.cluster_announce_addr
                           ? srv->cfg.cluster_announce_addr : "127.0.0.1";
        uint16_t port = srv->cfg.cluster_announce_port
                        ? srv->cfg.cluster_announce_port : srv->cfg.port;
        snprintf(line, sizeof(line),
            "%s %s:%u@%u myself,master - 0 0 1 connected 0-16383\n",
            srv->node_id, addr, port, port + 10000);
        BULK(conn, line, strlen(line));
        return;
    }
    if (strcasecmp(sub, "SLOTS") == 0) {
        /* Return single slot range for this node */
        resp_write_array_header(WBUF(conn), 1);
        resp_write_array_header(WBUF(conn), 3);
        INT(conn, 0);        /* slot start */
        INT(conn, 16383);    /* slot end */
        resp_write_array_header(WBUF(conn), 2);
        const char *addr = srv->cfg.cluster_announce_addr
                           ? srv->cfg.cluster_announce_addr : "127.0.0.1";
        BULK(conn, addr, strlen(addr));
        INT(conn, srv->cfg.port);
        return;
    }
    ERR(conn, "ERR", "unknown CLUSTER subcommand");
}

/* ══════════════════════════════════════════════════════════════
 * DISPATCH TABLE
 * ══════════════════════════════════════════════════════════════ */

typedef void (*cmd_handler_t)(vcserver_t *, vc_conn_t *, vc_cmd_t *);

typedef struct {
    const char    *name;
    cmd_handler_t  fn;
    uint32_t       acl_needed;   /* 0 = always allowed (PING/AUTH/HELLO) */
} vc_cmd_entry_t;

static const vc_cmd_entry_t cmd_table[] = {
    /* Connection */
    { "PING",         cmd_ping,     0 },
    { "ECHO",         cmd_echo,     0 },
    { "HELLO",        cmd_hello,    0 },
    { "AUTH",         cmd_auth,     0 },
    { "SELECT",       cmd_select,   0 },
    { "QUIT",         cmd_quit,     0 },
    { "CLIENT",       cmd_client,   0 },
    /* Read */
    { "GET",          cmd_get,      VC_ACL_READ },
    { "MGET",         cmd_mget,     VC_ACL_READ },
    { "EXISTS",       cmd_exists,   VC_ACL_READ },
    { "TTL",          cmd_ttl,      VC_ACL_READ },
    { "PTTL",         cmd_pttl,     VC_ACL_READ },
    { "TYPE",         cmd_type,     VC_ACL_READ },
    { "DBSIZE",       cmd_dbsize,   VC_ACL_READ },
    { "KEYS",         cmd_keys,     VC_ACL_READ },
    { "SCAN",         cmd_scan,     VC_ACL_READ },
    { "INFO",         cmd_info,     VC_ACL_READ },
    /* Write */
    { "SET",          cmd_set,      VC_ACL_WRITE },
    { "MSET",         cmd_mset,     VC_ACL_WRITE },
    { "DEL",          cmd_del,      VC_ACL_WRITE },
    { "EXPIRE",       cmd_expire,   VC_ACL_WRITE },
    { "PERSIST",      cmd_persist,  VC_ACL_WRITE },
    { "RENAME",       cmd_rename,   VC_ACL_WRITE },
    { "APPEND",       cmd_append,   VC_ACL_WRITE },
    { "INCR",         cmd_incr,     VC_ACL_WRITE },
    { "DECR",         cmd_decr,     VC_ACL_WRITE },
    { "INCRBY",       cmd_incrby2,  VC_ACL_WRITE },
    { "DECRBY",       cmd_decrby,   VC_ACL_WRITE },
    /* Admin */
    { "FLUSHDB",      cmd_flushdb,  VC_ACL_ADMIN },
    { "FLUSHALL",     cmd_flushdb,  VC_ACL_ADMIN },
    { "CONFIG",       cmd_config,   VC_ACL_ADMIN },
    { "DEBUG",        cmd_debug,    VC_ACL_ADMIN },
    { "COMMAND",      cmd_command,  0 },
    /* Cluster */
    { "CLUSTER",      cmd_cluster,  0 },
    /* VoidCache extended */
    { "VCSET",        cmd_vcset,    VC_ACL_WRITE },
    { "VCGET",        cmd_vcget,    VC_ACL_READ  },
    { "VCINFO",       cmd_vcinfo,   VC_ACL_READ  },
};

#define CMD_TABLE_SIZE (sizeof(cmd_table) / sizeof(cmd_table[0]))

void vc_dispatch(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd) {
    if (cmd->argc == 0) return;

    for (size_t i = 0; i < CMD_TABLE_SIZE; i++) {
        if (strcasecmp(cmd->argv[0], cmd_table[i].name) == 0) {
            /* Auth check for guarded commands */
            if (cmd_table[i].acl_needed && need_auth(srv, conn, cmd_table[i].acl_needed))
                return;
            cmd_table[i].fn(srv, conn, cmd);
            atomic_fetch_add(&srv->total_commands, 1);
            return;
        }
    }
    resp_write_error(WBUF(conn), "ERR",
                     "unknown command — see https://voidcache.io/commands");
}
