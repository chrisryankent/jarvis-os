/*
 * =============================================================================
 * JARVIS AI OS — TLS 1.3 client (kernel/tls.c)
 * Language: C17
 *
 * HTTPS for the browser, curl/wget and downloads (RFC 8446):
 *   key exchange   X25519
 *   ciphers        TLS_AES_128_GCM_SHA256, TLS_CHACHA20_POLY1305_SHA256,
 *                  TLS_AES_256_GCM_SHA384
 *   server proof   certificate chain checked against the built-in roots
 *                  (kernel/x509.c), host name checked, CertificateVerify
 *                  signature (ECDSA P-256/P-384, RSA-PSS) and Finished MAC
 *                  checked
 * Not supported: TLS 1.2 servers, client certificates, session resumption,
 * 0-RTT. A server that only speaks TLS 1.2 is reported as such.
 * =============================================================================
 */

#include "crypto.h"
#include <string.h>
#include <stdio.h>

#define TLS_REC_MAX  (16384 + 256)
#define TLS_HS_MAX   (64 * 1024)

struct tls_conn {
    int sock, suite, alg, keylen, hslen;
    int aead_chacha;
    uint8_t c_key[32], c_iv[12], s_key[32], s_iv[12];
    aes_ctx_t c_aes, s_aes;
    uint64_t c_seq, s_seq;
    int s_encrypted, c_encrypted;
    uint8_t c_secret[48], s_secret[48], hs_secret[48];
    hash_ctx_t transcript;
    uint8_t in[TLS_REC_MAX + 5];               /* raw incoming bytes */
    size_t in_len;
    uint8_t rec[TLS_REC_MAX];                  /* current decrypted record */
    size_t rec_off, rec_len;
    int rec_type;
    uint8_t hs[TLS_HS_MAX];                    /* handshake reassembly */
    size_t hs_len;
    int eof;
    char err[128];
};

/* ---- key schedule ------------------------------------------------------------ */
static void expand_label(int alg, const uint8_t *secret, const char *label, const uint8_t *ctx, size_t cl, uint8_t *out, size_t ol)
{
    uint8_t info[2 + 1 + 6 + 32 + 1 + 64];
    size_t ll = strlen(label), n = 0;
    info[n++] = (uint8_t)(ol >> 8); info[n++] = (uint8_t)ol;
    info[n++] = (uint8_t)(6 + ll);
    memcpy(info + n, "tls13 ", 6); n += 6;
    memcpy(info + n, label, ll); n += ll;
    info[n++] = (uint8_t)cl;
    memcpy(info + n, ctx, cl); n += cl;
    hkdf_expand(alg, secret, info, n, out, ol);
}

static void transcript_hash(struct tls_conn *t, uint8_t *out)
{
    hash_ctx_t c = t->transcript;
    hash_final(&c, out);
}

static void derive_secret(struct tls_conn *t, const uint8_t *secret, const char *label, const uint8_t *thash, uint8_t *out)
{
    expand_label(t->alg, secret, label, thash, (size_t)t->hslen, out, (size_t)t->hslen);
}

static void set_keys(struct tls_conn *t, const uint8_t *secret, int server)
{
    uint8_t *key = server ? t->s_key : t->c_key, *iv = server ? t->s_iv : t->c_iv;
    expand_label(t->alg, secret, "key", NULL, 0, key, (size_t)t->keylen);
    expand_label(t->alg, secret, "iv", NULL, 0, iv, 12);
    if (!t->aead_chacha) aes_setkey(server ? &t->s_aes : &t->c_aes, key, t->keylen * 8);
    if (server) { t->s_seq = 0; t->s_encrypted = 1; } else { t->c_seq = 0; t->c_encrypted = 1; }
}

/* ---- records ------------------------------------------------------------------- */
static int send_all(struct tls_conn *t, const uint8_t *p, size_t n)
{
    return crypto_tcp_send(t->sock, p, n) == 0 ? 0 : -1;
}

