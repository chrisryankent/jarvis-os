/*
 * =============================================================================
 * JARVIS AI OS — Accounts, sign-in and settings store (kernel/accounts.c)
 * Language: C17
 *
 * One owner account with up to three ways to sign in:
 *   password   any text, stored as salted SHA-256 (never in plain text)
 *   PIN        4-12 digits, stored as salted SHA-256
 *   voice      a spoken passphrase. Speech recognition mishears, so a spoken
 *              attempt is accepted when it is close enough to the phrase
 *              (spelling, spacing, punctuation and number words are
 *              ignored; most letters must match). Because the comparison
 *              is approximate the phrase itself has to be stored; it is
 *              kept in /jarvis/system, which programs cannot read.
 *
 * Wrong typed attempts slow down further tries (30 s after 5, doubling), but
 * never lock the account permanently. Wrong spoken attempts do not count
 * towards that, so mishearing can never lock you out; after 10 misses in a
 * row voice sign-in pauses until a typed sign-in succeeds.
 *
 * Settings (key=value) live in /jarvis/system/settings.conf.
 * =============================================================================
 */

#include "kernel.h"
#include <string.h>
#include <stdio.h>

#define ACCOUNT_FILE  "/jarvis/system/account.conf"
#define SETTINGS_FILE "/jarvis/system/settings.conf"

/* ==========================================================================
 * SHA-256 (FIPS 180-4)
 * ========================================================================== */
static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t h[8], const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) | ((uint32_t)p[i * 4 + 2] << 8) | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void sha256(const void *data, size_t len, uint8_t out[32])
{
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    const uint8_t *p = (const uint8_t *)data;
    uint8_t block[64];
    size_t i = 0;
    for (; i + 64 <= len; i += 64) sha256_block(h, p + i);
    size_t rem = len - i;
    memset(block, 0, 64);
    memcpy(block, p + i, rem);
    block[rem] = 0x80;
    if (rem >= 56) { sha256_block(h, block); memset(block, 0, 64); }
    uint64_t bits = (uint64_t)len * 8;
    for (int k = 0; k < 8; k++) block[63 - k] = (uint8_t)(bits >> (k * 8));
    sha256_block(h, block);
    for (int k = 0; k < 8; k++) {
        out[k * 4] = (uint8_t)(h[k] >> 24); out[k * 4 + 1] = (uint8_t)(h[k] >> 16);
        out[k * 4 + 2] = (uint8_t)(h[k] >> 8); out[k * 4 + 3] = (uint8_t)h[k];
    }
}

static void to_hex(const uint8_t *b, size_t n, char *out)
{
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[i * 2] = hx[b[i] >> 4]; out[i * 2 + 1] = hx[b[i] & 15]; }
    out[n * 2] = 0;
}

/* Salted, stretched hash: SHA-256 applied 10 000 times, to make guessing a
 * stolen hash slow. */
static void secret_hash(const char *salt, const char *secret, char out_hex[65])
{
    uint8_t d[32];
    char buf[256];
    snprintf(buf, sizeof(buf), "%s:%s", salt, secret);
    sha256(buf, strlen(buf), d);
    for (int i = 0; i < 10000; i++) sha256(d, 32, d);
    to_hex(d, 32, out_hex);
}

static void new_salt(char out_hex[33])
{
    uint8_t seed[32];
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t t = ((uint64_t)hi << 32) | lo;
    char mix[96];
    snprintf(mix, sizeof(mix), "%llu:%llu:%p", (unsigned long long)t,
             (unsigned long long)g_kstate.uptime_ticks, (void *)&seed);
    sha256(mix, strlen(mix), seed);
    to_hex(seed, 16, out_hex);
}

/* ==========================================================================
 * Key/value files
 * ========================================================================== */
