/*
 * =============================================================================
 * JARVIS AI OS — Cryptography for TLS (kernel/crypto.c)
 * Language: C17
 *
 * Everything the TLS 1.3 client (kernel/tls.c) needs, with no outside code:
 *   hashes        SHA-256, SHA-384, SHA-512; HMAC; HKDF (RFC 5869)
 *   ciphers       AES-128/256-GCM, ChaCha20-Poly1305 (RFC 8439)
 *   key exchange  X25519 (RFC 7748)
 *   signatures    RSA PKCS#1 v1.5 and PSS verification, ECDSA verification
 *                 on P-256 and P-384 (big-number arithmetic below)
 *   randomness    a ChaCha20 generator seeded from RDRAND (when the CPU
 *                 has it) and CPU timing jitter
 *
 * Verification only: JARVIS OS never signs, so no private-key RSA/ECDSA
 * code exists. Built so it also compiles on a normal Linux host for tests
 * (tools/test, CRYPTO_HOST).
 * =============================================================================
 */

#include "crypto.h"
#include <string.h>

/* ==========================================================================
 * SHA-256
 * ========================================================================== */
static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t *h, const uint8_t *b)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)b[i * 4] << 24 | (uint32_t)b[i * 4 + 1] << 16 | (uint32_t)b[i * 4 + 2] << 8 | b[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
    }
    h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

/* ==========================================================================
 * SHA-512 / SHA-384
 * ========================================================================== */
static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL,0x142929670a0e6e70ULL,0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
    0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,0xca273eceea26619cULL,0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
    0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL };
#define ROR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