static int send_record(struct tls_conn *t, int type, const uint8_t *data, size_t n)
{
    static uint8_t out[TLS_REC_MAX + 5 + 32];
    if (!t->c_encrypted) {
        out[0] = (uint8_t)type; out[1] = 3; out[2] = 1; out[3] = (uint8_t)(n >> 8); out[4] = (uint8_t)n;
        memcpy(out + 5, data, n);
        return send_all(t, out, n + 5);
    }
    static uint8_t inner[TLS_REC_MAX];
    memcpy(inner, data, n);
    inner[n] = (uint8_t)type;
    size_t il = n + 1, cl = il + 16;
    out[0] = 23; out[1] = 3; out[2] = 3; out[3] = (uint8_t)(cl >> 8); out[4] = (uint8_t)cl;
    uint8_t nonce[12];
    memcpy(nonce, t->c_iv, 12);
    for (int i = 0; i < 8; i++) nonce[11 - i] ^= (uint8_t)(t->c_seq >> (8 * i));
    t->c_seq++;
    if (t->aead_chacha) chacha_poly_encrypt(t->c_key, nonce, out, 5, inner, il, out + 5, out + 5 + il);
    else aes_gcm_encrypt(&t->c_aes, nonce, out, 5, inner, il, out + 5, out + 5 + il);
    return send_all(t, out, cl + 5);
}

static int fill(struct tls_conn *t, size_t need, int timeout_ms)
{
    while (t->in_len < need) {
        int n = crypto_tcp_recv(t->sock, t->in + t->in_len, sizeof(t->in) - t->in_len, timeout_ms);
        if (n <= 0) return n == 0 ? 0 : -1;
        t->in_len += (size_t)n;
    }
    return 1;
}

/* Reads and (when keys are set) decrypts the next record into t->rec.
 * Returns 1, 0 at the end of the connection, -1 on errors. */
static int read_record(struct tls_conn *t, int timeout_ms)
{
    for (;;) {
        int r = fill(t, 5, timeout_ms);
        if (r <= 0) return r;
        size_t len = (size_t)t->in[3] << 8 | t->in[4];
        if (len > TLS_REC_MAX) { snprintf(t->err, sizeof(t->err), "record too large"); return -1; }
        r = fill(t, 5 + len, timeout_ms);
        if (r <= 0) return r < 0 ? -1 : 0;
        int type = t->in[0];
        uint8_t hdr[5];
        memcpy(hdr, t->in, 5);
        const uint8_t *body = t->in + 5;
        if (type == 20) {                                  /* change_cipher_spec: ignored in TLS 1.3 */
            memmove(t->in, t->in + 5 + len, t->in_len - 5 - len);
            t->in_len -= 5 + len;
            continue;
        }
        if (type == 23 && t->s_encrypted) {
            if (len < 17) return -1;
            uint8_t nonce[12];
            memcpy(nonce, t->s_iv, 12);
            for (int i = 0; i < 8; i++) nonce[11 - i] ^= (uint8_t)(t->s_seq >> (8 * i));
            t->s_seq++;
            size_t cl = len - 16;
            int bad = t->aead_chacha ? chacha_poly_decrypt(t->s_key, nonce, hdr, 5, body, cl, t->rec, body + cl)
                                     : aes_gcm_decrypt(&t->s_aes, nonce, hdr, 5, body, cl, t->rec, body + cl);
            if (bad) { snprintf(t->err, sizeof(t->err), "a record failed its integrity check"); return -1; }
            size_t pl = cl;
            while (pl && !t->rec[pl - 1]) pl--;              /* zero padding */
            if (!pl) return -1;
            t->rec_type = t->rec[pl - 1];
            t->rec_len = pl - 1;
        } else {
            memcpy(t->rec, body, len);
            t->rec_len = len;
            t->rec_type = type;
        }
        t->rec_off = 0;
        memmove(t->in, t->in + 5 + len, t->in_len - 5 - len);
        t->in_len -= 5 + len;
        if (t->rec_type == 21) {                           /* alert */
            if (t->rec_len >= 2 && t->rec[1] == 0) { t->eof = 1; return 0; }        /* close_notify */
            static const struct { int code; const char *text; } al[] = {
                { 40, "handshake failure (the server found nothing in common, perhaps it only supports TLS 1.2)" },
                { 70, "protocol version not supported: the server only speaks an older TLS" },
                { 112, "unknown server name" }, { 42, "bad certificate" }, { 20, "bad record MAC" }, { 47, "illegal parameter" },
            };
            int code = t->rec_len >= 2 ? t->rec[1] : 0;
            snprintf(t->err, sizeof(t->err), "the server ended the connection (alert %d)", code);
            for (size_t i = 0; i < sizeof(al) / sizeof(al[0]); i++) if (al[i].code == code) snprintf(t->err, sizeof(t->err), "%s", al[i].text);
            return -1;
        }
        return 1;
    }
}

