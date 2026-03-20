/*
 * net/proto.c  –  RESP3 protocol parser / writer implementation.
 */
#define _POSIX_C_SOURCE 200809L
#ifdef _MSC_VER
# include "../compat/msvc.h"
#endif

#include <stdarg.h>
#include <inttypes.h>
#include "proto.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <math.h>

/* ══════════════════════════════════════════════════════════════
 * I/O BUFFER
 * ══════════════════════════════════════════════════════════════ */

vc_buf_t *vc_buf_new(size_t cap) {
    vc_buf_t *b = calloc(1, sizeof(vc_buf_t));
    if (!b) return NULL;
    b->data = malloc(cap);
    if (!b->data) { free(b); return NULL; }
    b->cap = cap;
    return b;
}

void vc_buf_free(vc_buf_t *b) {
    if (!b) return;
    free(b->data);
    free(b);
}

int vc_buf_grow(vc_buf_t *b, size_t need) {
    if (b->cap - b->len >= need) return 0;
    size_t nc = b->cap * 2;
    while (nc - b->len < need) nc *= 2;
    char *nd = realloc(b->data, nc);
    if (!nd) return -1;
    b->data = nd;
    b->cap  = nc;
    return 0;
}

void vc_buf_reset(vc_buf_t *b) {
    b->len = b->rpos = 0;
}

void vc_buf_consume(vc_buf_t *b, size_t n) {
    if (n >= b->len) { vc_buf_reset(b); return; }
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
    if (b->rpos >= n) b->rpos -= n; else b->rpos = 0;
}

/* ══════════════════════════════════════════════════════════════
 * vc_val helpers
 * ══════════════════════════════════════════════════════════════ */

void vc_val_free(vc_val_t *v) {
    if (!v) return;
    switch (v->type) {
    case VC_VAL_STRING:
    case VC_VAL_ERROR:
    case VC_VAL_JSON:
    case VC_VAL_BINARY:
        free(v->v.str.ptr);
        break;
    case VC_VAL_ARRAY:
    case VC_VAL_MAP:
    case VC_VAL_SET:
    case VC_VAL_PUSH:
        for (size_t i = 0; i < v->v.arr.count; i++)
            vc_val_free(v->v.arr.elems[i]);
        free(v->v.arr.elems);
        break;
    default: break;
    }
    free(v);
}

/* ══════════════════════════════════════════════════════════════
 * RESP3 COMMAND PARSER
 * ══════════════════════════════════════════════════════════════ */

/* Find \r\n in buffer; return pointer or NULL */
static const char *find_crlf(const char *buf, size_t len) {
    for (size_t i = 0; i + 1 < len; i++)
        if (buf[i] == '\r' && buf[i+1] == '\n') return buf + i;
    return NULL;
}

/* Parse a decimal int64 from the line starting at buf (before \r\n).
 * Returns 0 on success, -1 on error. */
static int parse_int64(const char *buf, size_t len, int64_t *out) {
    if (len == 0) return -1;
    char tmp[32];
    if (len >= sizeof(tmp)) return -1;
    memcpy(tmp, buf, len);
    tmp[len] = '\0';
    char *end;
    errno = 0;
    *out = (int64_t)strtoll(tmp, &end, 10);
    return (errno || end != tmp + len) ? -1 : 0;
}

/*
 * resp_parse_command – parse one RESP3 *N\r\n array of bulk strings.
 *
 * Handles both RESP2 (*N inline-array) and inline command format.
 * VoidCache extended types: if an argument's bulk-string payload starts
 * with VC_WIRE_MAGIC ("*VC1"), the following byte is the vc_type and
 * the remainder is the value payload.  We set cmd->vc_type and strip
 * the header so cmd->argv[i] contains only the raw payload.
 */
