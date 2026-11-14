/*
 * =============================================================================
 * JARVIS AI OS — .xz decompressor (kernel/xz.c)
 * Language: C17, freestanding (also builds on the host for testing)
 *
 * Decodes the .xz format (LZMA2 inside xz streams) used by Debian packages
 * (data.tar.xz, control.tar.xz) and by the apt indexes (Packages.xz).
 *
 *   xz_decode(in, len, out_fn, ctx, ...)   the whole input is in memory; the
 *                                          output is streamed to out_fn in
 *                                          pieces, so a 50 MB result never
 *                                          has to fit in memory at once
 *
 * Supports: concatenated streams and stream padding, multiple blocks,
 * LZMA2 (filter 0x21) with any dictionary size, check types none/CRC32/
 * CRC64 (both verified) and SHA-256 (skipped). Other filters (BCJ, delta)
 * are reported as unsupported.
 * =============================================================================
 */

#include "xz.h"
#include <string.h>

/* ---- checks ----------------------------------------------------------------------- */
static uint32_t g_crc32[256];
static uint64_t g_crc64[256];

static void crc_init(void)
{
    if (g_crc32[1]) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        uint64_t d = i;
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
            d = (d >> 1) ^ (0xC96C5795D7870F42ULL & (0ULL - (d & 1)));
        }
        g_crc32[i] = c;
        g_crc64[i] = d;
    }
}

