/*
 * net/proto.h  –  RESP3 protocol parser/writer + VoidCache extended types.
 *
 * ── RESP3 compatibility ──────────────────────────────────────────────────────
 * Standard Redis RESP3 types are supported verbatim so any Redis driver works
 * out-of-the-box.  VoidCache extensions are layered on top via the RESP3 Blob
 * Error / Verbatim String / Big Number types that Redis drivers pass through
 * transparently as opaque byte strings.
 *
 * ── VoidCache Extended Types (sent as RESP3 Bulk String with type header) ───
 *   Wire format:   *VC1\r\n<type_byte><payload_bytes>
 *
 *   Type byte:
 *     'i'  – int64   (8 bytes LE)
 *     'd'  – float64 (8 bytes LE, IEEE 754)
 *     'b'  – bool    (1 byte: 0x00 or 0x01)
 *     'j'  – JSON    (UTF-8 text, arbitrary length)
 *     'l'  – List    (RESP3 array of bulk strings, nested)
 *     'h'  – Hash    (RESP3 map, nested)
 *     'z'  – ZSet    (sorted set: RESP3 array of [member score] pairs)
 *     's'  – Stream  (sequence of [id field value ...] entries)
 *     'B'  – Binary  (raw bytes with no text assumption)
 *     't'  – Typed   (self-describing: <2B type_tag><payload>)
 *
 * Standard Redis commands (GET, SET, DEL, EXPIRE, TTL, PING, AUTH, SELECT,
 * KEYS, DBSIZE, FLUSHDB, INFO, CLIENT, CLUSTER) all work as-is.
 * New VoidCache commands extend the command set (see commands.h).
 * ─────────────────────────────────────────────────────────────────────────── */
#pragma once
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
# define _POSIX_C_SOURCE 200809L
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

/* ── RESP3 type bytes ─────────────────────────────────────────────────────── */
#define RESP_SIMPLE_STRING  '+'   /* +OK\r\n                             */
#define RESP_SIMPLE_ERROR   '-'   /* -ERR message\r\n                    */
#define RESP_INTEGER        ':'   /* :42\r\n                             */
#define RESP_BULK_STRING    '$'   /* $6\r\nfoobar\r\n                   */
#define RESP_ARRAY          '*'   /* *3\r\n...                           */
#define RESP_NULL           '_'   /* _\r\n                               */
#define RESP_BOOL           '#'   /* #t\r\n / #f\r\n                    */
#define RESP_DOUBLE         ','   /* ,3.14\r\n                           */
#define RESP_BIG_NUMBER     '('   /* (1234567890\r\n                     */
#define RESP_BLOB_ERROR     '!'   /* !21\r\nSYNTAX invalid cmd\r\n       */
#define RESP_VERBATIM       '='   /* =15\r\ntxt:hello world\r\n          */
#define RESP_MAP            '%'   /* %2\r\n<key><val><key><val>           */
#define RESP_SET_TYPE       '~'   /* ~3\r\n...                           */
#define RESP_PUSH           '>'   /* >3\r\n...  (server push)            */
#define RESP_HELLO          'H'   /* HELLO negotiation                   */

/* ── VoidCache type prefix (inside bulk string payload) ─────────────────── */
#define VC_WIRE_MAGIC       "*VC1"
#define VC_WIRE_MAGIC_LEN   4

#define VC_TYPE_INT64       'i'
#define VC_TYPE_FLOAT64     'd'
#define VC_TYPE_BOOL        'b'
#define VC_TYPE_JSON        'j'
#define VC_TYPE_LIST        'l'
#define VC_TYPE_HASH        'h'
#define VC_TYPE_ZSET        'z'
#define VC_TYPE_STREAM      's'
#define VC_TYPE_BINARY      'B'
#define VC_TYPE_STRING      'S'   /* explicit string (default for plain SET) */

/* ── I/O buffer ──────────────────────────────────────────────────────────── */
#define VC_IOBUF_SIZE       (256 * 1024)   /* 256 KB read/write buffer    */
#define VC_MAX_INLINE_ARG   (64 * 1024)    /* max single argument         */
#define VC_MAX_ARGS         128            /* max args per command         */
#define VC_MAX_CMD_LEN      64             /* max command name length      */