static void sha512_block(uint64_t *h, const uint8_t *b)
{
    uint64_t w[80];
    for (int i = 0; i < 16; i++) {
        uint64_t v = 0;
        for (int k = 0; k < 8; k++) v = v << 8 | b[i * 8 + k];
        w[i] = v;
    }
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = ROR64(w[i - 15], 1) ^ ROR64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = ROR64(w[i - 2], 19) ^ ROR64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint64_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 80; i++) {
        uint64_t t1 = hh + (ROR64(e, 14) ^ ROR64(e, 18) ^ ROR64(e, 41)) + ((e & f) ^ (~e & g)) + K512[i] + w[i];
        uint64_t t2 = (ROR64(a, 28) ^ ROR64(a, 34) ^ ROR64(a, 39)) + ((a & bb) ^ (a & c) ^ (bb & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
    }
    h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

/* ---- generic streaming interface ---------------------------------------------- */
int hash_len(int alg) { return alg == HASH_SHA256 ? 32 : alg == HASH_SHA384 ? 48 : 64; }

void hash_init(hash_ctx_t *c, int alg)
{
    memset(c, 0, sizeof(*c));
    c->alg = alg;
    if (alg == HASH_SHA256) {
        static const uint32_t iv[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
        memcpy(c->h32, iv, sizeof(iv));
    } else if (alg == HASH_SHA384) {
        static const uint64_t iv[8] = { 0xcbbb9d5dc1059ed8ULL,0x629a292a367cd507ULL,0x9159015a3070dd17ULL,0x152fecd8f70e5939ULL,
                                        0x67332667ffc00b31ULL,0x8eb44a8768581511ULL,0xdb0c2e0d64f98fa7ULL,0x47b5481dbefa4fa4ULL };
        memcpy(c->h64, iv, sizeof(iv));
    } else {
        static const uint64_t iv[8] = { 0x6a09e667f3bcc908ULL,0xbb67ae8584caa73bULL,0x3c6ef372fe94f82bULL,0xa54ff53a5f1d36f1ULL,
                                        0x510e527fade682d1ULL,0x9b05688c2b3e6c1fULL,0x1f83d9abfb41bd6bULL,0x5be0cd19137e2179ULL };
        memcpy(c->h64, iv, sizeof(iv));
    }
}

void hash_update(hash_ctx_t *c, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t bs = c->alg == HASH_SHA256 ? 64 : 128;
    c->total += n;
    while (n) {
        size_t k = bs - c->blen;
        if (k > n) k = n;
        memcpy(c->buf + c->blen, p, k);
        c->blen += k; p += k; n -= k;
        if (c->blen == bs) {
            if (bs == 64) sha256_block(c->h32, c->buf); else sha512_block(c->h64, c->buf);
            c->blen = 0;
        }
    }
}

void hash_final(hash_ctx_t *c, uint8_t *out)
{
    size_t bs = c->alg == HASH_SHA256 ? 64 : 128, lenb = bs == 64 ? 8 : 16;
    uint64_t bits = c->total * 8;
    uint8_t pad = 0x80;
    hash_update(c, &pad, 1);
    uint8_t z = 0;
    while (c->blen != bs - lenb) hash_update(c, &z, 1);
    uint8_t L[16] = { 0 };
    for (int i = 0; i < 8; i++) L[lenb - 1 - i] = (uint8_t)(bits >> (8 * i));
    hash_update(c, L, lenb);
    int hl = hash_len(c->alg);
    for (int i = 0; i < hl; i++)
        out[i] = bs == 64 ? (uint8_t)(c->h32[i / 4] >> (24 - 8 * (i % 4))) : (uint8_t)(c->h64[i / 8] >> (56 - 8 * (i % 8)));
}

void hash_once(int alg, const void *data, size_t n, uint8_t *out)
{
    hash_ctx_t c;
    hash_init(&c, alg);
    hash_update(&c, data, n);
    hash_final(&c, out);
}

/* ---- HMAC / HKDF ---------------------------------------------------------------- */
void hmac(int alg, const uint8_t *key, size_t kl, const uint8_t *msg, size_t ml, uint8_t *out)
{
    size_t bs = alg == HASH_SHA256 ? 64 : 128;
    uint8_t k[128] = { 0 }, pad[128];
    if (kl > bs) hash_once(alg, key, kl, k); else memcpy(k, key, kl);
    hash_ctx_t c;
    uint8_t inner[64];
    for (size_t i = 0; i < bs; i++) pad[i] = k[i] ^ 0x36;
    hash_init(&c, alg); hash_update(&c, pad, bs); hash_update(&c, msg, ml); hash_final(&c, inner);
    for (size_t i = 0; i < bs; i++) pad[i] = k[i] ^ 0x5c;
    hash_init(&c, alg); hash_update(&c, pad, bs); hash_update(&c, inner, (size_t)hash_len(alg)); hash_final(&c, out);
}

void hkdf_extract(int alg, const uint8_t *salt, size_t sl, const uint8_t *ikm, size_t il, uint8_t *prk)
{
    uint8_t zero[64] = { 0 };
    if (!salt) { salt = zero; sl = (size_t)hash_len(alg); }
    hmac(alg, salt, sl, ikm, il, prk);
}

void hkdf_expand(int alg, const uint8_t *prk, const uint8_t *info, size_t il, uint8_t *out, size_t ol)
{
    size_t hl = (size_t)hash_len(alg), done = 0;
    uint8_t t[64], buf[64 + 300 + 1];
    size_t tl = 0;
    for (uint8_t ctr = 1; done < ol; ctr++) {
        size_t bl = 0;
        memcpy(buf, t, tl); bl += tl;
        memcpy(buf + bl, info, il); bl += il;
        buf[bl++] = ctr;
        hmac(alg, prk, hl, buf, bl, t);
        tl = hl;
        size_t k = ol - done < hl ? ol - done : hl;
        memcpy(out + done, t, k);
        done += k;
    }
}

/* ==========================================================================
 * AES (table-based) and GCM
 * ========================================================================== */
static uint8_t SBOX[256];
static uint32_t TE[4][256];
static int aes_ready;

static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }

static void aes_tables(void)
{
    /* S-box from the multiplicative inverse and the affine map */
    uint8_t p = 1, q = 1;
    do {
        p = (uint8_t)(p ^ (p << 1) ^ (p & 0x80 ? 0x1B : 0));
        q ^= (uint8_t)(q << 1); q ^= (uint8_t)(q << 2); q ^= (uint8_t)(q << 4);
        if (q & 0x80) q ^= 0x09;
        uint8_t x = (uint8_t)(q ^ (q << 1 | q >> 7) ^ (q << 2 | q >> 6) ^ (q << 3 | q >> 5) ^ (q << 4 | q >> 4));
        SBOX[p] = x ^ 0x63;
    } while (p != 1);
    SBOX[0] = 0x63;
    for (int i = 0; i < 256; i++) {
        uint8_t s = SBOX[i], s2 = xt(s), s3 = s2 ^ s;
        uint32_t t = (uint32_t)s2 << 24 | (uint32_t)s << 16 | (uint32_t)s << 8 | s3;
        TE[0][i] = t;
        TE[1][i] = ROR32(t, 8);
        TE[2][i] = ROR32(t, 16);
        TE[3][i] = ROR32(t, 24);
    }
    aes_ready = 1;
}

void aes_setkey(aes_ctx_t *c, const uint8_t *key, int bits)
{
    if (!aes_ready) aes_tables();
    int nk = bits / 32;
    c->rounds = nk + 6;
    uint32_t *w = c->rk;
    for (int i = 0; i < nk; i++) w[i] = (uint32_t)key[4 * i] << 24 | (uint32_t)key[4 * i + 1] << 16 | (uint32_t)key[4 * i + 2] << 8 | key[4 * i + 3];
    uint8_t rcon = 1;
    for (int i = nk; i < 4 * (c->rounds + 1); i++) {
        uint32_t t = w[i - 1];
        if (i % nk == 0) {
            t = (uint32_t)SBOX[(t >> 16) & 255] << 24 | (uint32_t)SBOX[(t >> 8) & 255] << 16 | (uint32_t)SBOX[t & 255] << 8 | SBOX[t >> 24];
            t ^= (uint32_t)rcon << 24;
            rcon = xt(rcon);
        } else if (nk > 6 && i % nk == 4) {
            t = (uint32_t)SBOX[t >> 24] << 24 | (uint32_t)SBOX[(t >> 16) & 255] << 16 | (uint32_t)SBOX[(t >> 8) & 255] << 8 | SBOX[t & 255];
        }
        w[i] = w[i - nk] ^ t;
    }
}

void aes_encrypt(const aes_ctx_t *c, const uint8_t *in, uint8_t *out)
{
    const uint32_t *rk = c->rk;
    uint32_t s[4], t[4];
    for (int i = 0; i < 4; i++) s[i] = ((uint32_t)in[4 * i] << 24 | (uint32_t)in[4 * i + 1] << 16 | (uint32_t)in[4 * i + 2] << 8 | in[4 * i + 3]) ^ rk[i];
    for (int r = 1; r < c->rounds; r++) {
        for (int i = 0; i < 4; i++)
            t[i] = TE[0][s[i] >> 24] ^ TE[1][(s[(i + 1) & 3] >> 16) & 255] ^ TE[2][(s[(i + 2) & 3] >> 8) & 255] ^ TE[3][s[(i + 3) & 3] & 255] ^ rk[4 * r + i];
        memcpy(s, t, sizeof(s));
    }
    for (int i = 0; i < 4; i++) {
        uint32_t v = (uint32_t)SBOX[s[i] >> 24] << 24 | (uint32_t)SBOX[(s[(i + 1) & 3] >> 16) & 255] << 16 |
                     (uint32_t)SBOX[(s[(i + 2) & 3] >> 8) & 255] << 8 | SBOX[s[(i + 3) & 3] & 255];
        v ^= rk[4 * c->rounds + i];
        out[4 * i] = (uint8_t)(v >> 24); out[4 * i + 1] = (uint8_t)(v >> 16); out[4 * i + 2] = (uint8_t)(v >> 8); out[4 * i + 3] = (uint8_t)v;
    }
}

/* GHASH multiply in GF(2^128) (bit-by-bit; fine for web-sized records) */
static void gf128_mul(uint8_t *x, const uint8_t *h)
{
    uint8_t z[16] = { 0 }, v[16];
    memcpy(v, h, 16);
    for (int i = 0; i < 128; i++) {
        if (x[i / 8] & (0x80 >> (i % 8))) for (int k = 0; k < 16; k++) z[k] ^= v[k];
        int lsb = v[15] & 1;
        for (int k = 15; k > 0; k--) v[k] = (uint8_t)(v[k] >> 1 | v[k - 1] << 7);
        v[0] >>= 1;
        if (lsb) v[0] ^= 0xE1;
    }
    memcpy(x, z, 16);
}

static void ghash(const uint8_t *h, const uint8_t *aad, size_t al, const uint8_t *ct, size_t cl, uint8_t *out)
{
    uint8_t y[16] = { 0 }, blk[16];
    for (size_t i = 0; i < al; i += 16) {
        memset(blk, 0, 16); memcpy(blk, aad + i, al - i < 16 ? al - i : 16);
        for (int k = 0; k < 16; k++) y[k] ^= blk[k];
        gf128_mul(y, h);
    }
    for (size_t i = 0; i < cl; i += 16) {
        memset(blk, 0, 16); memcpy(blk, ct + i, cl - i < 16 ? cl - i : 16);
        for (int k = 0; k < 16; k++) y[k] ^= blk[k];
        gf128_mul(y, h);
    }
    uint64_t abits = (uint64_t)al * 8, cbits = (uint64_t)cl * 8;
    for (int k = 0; k < 8; k++) { blk[k] = (uint8_t)(abits >> (56 - 8 * k)); blk[8 + k] = (uint8_t)(cbits >> (56 - 8 * k)); }
    for (int k = 0; k < 16; k++) y[k] ^= blk[k];
    gf128_mul(y, h);
    memcpy(out, y, 16);
}

static void gcm_ctr(const aes_ctx_t *c, const uint8_t *iv, const uint8_t *in, uint8_t *out, size_t n)
{
    uint8_t ctr[16], ks[16];
    memcpy(ctr, iv, 12);
    uint32_t cnt = 2;
    for (size_t i = 0; i < n; i += 16) {
        ctr[12] = (uint8_t)(cnt >> 24); ctr[13] = (uint8_t)(cnt >> 16); ctr[14] = (uint8_t)(cnt >> 8); ctr[15] = (uint8_t)cnt;
        cnt++;
        aes_encrypt(c, ctr, ks);
        for (size_t k = 0; k < 16 && i + k < n; k++) out[i + k] = in[i + k] ^ ks[k];
    }
}

static void gcm_tag(const aes_ctx_t *c, const uint8_t *iv, const uint8_t *aad, size_t al, const uint8_t *ct, size_t cl, uint8_t *tag)
{
    uint8_t h[16] = { 0 }, j0[16], s[16];
    aes_encrypt(c, h, h);
    ghash(h, aad, al, ct, cl, s);
    memcpy(j0, iv, 12); j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
    aes_encrypt(c, j0, j0);
    for (int k = 0; k < 16; k++) tag[k] = s[k] ^ j0[k];
}

void aes_gcm_encrypt(const aes_ctx_t *c, const uint8_t *iv, const uint8_t *aad, size_t al, const uint8_t *in, size_t n, uint8_t *out, uint8_t *tag)
{
    gcm_ctr(c, iv, in, out, n);
    gcm_tag(c, iv, aad, al, out, n, tag);
}

int aes_gcm_decrypt(const aes_ctx_t *c, const uint8_t *iv, const uint8_t *aad, size_t al, const uint8_t *in, size_t n, uint8_t *out, const uint8_t *tag)
{
    uint8_t t[16];
    gcm_tag(c, iv, aad, al, in, n, t);
    uint8_t diff = 0;
    for (int k = 0; k < 16; k++) diff |= t[k] ^ tag[k];
    if (diff) return -1;
    gcm_ctr(c, iv, in, out, n);
    return 0;
}

/* ==========================================================================
 * ChaCha20-Poly1305 (RFC 8439)
 * ========================================================================== */
#define ROL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define QR(a, b, c, d) a += b; d ^= a; d = ROL32(d, 16); c += d; b ^= c; b = ROL32(b, 12); \
                       a += b; d ^= a; d = ROL32(d, 8);  c += d; b ^= c; b = ROL32(b, 7)
static uint32_t le32r(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

void chacha20_block(const uint8_t *key, uint32_t counter, const uint8_t *nonce, uint8_t *out)
{
    uint32_t s[16], x[16];
    s[0] = 0x61707865; s[1] = 0x3320646e; s[2] = 0x79622d32; s[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) s[4 + i] = le32r(key + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; i++) s[13 + i] = le32r(nonce + 4 * i);
    memcpy(x, s, sizeof(s));
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]); QR(x[1], x[5], x[9], x[13]); QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]); QR(x[2], x[7], x[8], x[13]); QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) {
        uint32_t v = x[i] + s[i];
        out[4 * i] = (uint8_t)v; out[4 * i + 1] = (uint8_t)(v >> 8); out[4 * i + 2] = (uint8_t)(v >> 16); out[4 * i + 3] = (uint8_t)(v >> 24);
    }
}

