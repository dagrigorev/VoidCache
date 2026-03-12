/*
 * cli/vcli.c  –  VoidCache CLI  (vcli)
 *
 * Redis-compatible interactive client with VoidCache extensions:
 *   - RESP3 type-aware display (int, float, bool, json, binary)
 *   - Connects to single node or cluster (auto-detects CLUSTER NODES)
 *   - TLS support via --tls / --cert / --key
 *   - Auth: --user / --pass (or AUTH command inline)
 *   - Colorized output, timing, pipe mode
 *
 * Usage:
 *   vcli [options] [command [args...]]
 *
 *   -h <host>         server hostname (default: 127.0.0.1)
 *   -p <port>         server port     (default: 6379)
 *   -a <password>     password
 *   -u <username>     username (for ACL AUTH)
 *   --tls             use TLS
 *   --cacert <file>   CA cert for TLS verification
 *   -n <db>           select DB (always 0 in VoidCache)
 *   --no-auth         skip auth even if requirepass set
 *   --cluster         enable cluster routing (multi-node)
 *   --latency         print per-command latency
 *   --pipe            read commands from stdin (batch mode)
 *   --raw             print raw bytes, no type decoration
 */
#ifndef _WIN32
# define _POSIX_C_SOURCE 200809L
#endif
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <inttypes.h>

#ifdef _MSC_VER
# include "../compat/msvc.h"
#elif defined(_WIN32)
# include "../compat/windows.h"
#endif
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "../net/vc_ssl_abi.h"
#include "../net/proto.h"
#include "../net/cluster.h"
#include "../net/server.h"

/* ══════════════════════════════════════════════════════════════
 * ANSI colors
 * ══════════════════════════════════════════════════════════════ */
#define COL_RESET   "\x1b[0m"
#define COL_RED     "\x1b[31m"
#define COL_GREEN   "\x1b[32m"
#define COL_YELLOW  "\x1b[33m"
#define COL_BLUE    "\x1b[34m"
#define COL_MAGENTA "\x1b[35m"
#define COL_CYAN    "\x1b[36m"
#define COL_BOLD    "\x1b[1m"
#define COL_DIM     "\x1b[2m"

static bool use_color = true;
static bool raw_mode  = false;
static bool show_latency = false;

#define COLOR(c, s) (use_color ? c s COL_RESET : s)

/* ══════════════════════════════════════════════════════════════
 * CONNECTION
 * ══════════════════════════════════════════════════════════════ */

typedef struct {
    int      fd;
    SSL     *ssl;
    SSL_CTX *ssl_ctx;
    bool     resp3;
    bool     cluster_mode;
    vc_cluster_t *cluster;
    char     host[256];
    uint16_t port;
} vcli_conn_t;

static int64_t cli_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int cli_tcp_connect(const char *host, uint16_t port) {
    struct addrinfo hints = {0}, *res;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[8]; snprintf(portstr, sizeof(portstr), "%u", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0) {
        fprintf(stderr, "Could not resolve %s\n", host); return -1;
    }
    int fd = socket(res->ai_family, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        fprintf(stderr, "Could not connect to %s:%u: %s\n",
                host, port, strerror(errno));
        close(fd); freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res);
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    return fd;
}

/* ══════════════════════════════════════════════════════════════
 * I/O (TLS-aware)
 * ══════════════════════════════════════════════════════════════ */

static int cli_sendall(vcli_conn_t *c, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = c->ssl ? (ssize_t)SSL_write(c->ssl, buf, (int)len)
                           : send(c->fd, buf, len, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        buf += n; len -= (size_t)n;
    }
    return 0;
}

static ssize_t cli_recv(vcli_conn_t *c, char *buf, size_t len) {
    return c->ssl ? (ssize_t)SSL_read(c->ssl, buf, (int)len)
                 : recv(c->fd, buf, len, 0);
}

/* ══════════════════════════════════════════════════════════════
 * RESP3 PRINTER  (recursive, colorized)
 * ══════════════════════════════════════════════════════════════ */

