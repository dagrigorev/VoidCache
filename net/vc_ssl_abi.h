/*
 * vc_ssl_abi.h  –  Minimal OpenSSL 3.x ABI declarations.
 *
 * We have libssl.so.3 and libcrypto.so.3 installed but no dev headers.
 * We declare only the opaque types and function signatures we actually use.
 * All types are kept opaque (pointer-to-struct) so no struct layout knowledge
 * is required — just the function signatures exported by the DSO.
 *
 * Link with: /usr/lib/x86_64-linux-gnu/libssl.so.3
 *            /usr/lib/x86_64-linux-gnu/libcrypto.so.3
 */
#pragma once
#include <stddef.h>

/* Opaque OpenSSL types */
typedef struct ssl_ctx_st   SSL_CTX;
typedef struct ssl_st       SSL;
typedef struct ssl_method_st SSL_METHOD;
typedef struct x509_st      X509;
typedef struct evp_pkey_st  EVP_PKEY;
typedef struct evp_md_ctx_st EVP_MD_CTX;
typedef struct evp_md_st    EVP_MD;
typedef struct hmac_ctx_st  HMAC_CTX;

#define SSL_FILETYPE_PEM        1
#define SSL_ERROR_WANT_READ     2
#define SSL_ERROR_WANT_WRITE    3
#define SSL_ERROR_SYSCALL       5
#define SSL_ERROR_ZERO_RETURN   6
#define SSL_ERROR_SSL           1
#define SSL_MODE_AUTO_RETRY     0x00000004L
#define SSL_OP_NO_SSLv2         0x01000000L
#define SSL_OP_NO_SSLv3         0x02000000L
#define SSL_OP_NO_TLSv1         0x04000000L
#define SSL_OP_NO_TLSv1_1       0x10000000L
#define SSL_VERIFY_NONE         0x00
#define SSL_VERIFY_PEER         0x01

/* libssl functions */
extern SSL_METHOD *TLS_server_method(void);
extern SSL_METHOD *TLS_client_method(void);
extern SSL_CTX   *SSL_CTX_new(SSL_METHOD *method);
extern void       SSL_CTX_free(SSL_CTX *ctx);
extern long       SSL_CTX_set_options(SSL_CTX *ctx, long options);
extern long       SSL_CTX_ctrl(SSL_CTX *ctx, int cmd, long larg, void *parg);
#define SSL_CTRL_MODE 33
#define SSL_CTX_set_mode(ctx,op) SSL_CTX_ctrl((ctx),SSL_CTRL_MODE,(op),NULL)
extern int        SSL_CTX_use_certificate_file(SSL_CTX *ctx, const char *file, int type);
extern int        SSL_CTX_use_PrivateKey_file(SSL_CTX *ctx, const char *file, int type);
extern int        SSL_CTX_check_private_key(SSL_CTX *ctx);
extern void       SSL_CTX_set_verify(SSL_CTX *ctx, int mode, void *cb);
extern SSL       *SSL_new(SSL_CTX *ctx);
extern void       SSL_free(SSL *ssl);
extern int        SSL_set_fd(SSL *ssl, int fd);
extern int        SSL_accept(SSL *ssl);
extern int        SSL_connect(SSL *ssl);
extern int        SSL_read(SSL *ssl, void *buf, int num);
extern int        SSL_write(SSL *ssl, const void *buf, int num);
extern int        SSL_get_error(const SSL *ssl, int ret);
extern int        SSL_shutdown(SSL *ssl);
extern int        SSL_pending(const SSL *ssl);

/* libcrypto: HMAC-SHA256 for auth tokens */
extern unsigned char *HMAC(const EVP_MD *evp_md,
                            const void *key, int key_len,
                            const unsigned char *d, size_t n,
                            unsigned char *md, unsigned int *md_len);
extern const EVP_MD  *EVP_sha256(void);

/* libcrypto: random bytes */
extern int RAND_bytes(unsigned char *buf, int num);

/* libcrypto: error string */
extern unsigned long ERR_get_error(void);
extern char         *ERR_error_string(unsigned long e, char *buf);