static void chacha20_xor(const uint8_t *key, uint32_t ctr, const uint8_t *nonce, const uint8_t *in, uint8_t *out, size_t n)
{
    uint8_t ks[64];
    for (size_t i = 0; i < n; i += 64, ctr++) {
        chacha20_block(key, ctr, nonce, ks);
        for (size_t k = 0; k < 64 && i + k < n; k++) out[i + k] = in[i + k] ^ ks[k];
    }
}

/* Poly1305 with 26-bit limbs (after poly1305-donna). */
static void poly1305(const uint8_t *key, const uint8_t *m, size_t n, uint8_t *tag)
{
    uint32_t r0 = le32r(key) & 0x3ffffff, r1 = (le32r(key + 3) >> 2) & 0x3ffff03, r2 = (le32r(key + 6) >> 4) & 0x3ffc0ff,
             r3 = (le32r(key + 9) >> 6) & 0x3f03fff, r4 = (le32r(key + 12) >> 8) & 0x00fffff;
    uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = 0, h1 = 0, h2 = 0, h3 = 0, h4 = 0;
    while (n) {
        uint8_t b[17] = { 0 };
        size_t k = n < 16 ? n : 16;
        memcpy(b, m, k);
        b[k] = 1;
        h0 += le32r(b) & 0x3ffffff;
        h1 += (le32r(b + 3) >> 2) & 0x3ffffff;
        h2 += (le32r(b + 6) >> 4) & 0x3ffffff;
        h3 += (le32r(b + 9) >> 6) & 0x3ffffff;
        h4 += (le32r(b + 12) >> 8) | ((uint32_t)b[16] << 24);
        uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;
        uint32_t c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff;
        d1 += c; c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff;
        d2 += c; c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff;
        d3 += c; c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff;
        d4 += c; c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
        h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
        m += k; n -= k;
    }
    uint32_t c = h1 >> 26; h1 &= 0x3ffffff; h2 += c; c = h2 >> 26; h2 &= 0x3ffffff; h3 += c;
    c = h3 >> 26; h3 &= 0x3ffffff; h4 += c; c = h4 >> 26; h4 &= 0x3ffffff; h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
    uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1u << 26);
    uint32_t mask = (g4 >> 31) - 1;
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2; h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;
    h0 = (h0 | (h1 << 26)); h1 = (h1 >> 6) | (h2 << 20); h2 = (h2 >> 12) | (h3 << 14); h3 = (h3 >> 18) | (h4 << 8);
    uint64_t f = (uint64_t)h0 + le32r(key + 16); h0 = (uint32_t)f;
    f = (uint64_t)h1 + le32r(key + 20) + (f >> 32); h1 = (uint32_t)f;
    f = (uint64_t)h2 + le32r(key + 24) + (f >> 32); h2 = (uint32_t)f;
    f = (uint64_t)h3 + le32r(key + 28) + (f >> 32); h3 = (uint32_t)f;
    uint32_t hs[4] = { h0, h1, h2, h3 };
    for (int i = 0; i < 4; i++) { tag[4 * i] = (uint8_t)hs[i]; tag[4 * i + 1] = (uint8_t)(hs[i] >> 8); tag[4 * i + 2] = (uint8_t)(hs[i] >> 16); tag[4 * i + 3] = (uint8_t)(hs[i] >> 24); }
}