/* Next complete handshake message (type, body) into t->hs; adds it to the
 * transcript if add is set. */
static int next_handshake(struct tls_conn *t, int *type, const uint8_t **body, size_t *blen, uint8_t **whole, size_t *wlen)
{
    for (;;) {
        if (t->hs_len >= 4) {
            size_t ml = (size_t)t->hs[1] << 16 | (size_t)t->hs[2] << 8 | t->hs[3];
            if (ml + 4 > TLS_HS_MAX) return -1;
            if (t->hs_len >= ml + 4) {
                *type = t->hs[0];
                *body = t->hs + 4;
                *blen = ml;
                *whole = t->hs;
                *wlen = ml + 4;
                return 1;
            }
        }
        /* leftover bytes of the current record first */
        if (t->rec_type == 22 && t->rec_off < t->rec_len) {
            size_t k = t->rec_len - t->rec_off;
            if (t->hs_len + k > TLS_HS_MAX) return -1;
            memcpy(t->hs + t->hs_len, t->rec + t->rec_off, k);
            t->hs_len += k;
            t->rec_off = t->rec_len;
            continue;
        }
        int r = read_record(t, 15000);
        if (r <= 0) return -1;
        if (t->rec_type != 22) { snprintf(t->err, sizeof(t->err), "unexpected record type %d during the handshake", t->rec_type); return -1; }
    }
}

static void consume_handshake(struct tls_conn *t, size_t wlen)
{
    memmove(t->hs, t->hs + wlen, t->hs_len - wlen);
    t->hs_len -= wlen;
}

/* ---- handshake ------------------------------------------------------------------- */
static const uint8_t HRR_RANDOM[32] = { 0xCF,0x21,0xAD,0x74,0xE5,0x9A,0x61,0x11,0xBE,0x1D,0x8C,0x02,0x1E,0x65,0xB8,0x91,
                                        0xC2,0xA2,0x11,0x16,0x7A,0xBB,0x8C,0x5E,0x07,0x9E,0x09,0xE2,0xC8,0xA8,0x33,0x9C };

static size_t put16(uint8_t *p, size_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; return 2; }