static int kv_get(const char *text, const char *key, char *out, size_t n)
{
    size_t kl = strlen(key);
    out[0] = 0;
    for (const char *p = text; *p;) {
        const char *eol = strchr(p, '\n');
        if (!eol) eol = p + strlen(p);
        if ((size_t)(eol - p) > kl && !strncmp(p, key, kl) && p[kl] == '=') {
            size_t vl = (size_t)(eol - p) - kl - 1;
            if (vl >= n) vl = n - 1;
            memcpy(out, p + kl + 1, vl);
            out[vl] = 0;
            return 1;
        }
        p = *eol ? eol + 1 : eol;
    }
    return 0;
}

static struct {
    int  exists;
    char name[32];
    char pass_salt[33], pass_hash[65];
    char pin_salt[33], pin_hash[65];
    char voice[96];                         /* normalised spoken passphrase */
    int  typed_failures;
    uint64_t retry_after;                   /* tick before which typed tries wait */
    int  voice_misses;
} g_acct;

static char g_settings[2048];

static int account_save(void)
{
    char buf[1024];
    vfs_mkdir("/jarvis/system");
    int n = snprintf(buf, sizeof(buf), "name=%s\npass_salt=%s\npass_hash=%s\npin_salt=%s\npin_hash=%s\nvoice=%s\n",
                     g_acct.name, g_acct.pass_salt, g_acct.pass_hash, g_acct.pin_salt, g_acct.pin_hash, g_acct.voice);
    return vfs_write_file(ACCOUNT_FILE, buf, (size_t)n);
}

static void sync_kstate(void)
{
    snprintf(g_kstate.username, sizeof(g_kstate.username), "%s", g_acct.name[0] ? g_acct.name : "User");
    g_kstate.security_enabled = account_has_sign_in();
    g_kstate.security_pin[0] = 0;           /* the plain PIN is never kept */
}

void accounts_init(void)
{
    char buf[1024];
    memset(&g_acct, 0, sizeof(g_acct));
    int n = vfs_read_file(ACCOUNT_FILE, buf, sizeof(buf));
    if (n > 0) {
        g_acct.exists = 1;
        kv_get(buf, "name", g_acct.name, sizeof(g_acct.name));
        kv_get(buf, "pass_salt", g_acct.pass_salt, sizeof(g_acct.pass_salt));
        kv_get(buf, "pass_hash", g_acct.pass_hash, sizeof(g_acct.pass_hash));
        kv_get(buf, "pin_salt", g_acct.pin_salt, sizeof(g_acct.pin_salt));
        kv_get(buf, "pin_hash", g_acct.pin_hash, sizeof(g_acct.pin_hash));
        kv_get(buf, "voice", g_acct.voice, sizeof(g_acct.voice));
    }
    n = vfs_read_file(SETTINGS_FILE, g_settings, sizeof(g_settings));
    if (n < 0) g_settings[0] = 0;
    sync_kstate();
    terminal_printf("[AUTH] %s\n", !g_acct.exists ? "No account yet: first-run setup" :
                    account_has_sign_in() ? "Account found: sign-in required" : "Account found: no sign-in set");
}

/* ---- Settings ------------------------------------------------------------ */
int setting_get(const char *key, char *out, size_t n)
{
    return kv_get(g_settings, key, out, n);
}

int setting_get_int(const char *key, int dflt)
{
    char v[16];
    if (!kv_get(g_settings, key, v, sizeof(v)) || !v[0]) return dflt;
    int r = 0, neg = v[0] == '-';
    for (const char *p = v + neg; *p >= '0' && *p <= '9'; p++) r = r * 10 + (*p - '0');
    return neg ? -r : r;
}

void setting_set(const char *key, const char *value)
{
    static char out[2048];
    size_t kl = strlen(key), o = 0;
    for (const char *p = g_settings; *p;) {                 /* copy every other key */
        const char *eol = strchr(p, '\n');
        if (!eol) eol = p + strlen(p);
        int same = (size_t)(eol - p) > kl && !strncmp(p, key, kl) && p[kl] == '=';
        if (!same && eol > p && o + (size_t)(eol - p) + 2 < sizeof(out)) {
            memcpy(out + o, p, (size_t)(eol - p));
            o += (size_t)(eol - p);
            out[o++] = '\n';
        }
        p = *eol ? eol + 1 : eol;
    }
    int w = snprintf(out + o, sizeof(out) - o, "%s=%s\n", key, value);
    if (w > 0) o += (size_t)w < sizeof(out) - o ? (size_t)w : sizeof(out) - o - 1;
    out[o] = 0;
    memcpy(g_settings, out, o + 1);
    vfs_mkdir("/jarvis/system");
    vfs_write_file(SETTINGS_FILE, g_settings, o);
}

