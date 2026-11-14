/*
 * =============================================================================
 * JARVIS AI OS — X.509 certificates (kernel/x509.c)
 * Language: C17
 *
 * Parses DER certificates and decides whether a server's certificate chain
 * is trusted:
 *   - every certificate is signed by the next (RSA PKCS#1 v1.5 with
 *     SHA-256/384/512, ECDSA P-256/P-384), the last by a built-in root
 *     (kernel/rootcerts.c, from the Mozilla CA list)
 *   - issuers are certificate authorities (basicConstraints CA)
 *   - every certificate is within its validity dates
 *   - the server certificate names the host (subjectAltName DNS entries,
 *     including "*.example.com" wildcards)
 * =============================================================================
 */

#include "crypto.h"
#include <string.h>
#include <stdio.h>

extern const uint8_t *const g_root_certs[];         /* kernel/rootcerts.c */
extern const uint32_t g_root_cert_len[];
extern const int g_root_cert_count;

/* ---- DER ---------------------------------------------------------------------- */
/* Reads one element at *p (< end). Returns 0 and sets tag/val/len, advancing *p. */
static int der_next(const uint8_t **p, const uint8_t *end, int *tag, const uint8_t **val, size_t *len)
{
    const uint8_t *q = *p;
    if (q + 2 > end) return -1;
    *tag = *q++;
    size_t l = *q++;
    if (l & 0x80) {
        int nb = (int)(l & 0x7F);
        if (nb < 1 || nb > 4 || q + nb > end) return -1;
        l = 0;
        while (nb--) l = l << 8 | *q++;
    }
    if (q + l > end || q + l < q) return -1;
    *val = q;
    *len = l;
    *p = q + l;
    return 0;
}

static int oid_is(const uint8_t *v, size_t l, const uint8_t *oid, size_t ol) { return l == ol && !memcmp(v, oid, ol); }

static const uint8_t OID_RSA_SHA256[] = { 0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0b };
static const uint8_t OID_RSA_SHA384[] = { 0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0c };
static const uint8_t OID_RSA_SHA512[] = { 0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0d };
static const uint8_t OID_EC_SHA256[]  = { 0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x02 };
static const uint8_t OID_EC_SHA384[]  = { 0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x03 };
static const uint8_t OID_EC_SHA512[]  = { 0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x04 };
static const uint8_t OID_RSA_KEY[]    = { 0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01 };
static const uint8_t OID_EC_KEY[]     = { 0x2a,0x86,0x48,0xce,0x3d,0x02,0x01 };
static const uint8_t OID_P256[]       = { 0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07 };
static const uint8_t OID_P384[]       = { 0x2b,0x81,0x04,0x00,0x22 };
static const uint8_t OID_SAN[]        = { 0x55,0x1d,0x11 };
static const uint8_t OID_BASIC[]      = { 0x55,0x1d,0x13 };
static const uint8_t OID_CN[]         = { 0x55,0x04,0x03 };

/* UTCTime / GeneralizedTime -> Unix time */
static int64_t der_time(int tag, const uint8_t *v, size_t l)
{
    int i = 0, y;
    if (tag == 0x17 && l >= 12) { y = (v[0] - '0') * 10 + (v[1] - '0'); y += y < 50 ? 2000 : 1900; i = 2; }
    else if (tag == 0x18 && l >= 14) { y = (v[0] - '0') * 1000 + (v[1] - '0') * 100 + (v[2] - '0') * 10 + (v[3] - '0'); i = 4; }
    else return 0;
    int mo = (v[i] - '0') * 10 + (v[i + 1] - '0'), d = (v[i + 2] - '0') * 10 + (v[i + 3] - '0');
    int h = (v[i + 4] - '0') * 10 + (v[i + 5] - '0'), mi = (v[i + 6] - '0') * 10 + (v[i + 7] - '0');
    int s = (v[i + 8] - '0') * 10 + (v[i + 9] - '0');
    /* days from civil (Howard Hinnant) */
    int yy = y - (mo <= 2);
    int era = (yy >= 0 ? yy : yy - 399) / 400;
    int yoe = yy - era * 400;
    int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + doe - 719468;
    return days * 86400 + h * 3600 + mi * 60 + s;
}