tls_conn_t *tls_connect(int sock, const char *host, char *err, size_t en)
{
    struct tls_conn *t = (struct tls_conn *)crypto_alloc(sizeof(struct tls_conn));
    if (!t) { snprintf(err, en, "out of memory"); return NULL; }
    memset(t, 0, sizeof(*t));
    t->sock = sock;

    /* ---- ClientHello ---- */
    uint8_t priv[32], pub[32];
    crypto_random(priv, 32);
    x25519_base(pub, priv);
    static uint8_t ch[1024];
    size_t n = 4;
    ch[n++] = 3; ch[n++] = 3;
    crypto_random(ch + n, 32); n += 32;
    ch[n++] = 32; crypto_random(ch + n, 32); n += 32;           /* legacy session id (middlebox compatibility) */
    n += put16(ch + n, 6);
    n += put16(ch + n, 0x1301); n += put16(ch + n, 0x1303); n += put16(ch + n, 0x1302);
    ch[n++] = 1; ch[n++] = 0;
    size_t ext_len_at = n;
    n += 2;
    size_t hl = strlen(host);
    if (hl > 200) hl = 200;
    n += put16(ch + n, 0x0000); n += put16(ch + n, hl + 5); n += put16(ch + n, hl + 3);
    ch[n++] = 0; n += put16(ch + n, hl); memcpy(ch + n, host, hl); n += hl;
    n += put16(ch + n, 0x000a); n += put16(ch + n, 4); n += put16(ch + n, 2); n += put16(ch + n, 0x001d);
    static const uint16_t sigs[] = { 0x0403, 0x0503, 0x0804, 0x0805, 0x0806, 0x0401, 0x0501, 0x0601 };
    n += put16(ch + n, 0x000d); n += put16(ch + n, 2 + 2 * 8); n += put16(ch + n, 2 * 8);
    for (int i = 0; i < 8; i++) n += put16(ch + n, sigs[i]);
    n += put16(ch + n, 0x002b); n += put16(ch + n, 3); ch[n++] = 2; n += put16(ch + n, 0x0304);
    n += put16(ch + n, 0x0033); n += put16(ch + n, 38); n += put16(ch + n, 36); n += put16(ch + n, 0x001d); n += put16(ch + n, 32);
    memcpy(ch + n, pub, 32); n += 32;
    n += put16(ch + n, 0x0010); n += put16(ch + n, 11); n += put16(ch + n, 9); ch[n++] = 8; memcpy(ch + n, "http/1.1", 8); n += 8;
    put16(ch + ext_len_at, n - ext_len_at - 2);
    ch[0] = 1; ch[1] = (uint8_t)((n - 4) >> 16); ch[2] = (uint8_t)((n - 4) >> 8); ch[3] = (uint8_t)(n - 4);
    if (send_record(t, 22, ch, n) != 0) { snprintf(err, en, "could not send to the server"); goto fail; }

    /* ---- ServerHello ---- */
    int type;
    const uint8_t *b;
    size_t bl, wl;
    uint8_t *whole;
    if (next_handshake(t, &type, &b, &bl, &whole, &wl) != 1 || type != 2 || bl < 38) {
        snprintf(err, en, "%s", t->err[0] ? t->err : "no reply from the server (is it really https?)");
        goto fail;
    }
    if (!memcmp(b + 2, HRR_RANDOM, 32)) { snprintf(err, en, "the server wants a key exchange other than X25519"); goto fail; }
    size_t p = 34;
    size_t sid = b[p++];
    p += sid;
    if (p + 3 > bl) goto bad;
    t->suite = b[p] << 8 | b[p + 1];
    p += 3;
    int version = 0;
    const uint8_t *spub = NULL;
    if (p + 2 <= bl) {
        size_t el = (size_t)b[p] << 8 | b[p + 1], e = p + 2, ee = e + el;
        if (ee > bl) goto bad;
        while (e + 4 <= ee) {
            int et = b[e] << 8 | b[e + 1];
            size_t xl = (size_t)b[e + 2] << 8 | b[e + 3];
            const uint8_t *x = b + e + 4;
            if (e + 4 + xl > ee) goto bad;
            if (et == 0x002b && xl == 2) version = x[0] << 8 | x[1];
            if (et == 0x0033 && xl == 36 && (x[0] << 8 | x[1]) == 0x001d && (x[2] << 8 | x[3]) == 32) spub = x + 4;
            e += 4 + xl;
        }
    }
    if (version != 0x0304) { snprintf(err, en, "the server only supports TLS 1.2 or older, which JARVIS OS does not speak yet"); goto fail; }
    if (!spub) { snprintf(err, en, "the server did not send an X25519 key"); goto fail; }
    switch (t->suite) {
    case 0x1301: t->alg = HASH_SHA256; t->keylen = 16; break;
    case 0x1302: t->alg = HASH_SHA384; t->keylen = 32; break;
    case 0x1303: t->alg = HASH_SHA256; t->keylen = 32; t->aead_chacha = 1; break;
    default: snprintf(err, en, "the server chose an unknown cipher"); goto fail;
    }
    t->hslen = hash_len(t->alg);
    hash_init(&t->transcript, t->alg);
    hash_update(&t->transcript, ch, n);
    hash_update(&t->transcript, whole, wl);
    consume_handshake(t, wl);

    /* ---- key schedule: handshake secrets ---- */
    uint8_t shared[32], zero[48] = { 0 }, early[48], derived[48], th[64], empty_hash[64];
    x25519(shared, priv, spub);
    {
        uint8_t z = 0;
        for (int i = 0; i < 32; i++) z |= shared[i];
        if (!z) goto bad;
    }
    hkdf_extract(t->alg, NULL, 0, zero, (size_t)t->hslen, early);
    hash_once(t->alg, "", 0, empty_hash);
    derive_secret(t, early, "derived", empty_hash, derived);
    hkdf_extract(t->alg, derived, (size_t)t->hslen, shared, 32, t->hs_secret);
    transcript_hash(t, th);
    derive_secret(t, t->hs_secret, "c hs traffic", th, t->c_secret);
    derive_secret(t, t->hs_secret, "s hs traffic", th, t->s_secret);
    set_keys(t, t->s_secret, 1);
    memset(priv, 0, sizeof(priv));

    /* ---- EncryptedExtensions, Certificate, CertificateVerify, Finished ---- */
    static uint8_t certbuf[48 * 1024];
    const uint8_t *cert_der[8];
    size_t cert_len[8];
    int ncert = 0, got_cv = 0;
    x509_cert_t leaf;
    for (;;) {
        if (next_handshake(t, &type, &b, &bl, &whole, &wl) != 1) { snprintf(err, en, "%s", t->err[0] ? t->err : "the handshake stopped"); goto fail; }
        if (type == 8) {                                    /* EncryptedExtensions */
        } else if (type == 11) {                            /* Certificate */
            size_t q = 1 + b[0];
            if (q + 3 > bl) goto bad;
            size_t ll = (size_t)b[q] << 16 | (size_t)b[q + 1] << 8 | b[q + 2], end = q + 3 + ll, used = 0;
            q += 3;
            if (end > bl) goto bad;
            while (q + 3 <= end && ncert < 8) {
                size_t cl = (size_t)b[q] << 16 | (size_t)b[q + 1] << 8 | b[q + 2];
                q += 3;
                if (q + cl + 2 > end || used + cl > sizeof(certbuf)) goto bad;
                memcpy(certbuf + used, b + q, cl);
                cert_der[ncert] = certbuf + used;
                cert_len[ncert++] = cl;
                used += cl;
                q += cl;
                size_t xl = (size_t)b[q] << 8 | b[q + 1];
                q += 2 + xl;
            }
            if (!ncert) { snprintf(err, en, "the server sent no certificate"); goto fail; }
            char why[128];
            if (x509_verify_chain(cert_der, cert_len, ncert, host, why, sizeof(why)) != 0) { snprintf(err, en, "not secure: %s", why); goto fail; }
            if (x509_parse(&leaf, cert_der[0], cert_len[0]) != 0) goto bad;
        } else if (type == 15) {                            /* CertificateVerify */
            if (!ncert || bl < 4) goto bad;
            int scheme = b[0] << 8 | b[1];
            size_t sl = (size_t)b[2] << 8 | b[3];
            if (4 + sl > bl) goto bad;
            static uint8_t content[64 + 34 + 64];
            memset(content, 0x20, 64);
            memcpy(content + 64, "TLS 1.3, server CertificateVerify", 33);
            content[97] = 0;
            transcript_hash(t, content + 98);
            if (x509_key_verify(&leaf, scheme, content, 98 + (size_t)t->hslen, b + 4, sl) != 0) {
                snprintf(err, en, "not secure: the server could not prove it owns its certificate (signature scheme %04x)", scheme);
                goto fail;
            }
            got_cv = 1;
        } else if (type == 20) {                            /* Finished */
            if (!got_cv) { snprintf(err, en, "not secure: the server skipped its certificate"); goto fail; }
            uint8_t fk[48], expect[64];
            expand_label(t->alg, t->s_secret, "finished", NULL, 0, fk, (size_t)t->hslen);
            transcript_hash(t, th);
            hmac(t->alg, fk, (size_t)t->hslen, th, (size_t)t->hslen, expect);
            if (bl != (size_t)t->hslen || memcmp(expect, b, bl)) { snprintf(err, en, "not secure: the handshake was tampered with"); goto fail; }
            hash_update(&t->transcript, whole, wl);
            consume_handshake(t, wl);
            break;
        } else {
            snprintf(err, en, "unexpected handshake message %d", type);
            goto fail;
        }
        hash_update(&t->transcript, whole, wl);
        consume_handshake(t, wl);
    }

    /* ---- application secrets, then our Finished ---- */
    uint8_t master[48], th_sf[64], cfk[48], verify[64];
    transcript_hash(t, th_sf);
    derive_secret(t, t->hs_secret, "derived", empty_hash, derived);
    hkdf_extract(t->alg, derived, (size_t)t->hslen, zero, (size_t)t->hslen, master);
    uint8_t c_ap[48], s_ap[48];
    derive_secret(t, master, "c ap traffic", th_sf, c_ap);
    derive_secret(t, master, "s ap traffic", th_sf, s_ap);

    static const uint8_t ccs[6] = { 20, 3, 3, 0, 1, 1 };
    send_all(t, ccs, 6);
    set_keys(t, t->c_secret, 0);
    expand_label(t->alg, t->c_secret, "finished", NULL, 0, cfk, (size_t)t->hslen);
    hmac(t->alg, cfk, (size_t)t->hslen, th_sf, (size_t)t->hslen, verify);
    uint8_t fin[4 + 64];
    fin[0] = 20; fin[1] = 0; fin[2] = 0; fin[3] = (uint8_t)t->hslen;
    memcpy(fin + 4, verify, (size_t)t->hslen);
    if (send_record(t, 22, fin, 4 + (size_t)t->hslen) != 0) { snprintf(err, en, "could not send to the server"); goto fail; }
    memcpy(t->c_secret, c_ap, 48);
    memcpy(t->s_secret, s_ap, 48);
    set_keys(t, t->c_secret, 0);
    set_keys(t, t->s_secret, 1);
    t->rec_len = t->rec_off = 0;
    return t;

bad:
    snprintf(err, en, "the server's handshake was not understood");
fail:
    crypto_free(t, sizeof(*t));
    return NULL;
}