typedef struct {
    char   *data;
    size_t  cap;
    size_t  len;     /* bytes used */
    size_t  rpos;    /* read cursor */
} vc_buf_t;

vc_buf_t *vc_buf_new(size_t cap);
void      vc_buf_free(vc_buf_t *b);
int       vc_buf_grow(vc_buf_t *b, size_t need);
void      vc_buf_reset(vc_buf_t *b);
void      vc_buf_consume(vc_buf_t *b, size_t n);

/* ── parsed RESP3 value ───────────────────────────────────────────────────── */
typedef enum {
    VC_VAL_NONE = 0,
    VC_VAL_STRING,       /* simple string or bulk string                */
    VC_VAL_ERROR,        /* simple or blob error                        */
    VC_VAL_INTEGER,      /* :N                                          */
    VC_VAL_DOUBLE,       /* ,D                                          */
    VC_VAL_BOOL,         /* #t/#f                                       */
    VC_VAL_NULL,         /* _                                           */
    VC_VAL_ARRAY,        /* *N — array of vc_val_t                      */
    VC_VAL_MAP,          /* %N — alternating key/val pairs              */
    VC_VAL_SET,          /* ~N                                          */
    VC_VAL_PUSH,         /* >N                                          */
    /* VoidCache extended */
    VC_VAL_INT64,        /* typed int64                                 */
    VC_VAL_FLOAT64,      /* typed float64                               */
    VC_VAL_JSON,         /* JSON text                                   */
    VC_VAL_BINARY,       /* raw binary                                  */
} vc_val_type_t;

typedef struct vc_val {
    vc_val_type_t type;
    union {
        struct { char *ptr; size_t len; } str;   /* string/error/json/binary */
        int64_t                           i64;
        double                            f64;
        bool                              boolean;
        struct { struct vc_val **elems; size_t count; } arr;
    } v;
} vc_val_t;

void vc_val_free(vc_val_t *v);

/* ── RESP3 parser ─────────────────────────────────────────────────────────── */
typedef enum {
    RESP_PARSE_OK    =  0,
    RESP_PARSE_MORE  =  1,   /* need more data                          */
    RESP_PARSE_ERR   = -1,   /* protocol error                          */
} resp_parse_result_t;

/* Parsed command: array of bulk strings (the standard Redis command format) */
typedef struct {
    char   *argv[VC_MAX_ARGS];   /* NUL-terminated argument strings     */
    size_t  argl[VC_MAX_ARGS];   /* byte lengths (args may be binary)   */
    int     argc;
    uint8_t vc_type;             /* 0 or VC_TYPE_* if VC-extended SET   */
} vc_cmd_t;

/*
 * resp_parse_command – parse one RESP3 command from buf into cmd.
 * Returns RESP_PARSE_OK on success, RESP_PARSE_MORE if incomplete,
 * RESP_PARSE_ERR on protocol error.
 * On OK, *consumed is set to the number of bytes consumed from buf.
 */
resp_parse_result_t resp_parse_command(const char *buf, size_t len,
                                       vc_cmd_t *cmd, size_t *consumed);
void vc_cmd_free(vc_cmd_t *cmd);

/* ── RESP3 writers (append to vc_buf_t) ─────────────────────────────────── */
int resp_write_ok(vc_buf_t *b);
int resp_write_null(vc_buf_t *b);
int resp_write_error(vc_buf_t *b, const char *code, const char *msg);
int resp_write_simple(vc_buf_t *b, const char *s);
int resp_write_integer(vc_buf_t *b, int64_t n);
int resp_write_double(vc_buf_t *b, double d);
int resp_write_bool(vc_buf_t *b, bool v);
int resp_write_bulk(vc_buf_t *b, const char *data, size_t len);
int resp_write_array_header(vc_buf_t *b, int count);
int resp_write_map_header(vc_buf_t *b, int pairs);

/* VoidCache typed value writers */
int resp_write_vc_int64(vc_buf_t *b, int64_t v);
int resp_write_vc_float64(vc_buf_t *b, double v);
int resp_write_vc_bool(vc_buf_t *b, bool v);
int resp_write_vc_json(vc_buf_t *b, const char *json, size_t len);
int resp_write_vc_binary(vc_buf_t *b, const void *data, size_t len);

/* HELLO response (RESP3 handshake) */
int resp_write_hello(vc_buf_t *b, const char *server_ver, const char *node_id);
