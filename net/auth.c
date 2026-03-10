/*
 * net/auth.c  –  VoidCache authentication & ACL implementation.
 *
 * SHA-256: pure-C (RFC 6234 compliant), zero external deps.
 * HMAC-SHA256: via libcrypto.so.3 (dlopen'd for the challenge flow).
 * Password hashing: SHA-256(password) stored in ACL file as hex.
 * Session tokens: /dev/urandom or RAND_bytes from libcrypto.
 */
#define _POSIX_C_SOURCE 200809L
#include "auth.h"
#include "vc_ssl_abi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <stdint.h>

/* ══════════════════════════════════════════════════════════════
 * PURE-C SHA-256 (RFC 6234)
 * ══════════════════════════════════════════════════════════════ */

#define ROR32(x,n)  (((x) >> (n)) | ((x) << (32-(n))))
#define CH(x,y,z)   (((x)&(y))^(~(x)&(z)))
#define MAJ(x,y,z)  (((x)&(y))^((x)&(z))^((y)&(z)))
#define EP0(x)      (ROR32(x,2)^ROR32(x,13)^ROR32(x,22))
#define EP1(x)      (ROR32(x,6)^ROR32(x,11)^ROR32(x,25))
#define SIG0(x)     (ROR32(x,7)^ROR32(x,18)^((x)>>3))
#define SIG1(x)     (ROR32(x,17)^ROR32(x,19)^((x)>>10))

static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
    0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
    0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
    0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};

void vc_sha256(const void *data, size_t len, uint8_t out[32]) {
    uint32_t h[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
    };

    const uint8_t *msg  = (const uint8_t *)data;
    uint64_t bit_len    = (uint64_t)len * 8;
    size_t   padded_len = (len + 9 + 63) & ~(size_t)63;
    uint8_t *padded     = calloc(1, padded_len);
    if (!padded) { memset(out, 0, 32); return; }
    memcpy(padded, msg, len);
    padded[len] = 0x80;
    /* big-endian bit length at the end */
    for (int i = 0; i < 8; i++)
        padded[padded_len - 1 - i] = (uint8_t)(bit_len >> (i * 8));

    for (size_t blk = 0; blk < padded_len; blk += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            const uint8_t *b = padded + blk + i * 4;
            w[i] = ((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|
                   ((uint32_t)b[2]<<8)|b[3];
        }
        for (int i = 16; i < 64; i++)
            w[i] = SIG1(w[i-2]) + w[i-7] + SIG0(w[i-15]) + w[i-16];

        uint32_t a=h[0],b2=h[1],c=h[2],d=h[3],
                 e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + EP1(e) + CH(e,f,g) + K256[i] + w[i];
            uint32_t t2 = EP0(a) + MAJ(a,b2,c);
            hh=g; g=f; f=e; e=d+t1;
            d=c; c=b2; b2=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b2; h[2]+=c; h[3]+=d;
        h[4]+=e; h[5]+=f;  h[6]+=g; h[7]+=hh;
    }
    free(padded);
    for (int i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(h[i]>>24);
        out[i*4+1] = (uint8_t)(h[i]>>16);
        out[i*4+2] = (uint8_t)(h[i]>>8);
        out[i*4+3] = (uint8_t)h[i];
    }
}

/* ══════════════════════════════════════════════════════════════
 * HMAC-SHA256  (via libcrypto.so.3)
 * ══════════════════════════════════════════════════════════════ */

int vc_hmac_sha256(const uint8_t *key, size_t klen,
                   const uint8_t *msg, size_t mlen,
                   uint8_t out[32]) {
    unsigned int olen = 32;
    unsigned char *r = HMAC(EVP_sha256(),
                            key, (int)klen,
                            msg, mlen,
                            out, &olen);
    return r ? 0 : -1;
}

/* ══════════════════════════════════════════════════════════════
 * SESSION TOKEN
 * ══════════════════════════════════════════════════════════════ */

int vc_auth_gen_token(char out_hex[65]) {
    uint8_t raw[VC_AUTH_TOKEN_BYTES];

    /* Try libcrypto RAND_bytes first, fall back to /dev/urandom */
    if (RAND_bytes(raw, VC_AUTH_TOKEN_BYTES) <= 0) {
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd < 0) return -1;
        ssize_t n = read(fd, raw, VC_AUTH_TOKEN_BYTES);
        close(fd);
        if (n != VC_AUTH_TOKEN_BYTES) return -1;
    }

    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < VC_AUTH_TOKEN_BYTES; i++) {
        out_hex[i*2]   = hex[raw[i] >> 4];
        out_hex[i*2+1] = hex[raw[i] & 0xf];
    }
    out_hex[64] = '\0';
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 * ACL DB
 * ══════════════════════════════════════════════════════════════ */