resp_parse_result_t resp_parse_command(const char *buf, size_t len,
                                       vc_cmd_t *cmd, size_t *consumed) {
    if (len == 0) return RESP_PARSE_MORE;

    memset(cmd, 0, sizeof(*cmd));
    const char *p   = buf;
    const char *end = buf + len;

#define NEED(n) do { if ((size_t)(end - p) < (size_t)(n)) return RESP_PARSE_MORE; } while(0)

    /* ── Inline command (telnet-friendly, handles \r\n and bare \n) ── */
    if (*p != '*') {
        /* Find end of line — accept both \r\n and bare \n */
        const char *nl = NULL;
        size_t nl_skip = 2;  /* bytes to skip past the line ending */
        for (const char *q = p; q < end; q++) {
            if (*q == '\r' && q + 1 < end && *(q+1) == '\n') {
                nl = q; nl_skip = 2; break;
            }
            if (*q == '\n') {
                nl = q; nl_skip = 1; break;
            }
        }
        if (!nl) return RESP_PARSE_MORE;
        /* Split on spaces/tabs */
        const char *s = p;
        while (s < nl && cmd->argc < VC_MAX_ARGS) {
            while (s < nl && (*s == ' ' || *s == '\t')) s++;
            if (s >= nl) break;
            const char *e2 = s;
            while (e2 < nl && *e2 != ' ' && *e2 != '\t') e2++;
            size_t alen = (size_t)(e2 - s);
            cmd->argv[cmd->argc] = strndup(s, alen);
            cmd->argl[cmd->argc] = alen;
            cmd->argc++;
            s = e2;
        }
        *consumed = (size_t)(nl - buf) + nl_skip;
        return (cmd->argc > 0) ? RESP_PARSE_OK : RESP_PARSE_ERR;
    }

    /* ── RESP3 array *N\r\n ── */
    NEED(4); /* *N\r\n minimum */
    const char *crlf = find_crlf(p, (size_t)(end - p));
    if (!crlf) return RESP_PARSE_MORE;

    int64_t argc;
    if (parse_int64(p + 1, (size_t)(crlf - p - 1), &argc) < 0)
        return RESP_PARSE_ERR;
    if (argc < 0 || argc > VC_MAX_ARGS) return RESP_PARSE_ERR;

    p = crlf + 2;

    for (int i = 0; i < (int)argc; i++) {
        /* Each element must be a bulk string $N\r\n<data>\r\n */
        NEED(3);
        if (*p != '$') return RESP_PARSE_ERR;
        const char *cl2 = find_crlf(p, (size_t)(end - p));
        if (!cl2) return RESP_PARSE_MORE;

        int64_t blen;
        if (parse_int64(p + 1, (size_t)(cl2 - p - 1), &blen) < 0)
            return RESP_PARSE_ERR;
        if (blen < 0 || blen > VC_MAX_INLINE_ARG) return RESP_PARSE_ERR;

        p = cl2 + 2;
        NEED((size_t)blen + 2);

        /* Check for VoidCache extended type header */
        uint8_t vc_type = 0;
        const char *payload = p;
        size_t plen = (size_t)blen;

        if (plen > VC_WIRE_MAGIC_LEN &&
            memcmp(payload, VC_WIRE_MAGIC, VC_WIRE_MAGIC_LEN) == 0) {
            vc_type   = (uint8_t)payload[VC_WIRE_MAGIC_LEN];
            payload  += VC_WIRE_MAGIC_LEN + 1;
            plen     -= VC_WIRE_MAGIC_LEN + 1;
            if (i == 1) cmd->vc_type = vc_type; /* value arg */
        }

        cmd->argv[i] = malloc(plen + 1);
        if (!cmd->argv[i]) { vc_cmd_free(cmd); return RESP_PARSE_ERR; }
        memcpy(cmd->argv[i], payload, plen);
        cmd->argv[i][plen] = '\0';
        cmd->argl[i]       = plen;

        p += blen + 2; /* skip data + \r\n */
    }
    cmd->argc = (int)argc;

    /* Uppercase the command name in-place */
    for (size_t j = 0; j < cmd->argl[0] && j < VC_MAX_CMD_LEN; j++) {
        char c = cmd->argv[0][j];
        if (c >= 'a' && c <= 'z') cmd->argv[0][j] = c - 32;
    }

    *consumed = (size_t)(p - buf);
    return RESP_PARSE_OK;

#undef NEED
}

void vc_cmd_free(vc_cmd_t *cmd) {
    for (int i = 0; i < cmd->argc; i++) {
        free(cmd->argv[i]);
        cmd->argv[i] = NULL;
    }
    cmd->argc = 0;
}

/* ══════════════════════════════════════════════════════════════
 * RESP3 WRITERS
 * ══════════════════════════════════════════════════════════════ */

#define APPEND(b, s, n) do { \
    if (vc_buf_grow((b), (n)) < 0) return -1; \
    memcpy((b)->data + (b)->len, (s), (n)); \
    (b)->len += (n); \
} while(0)

#define APPENDZ(b, s) APPEND(b, s, strlen(s))

static int buf_printf(vc_buf_t *b, const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return -1;
    APPEND(b, tmp, (size_t)n);
    return 0;
}

/* Suppress variadic macro warning for buf_printf */

int resp_write_ok(vc_buf_t *b) {
    APPENDZ(b, "+OK\r\n");
    return 0;
}

int resp_write_null(vc_buf_t *b) {
    APPENDZ(b, "_\r\n");
    return 0;
}