uint32_t xz_crc32(const uint8_t *p, size_t n, uint32_t crc)
{
    crc_init();
    crc = ~crc;
    while (n--) crc = g_crc32[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static uint64_t crc64(const uint8_t *p, size_t n, uint64_t crc)
{
    crc = ~crc;
    while (n--) crc = g_crc64[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ---- LZMA ---------------------------------------------------------------------------- */
#define STATES          12
#define LIT_STATES      7
#define POS_STATES_MAX  16
#define DIST_STATES     4
#define DIST_SLOTS      64
#define DIST_MODEL_START 4
#define DIST_MODEL_END  14
#define FULL_DISTANCES  128
#define ALIGN_BITS      4
#define ALIGN_SIZE      16
#define MATCH_LEN_MIN   2
#define LOW_SYMBOLS     8
#define MID_SYMBOLS     8
#define HIGH_SYMBOLS    256
#define LITERAL_CODER_SIZE 0x300
#define LITERAL_CODERS_MAX 16

typedef struct {
    uint16_t choice, choice2;
    uint16_t low[POS_STATES_MAX][LOW_SYMBOLS];
    uint16_t mid[POS_STATES_MAX][MID_SYMBOLS];
    uint16_t high[HIGH_SYMBOLS];
} len_dec_t;

typedef struct {
    /* range decoder */
    const uint8_t *in;
    size_t in_pos, in_end;
    uint32_t range, code;
    int bad;
    /* dictionary */
    uint8_t *buf;
    size_t size, pos, flushed;
    int full;
    uint64_t total;                 /* bytes since the last dictionary reset */
    /* LZMA state */
    uint32_t state, rep0, rep1, rep2, rep3;
    uint32_t lc, lp, pb;
    int need_props, need_dict_reset;
    uint16_t is_match[STATES][POS_STATES_MAX];
    uint16_t is_rep[STATES], is_rep0[STATES], is_rep1[STATES], is_rep2[STATES];
    uint16_t is_rep0_long[STATES][POS_STATES_MAX];
    uint16_t dist_slot[DIST_STATES][DIST_SLOTS];
    uint16_t dist_special[FULL_DISTANCES - DIST_MODEL_END];
    uint16_t dist_align[ALIGN_SIZE];
    len_dec_t match_len, rep_len;
    uint16_t literal[LITERAL_CODERS_MAX][LITERAL_CODER_SIZE];
    /* output */
    xz_out_fn out;
    void *ctx;
    int aborted;
    int check;                       /* block check type */
    uint32_t crc32;
    uint64_t crc64;
} lz_t;

static void lz_flush(lz_t *z)
{
    if (z->pos > z->flushed && !z->aborted) {
        const uint8_t *p = z->buf + z->flushed;
        size_t n = z->pos - z->flushed;
        if (z->check == 1) z->crc32 = xz_crc32(p, n, z->crc32);
        else if (z->check == 4) z->crc64 = crc64(p, n, z->crc64);
        if (z->out(z->ctx, p, n)) z->aborted = 1;
    }
    z->flushed = z->pos;
}

static inline void dict_put(lz_t *z, uint8_t b)
{
    z->buf[z->pos++] = b;
    z->total++;
    if (z->pos == z->size) { lz_flush(z); z->pos = z->flushed = 0; z->full = 1; }
}

static inline uint8_t dict_get(const lz_t *z, uint32_t dist)
{
    size_t i = z->pos >= (size_t)dist + 1 ? z->pos - dist - 1 : z->pos + z->size - dist - 1;
    return z->buf[i];
}

static inline int dict_has(const lz_t *z, uint32_t dist)
{
    return (uint64_t)dist < (z->full ? z->size : z->pos);
}

static inline void rc_normalize(lz_t *z)
{
    if (z->range < (1u << 24)) {
        z->range <<= 8;
        uint8_t b = 0;
        if (z->in_pos < z->in_end) b = z->in[z->in_pos++]; else z->bad = 1;
        z->code = (z->code << 8) | b;
    }
}

static inline int rc_bit(lz_t *z, uint16_t *prob)
{
    rc_normalize(z);
    uint32_t bound = (z->range >> 11) * *prob;
    if (z->code < bound) {
        z->range = bound;
        *prob = (uint16_t)(*prob + ((2048 - *prob) >> 5));
        return 0;
    }
    z->range -= bound;
    z->code -= bound;
    *prob = (uint16_t)(*prob - (*prob >> 5));
    return 1;
}

static inline uint32_t rc_bittree(lz_t *z, uint16_t *probs, uint32_t limit)
{
    uint32_t symbol = 1;
    do symbol = (symbol << 1) | (uint32_t)rc_bit(z, &probs[symbol]);
    while (symbol < limit);
    return symbol - limit;
}

static inline void rc_bittree_reverse(lz_t *z, uint16_t *probs, uint32_t *dest, uint32_t limit)
{
    uint32_t symbol = 1, i = 0;
    do {
        if (rc_bit(z, &probs[symbol])) { symbol = (symbol << 1) + 1; *dest += 1u << i; }
        else symbol <<= 1;
    } while (++i < limit);
}

static inline void rc_direct(lz_t *z, uint32_t *dest, uint32_t limit)
{
    do {
        rc_normalize(z);
        z->range >>= 1;
        z->code -= z->range;
        uint32_t mask = 0u - (z->code >> 31);
        z->code += z->range & mask;
        *dest = (*dest << 1) + (mask + 1);
    } while (--limit > 0);
}

static void probs_reset(uint16_t *p, size_t n) { for (size_t i = 0; i < n; i++) p[i] = 1024; }

static void lzma_reset(lz_t *z)
{
    z->state = 0;
    z->rep0 = z->rep1 = z->rep2 = z->rep3 = 0;
    probs_reset(&z->is_match[0][0], sizeof(z->is_match) / 2);
    probs_reset(z->is_rep, STATES);
    probs_reset(z->is_rep0, STATES);
    probs_reset(z->is_rep1, STATES);
    probs_reset(z->is_rep2, STATES);
    probs_reset(&z->is_rep0_long[0][0], sizeof(z->is_rep0_long) / 2);
    probs_reset(&z->dist_slot[0][0], sizeof(z->dist_slot) / 2);
    probs_reset(z->dist_special, sizeof(z->dist_special) / 2);
    probs_reset(z->dist_align, ALIGN_SIZE);
    probs_reset((uint16_t *)&z->match_len, sizeof(z->match_len) / 2);
    probs_reset((uint16_t *)&z->rep_len, sizeof(z->rep_len) / 2);
    probs_reset(&z->literal[0][0], sizeof(z->literal) / 2);
}

static uint32_t len_decode(lz_t *z, len_dec_t *l, uint32_t pos_state)
{
    if (!rc_bit(z, &l->choice)) return MATCH_LEN_MIN + rc_bittree(z, l->low[pos_state], LOW_SYMBOLS);
    if (!rc_bit(z, &l->choice2)) return MATCH_LEN_MIN + LOW_SYMBOLS + rc_bittree(z, l->mid[pos_state], MID_SYMBOLS);
    return MATCH_LEN_MIN + LOW_SYMBOLS + MID_SYMBOLS + rc_bittree(z, l->high, HIGH_SYMBOLS);
}

/* Decodes one LZMA chunk producing exactly `unpacked` bytes. */
static int lzma_chunk(lz_t *z, uint32_t unpacked)
{
    uint32_t pb_mask = (1u << z->pb) - 1, lp_mask = (1u << z->lp) - 1;
    while (unpacked && !z->bad && !z->aborted) {
        uint32_t pos_state = (uint32_t)z->total & pb_mask;
        if (!rc_bit(z, &z->is_match[z->state][pos_state])) {
            /* literal */
            uint32_t prev = (z->total || z->full) ? dict_get(z, 0) : 0;
            uint32_t idx = ((((uint32_t)z->total & lp_mask) << z->lc) + (prev >> (8 - z->lc)));
            uint16_t *probs = z->literal[idx];
            uint32_t symbol = 1;
            if (z->state < LIT_STATES) {
                do symbol = (symbol << 1) | (uint32_t)rc_bit(z, &probs[symbol]);
                while (symbol < 0x100);
            } else {
                if (!dict_has(z, z->rep0)) return -1;
                uint32_t match_byte = (uint32_t)dict_get(z, z->rep0) << 1, offset = 0x100;
                do {
                    uint32_t match_bit = match_byte & offset;
                    match_byte <<= 1;
                    uint32_t i = offset + match_bit + symbol;
                    if (rc_bit(z, &probs[i])) { symbol = (symbol << 1) + 1; offset &= match_bit; }
                    else { symbol <<= 1; offset &= ~match_bit; }
                } while (symbol < 0x100);
            }
            dict_put(z, (uint8_t)symbol);
            z->state = z->state < 4 ? 0 : z->state < 10 ? z->state - 3 : z->state - 6;
            unpacked--;
            continue;
        }
        uint32_t len;
        if (rc_bit(z, &z->is_rep[z->state])) {
            if (!rc_bit(z, &z->is_rep0[z->state])) {
                if (!rc_bit(z, &z->is_rep0_long[z->state][pos_state])) {
                    /* short rep: one byte from rep0 */
                    if (!dict_has(z, z->rep0)) return -1;
                    z->state = z->state < LIT_STATES ? 9 : 11;
                    dict_put(z, dict_get(z, z->rep0));
                    unpacked--;
                    continue;
                }
            } else {
                uint32_t tmp;
                if (!rc_bit(z, &z->is_rep1[z->state])) tmp = z->rep1;
                else {
                    if (!rc_bit(z, &z->is_rep2[z->state])) tmp = z->rep2;
                    else { tmp = z->rep3; z->rep3 = z->rep2; }
                    z->rep2 = z->rep1;
                }
                z->rep1 = z->rep0;
                z->rep0 = tmp;
            }
            z->state = z->state < LIT_STATES ? 8 : 11;
            len = len_decode(z, &z->rep_len, pos_state);
        } else {
            z->rep3 = z->rep2;
            z->rep2 = z->rep1;
            z->rep1 = z->rep0;
            len = len_decode(z, &z->match_len, pos_state);
            z->state = z->state < LIT_STATES ? 7 : 10;
            uint32_t ds = len - MATCH_LEN_MIN < DIST_STATES ? len - MATCH_LEN_MIN : DIST_STATES - 1;
            uint32_t slot = rc_bittree(z, z->dist_slot[ds], DIST_SLOTS);
            if (slot < DIST_MODEL_START) z->rep0 = slot;
            else {
                uint32_t limit = (slot >> 1) - 1;
                z->rep0 = 2 + (slot & 1);
                if (slot < DIST_MODEL_END) {
                    z->rep0 <<= limit;
                    /* dist_special is indexed from rep0 - slot - 1 (may start at -1) */
                    uint32_t symbol = 1, i = 0;
                    int base = (int)z->rep0 - (int)slot - 1;
                    do {
                        if (rc_bit(z, &z->dist_special[base + (int)symbol])) { symbol = (symbol << 1) + 1; z->rep0 += 1u << i; }
                        else symbol <<= 1;
                    } while (++i < limit);
                } else {
                    rc_direct(z, &z->rep0, limit - ALIGN_BITS);
                    z->rep0 <<= ALIGN_BITS;
                    rc_bittree_reverse(z, z->dist_align, &z->rep0, ALIGN_BITS);
                }
            }
            if (z->rep0 == 0xFFFFFFFFu) return -1;          /* end marker: not allowed in LZMA2 */
        }
        if (!dict_has(z, z->rep0) || len > unpacked) return -1;
        unpacked -= len;
        while (len--) dict_put(z, dict_get(z, z->rep0));
    }
    return z->bad || z->aborted ? -1 : 0;
}

/* ---- LZMA2 ------------------------------------------------------------------------------- */
static int lzma2_decode(lz_t *z, const uint8_t *in, size_t len, size_t *used, char **why)
{
    size_t p = 0;
    int first = 1;
    for (;;) {
        if (p >= len) { *why = "truncated LZMA2 data"; return -1; }
        uint8_t ctl = in[p++];
        if (ctl == 0x00) break;
        if (ctl == 0x01 || ctl == 0x02) {                   /* stored chunk */
            if (p + 2 > len) { *why = "truncated LZMA2 data"; return -1; }
            uint32_t n = ((uint32_t)in[p] << 8 | in[p + 1]) + 1;
            p += 2;
            if (ctl == 0x01) { z->pos = z->flushed = 0; z->full = 0; z->total = 0; z->need_dict_reset = 0; }
            else if (z->need_dict_reset) { *why = "LZMA2: missing dictionary reset"; return -1; }
            if (p + n > len) { *why = "truncated LZMA2 data"; return -1; }
            for (uint32_t i = 0; i < n; i++) dict_put(z, in[p + i]);
            p += n;
            lz_flush(z);
            first = 0;
            continue;
        }
        if (ctl < 0x80) { *why = "LZMA2: bad chunk"; return -1; }
        if (p + 4 > len) { *why = "truncated LZMA2 data"; return -1; }
        uint32_t unpacked = ((uint32_t)(ctl & 0x1F) << 16) + ((uint32_t)in[p] << 8) + in[p + 1] + 1;
        uint32_t packed = ((uint32_t)in[p + 2] << 8 | in[p + 3]) + 1;
        p += 4;
        int mode = (ctl >> 5) & 3;
        if (mode == 3) { z->pos = z->flushed = 0; z->full = 0; z->total = 0; z->need_dict_reset = 0; }
        else if (z->need_dict_reset) { *why = "LZMA2: missing dictionary reset"; return -1; }
        if (mode >= 2) {
            if (p >= len) { *why = "truncated LZMA2 data"; return -1; }
            uint32_t pr = in[p++];
            if (pr >= 9 * 5 * 5) { *why = "LZMA2: bad properties"; return -1; }
            z->pb = pr / 45; pr %= 45;
            z->lp = pr / 9;
            z->lc = pr % 9;
            if (z->lc + z->lp > 4) { *why = "LZMA2: bad properties"; return -1; }
            z->need_props = 0;
        } else if (z->need_props) { *why = "LZMA2: missing properties"; return -1; }
        if (mode >= 1) lzma_reset(z);
        else if (first) { *why = "LZMA2: missing state reset"; return -1; }
        if (p + packed > len) { *why = "truncated LZMA2 data"; return -1; }
        /* range coder for this chunk */
        z->in = in + p;
        z->in_pos = 0;
        z->in_end = packed;
        z->bad = 0;
        if (packed < 5 || z->in[0] != 0) { *why = "LZMA2: bad range coder start"; return -1; }
        z->range = 0xFFFFFFFFu;
        z->code = 0;
        for (int i = 1; i < 5; i++) z->code = (z->code << 8) | z->in[i];
        z->in_pos = 5;
        if (lzma_chunk(z, unpacked) != 0) { *why = z->aborted ? "stopped" : "corrupt LZMA2 data"; return -1; }
        lz_flush(z);
        p += packed;
        first = 0;
    }
    *used = p;
    return 0;
}

/* ---- the .xz container --------------------------------------------------------------- */
static int vli(const uint8_t *in, size_t len, size_t *p, uint64_t *v)
{
    *v = 0;
    for (int i = 0; i < 9; i++) {
        if (*p >= len) return -1;
        uint8_t b = in[(*p)++];
        *v |= (uint64_t)(b & 0x7F) << (7 * i);
        if (!(b & 0x80)) return 0;
    }
    return -1;
}

static void fail(char *err, size_t en, const char *why)
{
    if (err && en) { size_t l = strlen(why); if (l >= en) l = en - 1; memcpy(err, why, l); err[l] = 0; }
}

int xz_decode(const uint8_t *in, size_t len, xz_out_fn out, void *ctx,
              void *(*alloc)(size_t), void (*release)(void *, size_t), char *err, size_t en)
{
    crc_init();
    lz_t *z = (lz_t *)alloc(sizeof(lz_t));
    if (!z) { fail(err, en, "out of memory"); return -1; }
    memset(z, 0, sizeof(*z));
    z->out = out;
    z->ctx = ctx;
    size_t dict_alloc = 0;
    int rc = -1;
    char *why = "not an .xz file";
    size_t p = 0;
    int streams = 0;

    while (p < len) {
        /* stream padding between streams: zero bytes in groups of four */
        if (streams && in[p] == 0) {
            while (p < len && in[p] == 0) p++;
            continue;
        }
        if (p + 12 > len || memcmp(in + p, "\xFD" "7zXZ\0", 6)) { why = streams ? "garbage after the data" : "not an .xz file"; goto done; }
        if (xz_crc32(in + p + 6, 2, 0) != (uint32_t)(in[p + 8] | in[p + 9] << 8 | in[p + 10] << 16 | (uint32_t)in[p + 11] << 24)) { why = "damaged stream header"; goto done; }
        int check = in[p + 7] & 0x0F;
        size_t check_size = check == 0 ? 0 : check <= 3 ? 4 : check <= 6 ? 8 : check <= 9 ? 16 : check <= 12 ? 32 : 64;
        p += 12;
        streams++;
        /* blocks */
        for (;;) {
            if (p >= len) { why = "truncated .xz file"; goto done; }
            if (in[p] == 0) break;                          /* the index follows */
            size_t hsize = ((size_t)in[p] + 1) * 4;
            if (p + hsize > len) { why = "truncated block header"; goto done; }
            uint32_t want = in[p + hsize - 4] | in[p + hsize - 3] << 8 | in[p + hsize - 2] << 16 | (uint32_t)in[p + hsize - 1] << 24;
            if (xz_crc32(in + p, hsize - 4, 0) != want) { why = "damaged block header"; goto done; }
            uint8_t flags = in[p + 1];
            size_t q = p + 2;
            uint64_t v;
            if ((flags & 0x40) && vli(in, p + hsize - 4, &q, &v)) { why = "damaged block header"; goto done; }
            if ((flags & 0x80) && vli(in, p + hsize - 4, &q, &v)) { why = "damaged block header"; goto done; }
            int nfilters = (flags & 3) + 1;
            uint64_t dict_size = 0;
            for (int f = 0; f < nfilters; f++) {
                uint64_t id, psize;
                if (vli(in, p + hsize - 4, &q, &id) || vli(in, p + hsize - 4, &q, &psize) || q + psize > p + hsize - 4) { why = "damaged block header"; goto done; }
                if (id != 0x21 || f != nfilters - 1) { why = id == 0x04 ? "uses the x86 BCJ filter (not supported)" : "uses an unsupported filter"; goto done; }
                if (psize != 1 || in[q] > 40) { why = "bad LZMA2 dictionary size"; goto done; }
                uint32_t bits = in[q];
                dict_size = bits == 40 ? 0xFFFFFFFFull : (uint64_t)(2 | (bits & 1)) << (bits / 2 + 11);
                q += psize;
            }
            /* dictionary: no larger than the rest of the input could need */
            size_t want_dict = dict_size > (64ull << 20) ? (64u << 20) : (size_t)dict_size;
            if (want_dict < 4096) want_dict = 4096;
            if (want_dict > dict_alloc) {
                if (z->buf) release(z->buf, dict_alloc);
                z->buf = (uint8_t *)alloc(want_dict);
                if (!z->buf) { dict_alloc = 0; why = "out of memory for the dictionary"; goto done; }
                dict_alloc = want_dict;
            }
            z->size = want_dict;
            z->pos = z->flushed = 0;
            z->full = 0;
            z->total = 0;
            z->need_props = 1;
            z->need_dict_reset = 1;
            z->check = check;
            z->crc32 = 0;
            z->crc64 = 0;
            p += hsize;
            size_t used = 0;
            if (lzma2_decode(z, in + p, len - p, &used, &why)) goto done;
            p += used;
            while (p & 3) { if (p >= len || in[p]) { why = "damaged block padding"; goto done; } p++; }
            /* p is relative to the start of the file; block padding is
             * relative to the block start, which is 4-aligned too */
            if (p + check_size > len) { why = "truncated .xz file"; goto done; }
            if (check == 1) {
                uint32_t c = in[p] | in[p + 1] << 8 | in[p + 2] << 16 | (uint32_t)in[p + 3] << 24;
                if (c != z->crc32) { why = "CRC32 mismatch: the data is damaged"; goto done; }
            } else if (check == 4) {
                uint64_t c = 0;
                for (int i = 7; i >= 0; i--) c = (c << 8) | in[p + i];
                if (c != z->crc64) { why = "CRC64 mismatch: the data is damaged"; goto done; }
            }
            p += check_size;
        }
        /* index */
        size_t ix = p;
        p++;
        uint64_t records, v;
        if (vli(in, len, &p, &records)) { why = "damaged index"; goto done; }
        for (uint64_t r = 0; r < records; r++)
            if (vli(in, len, &p, &v) || vli(in, len, &p, &v)) { why = "damaged index"; goto done; }
        while ((p - ix) & 3) { if (p >= len || in[p]) { why = "damaged index"; goto done; } p++; }
        if (p + 4 > len || xz_crc32(in + ix, p - ix, 0) != (uint32_t)(in[p] | in[p + 1] << 8 | in[p + 2] << 16 | (uint32_t)in[p + 3] << 24)) { why = "damaged index"; goto done; }
        p += 4;
        /* footer */
        if (p + 12 > len || memcmp(in + p + 10, "YZ", 2)) { why = "damaged stream footer"; goto done; }
        p += 12;
    }
    rc = streams ? 0 : -1;
    if (!streams) why = "empty input";
done:
    if (rc) fail(err, en, why);
    if (z->buf) release(z->buf, dict_alloc);
    release(z, sizeof(lz_t));
    return rc;
}