static void cp_tag(const uint8_t *key, const uint8_t *nonce, const uint8_t *aad, size_t al, const uint8_t *ct, size_t cl, uint8_t *tag)
{
    uint8_t blk[64];
    chacha20_block(key, 0, nonce, blk);
    /* mac data: aad || pad || ct || pad || len(aad) || len(ct) (all le64) */
    size_t total = ((al + 15) & ~(size_t)15) + ((cl + 15) & ~(size_t)15) + 16;
    uint8_t *mac = (uint8_t *)crypto_alloc(total);
    if (!mac) { memset(tag, 0, 16); return; }
    memset(mac, 0, total);
    memcpy(mac, aad, al);
    size_t o = (al + 15) & ~(size_t)15;
    memcpy(mac + o, ct, cl);
    o += (cl + 15) & ~(size_t)15;
    for (int i = 0; i < 8; i++) { mac[o + i] = (uint8_t)((uint64_t)al >> (8 * i)); mac[o + 8 + i] = (uint8_t)((uint64_t)cl >> (8 * i)); }
    poly1305(blk, mac, total, tag);
    crypto_free(mac, total);
}

void chacha_poly_encrypt(const uint8_t *key, const uint8_t *nonce, const uint8_t *aad, size_t al, const uint8_t *in, size_t n, uint8_t *out, uint8_t *tag)
{
    chacha20_xor(key, 1, nonce, in, out, n);
    cp_tag(key, nonce, aad, al, out, n, tag);
}

int chacha_poly_decrypt(const uint8_t *key, const uint8_t *nonce, const uint8_t *aad, size_t al, const uint8_t *in, size_t n, uint8_t *out, const uint8_t *tag)
{
    uint8_t t[16], diff = 0;
    cp_tag(key, nonce, aad, al, in, n, t);
    for (int k = 0; k < 16; k++) diff |= t[k] ^ tag[k];
    if (diff) return -1;
    chacha20_xor(key, 1, nonce, in, out, n);
    return 0;
}

/* ==========================================================================
 * X25519 (after TweetNaCl, public domain)
 * ========================================================================== */
typedef int64_t gf[16];
static const gf G121665 = { 0xDB41, 1 };

static void car25519(gf o)
{
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}
static void sel25519(gf p, gf q, int b)
{
    int64_t c = ~(b - 1);
    for (int i = 0; i < 16; i++) { int64_t t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; }
}
static void pack25519(uint8_t *o, const gf n)
{
    gf m, t;
    for (int i = 0; i < 16; i++) t[i] = n[i];
    car25519(t); car25519(t); car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) { m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1); m[i - 1] &= 0xffff; }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) { o[2 * i] = (uint8_t)(t[i] & 0xff); o[2 * i + 1] = (uint8_t)(t[i] >> 8); }
}
static void unpack25519(gf o, const uint8_t *n)
{
    for (int i = 0; i < 16; i++) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}
static void fA(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void fZ(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }
static void fM(gf o, const gf a, const gf b)
{
    int64_t t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car25519(o); car25519(o);
}
static void fS(gf o, const gf a) { fM(o, a, a); }
static void inv25519(gf o, const gf in)
{
    gf c;
    for (int a = 0; a < 16; a++) c[a] = in[a];
    for (int a = 253; a >= 0; a--) { fS(c, c); if (a != 2 && a != 4) fM(c, c, in); }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}

void x25519(uint8_t *out, const uint8_t *scalar, const uint8_t *point)
{
    uint8_t z[32];
    int64_t x[80];
    gf a, b, c, d, e, f;
    for (int i = 0; i < 31; i++) z[i] = scalar[i];
    z[31] = (uint8_t)((scalar[31] & 127) | 64);
    z[0] &= 248;
    unpack25519(x, point);
    for (int i = 0; i < 16; i++) { b[i] = x[i]; d[i] = a[i] = c[i] = 0; }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        int r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, r); sel25519(c, d, r);
        fA(e, a, c); fZ(a, a, c); fA(c, b, d); fZ(b, b, d);
        fS(d, e); fS(f, a); fM(a, c, a); fM(c, b, e);
        fA(e, a, c); fZ(a, a, c); fS(b, a); fZ(c, d, f);
        fM(a, c, G121665); fA(a, a, d); fM(c, c, a); fM(a, d, f);
        fM(d, b, x); fS(b, e);
        sel25519(a, b, r); sel25519(c, d, r);
    }
    for (int i = 0; i < 16; i++) { x[i + 16] = a[i]; x[i + 32] = c[i]; x[i + 48] = b[i]; x[i + 64] = d[i]; }
    inv25519(x + 32, x + 32);
    fM(x + 16, x + 16, x + 32);
    pack25519(out, x + 16);
}

void x25519_base(uint8_t *out, const uint8_t *scalar)
{
    static const uint8_t nine[32] = { 9 };
    x25519(out, scalar, nine);
}

/* ==========================================================================
 * Big numbers (32-bit limbs, little-endian) and Montgomery arithmetic
 * ========================================================================== */
#define BN_LIMBS 136                        /* up to 4352 bits */
typedef struct { uint32_t d[BN_LIMBS]; int n; } bn_t;      /* n = limbs in use */

static void bn_from_bytes(bn_t *a, const uint8_t *p, size_t len, int limbs)
{
    memset(a, 0, sizeof(*a));
    a->n = limbs;
    for (size_t i = 0; i < len; i++) {
        size_t bit = (len - 1 - i) * 8;
        if (bit / 32 < BN_LIMBS) a->d[bit / 32] |= (uint32_t)p[i] << (bit % 32);
    }
}