static void hex_to_bytes(const char *hex, uint8_t *out, size_t nbytes) {
    for (size_t i = 0; i < nbytes; i++) {
        unsigned v = 0;
        sscanf(hex + i*2, "%02x", &v);
        out[i] = (uint8_t)v;
    }
}

static uint32_t parse_acl_flags(const char *flags) {
    uint32_t acl = 0;
    for (; *flags; flags++) {
        switch (*flags) {
        case 'r': acl |= VC_ACL_READ;   break;
        case 'w': acl |= VC_ACL_WRITE;  break;
        case 'a': acl |= VC_ACL_ADMIN;  break;
        case 'p': acl |= VC_ACL_PUBSUB; break;
        case '*': acl  = VC_ACL_ALL;    break;
        }
    }
    return acl;
}

int vc_auth_load(vc_auth_db_t *db, const char *path) {
    memset(db, 0, sizeof(*db));
    FILE *f = fopen(path, "r");
    if (!f) {
        /* No ACL file → no auth required (single-node dev mode) */
        db->require_auth = false;
        return 0;
    }
    db->require_auth = true;
    char line[256];
    while (fgets(line, sizeof(line), f) && db->count < VC_AUTH_MAX_USERS) {
        /* Skip comments and blank lines */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        char user[VC_AUTH_USERNAME_MAX], pw_hex[VC_AUTH_HASH_HEX+1], flags[16];
        if (sscanf(p, "%63s %64s %15s", user, pw_hex, flags) != 3) continue;

        vc_user_t *u = &db->users[db->count++];
        snprintf(u->username, VC_AUTH_USERNAME_MAX, "%s", user);
        hex_to_bytes(pw_hex, u->pw_sha256, 32);
        u->acl     = parse_acl_flags(flags);
        u->enabled = true;
    }
    fclose(f);
    return 0;
}

int vc_auth_add_user(vc_auth_db_t *db, const char *username,
                     const char *password, uint32_t acl) {
    if (db->count >= VC_AUTH_MAX_USERS) return -1;
    vc_user_t *u = &db->users[db->count++];
    strncpy(u->username, username, VC_AUTH_USERNAME_MAX - 1);
    vc_sha256(password, strlen(password), u->pw_sha256);
    u->acl     = acl;
    u->enabled = true;
    db->require_auth = true;
    return 0;
}

const vc_user_t *vc_auth_verify(const vc_auth_db_t *db,
                                 const char *username, size_t ulen,
                                 const char *password, size_t plen) {
    if (!db->require_auth) {
        /* Return a synthetic superuser */
        static vc_user_t anon = { .username = "anonymous", .acl = VC_ACL_ALL, .enabled = true };
        return &anon;
    }

    uint8_t pw_hash[32];
    vc_sha256(password, plen, pw_hash);

    for (int i = 0; i < db->count; i++) {
        const vc_user_t *u = &db->users[i];
        if (!u->enabled) continue;
        size_t nlen = strlen(u->username);
        if (nlen != ulen) continue;
        if (memcmp(u->username, username, ulen) != 0) continue;
        if (memcmp(u->pw_sha256, pw_hash, 32) == 0) return u;
    }
    return NULL;
}