/* RESP2-compatible null: $-1 for RESP2, _ for RESP3.
 * Sending RESP3 _ to a RESP2 client (e.g. redis-cli) causes it to stall. */
int resp_write_null_compat(vc_buf_t *b, bool resp3) {
    if (resp3) APPENDZ(b, "_\r\n");
    else        APPENDZ(b, "$-1\r\n");
    return 0;
}

int resp_write_error(vc_buf_t *b, const char *code, const char *msg) {
    return buf_printf(b, "-%s %s\r\n", code, msg);
}

int resp_write_simple(vc_buf_t *b, const char *s) {
    return buf_printf(b, "+%s\r\n", s);
}

int resp_write_integer(vc_buf_t *b, int64_t n) {
    return buf_printf(b, ":%" PRId64 "\r\n", n);
}

int resp_write_double(vc_buf_t *b, double d) {
    return buf_printf(b, ",%g\r\n", d);
}

int resp_write_bool(vc_buf_t *b, bool v) {
    return buf_printf(b, "#%c\r\n", v ? 't' : 'f');
}

int resp_write_bulk(vc_buf_t *b, const char *data, size_t len) {
    if (buf_printf(b, "$%zu\r\n", len) < 0) return -1;
    APPEND(b, data, len);
    APPENDZ(b, "\r\n");
    return 0;
}

int resp_write_array_header(vc_buf_t *b, int count) {
    return buf_printf(b, "*%d\r\n", count);
}

int resp_write_map_header(vc_buf_t *b, int pairs) {
    return buf_printf(b, "%%%d\r\n", pairs);
}

/* ── VoidCache typed writers ─────────────────────────────────────────────── */

/* Encode: $<total_len>\r\n*VC1<type_byte><payload>\r\n */
static int resp_write_vc_typed(vc_buf_t *b, uint8_t type,
                                const void *payload, size_t plen) {
    size_t total = VC_WIRE_MAGIC_LEN + 1 + plen;
    if (buf_printf(b, "$%zu\r\n", total) < 0) return -1;
    APPENDZ(b, VC_WIRE_MAGIC);
    APPEND(b, &type, 1);
    APPEND(b, payload, plen);
    APPENDZ(b, "\r\n");
    return 0;
}

int resp_write_vc_int64(vc_buf_t *b, int64_t v) {
    uint8_t buf8[8];
    /* little-endian */
    for (int i = 0; i < 8; i++) buf8[i] = (uint8_t)(v >> (i * 8));
    return resp_write_vc_typed(b, VC_TYPE_INT64, buf8, 8);
}

int resp_write_vc_float64(vc_buf_t *b, double v) {
    uint8_t buf8[8];
    memcpy(buf8, &v, 8);
    return resp_write_vc_typed(b, VC_TYPE_FLOAT64, buf8, 8);
}

int resp_write_vc_bool(vc_buf_t *b, bool v) {
    uint8_t byte = v ? 0x01 : 0x00;
    return resp_write_vc_typed(b, VC_TYPE_BOOL, &byte, 1);
}

int resp_write_vc_json(vc_buf_t *b, const char *json, size_t len) {
    return resp_write_vc_typed(b, VC_TYPE_JSON, json, len);
}

int resp_write_vc_binary(vc_buf_t *b, const void *data, size_t len) {
    return resp_write_vc_typed(b, VC_TYPE_BINARY, data, len);
}

/* ── HELLO (RESP3 handshake) ─────────────────────────────────────────────── */
int resp_write_hello(vc_buf_t *b, const char *server_ver, const char *node_id,
                     bool resp3) {
    /*
     * HELLO response must match the negotiated protocol version:
     *   RESP3 (proto=3): %map header  — redis-cli and drivers expect this.
     *   RESP2 (proto=2): *array header — RESP2 clients cannot parse %map
     *                    and will stall reading the response.
     */
    int pairs = 5;
    if (resp3) {
        if (resp_write_map_header(b, pairs) < 0) return -1;
    } else {
        if (resp_write_array_header(b, pairs * 2) < 0) return -1;
    }
    resp_write_bulk(b, "server", 6);
    resp_write_bulk(b, "voidcache", 9);
    resp_write_bulk(b, "version", 7);
    resp_write_bulk(b, server_ver, strlen(server_ver));
    resp_write_bulk(b, "proto", 5);
    resp_write_integer(b, resp3 ? 3 : 2);
    resp_write_bulk(b, "id", 2);
    resp_write_bulk(b, node_id, strlen(node_id));
    resp_write_bulk(b, "mode", 4);
    resp_write_bulk(b, "standalone", 10);
    return 0;
}