int x509_parse(x509_cert_t *c, const uint8_t *der, size_t len)
{
    memset(c, 0, sizeof(*c));
    const uint8_t *p = der, *end = der + len, *v, *cp;
    size_t l;
    int tag;
    if (der_next(&p, end, &tag, &v, &l) || tag != 0x30) return -1;
    c->raw.p = der; c->raw.len = (size_t)(p - der);
    const uint8_t *cend = v + l;
    cp = v;
    /* tbsCertificate (keep it whole: it is what the issuer signed) */
    const uint8_t *tbs_start = cp;
    if (der_next(&cp, cend, &tag, &v, &l) || tag != 0x30) return -1;
    c->tbs.p = tbs_start; c->tbs.len = (size_t)(cp - tbs_start);
    const uint8_t *t = v, *tend = v + l;
    /* signatureAlgorithm */
    const uint8_t *sv; size_t sl;
    if (der_next(&cp, cend, &tag, &sv, &sl) || tag != 0x30) return -1;
    {
        const uint8_t *q = sv, *ov; size_t ol; int ot;
        if (der_next(&q, sv + sl, &ot, &ov, &ol) || ot != 0x06) return -1;
        c->sig_alg = oid_is(ov, ol, OID_RSA_SHA256, 9) ? X509_SIG_RSA_SHA256 : oid_is(ov, ol, OID_RSA_SHA384, 9) ? X509_SIG_RSA_SHA384 :
                     oid_is(ov, ol, OID_RSA_SHA512, 9) ? X509_SIG_RSA_SHA512 : oid_is(ov, ol, OID_EC_SHA256, 8) ? X509_SIG_ECDSA_SHA256 :
                     oid_is(ov, ol, OID_EC_SHA384, 8) ? X509_SIG_ECDSA_SHA384 : oid_is(ov, ol, OID_EC_SHA512, 8) ? X509_SIG_ECDSA_SHA512 : X509_SIG_UNKNOWN;
    }
    /* signatureValue BIT STRING */
    if (der_next(&cp, cend, &tag, &sv, &sl) || tag != 0x03 || sl < 1) return -1;
    c->sig.p = sv + 1; c->sig.len = sl - 1;

    /* inside tbs: [0] version, serial, signature, issuer, validity, subject, spki, ... [3] extensions */
    if (der_next(&t, tend, &tag, &v, &l)) return -1;
    if (tag == 0xA0 && der_next(&t, tend, &tag, &v, &l)) return -1;      /* version present: next is serial */
    if (der_next(&t, tend, &tag, &v, &l) || tag != 0x30) return -1;     /* signature alg */
    const uint8_t *istart = t;
    if (der_next(&t, tend, &tag, &v, &l) || tag != 0x30) return -1;     /* issuer */
    c->issuer.p = istart; c->issuer.len = (size_t)(t - istart);
    if (der_next(&t, tend, &tag, &v, &l) || tag != 0x30) return -1;     /* validity */
    {
        const uint8_t *q = v, *tv; size_t tl; int tt;
        if (der_next(&q, v + l, &tt, &tv, &tl)) return -1;
        c->not_before = der_time(tt, tv, tl);
        if (der_next(&q, v + l, &tt, &tv, &tl)) return -1;
        c->not_after = der_time(tt, tv, tl);
    }
    const uint8_t *sstart = t;
    if (der_next(&t, tend, &tag, &v, &l) || tag != 0x30) return -1;     /* subject */
    c->subject.p = sstart; c->subject.len = (size_t)(t - sstart);
    {   /* common name */
        const uint8_t *q = v, *qe = v + l, *setv, *sqv, *ov, *vv; size_t setl, sql, ol, vl; int st, qt, ot, vt;
        while (q < qe && !der_next(&q, qe, &st, &setv, &setl)) {
            const uint8_t *r = setv;
            while (r < setv + setl && !der_next(&r, setv + setl, &qt, &sqv, &sql)) {
                const uint8_t *z = sqv;
                if (der_next(&z, sqv + sql, &ot, &ov, &ol) || der_next(&z, sqv + sql, &vt, &vv, &vl)) break;
                if (oid_is(ov, ol, OID_CN, 3)) { c->cn.p = vv; c->cn.len = vl; }
            }
        }
    }
    /* subjectPublicKeyInfo */
    if (der_next(&t, tend, &tag, &v, &l) || tag != 0x30) return -1;
    {
        const uint8_t *q = v, *qe = v + l, *av, *ov, *bv; size_t al, ol, bl; int at, ot, bt;
        if (der_next(&q, qe, &at, &av, &al) || at != 0x30) return -1;
        const uint8_t *a = av;
        if (der_next(&a, av + al, &ot, &ov, &ol) || ot != 0x06) return -1;
        if (der_next(&q, qe, &bt, &bv, &bl) || bt != 0x03 || bl < 1) return -1;
        if (oid_is(ov, ol, OID_RSA_KEY, 9)) {
            c->key_type = X509_KEY_RSA;
            const uint8_t *k = bv + 1, *ke = bv + bl, *seqv, *nv, *ev; size_t seql, nl, el; int t1, t2, t3;
            if (der_next(&k, ke, &t1, &seqv, &seql) || t1 != 0x30) return -1;
            const uint8_t *z = seqv;
            if (der_next(&z, seqv + seql, &t2, &nv, &nl) || der_next(&z, seqv + seql, &t3, &ev, &el)) return -1;
            c->spki_key.p = nv; c->spki_key.len = nl;
            c->spki_e.p = ev; c->spki_e.len = el;
        } else if (oid_is(ov, ol, OID_EC_KEY, 7)) {
            c->key_type = X509_KEY_EC;
            const uint8_t *cv; size_t cl; int ct;
            if (der_next(&a, av + al, &ct, &cv, &cl) || ct != 0x06) return -1;
            c->curve = oid_is(cv, cl, OID_P256, 8) ? CURVE_P256 : oid_is(cv, cl, OID_P384, 5) ? CURVE_P384 : 0;
            c->spki_key.p = bv + 1; c->spki_key.len = bl - 1;
        }
    }
    /* extensions */
    while (t < tend) {
        if (der_next(&t, tend, &tag, &v, &l)) break;
        if (tag != 0xA3) continue;
        const uint8_t *q = v, *qe = v + l, *ev, *xv; size_t el, xl; int et, xt;
        if (der_next(&q, qe, &et, &ev, &el) || et != 0x30) break;
        const uint8_t *e = ev, *ee = ev + el;
        while (e < ee && !der_next(&e, ee, &xt, &xv, &xl)) {
            const uint8_t *z = xv, *ze = xv + xl, *ov, *fv; size_t ol, fl; int ot, ft;
            if (der_next(&z, ze, &ot, &ov, &ol) || ot != 0x06) continue;
            if (der_next(&z, ze, &ft, &fv, &fl)) continue;
            if (ft == 0x01 && der_next(&z, ze, &ft, &fv, &fl)) continue;     /* critical flag */
            if (ft != 0x04) continue;
            if (oid_is(ov, ol, OID_SAN, 3)) {
                const uint8_t *s = fv, *gv; size_t gl; int gt;
                if (!der_next(&s, fv + fl, &gt, &gv, &gl) && gt == 0x30) { c->san.p = gv; c->san.len = gl; }
            } else if (oid_is(ov, ol, OID_BASIC, 3)) {
                const uint8_t *s = fv, *gv, *bv2; size_t gl, bl2; int gt, bt2;
                if (!der_next(&s, fv + fl, &gt, &gv, &gl) && gt == 0x30) {
                    const uint8_t *b2 = gv;
                    if (gl && !der_next(&b2, gv + gl, &bt2, &bv2, &bl2) && bt2 == 0x01 && bl2 == 1 && bv2[0]) c->is_ca = 1;
                }
            }
        }
    }
    return 0;
}

