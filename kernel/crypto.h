/*
 * JARVIS OS — cryptography, X.509 and TLS 1.3 (kernel/crypto.c, x509.c, tls.c)
 *
 * The three files only use these declarations plus a few platform hooks
 * (memory, entropy, time, TCP), so they also build on a Linux host for
 * testing against OpenSSL and real web servers.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* ---- platform hooks (kernel: kernel/tls.c; host tests: their own) ---- */
void   *crypto_alloc(size_t n);
void    crypto_free(void *p, size_t n);
void    crypto_platform_entropy(void);          /* calls crypto_add_entropy() */
int64_t crypto_now(void);                       /* Unix time, for certificate dates */
int     crypto_tcp_send(int sock, const void *data, size_t len);            /* 0 ok */
int     crypto_tcp_recv(int sock, void *buf, size_t max, int timeout_ms);    /* >0, 0 closed, <0 */

/* ---- hashes ---- */
enum { HASH_SHA256 = 1, HASH_SHA384, HASH_SHA512 };
typedef struct {
    int alg;
    uint32_t h32[8];
    uint64_t h64[8];
    uint8_t buf[128];
    size_t blen;
    uint64_t total;
} hash_ctx_t;
int  hash_len(int alg);
void hash_init(hash_ctx_t *c, int alg);
void hash_update(hash_ctx_t *c, const void *data, size_t n);
void hash_final(hash_ctx_t *c, uint8_t *out);
void hash_once(int alg, const void *data, size_t n, uint8_t *out);
void hmac(int alg, const uint8_t *key, size_t kl, const uint8_t *msg, size_t ml, uint8_t *out);
void hkdf_extract(int alg, const uint8_t *salt, size_t sl, const uint8_t *ikm, size_t il, uint8_t *prk);
void hkdf_expand(int alg, const uint8_t *prk, const uint8_t *info, size_t il, uint8_t *out, size_t ol);

/* ---- ciphers ---- */
typedef struct { uint32_t rk[60]; int rounds; } aes_ctx_t;
void aes_setkey(aes_ctx_t *c, const uint8_t *key, int bits);
void aes_encrypt(const aes_ctx_t *c, const uint8_t *in, uint8_t *out);
void aes_gcm_encrypt(const aes_ctx_t *c, const uint8_t *iv, const uint8_t *aad, size_t al, const uint8_t *in, size_t n, uint8_t *out, uint8_t *tag);
int  aes_gcm_decrypt(const aes_ctx_t *c, const uint8_t *iv, const uint8_t *aad, size_t al, const uint8_t *in, size_t n, uint8_t *out, const uint8_t *tag);
void chacha20_block(const uint8_t *key, uint32_t counter, const uint8_t *nonce, uint8_t *out);
void chacha_poly_encrypt(const uint8_t *key, const uint8_t *nonce, const uint8_t *aad, size_t al, const uint8_t *in, size_t n, uint8_t *out, uint8_t *tag);
int  chacha_poly_decrypt(const uint8_t *key, const uint8_t *nonce, const uint8_t *aad, size_t al, const uint8_t *in, size_t n, uint8_t *out, const uint8_t *tag);

/* ---- public keys ---- */
void x25519(uint8_t *out, const uint8_t *scalar, const uint8_t *point);
void x25519_base(uint8_t *out, const uint8_t *scalar);
enum { CURVE_P256 = 1, CURVE_P384 };
int  ecdsa_verify(int curve, const uint8_t *pub, size_t pl, const uint8_t *hashv, size_t hl,
                  const uint8_t *r, size_t rl, const uint8_t *s, size_t sl);
int  rsa_verify_pkcs1(int alg, const uint8_t *n, size_t nl, const uint8_t *e, size_t el, const uint8_t *hashv, const uint8_t *sig, size_t sl);
int  rsa_verify_pss(int alg, const uint8_t *n, size_t nl, const uint8_t *e, size_t el, const uint8_t *mhash, const uint8_t *sig, size_t sl);

/* ---- randomness ---- */
void crypto_add_entropy(const void *data, size_t n);
void crypto_random(void *out, size_t n);

/* ---- X.509 (kernel/x509.c) ---- */
typedef struct { const uint8_t *p; size_t len; } x509_buf_t;
typedef struct {
    x509_buf_t raw, tbs, issuer, subject, sig, spki_key, spki_e;   /* spki_key: RSA modulus or EC point */
    int sig_alg;                /* X509_SIG_* */
    int key_type;               /* X509_KEY_RSA / X509_KEY_EC */
    int curve;                  /* CURVE_* for EC keys */
    int64_t not_before, not_after;
    int is_ca;
    x509_buf_t san;             /* subjectAltName extension value */
    x509_buf_t cn;              /* subject common name */
} x509_cert_t;
enum { X509_SIG_UNKNOWN = 0, X509_SIG_RSA_SHA256, X509_SIG_RSA_SHA384, X509_SIG_RSA_SHA512,
       X509_SIG_ECDSA_SHA256, X509_SIG_ECDSA_SHA384, X509_SIG_ECDSA_SHA512 };
enum { X509_KEY_NONE = 0, X509_KEY_RSA, X509_KEY_EC };
int  x509_parse(x509_cert_t *c, const uint8_t *der, size_t len);
int  x509_verify_signed_by(const x509_cert_t *child, const x509_cert_t *issuer);
int  x509_host_matches(const x509_cert_t *c, const char *host);
/* Checks the chain (certs[0] = server) up to a built-in root. 0 = trusted. */
int  x509_verify_chain(const uint8_t *const *der, const size_t *len, int n, const char *host, char *err, size_t en);
/* Signature check with a certificate's key (TLS CertificateVerify). */
int  x509_key_verify(const x509_cert_t *c, int tls_scheme, const uint8_t *msg, size_t ml, const uint8_t *sig, size_t sl);

/* ---- TLS 1.3 client (kernel/tls.c) ---- */
typedef struct tls_conn tls_conn_t;
tls_conn_t *tls_connect(int sock, const char *host, char *err, size_t en);
int  tls_write(tls_conn_t *t, const void *data, size_t len);                 /* 0 ok */
int  tls_read(tls_conn_t *t, void *buf, size_t max, int timeout_ms);         /* >0, 0 = end, <0 error */
void tls_close(tls_conn_t *t);
const char *tls_cipher_name(tls_conn_t *t);