const char *tls_cipher_name(tls_conn_t *t)
{
    return t->suite == 0x1301 ? "TLS 1.3, AES-128-GCM" : t->suite == 0x1302 ? "TLS 1.3, AES-256-GCM" : "TLS 1.3, ChaCha20-Poly1305";
}

int tls_write(tls_conn_t *t, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    while (len) {
        size_t k = len > 16000 ? 16000 : len;
        if (send_record(t, 23, p, k) != 0) return -1;
        p += k; len -= k;
    }
    return 0;
}

int tls_read(tls_conn_t *t, void *buf, size_t max, int timeout_ms)
{
    for (;;) {
        if (t->rec_type == 23 && t->rec_off < t->rec_len) {
            size_t k = t->rec_len - t->rec_off;
            if (k > max) k = max;
            memcpy(buf, t->rec + t->rec_off, k);
            t->rec_off += k;
            return (int)k;
        }
        if (t->eof) return 0;
        int r = read_record(t, timeout_ms);
        if (r <= 0) return r;
        if (t->rec_type == 22) {
            /* NewSessionTicket (ignored) or KeyUpdate */
            size_t q = 0;
            while (q + 4 <= t->rec_len) {
                int ht = t->rec[q];
                size_t ml = (size_t)t->rec[q + 1] << 16 | (size_t)t->rec[q + 2] << 8 | t->rec[q + 3];
                if (ht == 24 && ml == 1) {
                    int requested = t->rec[q + 4];
                    expand_label(t->alg, t->s_secret, "traffic upd", NULL, 0, t->s_secret, (size_t)t->hslen);
                    set_keys(t, t->s_secret, 1);
                    if (requested) {
                        uint8_t ku[5] = { 24, 0, 0, 1, 0 };
                        send_record(t, 22, ku, 5);
                        expand_label(t->alg, t->c_secret, "traffic upd", NULL, 0, t->c_secret, (size_t)t->hslen);
                        set_keys(t, t->c_secret, 0);
                    }
                }
                q += 4 + ml;
            }
            t->rec_off = t->rec_len;
        }
    }
}