static int sig_hash(int sig_alg)
{
    switch (sig_alg) {
    case X509_SIG_RSA_SHA256: case X509_SIG_ECDSA_SHA256: return HASH_SHA256;
    case X509_SIG_RSA_SHA384: case X509_SIG_ECDSA_SHA384: return HASH_SHA384;
    case X509_SIG_RSA_SHA512: case X509_SIG_ECDSA_SHA512: return HASH_SHA512;
    default: return 0;
    }
}

/* ECDSA signature DER SEQUENCE { r, s } -> verify */
static int ecdsa_der_verify(int curve, const x509_buf_t *pub, const uint8_t *h, size_t hl, const uint8_t *sig, size_t sl)
{
    const uint8_t *p = sig, *v, *rv, *sv;
    size_t l, rl, svl;
    int t;
    if (der_next(&p, sig + sl, &t, &v, &l) || t != 0x30) return -1;
    const uint8_t *q = v;
    if (der_next(&q, v + l, &t, &rv, &rl) || t != 0x02) return -1;
    if (der_next(&q, v + l, &t, &sv, &svl) || t != 0x02) return -1;
    return ecdsa_verify(curve, pub->p, pub->len, h, hl, rv, rl, sv, svl);
}

int x509_verify_signed_by(const x509_cert_t *child, const x509_cert_t *issuer)
{
    int alg = sig_hash(child->sig_alg);
    if (!alg) return -1;
    uint8_t h[64];
    hash_once(alg, child->tbs.p, child->tbs.len, h);
    if (child->sig_alg <= X509_SIG_RSA_SHA512) {
        if (issuer->key_type != X509_KEY_RSA) return -1;
        return rsa_verify_pkcs1(alg, issuer->spki_key.p, issuer->spki_key.len, issuer->spki_e.p, issuer->spki_e.len, h, child->sig.p, child->sig.len);
    }
    if (issuer->key_type != X509_KEY_EC || !issuer->curve) return -1;
    return ecdsa_der_verify(issuer->curve, &issuer->spki_key, h, (size_t)hash_len(alg), child->sig.p, child->sig.len);
}