static void bn_to_bytes(const bn_t *a, uint8_t *p, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        size_t bit = (len - 1 - i) * 8;
        p[i] = bit / 32 < BN_LIMBS ? (uint8_t)(a->d[bit / 32] >> (bit % 32)) : 0;
    }
}

static int bn_cmp(const bn_t *a, const bn_t *b, int n)
{
    for (int i = n - 1; i >= 0; i--) if (a->d[i] != b->d[i]) return a->d[i] > b->d[i] ? 1 : -1;
    return 0;
}

static uint32_t bn_sub(bn_t *r, const bn_t *a, const bn_t *b, int n)
{
    uint64_t borrow = 0;
    for (int i = 0; i < n; i++) {
        uint64_t t = (uint64_t)a->d[i] - b->d[i] - borrow;
        r->d[i] = (uint32_t)t;
        borrow = (t >> 63) & 1;
    }
    return (uint32_t)borrow;
}

static uint32_t bn_add(bn_t *r, const bn_t *a, const bn_t *b, int n)
{
    uint64_t c = 0;
    for (int i = 0; i < n; i++) { c += (uint64_t)a->d[i] + b->d[i]; r->d[i] = (uint32_t)c; c >>= 32; }
    return (uint32_t)c;
}

typedef struct { bn_t m, rr; uint32_t minv; int n; } mont_t;   /* modulus, R^2 mod m, -m^-1 mod 2^32 */

static void mont_mul(const mont_t *M, bn_t *r, const bn_t *a, const bn_t *b)
{
    int n = M->n;
    uint32_t t[BN_LIMBS + 2];
    memset(t, 0, sizeof(uint32_t) * (size_t)(n + 2));
    for (int i = 0; i < n; i++) {
        uint64_t c = 0;
        for (int j = 0; j < n; j++) { c += (uint64_t)t[j] + (uint64_t)a->d[j] * b->d[i]; t[j] = (uint32_t)c; c >>= 32; }
        c += t[n]; t[n] = (uint32_t)c; t[n + 1] = (uint32_t)(c >> 32);
        uint32_t m = t[0] * M->minv;
        c = ((uint64_t)t[0] + (uint64_t)m * M->m.d[0]) >> 32;
        for (int j = 1; j < n; j++) { c += (uint64_t)t[j] + (uint64_t)m * M->m.d[j]; t[j - 1] = (uint32_t)c; c >>= 32; }
        c += t[n]; t[n - 1] = (uint32_t)c; c >>= 32;
        t[n] = t[n + 1] + (uint32_t)c;
    }
    bn_t res;
    res.n = n;
    memcpy(res.d, t, sizeof(uint32_t) * (size_t)n);
    if (t[n] || bn_cmp(&res, &M->m, n) >= 0) bn_sub(&res, &res, &M->m, n);
    memset(r->d, 0, sizeof(r->d));
    memcpy(r->d, res.d, sizeof(uint32_t) * (size_t)n);
    r->n = n;
}

static void mont_init(mont_t *M, const bn_t *m)
{
    memset(M, 0, sizeof(*M));
    M->m = *m;
    int n = m->n;
    while (n > 1 && !m->d[n - 1]) n--;
    M->n = n;
    M->m.n = n;
    uint32_t inv = 1;                                   /* Newton: m0^-1 mod 2^32 */
    for (int i = 0; i < 5; i++) inv *= 2 - m->d[0] * inv;
    M->minv = (uint32_t)(0u - inv);
    /* R^2 mod m by doubling 1 2*32*n times */
    bn_t r;
    memset(&r, 0, sizeof(r));
    r.n = n;
    r.d[0] = 1;
    for (int i = 0; i < 64 * n; i++) {
        uint32_t carry = bn_add(&r, &r, &r, n);
        if (carry || bn_cmp(&r, &M->m, n) >= 0) bn_sub(&r, &r, &M->m, n);
    }
    M->rr = r;
}

static void mont_to(const mont_t *M, bn_t *r, const bn_t *a) { mont_mul(M, r, a, &M->rr); }
static void mont_from(const mont_t *M, bn_t *r, const bn_t *a)
{
    bn_t one;
    memset(&one, 0, sizeof(one));
    one.n = M->n;
    one.d[0] = 1;
    mont_mul(M, r, a, &one);
}

/* r = a^e mod m, e as big-endian bytes. a in normal form. */
static void mont_pow(const mont_t *M, bn_t *r, const bn_t *a, const uint8_t *e, size_t el)
{
    bn_t x, am, one;
    memset(&one, 0, sizeof(one));
    one.n = M->n;
    one.d[0] = 1;
    mont_to(M, &x, &one);
    mont_to(M, &am, a);
    for (size_t i = 0; i < el; i++)
        for (int b = 7; b >= 0; b--) {
            mont_mul(M, &x, &x, &x);
            if ((e[i] >> b) & 1) mont_mul(M, &x, &x, &am);
        }
    mont_from(M, r, &x);
}

/* ---- RSA verification ------------------------------------------------------------ */
static int rsa_public(const uint8_t *n, size_t nl, const uint8_t *e, size_t el, const uint8_t *sig, size_t sl, uint8_t *em)
{
    while (nl && !*n) { n++; nl--; }
    if (nl < 128 || nl > 512 || sl > nl) return -1;
    int limbs = (int)((nl + 3) / 4);
    static bn_t N, S, R;
    static mont_t M;
    bn_from_bytes(&N, n, nl, limbs);
    bn_from_bytes(&S, sig, sl, limbs);
    if (bn_cmp(&S, &N, limbs) >= 0) return -1;
    mont_init(&M, &N);
    mont_pow(&M, &R, &S, e, el);
    bn_to_bytes(&R, em, nl);
    return (int)nl;
}

static const uint8_t DI_SHA256[] = { 0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20 };
static const uint8_t DI_SHA384[] = { 0x30,0x41,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x02,0x05,0x00,0x04,0x30 };
static const uint8_t DI_SHA512[] = { 0x30,0x51,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x03,0x05,0x00,0x04,0x40 };

int rsa_verify_pkcs1(int alg, const uint8_t *n, size_t nl, const uint8_t *e, size_t el, const uint8_t *hashv, const uint8_t *sig, size_t sl)
{
    static uint8_t em[512];
    int k = rsa_public(n, nl, e, el, sig, sl, em);
    if (k < 0) return -1;
    const uint8_t *di = alg == HASH_SHA256 ? DI_SHA256 : alg == HASH_SHA384 ? DI_SHA384 : DI_SHA512;
    size_t dl = sizeof(DI_SHA256), hl = (size_t)hash_len(alg);
    size_t tlen = dl + hl;
    if ((size_t)k < tlen + 11) return -1;
    if (em[0] != 0 || em[1] != 1) return -1;
    size_t ps = (size_t)k - tlen - 3;
    for (size_t i = 0; i < ps; i++) if (em[2 + i] != 0xFF) return -1;
    if (em[2 + ps] != 0) return -1;
    if (memcmp(em + 3 + ps, di, dl) || memcmp(em + 3 + ps + dl, hashv, hl)) return -1;
    return 0;
}

