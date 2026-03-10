/*
 * net/auth.h  –  VoidCache service authentication and ACL.
 *
 * ── Auth model ───────────────────────────────────────────────────────────────
 *
 *  Redis AUTH compatibility:
 *    AUTH <password>            – password-only (legacy, maps to default user)
 *    AUTH <username> <password> – Redis 6+ ACL AUTH
 *
 *  VoidCache token flow (used by the cluster layer):
 *    1. Client sends HELLO 3 AUTH <user> <password>
 *    2. Server verifies with HMAC-SHA256(password, challenge_nonce)
 *    3. On success, issues a short-lived session token (32 random bytes,
 *       hex-encoded) stored in the connection state.  The token is also
 *       sent in the HELLO response so cluster proxies can forward it.
 *
 *  ACL permissions (bitmask per user):
 *    VC_ACL_READ    – GET, KEYS, TTL, TYPE, SCAN, EXISTS
 *    VC_ACL_WRITE   – SET, DEL, EXPIRE, APPEND, INCR, RENAME
 *    VC_ACL_ADMIN   – FLUSHDB, INFO, CONFIG, CLUSTER, DEBUG
 *    VC_ACL_PUBSUB  – SUBSCRIBE, PUBLISH (future)
 *
 *  Users are stored in a simple in-memory table loaded from a config file
 *  at startup (vcache.acl).  Format: one line per user:
 *    <username> <sha256_of_password_hex> <acl_flags>
 *  Example:
 *    default  e3b0c44298fc1c149afbf4c8996fb924... rw
 *    reader   aabbcc...                            r
 *    admin    ddeeff...                            rwa
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define VC_ACL_READ    0x01u
#define VC_ACL_WRITE   0x02u
#define VC_ACL_ADMIN   0x04u
#define VC_ACL_PUBSUB  0x08u
#define VC_ACL_ALL     (VC_ACL_READ | VC_ACL_WRITE | VC_ACL_ADMIN | VC_ACL_PUBSUB)

#define VC_AUTH_MAX_USERS   64
#define VC_AUTH_TOKEN_BYTES 32     /* session token length */
#define VC_AUTH_USERNAME_MAX 64
#define VC_AUTH_HASH_HEX    64     /* SHA-256 hex = 64 chars */

typedef struct {
    char     username[VC_AUTH_USERNAME_MAX];
    uint8_t  pw_sha256[32];      /* SHA-256 of password          */
    uint32_t acl;                /* VC_ACL_* bitmask             */
    bool     enabled;
} vc_user_t;

typedef struct {
    vc_user_t users[VC_AUTH_MAX_USERS];
    int       count;
    bool      require_auth;      /* if false, all connections get VC_ACL_ALL */
} vc_auth_db_t;

/* Load ACL from file.  Returns 0 on success. */
int  vc_auth_load(vc_auth_db_t *db, const char *path);

/* Add a user programmatically (e.g. from CLI --user flag). */
int  vc_auth_add_user(vc_auth_db_t *db, const char *username,
                      const char *password, uint32_t acl);

/* Verify username+password.  Returns the user or NULL. */
const vc_user_t *vc_auth_verify(const vc_auth_db_t *db,
                                 const char *username, size_t ulen,
                                 const char *password, size_t plen);

/* Generate a 32-byte random session token (hex-encoded, 64 chars + NUL). */
int  vc_auth_gen_token(char out_hex[65]);

/* Compute SHA-256 of data (pure-C, no dep on OpenSSL headers). */
void vc_sha256(const void *data, size_t len, uint8_t out[32]);

/* HMAC-SHA256 via libcrypto (linked directly against .so.3). */
int  vc_hmac_sha256(const uint8_t *key, size_t klen,
                    const uint8_t *msg, size_t mlen,
                    uint8_t out[32]);

/* Check ACL permission.  Returns true if user has all requested flags. */
static inline bool vc_acl_check(const vc_user_t *u, uint32_t need) {
    return u && (u->acl & need) == need;
}