void setting_set_int(const char *key, int value)
{
    char v[16];
    snprintf(v, sizeof(v), "%d", value);
    setting_set(key, v);
}

/* ---- Account queries ----------------------------------------------------- */
int account_exists(void)       { return g_acct.exists; }
const char *account_name(void) { return g_acct.name[0] ? g_acct.name : "User"; }
int account_has_password(void) { return g_acct.pass_hash[0] != 0; }
int account_has_pin(void)      { return g_acct.pin_hash[0] != 0; }
int account_has_voice(void)    { return g_acct.voice[0] != 0; }
int account_has_sign_in(void)  { return account_has_password() || account_has_pin() || account_has_voice(); }

/* ---- Normalising and comparing spoken phrases ---------------------------- */
static const char *const g_numwords[] = { "zero","one","two","three","four","five","six","seven","eight","nine" };

/* lower-case letters only; digits become number words; everything else
 * (spaces, punctuation) is dropped, so "Blue-Falcon 7." == "blue falcon seven" */
static void normalise_spoken(const char *in, char *out, size_t n)
{
    size_t k = 0;
    for (const char *p = in; *p && k < n - 1; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c >= 'a' && c <= 'z') out[k++] = c;
        else if (c >= '0' && c <= '9') {
            for (const char *w = g_numwords[c - '0']; *w && k < n - 1; w++) out[k++] = *w;
        }
    }
    out[k] = 0;
}

static int edit_distance(const char *a, const char *b)
{
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la > 95 || lb > 95) return 1000;
    int prev[96], cur[96];
    for (int j = 0; j <= lb; j++) prev[j] = j;
    for (int i = 1; i <= la; i++) {
        cur[0] = i;
        for (int j = 1; j <= lb; j++) {
            int sub = prev[j - 1] + (a[i - 1] != b[j - 1]);
            int del = prev[j] + 1, ins = cur[j - 1] + 1;
            cur[j] = sub < del ? (sub < ins ? sub : ins) : (del < ins ? del : ins);
        }
        memcpy(prev, cur, sizeof(int) * (size_t)(lb + 1));
    }
    return prev[lb];
}

/* ---- Setup and changes --------------------------------------------------- */
static int set_secret(char *salt, char *hash, const char *secret)
{
    if (!secret || !secret[0]) { salt[0] = 0; hash[0] = 0; return 0; }
    new_salt(salt);
    secret_hash(salt, secret, hash);
    return 0;
}

static int valid_pin(const char *pin)
{
    size_t n = strlen(pin);
    if (n < 4 || n > 12) return 0;
    for (const char *p = pin; *p; p++) if (*p < '0' || *p > '9') return 0;
    return 1;
}

/* Create the account at first run. Any of password / pin / voice may be
 * empty. Returns 0, or -1 with a reason in err. */
int account_create(const char *name, const char *password, const char *pin, const char *voice,
                   char *err, size_t errn)
{
    char norm[96];
    if (!name || !name[0]) { snprintf(err, errn, "Type your name."); return -1; }
    if (pin && pin[0] && !valid_pin(pin)) { snprintf(err, errn, "A PIN is 4 to 12 digits."); return -1; }
    if (voice && voice[0]) {
        normalise_spoken(voice, norm, sizeof(norm));
        if (strlen(norm) < 10) { snprintf(err, errn, "Use a spoken passphrase of at least three words."); return -1; }
    }
    memset(&g_acct, 0, sizeof(g_acct));
    snprintf(g_acct.name, sizeof(g_acct.name), "%s", name);
    set_secret(g_acct.pass_salt, g_acct.pass_hash, password);
    set_secret(g_acct.pin_salt, g_acct.pin_hash, pin);
    if (voice && voice[0]) normalise_spoken(voice, g_acct.voice, sizeof(g_acct.voice));
    g_acct.exists = 1;
    sync_kstate();
    if (account_save() != 0) { snprintf(err, errn, "The account could not be saved."); return -1; }
    terminal_printf("[AUTH] Account created for %s\n", g_acct.name);
    return 0;
}