/* RSASSA-PSS with MGF1 of the same hash, salt length = hash length. */
int rsa_verify_pss(int alg, const uint8_t *n, size_t nl, const uint8_t *e, size_t el, const uint8_t *mhash, const uint8_t *sig, size_t sl)
{
    static uint8_t em[512], db[512];
    while (nl && !*n) { n++; nl--; }
    int k = rsa_public(n, nl, e, el, sig, sl, em);
    if (k < 0) return -1;
    /* modBits - 1 = emBits; the top byte may carry fewer bits */
    int topbits = 0;
    for (int b = 7; b >= 0; b--) if (n[0] & (1 << b)) { topbits = b + 1; break; }
    size_t emlen = (size_t)k;
    const uint8_t *EM = em;
    if (topbits == 1) { if (em[0]) return -1; EM++; emlen--; }
    size_t hl = (size_t)hash_len(alg);
    if (emlen < 2 * hl + 2 || EM[emlen - 1] != 0xBC) return -1;
    size_t dbl = emlen - hl - 1;
    const uint8_t *H = EM + dbl;
    /* MGF1 */
    uint8_t cnt[4] = { 0 }, t[64];
    for (size_t o = 0, c = 0; o < dbl; c++) {
        cnt[0] = (uint8_t)(c >> 24); cnt[1] = (uint8_t)(c >> 16); cnt[2] = (uint8_t)(c >> 8); cnt[3] = (uint8_t)c;
        hash_ctx_t hc;
        hash_init(&hc, alg); hash_update(&hc, H, hl); hash_update(&hc, cnt, 4); hash_final(&hc, t);
        for (size_t i = 0; i < hl && o < dbl; i++, o++) db[o] = EM[o] ^ t[i];
    }
    int clear = topbits == 1 ? 8 : 8 - (topbits - 1);
    if (topbits != 1) db[0] &= (uint8_t)(0xFF >> (clear % 8 ? clear : 0));
    size_t ps = dbl - hl - 1;
    for (size_t i = 0; i < ps; i++) if (db[i]) return -1;
    if (db[ps] != 1) return -1;
    const uint8_t *salt = db + ps + 1;
    uint8_t mp[8 + 64 + 64], hh[64];
    memset(mp, 0, 8);
    memcpy(mp + 8, mhash, hl);
    memcpy(mp + 8 + hl, salt, hl);
    hash_once(alg, mp, 8 + 2 * hl, hh);
    return memcmp(hh, H, hl) ? -1 : 0;
}

/* ---- ECDSA (P-256, P-384) ------------------------------------------------------------ */
typedef struct {
    int bytes;
    const uint8_t *p, *b, *n, *gx, *gy;
} curve_t;

static const uint8_t P256_P[32] = { 0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff };
static const uint8_t P256_B[32] = { 0x5a,0xc6,0x35,0xd8,0xaa,0x3a,0x93,0xe7,0xb3,0xeb,0xbd,0x55,0x76,0x98,0x86,0xbc,0x65,0x1d,0x06,0xb0,0xcc,0x53,0xb0,0xf6,0x3b,0xce,0x3c,0x3e,0x27,0xd2,0x60,0x4b };
static const uint8_t P256_N[32] = { 0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xbc,0xe6,0xfa,0xad,0xa7,0x17,0x9e,0x84,0xf3,0xb9,0xca,0xc2,0xfc,0x63,0x25,0x51 };
static const uint8_t P256_GX[32] = { 0x6b,0x17,0xd1,0xf2,0xe1,0x2c,0x42,0x47,0xf8,0xbc,0xe6,0xe5,0x63,0xa4,0x40,0xf2,0x77,0x03,0x7d,0x81,0x2d,0xeb,0x33,0xa0,0xf4,0xa1,0x39,0x45,0xd8,0x98,0xc2,0x96 };
static const uint8_t P256_GY[32] = { 0x4f,0xe3,0x42,0xe2,0xfe,0x1a,0x7f,0x9b,0x8e,0xe7,0xeb,0x4a,0x7c,0x0f,0x9e,0x16,0x2b,0xce,0x33,0x57,0x6b,0x31,0x5e,0xce,0xcb,0xb6,0x40,0x68,0x37,0xbf,0x51,0xf5 };
static const uint8_t P384_P[48] = { 0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
                                    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xfe,0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff };
static const uint8_t P384_B[48] = { 0xb3,0x31,0x2f,0xa7,0xe2,0x3e,0xe7,0xe4,0x98,0x8e,0x05,0x6b,0xe3,0xf8,0x2d,0x19,0x18,0x1d,0x9c,0x6e,0xfe,0x81,0x41,0x12,
                                    0x03,0x14,0x08,0x8f,0x50,0x13,0x87,0x5a,0xc6,0x56,0x39,0x8d,0x8a,0x2e,0xd1,0x9d,0x2a,0x85,0xc8,0xed,0xd3,0xec,0x2a,0xef };
static const uint8_t P384_N[48] = { 0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
                                    0xc7,0x63,0x4d,0x81,0xf4,0x37,0x2d,0xdf,0x58,0x1a,0x0d,0xb2,0x48,0xb0,0xa7,0x7a,0xec,0xec,0x19,0x6a,0xcc,0xc5,0x29,0x73 };
static const uint8_t P384_GX[48] = { 0xaa,0x87,0xca,0x22,0xbe,0x8b,0x05,0x37,0x8e,0xb1,0xc7,0x1e,0xf3,0x20,0xad,0x74,0x6e,0x1d,0x3b,0x62,0x8b,0xa7,0x9b,0x98,
                                     0x59,0xf7,0x41,0xe0,0x82,0x54,0x2a,0x38,0x55,0x02,0xf2,0x5d,0xbf,0x55,0x29,0x6c,0x3a,0x54,0x5e,0x38,0x72,0x76,0x0a,0xb7 };