void tls_close(tls_conn_t *t)
{
    if (!t) return;
    static const uint8_t close_notify[2] = { 1, 0 };
    send_record(t, 21, close_notify, 2);
    crypto_free(t, sizeof(*t));
}

/* ---- kernel platform hooks ------------------------------------------------------ */
#ifndef CRYPTO_HOST
#include "kernel.h"
void *crypto_alloc(size_t n) { return n >= LARGE_ALLOC_THRESHOLD ? kmalloc_large(n) : kmalloc(n); }
void crypto_free(void *p, size_t n) { if (!p) return; if (n >= LARGE_ALLOC_THRESHOLD) kfree_large(p, n); else kfree(p); }
int64_t crypto_now(void) { return sys_unix_time(); }
int crypto_tcp_send(int sock, const void *data, size_t len) { return tcp_send(sock, data, len); }
int crypto_tcp_recv(int sock, void *buf, size_t max, int timeout_ms) { return tcp_recv(sock, buf, max, timeout_ms); }
void crypto_platform_entropy(void)
{
    uint64_t v[6];
    v[0] = g_kstate.uptime_ticks;
    v[1] = (uint64_t)sys_unix_time();
    net_info_t ni;
    net_get_info(&ni);
    v[2] = ni.rx_frames ^ (ni.tx_frames << 20);
    v[3] = irq_count(0) ^ (irq_count(1) << 16) ^ (irq_count(12) << 32);
    v[4] = (uint64_t)(uintptr_t)&v;
    v[5] = irq_count(11) ^ (irq_count(10) << 24);
    crypto_add_entropy(v, sizeof(v));
}
#endif