/* Read a full RESP3 reply from the connection, print it, return 0 ok / -1 err */
static int print_reply(vcli_conn_t *c, int depth);

static ssize_t read_line(vcli_conn_t *c, char *buf, size_t maxlen) {
    size_t pos = 0;
    while (pos < maxlen - 1) {
        char ch;
        ssize_t n = cli_recv(c, &ch, 1);
        if (n <= 0) return -1;
        buf[pos++] = ch;
        if (pos >= 2 && buf[pos-2] == '\r' && buf[pos-1] == '\n') {
            buf[pos-2] = '\0';
            return (ssize_t)pos;
        }
    }
    return -1;
}

static int read_exactly(vcli_conn_t *c, char *buf, size_t n) {
    while (n > 0) {
        ssize_t r = cli_recv(c, buf, n);
        if (r <= 0) return -1;
        buf += r; n -= (size_t)r;
    }
    return 0;
}

static void print_indent(int depth) {
    for (int i = 0; i < depth; i++) printf("   ");
}

/* Print a VoidCache extended typed value (inside a bulk string) */
static void print_vc_typed(const char *payload, size_t plen) {
    if (plen < VC_WIRE_MAGIC_LEN + 1) {
        printf("%.*s", (int)plen, payload); return;
    }
    if (memcmp(payload, VC_WIRE_MAGIC, VC_WIRE_MAGIC_LEN) != 0) {
        printf("%.*s", (int)plen, payload); return;
    }
    uint8_t type = (uint8_t)payload[VC_WIRE_MAGIC_LEN];
    const uint8_t *data = (const uint8_t *)(payload + VC_WIRE_MAGIC_LEN + 1);
    size_t dlen = plen - VC_WIRE_MAGIC_LEN - 1;

    switch (type) {
    case VC_TYPE_INT64: {
        int64_t v = 0;
        for (size_t i = 0; i < 8 && i < dlen; i++) v |= ((int64_t)data[i] << (i*8));
        if (use_color) printf(COL_CYAN "(integer) " COL_BOLD "%" PRId64 COL_RESET, v);
        else           printf("(integer) %" PRId64, v);
        break;
    }
    case VC_TYPE_FLOAT64: {
        double v; memcpy(&v, data, 8);
        if (use_color) printf(COL_CYAN "(float) " COL_BOLD "%g" COL_RESET, v);
        else           printf("(float) %g", v);
        break;
    }
    case VC_TYPE_BOOL: {
        bool v = data[0] != 0;
        if (use_color) printf(COL_MAGENTA "(bool) " COL_BOLD "%s" COL_RESET,
                              v ? "true" : "false");
        else           printf("(bool) %s", v ? "true" : "false");
        break;
    }
    case VC_TYPE_JSON:
        if (!raw_mode) {
            if (use_color) printf(COL_YELLOW "(json) " COL_RESET);
            else           printf("(json) ");
        }
        printf("%.*s", (int)dlen, data);
        break;
    case VC_TYPE_BINARY:
        if (use_color) printf(COL_DIM "(binary %zu bytes)" COL_RESET, dlen);
        else           printf("(binary %zu bytes)", dlen);
        break;
    default:
        printf("%.*s", (int)plen, payload);
    }
}