static const uint8_t P384_GY[48] = { 0x36,0x17,0xde,0x4a,0x96,0x26,0x2c,0x6f,0x5d,0x9e,0x98,0xbf,0x92,0x92,0xdc,0x29,0xf8,0xf4,0x1d,0xbd,0x28,0x9a,0x14,0x7c,
                                     0xe9,0xda,0x31,0x13,0xb5,0xf0,0xb8,0xc0,0x0a,0x60,0xb1,0xce,0x1d,0x7e,0x81,0x9d,0x7a,0x43,0x1d,0x7c,0x90,0xea,0x0e,0x5f };

typedef struct { bn_t x, y, z; int inf; } ecp_t;            /* Jacobian, Montgomery form */

static mont_t g_fp;                                         /* field arithmetic of the curve in use */
#define FM(r, a, b) mont_mul(&g_fp, (r), (a), (b))

static void fadd(bn_t *r, const bn_t *a, const bn_t *b)
{
    int n = g_fp.n;
    uint32_t c = bn_add(r, a, b, n);
    if (c || bn_cmp(r, &g_fp.m, n) >= 0) bn_sub(r, r, &g_fp.m, n);
}
static void fsub(bn_t *r, const bn_t *a, const bn_t *b)
{
    int n = g_fp.n;
    if (bn_sub(r, a, b, n)) bn_add(r, r, &g_fp.m, n);
}

static void ec_double(ecp_t *r, const ecp_t *p)
{
    if (p->inf) { *r = *p; return; }
    bn_t d, s, m, t, x3, y3, z3, yy;
    /* a = -3: M = 3 (X - Z^2)(X + Z^2) */
    FM(&t, &p->z, &p->z);
    fsub(&d, &p->x, &t);
    fadd(&s, &p->x, &t);
    FM(&m, &d, &s);
    fadd(&t, &m, &m); fadd(&m, &t, &m);
    FM(&yy, &p->y, &p->y);
    FM(&s, &p->x, &yy); fadd(&s, &s, &s); fadd(&s, &s, &s);       /* S = 4 X Y^2 */
    FM(&x3, &m, &m); fsub(&x3, &x3, &s); fsub(&x3, &x3, &s);
    FM(&t, &yy, &yy); fadd(&t, &t, &t); fadd(&t, &t, &t); fadd(&t, &t, &t);   /* 8 Y^4 */
    fsub(&d, &s, &x3); FM(&y3, &m, &d); fsub(&y3, &y3, &t);
    FM(&z3, &p->y, &p->z); fadd(&z3, &z3, &z3);
    r->x = x3; r->y = y3; r->z = z3; r->inf = 0;
}

static void ec_add(ecp_t *r, const ecp_t *p, const ecp_t *q)
{
    if (p->inf) { *r = *q; return; }
    if (q->inf) { *r = *p; return; }
    bn_t z1z1, z2z2, u1, u2, s1, s2, h, rr, hh, hhh, v, t, x3, y3, z3;
    FM(&z1z1, &p->z, &p->z); FM(&z2z2, &q->z, &q->z);
    FM(&u1, &p->x, &z2z2); FM(&u2, &q->x, &z1z1);
    FM(&t, &q->z, &z2z2); FM(&s1, &p->y, &t);
    FM(&t, &p->z, &z1z1); FM(&s2, &q->y, &t);
    fsub(&h, &u2, &u1); fsub(&rr, &s2, &s1);
    int n = g_fp.n, hz = 1, rz = 1;
    for (int i = 0; i < n; i++) { if (h.d[i]) hz = 0; if (rr.d[i]) rz = 0; }
    if (hz) { if (rz) { ec_double(r, p); return; } r->inf = 1; return; }
    FM(&hh, &h, &h); FM(&hhh, &h, &hh); FM(&v, &u1, &hh);
    FM(&x3, &rr, &rr); fsub(&x3, &x3, &hhh); fsub(&x3, &x3, &v); fsub(&x3, &x3, &v);
    fsub(&t, &v, &x3); FM(&y3, &rr, &t); FM(&t, &s1, &hhh); fsub(&y3, &y3, &t);
    FM(&t, &p->z, &q->z); FM(&z3, &t, &h);
    r->x = x3; r->y = y3; r->z = z3; r->inf = 0;
}

static void ec_mul(ecp_t *r, const ecp_t *p, const uint8_t *k, int kl)
{
    ecp_t acc;
    acc.inf = 1;
    for (int i = 0; i < kl; i++)
        for (int b = 7; b >= 0; b--) {
            ec_double(&acc, &acc);
            if ((k[i] >> b) & 1) ec_add(&acc, &acc, p);
        }
    *r = acc;
}

/* a^(m-2) mod m: inverse for prime m (in the mont domain of M). */
static void mont_inv(const mont_t *M, bn_t *r, const bn_t *a_mont, const uint8_t *mod_bytes, int ml)
{
    uint8_t e[64];
    memcpy(e, mod_bytes, (size_t)ml);
    /* e = m - 2 */
    int i = ml - 1;
    uint16_t borrow = 2;
    while (borrow && i >= 0) { uint16_t v = e[i]; e[i] = (uint8_t)(v - borrow); borrow = v < borrow ? 1 : 0; i--; }
    bn_t x, one;
    memset(&one, 0, sizeof(one));
    one.n = M->n;
    one.d[0] = 1;
    mont_to(M, &x, &one);
    for (int k = 0; k < ml; k++)
        for (int b = 7; b >= 0; b--) {
            mont_mul(M, &x, &x, &x);
            if ((e[k] >> b) & 1) mont_mul(M, &x, &x, a_mont);
        }
    *r = x;
}

