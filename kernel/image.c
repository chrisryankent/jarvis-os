/*
 * =============================================================================
 * JARVIS OS — Image codecs (kernel/image.c)
 * Language: C17
 *
 * Decodes PNG (all colour types, 1-16 bit, interlaced or not), baseline JPEG
 * (Huffman, any sampling factors, 1 or 3 components) and BMP (8/24/32-bit)
 * into 0x00RRGGBB pixels, and writes PNG files (zlib with fixed-Huffman
 * LZ77). Used for wallpapers, the Photos app and screenshots.
 * =============================================================================
 */

#include "kernel.h"
#include <string.h>

static void *im_alloc(size_t n)
{
    void *p = n >= LARGE_ALLOC_THRESHOLD ? kmalloc_large(n) : kmalloc(n);
    if (p) memset(p, 0, n);
    return p;
}

static void im_free(void *p, size_t n)
{
    if (!p) return;
    if (n >= LARGE_ALLOC_THRESHOLD) kfree_large(p, n); else kfree(p);
}

void image_free(image_t *img)
{
    if (!img) return;
    im_free(img->px, (size_t)img->w * img->h * 4);
    img->px = NULL;
    img->w = img->h = 0;
}

static int im_new(image_t *img, int w, int h)
{
    if (w <= 0 || h <= 0 || (int64_t)w * h > IMAGE_MAX_PIXELS) return 0;
    img->w = w; img->h = h;
    img->px = (uint32_t *)im_alloc((size_t)w * h * 4);
    return img->px != NULL;
}

static void im_err(char *err, size_t n, const char *msg)
{
    if (err && n) { strncpy(err, msg, n - 1); err[n - 1] = 0; }
}

/* =========================================================================
 * INFLATE (RFC 1951)
 * ========================================================================= */
typedef struct { uint16_t count[16], sym[288]; } huff_t;

typedef struct {
    const uint8_t *in; size_t in_len, pos;
    uint32_t bitbuf; int bitcnt;
    uint8_t *out; size_t out_len, out_pos;
    int err;
} inf_t;

static int inf_bits(inf_t *s, int need)
{
    uint32_t v = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->pos >= s->in_len) { s->err = 1; return 0; }
        v |= (uint32_t)s->in[s->pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = v >> need;
    s->bitcnt -= need;
    return (int)(v & ((1u << need) - 1));
}