/* TLS 1.3 CertificateVerify: scheme 0x0403/0x0503 ECDSA, 0x0804-0x0806 RSA-PSS. */
int x509_key_verify(const x509_cert_t *c, int scheme, const uint8_t *msg, size_t ml, const uint8_t *sig, size_t sl)
{
    uint8_t h[64];
    switch (scheme) {
    case 0x0403: case 0x0503: {
        int alg = scheme == 0x0403 ? HASH_SHA256 : HASH_SHA384;
        if (c->key_type != X509_KEY_EC || c->curve != (scheme == 0x0403 ? CURVE_P256 : CURVE_P384)) return -1;
        hash_once(alg, msg, ml, h);
        return ecdsa_der_verify(c->curve, &c->spki_key, h, (size_t)hash_len(alg), sig, sl);
    }
    case 0x0804: case 0x0805: case 0x0806: {
        int alg = scheme == 0x0804 ? HASH_SHA256 : scheme == 0x0805 ? HASH_SHA384 : HASH_SHA512;
        if (c->key_type != X509_KEY_RSA) return -1;
        hash_once(alg, msg, ml, h);
        return rsa_verify_pss(alg, c->spki_key.p, c->spki_key.len, c->spki_e.p, c->spki_e.len, h, sig, sl);
    }
    default:
        return -1;
    }
}

static int label_eq(const char *a, size_t al, const uint8_t *b, size_t bl)
{
    if (al != bl) return 0;
    for (size_t i = 0; i < al; i++) {
        char x = a[i], y = (char)b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
    }
    return 1;
}

static int name_matches(const char *host, const uint8_t *n, size_t nl)
{
    size_t hl = strlen(host);
    if (nl > 2 && n[0] == '*' && n[1] == '.') {             /* *.example.com: exactly one label */
        const char *dot = strchr(host, '.');
        if (!dot || dot == host) return 0;
        return label_eq(dot + 1, hl - (size_t)(dot + 1 - host), n + 2, nl - 2);
    }
    return label_eq(host, hl, n, nl);
}

int x509_host_matches(const x509_cert_t *c, const char *host)
{
    if (c->san.p) {
        const uint8_t *p = c->san.p, *end = c->san.p + c->san.len, *v;
        size_t l;
        int t;
        while (p < end && !der_next(&p, end, &t, &v, &l))
            if (t == 0x82 && name_matches(host, v, l)) return 1;
        return 0;
    }
    return c->cn.p && name_matches(host, c->cn.p, c->cn.len);
}

/* ---- the chain ----------------------------------------------------------------- */
static x509_cert_t *g_roots;
static int g_nroots;

static void roots_load(void)
{
    if (g_roots) return;
    g_roots = (x509_cert_t *)crypto_alloc(sizeof(x509_cert_t) * (size_t)g_root_cert_count);
    if (!g_roots) return;
    for (int i = 0; i < g_root_cert_count; i++)
        if (x509_parse(&g_roots[g_nroots], g_root_certs[i], g_root_cert_len[i]) == 0) g_nroots++;
}