static int print_reply(vcli_conn_t *c, int depth) {
    char line[4096];
    if (read_line(c, line, sizeof(line)) <= 0) return -1;

    char type = line[0];
    const char *rest = line + 1;

    switch (type) {
    /* Simple string */
    case '+':
        if (!raw_mode) {
            if (use_color) printf(COL_GREEN "%s" COL_RESET, rest);
            else           printf("%s", rest);
        } else printf("%s", rest);
        printf("\n");
        break;

    /* Error */
    case '-':
        if (use_color) fprintf(stderr, COL_RED "(error) %s" COL_RESET "\n", rest);
        else           fprintf(stderr, "(error) %s\n", rest);
        break;

    /* Integer */
    case ':': {
        int64_t v = atoll(rest);
        if (!raw_mode) {
            if (use_color) printf(COL_CYAN "(integer) " COL_BOLD "%" PRId64 COL_RESET "\n", v);
            else           printf("(integer) %" PRId64 "\n", v);
        } else printf("%" PRId64 "\n", v);
        break;
    }

    /* Double (RESP3) */
    case ',': {
        double v = atof(rest);
        if (!raw_mode) {
            if (use_color) printf(COL_CYAN "(double) " COL_BOLD "%g" COL_RESET "\n", v);
            else           printf("(double) %g\n", v);
        } else printf("%g\n", v);
        break;
    }

    /* Boolean (RESP3) */
    case '#': {
        bool v = (rest[0] == 't');
        if (!raw_mode) {
            if (use_color) printf(COL_MAGENTA "(boolean) " COL_BOLD "%s" COL_RESET "\n",
                                  v ? "true" : "false");
            else           printf("(boolean) %s\n", v ? "true" : "false");
        } else printf("%s\n", v ? "true" : "false");
        break;
    }

    /* Null (RESP3) */
    case '_':
        if (!raw_mode) {
            if (use_color) printf(COL_DIM "(nil)" COL_RESET "\n");
            else           printf("(nil)\n");
        }
        break;

    /* Bulk string */
    case '$': {
        int64_t blen = atoll(rest);
        if (blen < 0) {
            if (!raw_mode) {
                if (use_color) printf(COL_DIM "(nil)" COL_RESET "\n");
                else           printf("(nil)\n");
            }
            break;
        }
        char *buf = malloc((size_t)blen + 3);
        if (!buf) return -1;
        if (read_exactly(c, buf, (size_t)blen + 2) < 0) { free(buf); return -1; }
        buf[blen] = '\0';

        /* Check for VoidCache typed payload */
        if (!raw_mode && (size_t)blen > VC_WIRE_MAGIC_LEN &&
            memcmp(buf, VC_WIRE_MAGIC, VC_WIRE_MAGIC_LEN) == 0) {
            print_indent(depth);
            print_vc_typed(buf, (size_t)blen);
            printf("\n");
        } else {
            /* Plain bulk string — check if printable */
            bool printable = true;
            for (int64_t i = 0; i < blen && printable; i++)
                if ((uint8_t)buf[i] < 0x09 || (uint8_t)buf[i] == 0x7f) printable = false;

            print_indent(depth);
            if (raw_mode) {
                fwrite(buf, 1, (size_t)blen, stdout);
                printf("\n");
            } else if (printable) {
                if (use_color) printf(COL_GREEN "\"%s\"" COL_RESET "\n", buf);
                else           printf("\"%s\"\n", buf);
            } else {
                if (use_color) printf(COL_DIM "(binary %lld bytes)" COL_RESET "\n", (long long)blen);
                else           printf("(binary %lld bytes)\n", (long long)blen);
            }
        }
        free(buf);
        break;
    }

    /* Array */
    case '*': {
        int64_t count = atoll(rest);
        if (count < 0) {
            if (!raw_mode) printf("(nil)\n");
            break;
        }
        if (count == 0) {
            if (!raw_mode) printf("(empty array)\n");
            break;
        }
        for (int64_t i = 0; i < count; i++) {
            if (!raw_mode) { print_indent(depth); printf("%lld) ", (long long)(i+1)); }
            if (print_reply(c, depth + 1) < 0) return -1;
        }
        break;
    }

    /* Map (RESP3) */
    case '%': {
        int64_t pairs = atoll(rest);
        if (!raw_mode && use_color) printf(COL_BOLD "(map)" COL_RESET "\n");
        else if (!raw_mode)         printf("(map)\n");
        for (int64_t i = 0; i < pairs; i++) {
            print_indent(depth + 1);
            if (!raw_mode) { if (use_color) printf(COL_BOLD "key: " COL_RESET); else printf("key: "); }
            if (print_reply(c, depth + 2) < 0) return -1;
            print_indent(depth + 1);
            if (!raw_mode) { if (use_color) printf(COL_BOLD "val: " COL_RESET); else printf("val: "); }
            if (print_reply(c, depth + 2) < 0) return -1;
        }
        break;
    }

    /* Set (RESP3) */
    case '~': {
        int64_t count = atoll(rest);
        if (!raw_mode && use_color) printf(COL_BOLD "(set)" COL_RESET "\n");
        else if (!raw_mode)         printf("(set)\n");
        for (int64_t i = 0; i < count; i++) {
            print_indent(depth + 1);
            if (print_reply(c, depth + 1) < 0) return -1;
        }
        break;
    }

    /* Blob error (RESP3) */
    case '!': {
        int64_t blen = atoll(rest);
        char *buf = malloc((size_t)blen + 3);
        if (!buf) return -1;
        read_exactly(c, buf, (size_t)blen + 2); buf[blen] = '\0';
        if (use_color) fprintf(stderr, COL_RED "(error) %s" COL_RESET "\n", buf);
        else           fprintf(stderr, "(error) %s\n", buf);
        free(buf);
        break;
    }

    default:
        fprintf(stderr, "Unknown RESP type: '%c' (0x%02x)\n", type, (uint8_t)type);
        return -1;
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 * COMMAND ENCODER
 * ══════════════════════════════════════════════════════════════ */

/* Build a RESP3 command from argv into buf.
 * Handles VoidCache type encoding for VCSET-style commands. */
static int encode_command(char *buf, size_t bufsz, int argc, char **argv) {
    int pos = snprintf(buf, bufsz, "*%d\r\n", argc);
    for (int i = 0; i < argc; i++) {
        size_t alen = strlen(argv[i]);
        pos += snprintf(buf + pos, bufsz - (size_t)pos,
                        "$%zu\r\n%s\r\n", alen, argv[i]);
        if ((size_t)pos >= bufsz) return -1;
    }
    return pos;
}

/* ══════════════════════════════════════════════════════════════
 * READLINE-LIKE INPUT  (no external dep)
 * ══════════════════════════════════════════════════════════════ */

static char *cli_readline(const char *prompt) {
    if (isatty(STDIN_FILENO)) printf("%s", prompt);
    fflush(stdout);

    char *line = NULL;
    size_t cap = 0;
    ssize_t n = getline(&line, &cap, stdin);
    if (n <= 0) { free(line); return NULL; }
    /* Strip trailing \n */
    if (n > 0 && line[n-1] == '\n') line[n-1] = '\0';
    return line;
}

/* Tokenize line into argv, respecting single and double quotes */
static int tokenize(char *line, char **argv, int maxargc) {
    int argc = 0;
    char *p = line;
    while (*p && argc < maxargc) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char *start;
        if (*p == '"') {
            p++;
            start = p;
            while (*p && *p != '"') p++;
            if (*p) *p++ = '\0';
        } else if (*p == '\'') {
            p++;
            start = p;
            while (*p && *p != '\'') p++;
            if (*p) *p++ = '\0';
        } else {
            start = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = '\0';
        }
        argv[argc++] = start;
    }
    return argc;
}

/* ══════════════════════════════════════════════════════════════
 * EXECUTE ONE COMMAND (non-cluster)
 * ══════════════════════════════════════════════════════════════ */

static int do_command(vcli_conn_t *c, int argc, char **argv) {
    char cmdbuf[256 * 1024];
    int  cmdlen = encode_command(cmdbuf, sizeof(cmdbuf), argc, argv);
    if (cmdlen < 0) { fprintf(stderr, "Command too long\n"); return -1; }

    int64_t t0 = show_latency ? cli_now_ns() : 0;

    if (cli_sendall(c, cmdbuf, (size_t)cmdlen) < 0) {
        fprintf(stderr, "Connection lost\n"); return -1;
    }

    int r = print_reply(c, 0);

    if (show_latency && r == 0) {
        int64_t dt = cli_now_ns() - t0;
        printf(COL_DIM " [%.3f ms]" COL_RESET "\n", (double)dt / 1e6);
    }
    return r;
}

/* ══════════════════════════════════════════════════════════════
 * SERVER ENTRYPOINT  (embedded — when run as server)
 * ══════════════════════════════════════════════════════════════ */

/* Forward declaration — server.h is included transitively */
struct vcserver_cfg;
int run_server(int argc, char **argv);

/* ══════════════════════════════════════════════════════════════
 * HELP
 * ══════════════════════════════════════════════════════════════ */

static void print_help(const char *prog) {
    printf(
        COL_BOLD "VoidCache CLI " COL_RESET "(" VC_SERVER_VERSION ")\n\n"
        "Usage: %s [options] [command [args...]]\n\n"
        "Connection options:\n"
        "  -h <hostname>     Server hostname (default: 127.0.0.1)\n"
        "  -p <port>         Server port (default: 6379)\n"
        "  -a <password>     Password\n"
        "  -u <username>     Username for ACL auth\n"
        "  -n <db>           Database number (VoidCache always uses 0)\n"
        "  --tls             Enable TLS\n"
        "  --cacert <file>   CA certificate for TLS verification\n"
        "  --cluster         Connect in cluster routing mode\n\n"
        "Display options:\n"
        "  --raw             Print raw output (no type decorations)\n"
        "  --no-color        Disable colored output\n"
        "  --latency         Show per-command latency\n\n"
        "Modes:\n"
        "  --pipe            Read commands from stdin (batch mode)\n"
        "  server            Start VoidCache server (see --help-server)\n\n"
        "VoidCache extended commands:\n"
        "  VCSET <key> <type> <value>   Store typed value\n"
        "    types: int, float, bool, json, binary, string\n"
        "  VCGET <key>                  Get with type metadata\n"
        "  VCINFO                       Server info as JSON\n\n"
        "Examples:\n"
        "  %s PING\n"
        "  %s SET mykey myvalue\n"
        "  %s VCSET score int 42\n"
        "  %s VCSET config json '{\"timeout\":30}'\n"
        "  %s -h 10.0.0.1 -p 6379 --tls -a password INFO\n"
        "  %s --cluster -h 10.0.0.1 GET mykey\n",
        prog, prog, prog, prog, prog, prog, prog);
}

static void print_server_help(void) {
    printf(
        COL_BOLD "VoidCache Server" COL_RESET "\n\n"
        "Usage: vcli server [options]\n\n"
        "  --port <port>         Listen port (default: 6379)\n"
        "  --bind <addr>         Bind address (default: 0.0.0.0)\n"
        "  --tls-cert <file>     TLS certificate (enables TLS)\n"
        "  --tls-key  <file>     TLS private key\n"
        "  --acl-file <file>     ACL file (username pw_sha256_hex flags)\n"
        "  --requirepass <pass>  Simple password (Redis compat)\n"
        "  --maxmemory <bytes>   Memory cap (e.g. 256m, 1g)\n"
        "  --wal <path>          WAL file path\n"
        "  --threads <n>         Worker threads (default: 4)\n"
        "  --cluster             Enable cluster mode\n"
        "  --announce-addr <ip>  Address reported to cluster peers\n"
        "  --announce-port <p>   Port reported to cluster peers\n\n"
        "ACL file format (one user per line):\n"
        "  <username> <sha256_of_password_hex> <acl_flags>\n"
        "  flags: r=read  w=write  a=admin  p=pubsub  *=all\n\n"
        "Example:\n"
        "  vcli server --port 6379 --tls-cert cert.pem --tls-key key.pem \\\n"
        "              --requirepass secret --maxmemory 1g\n");
}

/* ══════════════════════════════════════════════════════════════
 * MAIN
 * ══════════════════════════════════════════════════════════════ */

static size_t parse_mem(const char *s) {
    char *end;
    size_t v = (size_t)strtoull(s, &end, 10);
    if (*end == 'm' || *end == 'M') v *= 1024*1024;
    else if (*end == 'g' || *end == 'G') v *= 1024*1024*1024;
    else if (*end == 'k' || *end == 'K') v *= 1024;
    return v;
}

int main(int argc, char **argv) {
    if (!isatty(STDOUT_FILENO)) use_color = false;

#ifdef _WIN32
    /* Winsock must be initialised before any socket operations on Windows. */
    { WSADATA _wsa; WSAStartup(MAKEWORD(2,2), &_wsa); }
#endif

    signal(SIGPIPE, SIG_IGN);

    /* Defaults */
    char      host[256]  = "127.0.0.1";
    uint16_t  port       = 6379;
    char      username[64]  = {0};
    char      password[256] = {0};
    bool      use_tls    = false;
    char      cacert[256] = {0};
    bool      pipe_mode  = false;
    bool      cluster    = false;
    bool      do_server  = false;

    /* Server-mode config */
    vc_server_cfg_t scfg = {
        .bind_addr      = "0.0.0.0",
        .port           = 6379,
        .worker_threads = 4,
        .max_memory     = 256ULL * 1024 * 1024,
        .shard_slots    = 4096,
    };

    int cmd_start = argc; /* index of first non-option arg that is the command */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-?") == 0) {
            print_help(argv[0]); return 0;
        }
        if (strcmp(argv[i], "--help-server") == 0) { print_server_help(); return 0; }
        if (strcmp(argv[i], "server") == 0) { do_server = true; continue; }

        /* Server-mode flags */
        if (do_server) {
            if (strcmp(argv[i], "--port") == 0 && i+1 < argc)
                scfg.port = (uint16_t)atoi(argv[++i]);
            else if (strcmp(argv[i], "--bind") == 0 && i+1 < argc)
                scfg.bind_addr = argv[++i];
            else if (strcmp(argv[i], "--tls-cert") == 0 && i+1 < argc)
                scfg.tls_cert = argv[++i];
            else if (strcmp(argv[i], "--tls-key") == 0 && i+1 < argc)
                scfg.tls_key = argv[++i];
            else if (strcmp(argv[i], "--acl-file") == 0 && i+1 < argc)
                scfg.acl_file = argv[++i];
            else if (strcmp(argv[i], "--requirepass") == 0 && i+1 < argc)
                scfg.requirepass = argv[++i];
            else if (strcmp(argv[i], "--maxmemory") == 0 && i+1 < argc)
                scfg.max_memory = parse_mem(argv[++i]);
            else if (strcmp(argv[i], "--wal") == 0 && i+1 < argc)
                scfg.wal_path = argv[++i];
            else if (strcmp(argv[i], "--threads") == 0 && i+1 < argc)
                scfg.worker_threads = atoi(argv[++i]);
            else if (strcmp(argv[i], "--cluster") == 0)
                scfg.cluster_enabled = true;
            else if (strcmp(argv[i], "--announce-addr") == 0 && i+1 < argc)
                scfg.cluster_announce_addr = argv[++i];
            else if (strcmp(argv[i], "--announce-port") == 0 && i+1 < argc)
                scfg.cluster_announce_port = (uint16_t)atoi(argv[++i]);
            continue;
        }

        /* Client-mode flags */
        if      (strcmp(argv[i], "-h") == 0 && i+1 < argc) strncpy(host, argv[++i], sizeof(host)-1);
        else if (strcmp(argv[i], "-p") == 0 && i+1 < argc) port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "-a") == 0 && i+1 < argc) strncpy(password, argv[++i], sizeof(password)-1);
        else if (strcmp(argv[i], "-u") == 0 && i+1 < argc) strncpy(username, argv[++i], sizeof(username)-1);
        else if (strcmp(argv[i], "--tls") == 0)             use_tls = true;
        else if (strcmp(argv[i], "--cacert") == 0 && i+1 < argc) strncpy(cacert, argv[++i], sizeof(cacert)-1);
        else if (strcmp(argv[i], "--raw") == 0)             raw_mode = true;
        else if (strcmp(argv[i], "--no-color") == 0)        use_color = false;
        else if (strcmp(argv[i], "--latency") == 0)         show_latency = true;
        else if (strcmp(argv[i], "--pipe") == 0)            pipe_mode = true;
        else if (strcmp(argv[i], "--cluster") == 0)         cluster = true;
        else if (strcmp(argv[i], "-n") == 0 && i+1 < argc) i++; /* ignore */
        else { cmd_start = i; break; }
    }

    /* ── SERVER MODE ── */
    if (do_server) {
        vcserver_t *srv = vcserver_create(&scfg);
        if (!srv) { fprintf(stderr, "Failed to create server\n"); return 1; }
        if (vcserver_start(srv) < 0) {
            fprintf(stderr, "Failed to start workers\n"); vcserver_destroy(srv); return 1;
        }
        vcserver_run(srv);
        vcserver_destroy(srv);
        return 0;
    }

    /* ── CLIENT MODE ── */

    /* Cluster mode */
    if (cluster) {
        char seeds[512];
        snprintf(seeds, sizeof(seeds), "%s:%u", host, port);
        vc_cluster_t *cl = vc_cluster_connect(seeds,
            username[0] ? username : NULL,
            password[0] ? password : NULL,
            (use_tls && cacert[0]) ? cacert : NULL);
        if (!cl) { fprintf(stderr, "Cluster connection failed\n"); return 1; }

        if (cmd_start < argc) {
            /* Single command from CLI args */
            char cmdbuf[256*1024];
            int n = vc_resp_format(cmdbuf, sizeof(cmdbuf), argc - cmd_start,
                                   /* argv+cmd_start — varargs */
                                   argv[cmd_start]);
            /* Reconstruct proper varargs — use encode_command instead */
            n = encode_command(cmdbuf, sizeof(cmdbuf),
                               argc - cmd_start, argv + cmd_start);
            char *reply; size_t rlen;
            vc_cluster_cmd(cl, argc > cmd_start+1 ? argv[cmd_start+1] : NULL,
                           argc > cmd_start+1 ? strlen(argv[cmd_start+1]) : 0,
                           cmdbuf, (size_t)n, &reply, &rlen);
            if (reply) { printf("%s\n", reply); free(reply); }
        }
        vc_cluster_destroy(cl);
        return 0;
    }

    /* Direct TCP connection */
    int fd = cli_tcp_connect(host, port);
    if (fd < 0) return 1;

    vcli_conn_t conn = { .fd = fd, .resp3 = false };

    /* TLS handshake */
    if (use_tls) {
        conn.ssl_ctx = SSL_CTX_new(TLS_client_method());
        if (!conn.ssl_ctx) { close(fd); return 1; }
        SSL_CTX_set_verify(conn.ssl_ctx, SSL_VERIFY_NONE, NULL);
        conn.ssl = SSL_new(conn.ssl_ctx);
        SSL_set_fd(conn.ssl, fd);
        if (SSL_connect(conn.ssl) <= 0) {
            fprintf(stderr, "TLS handshake failed\n");
            SSL_free(conn.ssl); SSL_CTX_free(conn.ssl_ctx); close(fd); return 1;
        }
    }

    /* HELLO 3 to negotiate RESP3 */
    {
        const char *hello = "*2\r\n$5\r\nHELLO\r\n$1\r\n3\r\n";
        cli_sendall(&conn, hello, strlen(hello));
        /* Read and discard HELLO response */
        char line[256]; read_line(&conn, line, sizeof(line));
        if (line[0] == '%') {
            int64_t pairs = atoll(line + 1);
            for (int64_t k = 0; k < pairs * 2; k++) {
                read_line(&conn, line, sizeof(line));
                if (line[0] == '$') {
                    int64_t bl = atoll(line+1);
                    if (bl > 0) {
                        char *tmp = malloc((size_t)bl + 3);
                        if (tmp) { read_exactly(&conn, tmp, (size_t)bl+2); free(tmp); }
                    }
                }
            }
            conn.resp3 = true;
        }
    }

    /* Authenticate */
    if (password[0]) {
        char auth[512]; int alen;
        if (username[0])
            alen = snprintf(auth, sizeof(auth),
                "*3\r\n$4\r\nAUTH\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                strlen(username), username, strlen(password), password);
        else
            alen = snprintf(auth, sizeof(auth),
                "*2\r\n$4\r\nAUTH\r\n$%zu\r\n%s\r\n",
                strlen(password), password);
        cli_sendall(&conn, auth, (size_t)alen);
        print_reply(&conn, 0);
    }

    /* ── Single command from CLI args ── */
    if (cmd_start < argc) {
        char *cmd_argv[VC_MAX_ARGS];
        int   cmd_argc = 0;
        for (int i = cmd_start; i < argc && cmd_argc < VC_MAX_ARGS; i++)
            cmd_argv[cmd_argc++] = argv[i];
        do_command(&conn, cmd_argc, cmd_argv);
        goto done;
    }

    /* ── Pipe mode: read from stdin ── */
    if (pipe_mode) {
        char *line;
        while ((line = cli_readline("")) != NULL) {
            char *toks[VC_MAX_ARGS]; int ntoks = tokenize(line, toks, VC_MAX_ARGS);
            if (ntoks > 0) do_command(&conn, ntoks, toks);
            free(line);
        }
        goto done;
    }

    /* ── Interactive REPL ── */
    if (use_color) {
        printf(COL_BOLD "VoidCache %s" COL_RESET "  connected to %s:%u%s\n"
               COL_DIM "Type commands, HELP for extended commands, QUIT to exit."
               COL_RESET "\n",
               VC_SERVER_VERSION, host, port, use_tls ? " [TLS]" : "");
    } else {
        printf("VoidCache %s  connected to %s:%u%s\n",
               VC_SERVER_VERSION, host, port, use_tls ? " [TLS]" : "");
    }

    char prompt[64];
    snprintf(prompt, sizeof(prompt), "%s:%u> ", host, port);

    char *line;
    while ((line = cli_readline(prompt)) != NULL) {
        if (line[0] == '\0') { free(line); continue; }

        /* Local commands */
        if (strcasecmp(line, "quit") == 0 || strcasecmp(line, "exit") == 0) {
            free(line); break;
        }
        if (strcasecmp(line, "help") == 0) {
            printf(
                "Standard Redis commands:  GET SET DEL EXISTS EXPIRE TTL\n"
                "                          INCR DECR APPEND KEYS SCAN DBSIZE\n"
                "                          MGET MSET RENAME FLUSHDB INFO\n"
                "VoidCache extended:       VCSET VCGET VCINFO\n"
                "  VCSET <key> int <n>         — store typed integer\n"
                "  VCSET <key> float <n>       — store typed float\n"
                "  VCSET <key> bool true|false — store typed boolean\n"
                "  VCSET <key> json '<json>'   — store JSON (validated server-side)\n"
                "  VCSET <key> binary <data>   — store binary blob\n"
                "  VCGET <key>                 — get with type metadata\n"
                "  VCINFO                      — server info as JSON\n"
                "  CLUSTER INFO/NODES/MYID/SLOTS\n"
            );
            free(line); continue;
        }

        char *toks[VC_MAX_ARGS]; int ntoks = tokenize(line, toks, VC_MAX_ARGS);
        if (ntoks > 0) do_command(&conn, ntoks, toks);
        free(line);
    }

done:
    if (conn.ssl) { SSL_shutdown(conn.ssl); SSL_free(conn.ssl); }
    if (conn.ssl_ctx) SSL_CTX_free(conn.ssl_ctx);
    close(conn.fd);
    return 0;
}