static int huff_build(huff_t *h, const uint8_t *lengths, int n)
{
    uint16_t offs[16];
    memset(h->count, 0, sizeof(h->count));
    for (int i = 0; i < n; i++) h->count[lengths[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = offs[i] + h->count[i];
    for (int i = 0; i < n; i++)
        if (lengths[i]) h->sym[offs[lengths[i]]++] = (uint16_t)i;
    return 0;
}

static int huff_decode(inf_t *s, const huff_t *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= inf_bits(s, 1);
        int count = h->count[len];
        if (code - count < first) return h->sym[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        if (s->err) return -1;
    }
    s->err = 1;
    return -1;
}

static const uint16_t LBASE[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const uint8_t  LEXT[29]  = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const uint16_t DBASE[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const uint8_t  DEXT[30]  = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static int inf_codes(inf_t *s, const huff_t *lit, const huff_t *dist)
{
    for (;;) {
        int sym = huff_decode(s, lit);
        if (s->err || sym < 0) return -1;
        if (sym < 256) {
            if (s->out_pos >= s->out_len) return -1;
            s->out[s->out_pos++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int len = LBASE[sym] + inf_bits(s, LEXT[sym]);
            int ds = huff_decode(s, dist);
            if (ds < 0 || ds >= 30) return -1;
            size_t d = DBASE[ds] + (size_t)inf_bits(s, DEXT[ds]);
            if (s->err || d > s->out_pos || s->out_pos + (size_t)len > s->out_len) return -1;
            uint8_t *o = s->out + s->out_pos;
            for (int i = 0; i < len; i++) o[i] = o[(ptrdiff_t)i - (ptrdiff_t)d];
            s->out_pos += (size_t)len;
        }
    }
}

/* Raw DEFLATE (RFC 1951). Reentrant: the dpkg worker (gzip members) and
 * the desktop (PNG) may decode at the same time. */
int inflate_raw(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len, size_t *produced)
{
    huff_t lit, dist;                   /* 1.2 KB: every caller has a 16 KB+ stack */
    inf_t s = { in, in_len, 0, 0, 0, out, out_len, 0, 0 };
    int last;
    do {
        last = inf_bits(&s, 1);
        int type = inf_bits(&s, 2);
        if (s.err) return -1;
        if (type == 0) {
            s.bitbuf = 0; s.bitcnt = 0;
            if (s.pos + 4 > s.in_len) return -1;
            unsigned len = s.in[s.pos] | (s.in[s.pos + 1] << 8);
            s.pos += 4;
            if (s.pos + len > s.in_len || s.out_pos + len > s.out_len) return -1;
            memcpy(s.out + s.out_pos, s.in + s.pos, len);
            s.pos += len; s.out_pos += len;
        } else if (type == 1) {
            uint8_t l[288];
            int i = 0;
            for (; i < 144; i++) l[i] = 8;
            for (; i < 256; i++) l[i] = 9;
            for (; i < 280; i++) l[i] = 7;
            for (; i < 288; i++) l[i] = 8;
            huff_build(&lit, l, 288);
            for (i = 0; i < 30; i++) l[i] = 5;
            huff_build(&dist, l, 30);
            if (inf_codes(&s, &lit, &dist)) return -1;
        } else if (type == 2) {
            static const uint8_t ord[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
            int nlen = inf_bits(&s, 5) + 257, ndist = inf_bits(&s, 5) + 1, ncode = inf_bits(&s, 4) + 4;
            uint8_t l[320];
            memset(l, 0, sizeof(l));
            for (int i = 0; i < ncode; i++) l[ord[i]] = (uint8_t)inf_bits(&s, 3);
            huff_build(&lit, l, 19);
            int idx = 0;
            uint8_t lens[320];
            while (idx < nlen + ndist) {
                int sym = huff_decode(&s, &lit);
                if (s.err || sym < 0) return -1;
                if (sym < 16) { lens[idx++] = (uint8_t)sym; continue; }
                int rep, val = 0;
                if (sym == 16) { if (!idx) return -1; val = lens[idx - 1]; rep = 3 + inf_bits(&s, 2); }
                else if (sym == 17) rep = 3 + inf_bits(&s, 3);
                else rep = 11 + inf_bits(&s, 7);
                if (idx + rep > nlen + ndist) return -1;
                while (rep--) lens[idx++] = (uint8_t)val;
            }
            huff_build(&lit, lens, nlen);
            huff_build(&dist, lens + nlen, ndist);
            if (inf_codes(&s, &lit, &dist)) return -1;
        } else {
            return -1;
        }
    } while (!last);
    if (produced) *produced = s.out_pos;
    return 0;
}

/* =========================================================================
 * PNG
 * ========================================================================= */
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
}

static int png_unfilter(uint8_t *data, int rows, size_t stride, int bpp)
{
    uint8_t *prev = NULL;
    for (int y = 0; y < rows; y++) {
        uint8_t *row = data + (size_t)y * (stride + 1);
        int f = row[0];
        uint8_t *r = row + 1;
        for (size_t i = 0; i < stride; i++) {
            int a = i >= (size_t)bpp ? r[i - bpp] : 0;
            int b = prev ? prev[i] : 0;
            int c = (prev && i >= (size_t)bpp) ? prev[i - bpp] : 0;
            switch (f) {
            case 0: break;
            case 1: r[i] = (uint8_t)(r[i] + a); break;
            case 2: r[i] = (uint8_t)(r[i] + b); break;
            case 3: r[i] = (uint8_t)(r[i] + ((a + b) >> 1)); break;
            case 4: r[i] = (uint8_t)(r[i] + paeth(a, b, c)); break;
            default: return -1;
            }
        }
        prev = r;
    }
    return 0;
}

static int png_sample(const uint8_t *row, int x, int depth, int idx_in_px, int chans)
{
    if (depth == 8) return row[x * chans + idx_in_px];
    if (depth == 16) return row[(x * chans + idx_in_px) * 2];
    int per = 8 / depth, bit = (per - 1 - (x % per)) * depth;
    int v = (row[x / per] >> bit) & ((1 << depth) - 1);
    return v;
}

static int png_decode(const uint8_t *d, size_t n, image_t *img, char *err, size_t errn)
{
    uint32_t w = 0, h = 0;
    int depth = 0, ctype = 0, interlace = 0;
    uint8_t pal[256][4];
    int npal = 0, trns_gray = -1, trns_r = -1, trns_g = -1, trns_b = -1;
    memset(pal, 255, sizeof(pal));
    size_t idat_len = 0;

    for (size_t p = 8; p + 12 <= n;) {           /* pass 1: header + IDAT size */
        uint32_t len = be32(d + p);
        const uint8_t *t = d + p + 4, *c = d + p + 8;
        if (p + 12 + len > n) break;
        if (!memcmp(t, "IHDR", 4) && len >= 13) {
            w = be32(c); h = be32(c + 4); depth = c[8]; ctype = c[9]; interlace = c[12];
        } else if (!memcmp(t, "PLTE", 4)) {
            npal = (int)(len / 3);
            if (npal > 256) npal = 256;
            for (int i = 0; i < npal; i++) { pal[i][0] = c[i * 3]; pal[i][1] = c[i * 3 + 1]; pal[i][2] = c[i * 3 + 2]; }
        } else if (!memcmp(t, "tRNS", 4)) {
            if (ctype == 3) for (uint32_t i = 0; i < len && i < 256; i++) pal[i][3] = c[i];
            else if (ctype == 0 && len >= 2) trns_gray = (c[0] << 8) | c[1];
            else if (ctype == 2 && len >= 6) { trns_r = (c[0] << 8) | c[1]; trns_g = (c[2] << 8) | c[3]; trns_b = (c[4] << 8) | c[5]; }
        } else if (!memcmp(t, "IDAT", 4)) {
            idat_len += len;
        } else if (!memcmp(t, "IEND", 4)) {
            break;
        }
        p += 12 + len;
    }
    int chans = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
    if (!w || !h || !chans || !idat_len || (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16)) {
        im_err(err, errn, "Not a PNG this viewer understands");
        return -1;
    }
    if (!im_new(img, (int)w, (int)h)) { im_err(err, errn, "Picture is too large"); return -1; }

    uint8_t *z = (uint8_t *)im_alloc(idat_len);
    if (!z) { image_free(img); im_err(err, errn, "Out of memory"); return -1; }
    size_t zp = 0;
    for (size_t p = 8; p + 12 <= n;) {           /* pass 2: join IDAT chunks */
        uint32_t len = be32(d + p);
        if (p + 12 + len > n) break;
        if (!memcmp(d + p + 4, "IDAT", 4)) { memcpy(z + zp, d + p + 8, len); zp += len; }
        p += 12 + len;
    }

    int bits_pp = depth * chans, bpp = bits_pp >= 8 ? bits_pp / 8 : 1;
    /* Adam7 pass geometry (one pass when not interlaced). */
    static const int sx[7] = { 0, 4, 0, 2, 0, 1, 0 }, sy[7] = { 0, 0, 4, 0, 2, 0, 1 };
    static const int dx[7] = { 8, 8, 4, 4, 2, 2, 1 }, dy[7] = { 8, 8, 8, 4, 4, 2, 2 };
    int passes = interlace ? 7 : 1;
    size_t raw_len = 0;
    for (int ps = 0; ps < passes; ps++) {
        uint32_t pw = interlace ? (w - sx[ps] + dx[ps] - 1) / dx[ps] : w;
        uint32_t ph = interlace ? (h - sy[ps] + dy[ps] - 1) / dy[ps] : h;
        if (w <= (uint32_t)sx[ps]) pw = 0;
        if (h <= (uint32_t)sy[ps]) ph = 0;
        if (pw && ph) raw_len += ((size_t)pw * bits_pp + 7) / 8 * ph + ph;
    }
    uint8_t *raw = (uint8_t *)im_alloc(raw_len);
    size_t got = 0;
    int ok = raw && zp > 2 && inflate_raw(z + 2, zp - 2, raw, raw_len, &got) == 0 && got >= raw_len;
    im_free(z, idat_len);
    if (!ok) {
        im_free(raw, raw_len); image_free(img);
        im_err(err, errn, "The PNG data is damaged");
        return -1;
    }

    size_t off = 0;
    int maxv = (1 << (depth == 16 ? 8 : depth)) - 1;
    for (int ps = 0; ps < passes; ps++) {
        uint32_t pw = interlace ? (w - sx[ps] + dx[ps] - 1) / dx[ps] : w;
        uint32_t ph = interlace ? (h - sy[ps] + dy[ps] - 1) / dy[ps] : h;
        if (w <= (uint32_t)sx[ps]) pw = 0;
        if (h <= (uint32_t)sy[ps]) ph = 0;
        if (!pw || !ph) continue;
        size_t stride = ((size_t)pw * bits_pp + 7) / 8;
        if (png_unfilter(raw + off, (int)ph, stride, bpp)) { im_free(raw, raw_len); image_free(img); im_err(err, errn, "The PNG data is damaged"); return -1; }
        for (uint32_t y = 0; y < ph; y++) {
            const uint8_t *row = raw + off + y * (stride + 1) + 1;
            uint32_t oy = interlace ? sy[ps] + y * dy[ps] : y;
            for (uint32_t x = 0; x < pw; x++) {
                uint32_t ox = interlace ? sx[ps] + x * dx[ps] : x;
                int r, g, b, a = 255;
                if (ctype == 3) {
                    int i = png_sample(row, (int)x, depth, 0, 1);
                    r = pal[i][0]; g = pal[i][1]; b = pal[i][2]; a = pal[i][3];
                } else if (ctype == 0 || ctype == 4) {
                    int v = png_sample(row, (int)x, depth, 0, chans);
                    if (trns_gray >= 0 && v == (depth == 16 ? trns_gray >> 8 : trns_gray)) a = 0;
                    v = v * 255 / maxv;
                    r = g = b = v;
                    if (ctype == 4) a = png_sample(row, (int)x, depth, 1, chans) * 255 / maxv;
                } else {
                    r = png_sample(row, (int)x, depth, 0, chans);
                    g = png_sample(row, (int)x, depth, 1, chans);
                    b = png_sample(row, (int)x, depth, 2, chans);
                    if (ctype == 6) a = png_sample(row, (int)x, depth, 3, chans);
                    else if (trns_r >= 0 && depth == 8 && r == trns_r && g == trns_g && b == trns_b) a = 0;
                }
                /* Transparent areas are shown over black, the usual viewer
                 * background. */
                r = r * a / 255; g = g * a / 255; b = b * a / 255;
                img->px[(size_t)oy * w + ox] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
        }
        off += (stride + 1) * ph;
    }
    im_free(raw, raw_len);
    return 0;
}

/* =========================================================================
 * Baseline JPEG
 * ========================================================================= */
typedef struct { uint8_t bits[17]; uint8_t vals[256]; uint16_t code_max[18]; int32_t val_ptr[17]; uint16_t min_code[17]; int present; } jhuff_t;

typedef struct {
    int id, h, v, tq, td, ta;
    int bw, bh;                 /* blocks across / down, padded to the MCU */
    uint8_t *data;              /* decoded samples, bw*8 x bh*8           */
    int dcpred;
} jcomp_t;

typedef struct {
    const uint8_t *d; size_t n, pos;
    uint32_t bitbuf; int bitcnt;
    uint16_t q[4][64];
    jhuff_t hdc[4], hac[4];
    jcomp_t c[3]; int nc;
    int w, h, hmax, vmax, restart;
    int marker_hit;
} jpeg_t;

static const uint8_t ZZ[64] = {
    0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63 };

static void jh_build(jhuff_t *h)
{
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        h->val_ptr[l] = k;
        h->min_code[l] = (uint16_t)code;
        code += h->bits[l];
        k += h->bits[l];
        h->code_max[l] = h->bits[l] ? (uint16_t)(code - 1) : 0;
        if (!h->bits[l]) h->val_ptr[l] = -1;
        code <<= 1;
    }
    h->present = 1;
}

static int jbit(jpeg_t *j)
{
    if (!j->bitcnt) {
        if (j->marker_hit || j->pos >= j->n) { j->bitbuf = 0; j->bitcnt = 8; }
        else {
            uint8_t b = j->d[j->pos++];
            if (b == 0xFF) {
                uint8_t nx = j->pos < j->n ? j->d[j->pos] : 0;
                if (nx == 0) j->pos++;
                else { j->marker_hit = 1; b = 0; }
            }
            j->bitbuf = b; j->bitcnt = 8;
        }
    }
    j->bitcnt--;
    return (j->bitbuf >> j->bitcnt) & 1;
}

static int jbits(jpeg_t *j, int n)
{
    int v = 0;
    while (n--) v = (v << 1) | jbit(j);
    return v;
}

static int jdecode(jpeg_t *j, const jhuff_t *h)
{
    int code = 0;
    for (int l = 1; l <= 16; l++) {
        code = (code << 1) | jbit(j);
        if (h->val_ptr[l] >= 0 && code <= h->code_max[l] && code >= h->min_code[l])
            return h->vals[h->val_ptr[l] + code - h->min_code[l]];
    }
    return -1;
}

static int jextend(int v, int n) { return n && v < (1 << (n - 1)) ? v - (1 << n) + 1 : v; }

static uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* Integer IDCT (separable, scaled by 2^12; the classic AAN-free version). */
static void idct8x8(const int *in, uint8_t *out, int stride)
{
    static int c[8][8];
    static int ready;
    if (!ready) {
        /* c = cos((2x+1) u pi / 16) * C(u) * 4096. The angle is k*pi/16;
         * fold k into 0..8 and use a Taylor series there (|a| <= pi/2). */
        for (int x = 0; x < 8; x++)
            for (int u = 0; u < 8; u++) {
                int k = ((2 * x + 1) * u) % 32, sign = 1;
                if (k > 16) k = 32 - k;
                if (k > 8) { k = 16 - k; sign = -1; }
                double a = k * 3.14159265358979 / 16.0, a2 = a * a, cs = 1, term = 1;
                for (int t = 1; t < 10; t++) { term *= -a2 / ((2 * t - 1) * (2 * t)); cs += term; }
                cs *= sign;
                double cu = u ? 1.0 : 0.70710678118654752;
                c[x][u] = (int)(cs * cu * 4096.0 + (cs >= 0 ? 0.5 : -0.5));
            }
        ready = 1;
    }
    int tmp[64];
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int64_t s = 0;
            for (int u = 0; u < 8; u++) s += (int64_t)in[y * 8 + u] * c[x][u];
            tmp[y * 8 + x] = (int)(s >> 10);          /* keep 2 extra bits */
        }
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            int64_t s = 0;
            for (int v = 0; v < 8; v++) s += (int64_t)tmp[v * 8 + x] * c[y][v];
            /* total scale: 4096*4096/1024 = 2^14, plus the 1/4 of the 2-D DCT */
            out[y * stride + x] = clamp8((int)((s + (1 << 15)) >> 16) + 128);
        }
}

static int jpeg_decode(const uint8_t *d, size_t n, image_t *img, char *err, size_t errn)
{
    static jpeg_t j;
    memset(&j, 0, sizeof(j));
    j.d = d; j.n = n; j.pos = 2;
    int have_frame = 0;

    while (j.pos + 4 <= n) {
        if (d[j.pos] != 0xFF) { j.pos++; continue; }
        uint8_t m = d[j.pos + 1];
        j.pos += 2;
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01 || m == 0xFF) { if (m == 0xFF) j.pos--; continue; }
        if (m == 0xD9) break;
        size_t len = ((size_t)d[j.pos] << 8) | d[j.pos + 1];
        const uint8_t *s = d + j.pos + 2;
        if (j.pos + len > n || len < 2) break;
        if (m == 0xC2 || m == 0xC6 || m == 0xCA || m == 0xCE) { im_err(err, errn, "Progressive JPEG is not supported yet"); return -1; }
        if (m == 0xC3 || (m >= 0xC5 && m <= 0xCF && m != 0xC8 && m != 0xCC)) { im_err(err, errn, "This JPEG type is not supported"); return -1; }
        if (m == 0xDB) {
            for (size_t p = 0; p + 65 <= len - 2;) {
                int pq = s[p] >> 4, tq = s[p] & 3;
                p++;
                for (int i = 0; i < 64; i++) { j.q[tq][ZZ[i]] = pq ? (uint16_t)((s[p] << 8) | s[p + 1]) : s[p]; p += pq ? 2 : 1; }
            }
        } else if (m == 0xC4) {
            for (size_t p = 0; p + 17 <= len - 2;) {
                int tc = s[p] >> 4, th = s[p] & 3;
                jhuff_t *h = tc ? &j.hac[th] : &j.hdc[th];
                int total = 0;
                for (int i = 1; i <= 16; i++) { h->bits[i] = s[p + i]; total += h->bits[i]; }
                if (total > 256) return -1;
                memcpy(h->vals, s + p + 17, (size_t)total);
                jh_build(h);
                p += 17 + (size_t)total;
            }
        } else if (m == 0xC0 || m == 0xC1) {
            j.h = (s[1] << 8) | s[2];
            j.w = (s[3] << 8) | s[4];
            j.nc = s[5];
            if (j.nc != 1 && j.nc != 3) { im_err(err, errn, "CMYK JPEG is not supported"); return -1; }
            for (int i = 0; i < j.nc; i++) {
                j.c[i].id = s[6 + i * 3];
                j.c[i].h = s[7 + i * 3] >> 4;
                j.c[i].v = s[7 + i * 3] & 15;
                j.c[i].tq = s[8 + i * 3] & 3;
                if (j.c[i].h > j.hmax) j.hmax = j.c[i].h;
                if (j.c[i].v > j.vmax) j.vmax = j.c[i].v;
            }
            have_frame = 1;
        } else if (m == 0xDD) {
            j.restart = (s[0] << 8) | s[1];
        } else if (m == 0xDA) {
            if (!have_frame) return -1;
            int ns = s[0];
            for (int i = 0; i < ns; i++)
                for (int k = 0; k < j.nc; k++)
                    if (j.c[k].id == s[1 + i * 2]) { j.c[k].td = s[2 + i * 2] >> 4; j.c[k].ta = s[2 + i * 2] & 3; }
            j.pos += len;
            goto scan;
        }
        j.pos += len;
    }
    im_err(err, errn, "Not a JPEG this viewer understands");
    return -1;

scan:;
    int mcux = (j.w + 8 * j.hmax - 1) / (8 * j.hmax), mcuy = (j.h + 8 * j.vmax - 1) / (8 * j.vmax);
    if (!im_new(img, j.w, j.h)) { im_err(err, errn, "Picture is too large"); return -1; }
    size_t sizes[3] = { 0 };
    for (int i = 0; i < j.nc; i++) {
        j.c[i].bw = mcux * j.c[i].h;
        j.c[i].bh = mcuy * j.c[i].v;
        sizes[i] = (size_t)j.c[i].bw * 8 * j.c[i].bh * 8;
        j.c[i].data = (uint8_t *)im_alloc(sizes[i]);
        if (!j.c[i].data) { for (int k = 0; k < i; k++) im_free(j.c[k].data, sizes[k]); image_free(img); im_err(err, errn, "Out of memory"); return -1; }
    }
    int blk[64];
    int mcus = 0, bad = 0;
    for (int my = 0; my < mcuy && !bad; my++) {
        for (int mx = 0; mx < mcux && !bad; mx++) {
            if (j.restart && mcus && mcus % j.restart == 0) {
                /* RSTn: realign to the byte stream and reset predictors */
                j.bitcnt = 0;
                j.marker_hit = 0;
                while (j.pos + 1 < n && !(d[j.pos] == 0xFF && d[j.pos + 1] >= 0xD0 && d[j.pos + 1] <= 0xD7)) j.pos++;
                if (j.pos + 1 < n) j.pos += 2;
                for (int i = 0; i < j.nc; i++) j.c[i].dcpred = 0;
            }
            mcus++;
            for (int ci = 0; ci < j.nc; ci++) {
                jcomp_t *c = &j.c[ci];
                for (int by = 0; by < c->v; by++)
                    for (int bx = 0; bx < c->h; bx++) {
                        memset(blk, 0, sizeof(blk));
                        int t = jdecode(&j, &j.hdc[c->td]);
                        if (t < 0) { bad = 1; break; }
                        int diff = t ? jextend(jbits(&j, t), t) : 0;
                        c->dcpred += diff;
                        blk[0] = c->dcpred * j.q[c->tq][0];
                        for (int k = 1; k < 64;) {
                            int rs = jdecode(&j, &j.hac[c->ta]);
                            if (rs < 0) { bad = 1; break; }
                            int r = rs >> 4, sz = rs & 15;
                            if (!sz) { if (r != 15) break; k += 16; continue; }
                            k += r;
                            if (k > 63) break;
                            blk[ZZ[k]] = jextend(jbits(&j, sz), sz) * j.q[c->tq][ZZ[k]];
                            k++;
                        }
                        int ox = (mx * c->h + bx) * 8, oy = (my * c->v + by) * 8;
                        idct8x8(blk, c->data + (size_t)oy * c->bw * 8 + ox, c->bw * 8);
                    }
            }
        }
    }
    /* Colour conversion with simple upsampling of subsampled chroma. */
    for (int y = 0; y < j.h; y++) {
        for (int x = 0; x < j.w; x++) {
            int v[3];
            for (int ci = 0; ci < j.nc; ci++) {
                jcomp_t *c = &j.c[ci];
                if (c->h == j.hmax && c->v == j.vmax) { v[ci] = c->data[(size_t)y * c->bw * 8 + x]; continue; }
                /* bilinear upsampling of subsampled chroma (sample centres) */
                int fx = ((2 * x + 1) * c->h * 128) / j.hmax - 128, fy = ((2 * y + 1) * c->v * 128) / j.vmax - 128;
                if (fx < 0) fx = 0;
                if (fy < 0) fy = 0;
                int x0 = fx >> 8, y0 = fy >> 8, wx = fx & 255, wy = fy & 255;
                int x1 = x0 + 1 < c->bw * 8 ? x0 + 1 : x0, y1 = y0 + 1 < c->bh * 8 ? y0 + 1 : y0;
                const uint8_t *r0 = c->data + (size_t)y0 * c->bw * 8, *r1 = c->data + (size_t)y1 * c->bw * 8;
                int top = r0[x0] * (256 - wx) + r0[x1] * wx, bot = r1[x0] * (256 - wx) + r1[x1] * wx;
                v[ci] = (top * (256 - wy) + bot * wy + 32768) >> 16;
            }
            uint32_t px;
            if (j.nc == 1) px = ((uint32_t)v[0] << 16) | ((uint32_t)v[0] << 8) | (uint32_t)v[0];
            else {
                int Y = v[0] << 10, cb = v[1] - 128, cr = v[2] - 128;
                int r = (Y + 1436 * cr) >> 10, g = (Y - 352 * cb - 731 * cr) >> 10, b = (Y + 1815 * cb) >> 10;
                px = ((uint32_t)clamp8(r) << 16) | ((uint32_t)clamp8(g) << 8) | clamp8(b);
            }
            img->px[(size_t)y * j.w + x] = px;
        }
    }
    for (int i = 0; i < j.nc; i++) im_free(j.c[i].data, sizes[i]);
    return 0;
}

/* =========================================================================
 * BMP
 * ========================================================================= */
static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static int bmp_decode(const uint8_t *d, size_t n, image_t *img, char *err, size_t errn)
{
    if (n < 54) return -1;
    uint32_t off = le32(d + 10), hs = le32(d + 14);
    int32_t w = (int32_t)le32(d + 18), h = (int32_t)le32(d + 22);
    int bpp = d[28] | (d[29] << 8), comp = (int)le32(d + 30);
    int flip = h > 0;
    if (h < 0) h = -h;
    if ((bpp != 24 && bpp != 32 && bpp != 8) || (comp != 0 && comp != 3) || w <= 0) {
        im_err(err, errn, "Only uncompressed 8, 24 and 32-bit BMP files are supported");
        return -1;
    }
    if (!im_new(img, w, h)) { im_err(err, errn, "Picture is too large"); return -1; }
    size_t stride = ((size_t)w * bpp / 8 + 3) & ~(size_t)3;
    const uint8_t *pal = d + 14 + hs;
    for (int y = 0; y < h; y++) {
        size_t ro = off + (size_t)(flip ? h - 1 - y : y) * stride;
        if (ro + stride > n) break;
        const uint8_t *r = d + ro;
        for (int x = 0; x < w; x++) {
            const uint8_t *p = bpp == 8 ? pal + r[x] * 4 : r + x * (bpp / 8);
            img->px[(size_t)y * w + x] = ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
        }
    }
    return 0;
}

/* ========================================================================= */
int image_decode(const uint8_t *d, size_t n, image_t *img, char *err, size_t errn)
{
    memset(img, 0, sizeof(*img));
    if (n >= 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return png_decode(d, n, img, err, errn);
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8) return jpeg_decode(d, n, img, err, errn);
    if (n >= 2 && d[0] == 'B' && d[1] == 'M') return bmp_decode(d, n, img, err, errn);
    im_err(err, errn, "Not a picture (PNG, JPEG or BMP)");
    return -1;
}

int image_is_picture_name(const char *name)
{
    size_t l = strlen(name);
    static const char *const ext[] = { ".png", ".jpg", ".jpeg", ".bmp" };
    for (int i = 0; i < 4; i++) {
        size_t e = strlen(ext[i]);
        if (l > e) {
            int ok = 1;
            for (size_t k = 0; k < e; k++) {
                char c = name[l - e + k];
                if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
                if (c != ext[i][k]) { ok = 0; break; }
            }
            if (ok) return 1;
        }
    }
    return 0;
}

int image_load(const char *path, image_t *img, char *err, size_t errn)
{
    memset(img, 0, sizeof(*img));
    long size = vfs_size(path);
    if (size <= 0 || vfs_is_dir(path)) { im_err(err, errn, "File not found"); return -1; }
    if (size > 64L * 1024 * 1024) { im_err(err, errn, "File is too large (over 64 MB)"); return -1; }
    uint8_t *buf = (uint8_t *)im_alloc((size_t)size + 1);
    if (!buf) { im_err(err, errn, "Out of memory"); return -1; }
    int got = vfs_read_file(path, (char *)buf, (size_t)size + 1);
    int rc = got > 0 ? image_decode(buf, (size_t)got, img, err, errn) : -1;
    if (got <= 0) im_err(err, errn, "The file could not be read");
    im_free(buf, (size_t)size + 1);
    return rc;
}

/* Bilinear resample of src into dst (dw x dh). */
void image_scale(const image_t *src, uint32_t *dst, int dw, int dh)
{
    if (!src->px || dw <= 0 || dh <= 0) return;
    for (int y = 0; y < dh; y++) {
        int fy = (int)(((int64_t)y * 2 + 1) * src->h * 128 / dh) - 128;     /* 1/256 px */
        if (fy < 0) fy = 0;
        int y0 = fy >> 8, wy = fy & 255;
        int y1 = y0 + 1 < src->h ? y0 + 1 : y0;
        if (y0 >= src->h) { y0 = y1 = src->h - 1; wy = 0; }
        for (int x = 0; x < dw; x++) {
            int fx = (int)(((int64_t)x * 2 + 1) * src->w * 128 / dw) - 128;
            if (fx < 0) fx = 0;
            int x0 = fx >> 8, wx = fx & 255;
            int x1 = x0 + 1 < src->w ? x0 + 1 : x0;
            if (x0 >= src->w) { x0 = x1 = src->w - 1; wx = 0; }
            uint32_t a = src->px[(size_t)y0 * src->w + x0], b = src->px[(size_t)y0 * src->w + x1];
            uint32_t c = src->px[(size_t)y1 * src->w + x0], e = src->px[(size_t)y1 * src->w + x1];
            uint32_t out = 0;
            for (int sh = 0; sh <= 16; sh += 8) {
                int top = (int)((a >> sh) & 255) * (256 - wx) + (int)((b >> sh) & 255) * wx;
                int bot = (int)((c >> sh) & 255) * (256 - wx) + (int)((e >> sh) & 255) * wx;
                out |= (uint32_t)(((top * (256 - wy) + bot * wy) >> 16) & 255) << sh;
            }
            dst[(size_t)y * dw + x] = out;
        }
    }
}

/* =========================================================================
 * PNG writer: zlib stream with fixed-Huffman LZ77 (hash chains of depth 1).
 * ========================================================================= */
typedef struct { uint8_t *buf; size_t cap, len; uint32_t bits; int nbits; int fail; } bw_t;

static void bw_byte(bw_t *b, uint8_t v) { if (b->len < b->cap) b->buf[b->len++] = v; else b->fail = 1; }
static void bw_put(bw_t *b, uint32_t v, int n)              /* LSB first */
{
    b->bits |= v << b->nbits;
    b->nbits += n;
    while (b->nbits >= 8) { bw_byte(b, (uint8_t)b->bits); b->bits >>= 8; b->nbits -= 8; }
}
static void bw_huff(bw_t *b, uint32_t code, int n)          /* MSB first */
{
    uint32_t r = 0;
    for (int i = 0; i < n; i++) r |= ((code >> i) & 1) << (n - 1 - i);
    bw_put(b, r, n);
}
static void put_lit(bw_t *b, int v)
{
    if (v < 144) bw_huff(b, 0x30 + v, 8);
    else if (v < 256) bw_huff(b, 0x190 + v - 144, 9);
    else if (v < 280) bw_huff(b, v - 256, 7);
    else bw_huff(b, 0xC0 + v - 280, 8);
}
static void put_match(bw_t *b, int len, int dist)
{
    int i = 28;
    while (LBASE[i] > len) i--;
    put_lit(b, 257 + i);
    if (LEXT[i]) bw_put(b, (uint32_t)(len - LBASE[i]), LEXT[i]);
    int k = 29;
    while (DBASE[k] > dist) k--;
    bw_huff(b, (uint32_t)k, 5);
    if (DEXT[k]) bw_put(b, (uint32_t)(dist - DBASE[k]), DEXT[k]);
}

static uint32_t crc_table[256];
static uint32_t png_crc(const uint8_t *p, size_t n, uint32_t c)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    c = ~c;
    while (n--) c = crc_table[(c ^ *p++) & 255] ^ (c >> 8);
    return ~c;
}

static void put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

int image_save_png(const char *path, const uint32_t *px, int w, int h)
{
    size_t raw_len = (size_t)(w * 3 + 1) * h;
    uint8_t *raw = (uint8_t *)im_alloc(raw_len);
    size_t cap = raw_len + raw_len / 8 + 1024;
    uint8_t *out = (uint8_t *)im_alloc(cap);
    #define HBITS 15
    size_t hsz = sizeof(int32_t) << HBITS;
    int32_t *head = (int32_t *)im_alloc(hsz);
    if (!raw || !out || !head) { im_free(raw, raw_len); im_free(out, cap); im_free(head, hsz); return -1; }

    /* Rows with the "Sub" filter: flat UI areas become runs of zeros. */
    for (int y = 0; y < h; y++) {
        uint8_t *r = raw + (size_t)y * (w * 3 + 1);
        r[0] = 1;
        uint32_t prev = 0;
        for (int x = 0; x < w; x++) {
            uint32_t p = px[(size_t)y * w + x];
            r[1 + x * 3] = (uint8_t)((p >> 16) - (prev >> 16));
            r[2 + x * 3] = (uint8_t)((p >> 8) - (prev >> 8));
            r[3 + x * 3] = (uint8_t)(p - prev);
            prev = p;
        }
    }

    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    bw_t b = { out, cap, 0, 0, 0, 0 };
    for (int i = 0; i < 8; i++) bw_byte(&b, sig[i]);
    uint8_t ihdr[25] = { 0, 0, 0, 13, 'I', 'H', 'D', 'R' };
    put_be32(ihdr + 8, (uint32_t)w); put_be32(ihdr + 12, (uint32_t)h);
    ihdr[16] = 8; ihdr[17] = 2;
    put_be32(ihdr + 21, png_crc(ihdr + 4, 17, 0));
    for (int i = 0; i < 25; i++) bw_byte(&b, ihdr[i]);
    size_t idat_at = b.len;
    for (int i = 0; i < 8; i++) bw_byte(&b, 0);         /* length + "IDAT", filled later */
    bw_byte(&b, 0x78); bw_byte(&b, 0x01);

    bw_put(&b, 1, 1); bw_put(&b, 1, 2);                 /* final block, fixed Huffman */
    for (size_t i = 0; i < hsz / 4; i++) head[i] = -1;
    size_t i = 0;
    while (i < raw_len && !b.fail) {
        int best = 0; size_t dist = 0;
        if (i + 3 <= raw_len) {
            uint32_t hv = ((uint32_t)raw[i] << 16 | (uint32_t)raw[i + 1] << 8 | raw[i + 2]) * 2654435761u >> (32 - HBITS);
            int32_t cand = head[hv];
            head[hv] = (int32_t)i;
            if (cand >= 0 && i - (size_t)cand <= 32768) {
                size_t m = 0, maxm = raw_len - i < 258 ? raw_len - i : 258;
                while (m < maxm && raw[(size_t)cand + m] == raw[i + m]) m++;
                if (m >= 3) { best = (int)m; dist = i - (size_t)cand; }
            }
            /* Long runs of one byte (a flat area): distance 1. */
            if (best < 32 && i > 0) {
                size_t m = 0, maxm = raw_len - i < 258 ? raw_len - i : 258;
                while (m < maxm && raw[i + m] == raw[i - 1]) m++;
                if ((int)m > best && m >= 3) { best = (int)m; dist = 1; }
            }
        }
        if (best >= 3) { put_match(&b, best, (int)dist); i += (size_t)best; }
        else put_lit(&b, raw[i++]);
    }
    put_lit(&b, 256);
    if (b.nbits) bw_put(&b, 0, 8 - b.nbits);
    uint32_t s1 = 1, s2 = 0;
    for (size_t k = 0; k < raw_len; k++) { s1 = (s1 + raw[k]) % 65521; s2 = (s2 + s1) % 65521; }
    uint32_t adler = (s2 << 16) | s1;
    bw_byte(&b, (uint8_t)(adler >> 24)); bw_byte(&b, (uint8_t)(adler >> 16)); bw_byte(&b, (uint8_t)(adler >> 8)); bw_byte(&b, (uint8_t)adler);
    size_t zlen = b.len - idat_at - 8;
    put_be32(out + idat_at, (uint32_t)zlen);
    memcpy(out + idat_at + 4, "IDAT", 4);
    uint32_t crc = png_crc(out + idat_at + 4, zlen + 4, 0);
    uint8_t tail[16];
    put_be32(tail, crc);
    put_be32(tail + 4, 0); memcpy(tail + 8, "IEND", 4); put_be32(tail + 12, 0xAE426082);
    for (int k = 0; k < 16; k++) bw_byte(&b, tail[k]);

    int rc = b.fail ? -1 : vfs_write_file(path, out, b.len);
    im_free(raw, raw_len); im_free(out, cap); im_free(head, hsz);
    return rc < 0 ? -1 : 0;
}