static int same_name(const x509_buf_t *a, const x509_buf_t *b) { return a->len == b->len && !memcmp(a->p, b->p, a->len); }

static void cn_text(const x509_cert_t *c, char *out, size_t n)
{
    size_t l = c->cn.len < n - 1 ? c->cn.len : n - 1;
    if (c->cn.p) memcpy(out, c->cn.p, l); else l = 0;
    out[l] = 0;
}

int x509_verify_chain(const uint8_t *const *der, const size_t *len, int n, const char *host, char *err, size_t en)
{
    static x509_cert_t certs[8];
    if (n < 1) { snprintf(err, en, "the server sent no certificate"); return -1; }
    if (n > 8) n = 8;
    for (int i = 0; i < n; i++)
        if (x509_parse(&certs[i], der[i], len[i]) != 0) { snprintf(err, en, "certificate %d could not be read", i + 1); return -1; }
    char cn[96];
    if (!x509_host_matches(&certs[0], host)) {
        cn_text(&certs[0], cn, sizeof(cn));
        snprintf(err, en, "the certificate is for %s, not %s", cn[0] ? cn : "another name", host);
        return -1;
    }
    roots_load();
    if (!g_nroots) { snprintf(err, en, "no trusted root certificates"); return -1; }
    int64_t now = crypto_now();
    int cur = 0, used[8] = { 0 };
    for (int depth = 0; depth < 8; depth++) {
        x509_cert_t *c = &certs[cur];
        used[cur] = 1;
        if (now && (now < c->not_before || now > c->not_after)) {
            cn_text(c, cn, sizeof(cn));
            snprintf(err, en, "the certificate %s%s", cn, now < c->not_before ? " is not valid yet (check the clock)" : " has expired");
            return -1;
        }
        if (c->sig_alg == X509_SIG_UNKNOWN) { snprintf(err, en, "unsupported signature type in the certificate chain"); return -1; }
        /* signed by a built-in root? */
        for (int r = 0; r < g_nroots; r++)
            if (same_name(&c->issuer, &g_roots[r].subject) && x509_verify_signed_by(c, &g_roots[r]) == 0) return 0;
        /* a certificate the server sent itself may be a trusted root */
        for (int r = 0; r < g_nroots; r++)
            if (c->raw.len == g_roots[r].raw.len && !memcmp(c->raw.p, g_roots[r].raw.p, c->raw.len)) return 0;
        int next = -1;
        for (int j = 0; j < n; j++)
            if (!used[j] && certs[j].is_ca && same_name(&c->issuer, &certs[j].subject) && x509_verify_signed_by(c, &certs[j]) == 0) { next = j; break; }
        if (next < 0) {
            /* name the issuer for the message */
            const uint8_t *p = c->issuer.p, *end = p + c->issuer.len, *v;
            size_t l;
            int t;
            snprintf(cn, sizeof(cn), "unknown");
            if (!der_next(&p, end, &t, &v, &l)) {
                x509_cert_t tmp;
                memset(&tmp, 0, sizeof(tmp));
                const uint8_t *q = v, *qe = v + l, *setv, *sqv, *ov, *vv; size_t setl, sql, ol, vl; int st, qt, ot, vt;
                while (q < qe && !der_next(&q, qe, &st, &setv, &setl)) {
                    const uint8_t *r = setv;
                    while (r < setv + setl && !der_next(&r, setv + setl, &qt, &sqv, &sql)) {
                        const uint8_t *z = sqv;
                        if (der_next(&z, sqv + sql, &ot, &ov, &ol) || der_next(&z, sqv + sql, &vt, &vv, &vl)) break;
                        if (oid_is(ov, ol, OID_CN, 3)) { tmp.cn.p = vv; tmp.cn.len = vl; }
                    }
                }
                if (tmp.cn.p) cn_text(&tmp, cn, sizeof(cn));
            }
            snprintf(err, en, "the certificate is not from a trusted authority (issuer: %s)", cn);
            return -1;
        }
        cur = next;
    }
    snprintf(err, en, "the certificate chain is too long");
    return -1;
}