/* pub = 04 || X || Y; sig r, s big-endian (curve size); hashv = message hash. */
int ecdsa_verify(int curve, const uint8_t *pub, size_t pl, const uint8_t *hashv, size_t hl,
                 const uint8_t *r_, size_t rl, const uint8_t *s_, size_t sl)
{
    curve_t c = curve == CURVE_P256 ? (curve_t){ 32, P256_P, P256_B, P256_N, P256_GX, P256_GY }
                                    : (curve_t){ 48, P384_P, P384_B, P384_N, P384_GX, P384_GY };
    int B = c.bytes, limbs = B / 4;
    if (pl != (size_t)(1 + 2 * B) || pub[0] != 4) return -1;
    while (rl > (size_t)B && !*r_) { r_++; rl--; }
    while (sl > (size_t)B && !*s_) { s_++; sl--; }
    if (rl > (size_t)B || sl > (size_t)B) return -1;
    static bn_t n, r, s, e, w, u1, u2, t;
    static mont_t Mn;
    bn_from_bytes(&n, c.n, (size_t)B, limbs);
    bn_from_bytes(&r, r_, rl, limbs);
    bn_from_bytes(&s, s_, sl, limbs);
    int rz = 1, sz = 1;
    for (int i = 0; i < limbs; i++) { if (r.d[i]) rz = 0; if (s.d[i]) sz = 0; }
    if (rz || sz || bn_cmp(&r, &n, limbs) >= 0 || bn_cmp(&s, &n, limbs) >= 0) return -1;
    /* e = leftmost bits of the hash */
    uint8_t eb[64] = { 0 };
    if (hl >= (size_t)B) memcpy(eb, hashv, (size_t)B); else memcpy(eb + B - hl, hashv, hl);
    bn_from_bytes(&e, eb, (size_t)B, limbs);
    if (bn_cmp(&e, &n, limbs) >= 0) bn_sub(&e, &e, &n, limbs);
    mont_init(&Mn, &n);
    bn_t sm, em, rm;
    mont_to(&Mn, &sm, &s);
    mont_inv(&Mn, &w, &sm, c.n, B);                          /* w = s^-1 (mont) */
    mont_to(&Mn, &em, &e);
    mont_to(&Mn, &rm, &r);
    mont_mul(&Mn, &t, &em, &w); mont_from(&Mn, &u1, &t);
    mont_mul(&Mn, &t, &rm, &w); mont_from(&Mn, &u2, &t);
    uint8_t u1b[48], u2b[48];
    bn_to_bytes(&u1, u1b, (size_t)B);
    bn_to_bytes(&u2, u2b, (size_t)B);
    /* field */
    bn_t p;
    bn_from_bytes(&p, c.p, (size_t)B, limbs);
    mont_init(&g_fp, &p);
    static ecp_t G, Q, P1, P2, R;
    bn_t tmp, one;
    memset(&one, 0, sizeof(one)); one.n = limbs; one.d[0] = 1;
    bn_from_bytes(&tmp, c.gx, (size_t)B, limbs); mont_to(&g_fp, &G.x, &tmp);
    bn_from_bytes(&tmp, c.gy, (size_t)B, limbs); mont_to(&g_fp, &G.y, &tmp);
    mont_to(&g_fp, &G.z, &one); G.inf = 0;
    bn_from_bytes(&tmp, pub + 1, (size_t)B, limbs);
    if (bn_cmp(&tmp, &p, limbs) >= 0) return -1;
    mont_to(&g_fp, &Q.x, &tmp);
    bn_from_bytes(&tmp, pub + 1 + B, (size_t)B, limbs);
    if (bn_cmp(&tmp, &p, limbs) >= 0) return -1;
    mont_to(&g_fp, &Q.y, &tmp);
    mont_to(&g_fp, &Q.z, &one); Q.inf = 0;
    /* the public key must be on the curve: y^2 = x^3 - 3x + b */
    {
        bn_t y2, x3, bx, bm;
        FM(&y2, &Q.y, &Q.y);
        FM(&x3, &Q.x, &Q.x); FM(&x3, &x3, &Q.x);
        fsub(&x3, &x3, &Q.x); fsub(&x3, &x3, &Q.x); fsub(&x3, &x3, &Q.x);
        bn_from_bytes(&bx, c.b, (size_t)B, limbs); mont_to(&g_fp, &bm, &bx);
        fadd(&x3, &x3, &bm);
        if (bn_cmp(&y2, &x3, limbs)) return -1;
    }
    ec_mul(&P1, &G, u1b, B);
    ec_mul(&P2, &Q, u2b, B);
    ec_add(&R, &P1, &P2);
    if (R.inf) return -1;
    /* affine x = X / Z^2 */
    bn_t zi, z2, x;
    mont_inv(&g_fp, &zi, &R.z, c.p, B);
    FM(&z2, &zi, &zi);
    FM(&x, &R.x, &z2);
    mont_from(&g_fp, &x, &x);
    if (bn_cmp(&x, &n, limbs) >= 0) bn_sub(&x, &x, &n, limbs);
    return bn_cmp(&x, &r, limbs) == 0 ? 0 : -1;
}

/* ==========================================================================
 * Random numbers: ChaCha20 keystream, key from entropy (re-keyed per call)
 * ========================================================================== */
static uint8_t g_rng_key[32];
static uint32_t g_rng_ctr;
static int g_rng_seeded;

static uint64_t rd_tsc(void)
{
#if defined(__x86_64__)
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
#else
    return 0;
#endif
}

static int rd_rand(uint64_t *v)
{
#if defined(__x86_64__)
    uint32_t a, b, c, d;
    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (!(c & (1u << 30))) return 0;
    unsigned char ok;
    for (int i = 0; i < 10; i++) {
        __asm__ volatile ("rdrand %0; setc %1" : "=r"(*v), "=qm"(ok));
        if (ok) return 1;
    }
#endif
    (void)v;
    return 0;
}

void crypto_add_entropy(const void *data, size_t n)
{
    hash_ctx_t c;
    hash_init(&c, HASH_SHA256);
    hash_update(&c, g_rng_key, 32);
    hash_update(&c, data, n);
    uint64_t t = rd_tsc();
    hash_update(&c, &t, sizeof(t));
    hash_final(&c, g_rng_key);
}

static void rng_seed(void)
{
    /* timing jitter of a memory-touching loop, plus RDRAND when available */
    static uint8_t scratch[4096];
    uint64_t samples[256];
    for (int i = 0; i < 256; i++) {
        uint64_t t0 = rd_tsc();
        for (int k = 0; k < 64 + (int)(t0 & 63); k++) scratch[(t0 * 7 + (uint64_t)k * 131) & 4095] ^= (uint8_t)t0;
        samples[i] = rd_tsc() - t0 + ((uint64_t)scratch[i * 13 % 4096] << 56);
        uint64_t rr;
        if (rd_rand(&rr)) samples[i] ^= rr;
    }
    crypto_add_entropy(samples, sizeof(samples));
    crypto_platform_entropy();
    g_rng_seeded = 1;
}

void crypto_random(void *out, size_t n)
{
    if (!g_rng_seeded) rng_seed();
    uint64_t t = rd_tsc();
    crypto_add_entropy(&t, sizeof(t));
    uint8_t nonce[12] = { 0 }, blk[64];
    uint8_t *o = (uint8_t *)out;
    while (n) {
        chacha20_block(g_rng_key, g_rng_ctr++, nonce, blk);
        size_t k = n < 32 ? n : 32;                         /* use half; the other half re-keys */
        memcpy(o, blk, k);
        memcpy(g_rng_key, blk + 32, 32);
        o += k; n -= k;
    }
}