int account_set_name(const char *name)
{
    if (!name || !name[0]) return -1;
    snprintf(g_acct.name, sizeof(g_acct.name), "%s", name);
    sync_kstate();
    return account_save();
}

int account_set_password(const char *password)
{
    set_secret(g_acct.pass_salt, g_acct.pass_hash, password);
    sync_kstate();
    return account_save();
}

int account_set_pin(const char *pin, char *err, size_t errn)
{
    if (pin && pin[0] && !valid_pin(pin)) { snprintf(err, errn, "A PIN is 4 to 12 digits."); return -1; }
    set_secret(g_acct.pin_salt, g_acct.pin_hash, pin);
    sync_kstate();
    return account_save();
}

int account_set_voice(const char *phrase, char *err, size_t errn)
{
    char norm[96] = "";
    if (phrase && phrase[0]) {
        normalise_spoken(phrase, norm, sizeof(norm));
        if (strlen(norm) < 10) { snprintf(err, errn, "Use at least three words."); return -1; }
    }
    snprintf(g_acct.voice, sizeof(g_acct.voice), "%s", norm);
    g_acct.voice_misses = 0;
    sync_kstate();
    return account_save();
}

/* ---- Checking ------------------------------------------------------------ */
/* Seconds before another typed attempt is allowed (0 = now). */
int auth_wait_seconds(void)
{
    if (g_kstate.uptime_ticks >= g_acct.retry_after) return 0;
    return (int)((g_acct.retry_after - g_kstate.uptime_ticks + 99) / 100);
}

static void typed_result(int ok)
{
    if (ok) {
        g_acct.typed_failures = 0;
        g_acct.voice_misses = 0;                 /* a typed sign-in re-enables voice */
        return;
    }
    if (++g_acct.typed_failures >= 5) {
        int over = g_acct.typed_failures - 5;
        uint64_t wait = 3000ULL << (over < 5 ? over : 5);          /* 30 s, 60 s, ... 16 min */
        g_acct.retry_after = g_kstate.uptime_ticks + wait;
    }
}

/* Typed secret: checked against the password, or the PIN if it is digits. */
int auth_check_typed(const char *secret)
{
    char h[65];
    if (auth_wait_seconds() || !secret) return 0;
    int ok = 0;
    if (account_has_password()) {
        secret_hash(g_acct.pass_salt, secret, h);
        ok = !strcmp(h, g_acct.pass_hash);
    }
    if (!ok && account_has_pin() && valid_pin(secret)) {
        secret_hash(g_acct.pin_salt, secret, h);
        ok = !strcmp(h, g_acct.pin_hash);
    }
    typed_result(ok);
    return ok;
}

/* Spoken attempt. Returns 1 accepted, 0 not close enough, -1 voice sign-in
 * paused (too many misses: type once to re-enable). */
int auth_check_voice(const char *transcript)
{
    char said[128];
    if (!account_has_voice()) return 0;
    if (g_acct.voice_misses >= 10) return -1;
    normalise_spoken(transcript, said, sizeof(said));
    int len = (int)strlen(g_acct.voice);
    int dist = edit_distance(said, g_acct.voice);
    /* close enough: at most 1 wrong letter in 5 (80 % match) */
    int ok = len >= 10 && dist * 5 <= len;
    terminal_printf("[AUTH] Voice attempt: %d%% match\n", len ? 100 - dist * 100 / len : 0);
    if (ok) g_acct.voice_misses = 0; else g_acct.voice_misses++;
    return ok;
}
