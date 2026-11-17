/*
 * =============================================================================
 * JARVIS AI OS — Accounts, sign-in and settings store (kernel/accounts.c)
 * Language: C17
 *
 * Several user accounts (up to USER_ACCOUNT_MAX). Each has a numeric id (uid, from
 * 1000), a role (administrator or standard user), a private home folder
 * (/kryzen/users/<name>: Documents, Desktop, Recycle Bin ...) and its own
 * settings (the HKCU registry hive is saved and loaded per user). The first
 * account created is an administrator. One user is signed in at a time;
 * signing in as someone else ends the previous session.
 *
 * Administrators may manage users, disks, devices, packages and the system
 * registry; standard users may not, and cannot open another user's home.
 *
 * Each account has up to three ways to sign in:
 *   password   any text, stored as salted SHA-256 (never in plain text)
 *   PIN        4-12 digits, stored as salted SHA-256
 *   voice      a spoken passphrase. Speech recognition mishears, so a spoken
 *              attempt is accepted when it is close enough to the phrase
 *              (spelling, spacing, punctuation and number words are
 *              ignored; most letters must match). Because the comparison
 *              is approximate the phrase itself has to be stored; it is
 *              kept in /kryzen/system, which programs cannot read.
 *
 * Wrong typed attempts slow down further tries (30 s after 5, doubling), but
 * never lock the account permanently. Wrong spoken attempts do not count
 * towards that, so mishearing can never lock you out; after 10 misses in a
 * row voice sign-in pauses until a typed sign-in succeeds.
 *
 * Settings live in the registry: HKCU\\Software\\Kryzen OS\\Settings (kernel/registry.c).
 * =============================================================================
 */

#include "kernel.h"
#include <string.h>
#include <stdio.h>

#define USERS_FILE        "/kryzen/system/users.conf"
#define LEGACY_ACCOUNT    "/kryzen/system/account.conf"   /* single-owner format */
#define LEGACY_ACCOUNT_J  "/jarvis/system/account.conf"   /* before the rename to Kryzen */

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

typedef struct {
    int  uid, admin;
    char name[32];
    char home[24];                          /* folder name under /kryzen/users */
    char pass_salt[33], pass_hash[65];
    char pin_salt[33], pin_hash[65];
    char voice[96];                         /* normalised spoken passphrase */
    /* The password this person set before the system was renamed (their
     * earlier record on the same disk). It still signs them in, and is
     * dropped as soon as they choose a new password. */
    char old_salt[33], old_hash[65];
    int  typed_failures;
    uint64_t retry_after;                   /* tick before which typed tries wait */
    int  voice_misses;
} user_t;

static user_t g_users[USER_ACCOUNT_MAX];
static int g_user_count;
static int g_selected;                      /* the account the account_* calls refer to */
static int g_session = -1;                  /* who is signed in, -1 nobody */
static int g_next_uid = 1000;
static int g_hkcu_uid;                      /* whose settings the HKCU hive holds */
static int g_legacy_checked;                /* the pre-rename account file was looked at */

static user_t *cur(void)
{
    static user_t none;
    return g_user_count ? &g_users[g_selected < g_user_count ? g_selected : 0] : &none;
}

static int accounts_save(void)
{
    static char buf[8192];
    vfs_mkdir("/kryzen/system");
    int n = snprintf(buf, sizeof(buf), "next_uid=%d\nhkcu_uid=%d\nlegacy_checked=%d\n", g_next_uid, g_hkcu_uid, g_legacy_checked);
    for (int i = 0; i < g_user_count && n < (int)sizeof(buf) - 600; i++) {
        user_t *u = &g_users[i];
        n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                      "[user]\nuid=%d\nname=%s\nhome=%s\nadmin=%d\npass_salt=%s\npass_hash=%s\npin_salt=%s\npin_hash=%s\nvoice=%s\nold_salt=%s\nold_hash=%s\n",
                      u->uid, u->name, u->home, u->admin, u->pass_salt, u->pass_hash, u->pin_salt, u->pin_hash, u->voice,
                      u->old_salt, u->old_hash);
    }
    return vfs_write_file(USERS_FILE, buf, (size_t)n);
}

static void sync_kstate(void)
{
    snprintf(g_kstate.username, sizeof(g_kstate.username), "%s", g_user_count ? cur()->name : "User");
    g_kstate.security_enabled = account_has_sign_in();
    g_kstate.security_pin[0] = 0;           /* the plain PIN is never kept */
}

/* Folder name for a home: lower-case letters and digits, the rest as '-'. */
static void home_name(const char *name, char *out, size_t n)
{
    size_t k = 0;
    for (const char *p = name; *p && k < n - 1; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        out[k++] = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ? c : '-';
    }
    out[k] = 0;
    if (!k) snprintf(out, n, "user");
}

static int name_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        char y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y) return 0;
    }
    return !*a && !*b;
}

int user_find(const char *name)
{
    if (!name) return -1;
    for (int i = 0; i < g_user_count; i++)
        if (name_eq(g_users[i].name, name) || !strcmp(g_users[i].home, name)) return i;
    return -1;
}

static void user_home_path(const user_t *u, char *out, size_t n)
{
    snprintf(out, n, "/kryzen/users/%s", u->home);
}

/* Creates a user's folders (physical paths; no session view is involved). */
static void user_make_home(const user_t *u)
{
    static const char *const dirs[] = {
        "", "/data", "/data/Documents", "/data/Downloads", "/data/Pictures", "/data/Music",
        "/data/Videos", "/desktop", "/trash",
    };
    char home[64], path[128];
    user_home_path(u, home, sizeof(home));
    vfs_set_session(NULL, 1);                       /* system view while creating */
    vfs_mkdir("/kryzen/users");
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        snprintf(path, sizeof(path), "%s%s", home, dirs[i]);
        if (!vfs_is_dir(path)) vfs_mkdir(path);
    }
}

/* ---- Bringing an older disk along ------------------------------------------ */
typedef struct { const char *from, *to; int moved; } adopt_t;
static char g_adopt_names[64][64];
static int g_adopt_count;

static void adopt_collect(void *ctx, const char *name, size_t len, int is_dir)
{
    (void)ctx; (void)is_dir;
    if (g_adopt_count < 64 && len < 64) {
        memcpy(g_adopt_names[g_adopt_count], name, len);
        g_adopt_names[g_adopt_count++][len] = 0;
    }
}

/* Moves every item of folder `from` into `to` unless one of that name is
 * already there. Returns the number of items moved. */
static int adopt_folder(const char *from, const char *to)
{
    char a[320], b[320];
    int moved = 0;
    if (!vfs_is_dir(from)) return 0;
    g_adopt_count = 0;
    vfs_dir_entries(from, adopt_collect, NULL);
    for (int i = 0; i < g_adopt_count; i++) {
        snprintf(a, sizeof(a), "%s/%s", from, g_adopt_names[i]);
        snprintf(b, sizeof(b), "%s/%s", to, g_adopt_names[i]);
        if (vfs_exists(b)) continue;
        if (vfs_rename(a, b) == 0) moved++;
    }
    return moved;
}

/* The disk was used by the single-owner system: the owner's files become the
 * first user's home. Files from before the rename (/jarvis/...) come too. */
static void adopt_legacy_files(const user_t *u)
{
    char home[64], to[96];
    int moved = 0;
    user_home_path(u, home, sizeof(home));
    static const char *const roots[] = { "/kryzen", "/jarvis" };
    static const char *const parts[] = { "data", "desktop", "trash" };
    for (int r = 0; r < 2; r++)
        for (int k = 0; k < 3; k++) {
            char from[64];
            snprintf(from, sizeof(from), "%s/%s", roots[r], parts[k]);
            snprintf(to, sizeof(to), "%s/%s", home, parts[k]);
            /* Documents, Downloads ... exist on both sides: merge one level down */
            g_adopt_count = 0;
            if (!vfs_is_dir(from)) continue;
            static char subs[16][64];
            int nsub = 0;
            vfs_dir_entries(from, adopt_collect, NULL);
            for (int i = 0; i < g_adopt_count && nsub < 16; i++) {
                char a[160], b[160];
                snprintf(a, sizeof(a), "%s/%s", from, g_adopt_names[i]);
                snprintf(b, sizeof(b), "%s/%s", to, g_adopt_names[i]);
                if (vfs_is_dir(a) && vfs_is_dir(b)) snprintf(subs[nsub++], 64, "%s", g_adopt_names[i]);
            }
            moved += adopt_folder(from, to);
            for (int i = 0; i < nsub; i++) {
                char a[160], b[160];
                snprintf(a, sizeof(a), "%s/%s", from, subs[i]);
                snprintf(b, sizeof(b), "%s/%s", to, subs[i]);
                moved += adopt_folder(a, b);
            }
        }
    if (moved) terminal_printf("[AUTH] Moved %d existing item(s) into %s\n", moved, home);
}

static int load_legacy(const char *file)
{
    static char buf[1024];
    int n = vfs_read_file(file, buf, sizeof(buf));
    if (n <= 0) return 0;
    user_t *u = &g_users[0];
    memset(u, 0, sizeof(*u));
    kv_get(buf, "name", u->name, sizeof(u->name));
    if (!u->name[0]) return 0;
    kv_get(buf, "pass_salt", u->pass_salt, sizeof(u->pass_salt));
    kv_get(buf, "pass_hash", u->pass_hash, sizeof(u->pass_hash));
    kv_get(buf, "pin_salt", u->pin_salt, sizeof(u->pin_salt));
    kv_get(buf, "pin_hash", u->pin_hash, sizeof(u->pin_hash));
    kv_get(buf, "voice", u->voice, sizeof(u->voice));
    u->uid = 1000;
    u->admin = 1;
    home_name(u->name, u->home, sizeof(u->home));
    g_user_count = 1;
    g_next_uid = 1001;
    g_hkcu_uid = 1000;                      /* the saved settings are this user's */
    terminal_printf("[AUTH] Account %s from %s becomes the first user (administrator)\n", u->name, file);
    return 1;
}

static int kv_int(const char *text, const char *key, int dflt)
{
    char v[16];
    if (!kv_get(text, key, v, sizeof(v)) || !v[0]) return dflt;
    int r = 0;
    for (const char *p = v; *p >= '0' && *p <= '9'; p++) r = r * 10 + (*p - '0');
    return r;
}

static int load_users(void)
{
    static char buf[8192];
    int n = vfs_read_file(USERS_FILE, buf, sizeof(buf));
    if (n <= 0) return 0;
    g_next_uid = kv_int(buf, "next_uid", 1000);
    g_hkcu_uid = kv_int(buf, "hkcu_uid", 0);
    g_legacy_checked = kv_int(buf, "legacy_checked", 0);
    g_user_count = 0;
    for (char *p = strstr(buf, "[user]"); p && g_user_count < USER_ACCOUNT_MAX;) {
        char *next = strstr(p + 6, "[user]");
        char keep = 0;
        if (next) { keep = *next; *next = 0; }
        user_t *u = &g_users[g_user_count];
        memset(u, 0, sizeof(*u));
        const char *rec = p + 6;
        if (*rec == '\n') rec++;
        u->uid = kv_int(rec, "uid", 0);
        u->admin = kv_int(rec, "admin", 0);
        kv_get(rec, "name", u->name, sizeof(u->name));
        kv_get(rec, "home", u->home, sizeof(u->home));
        kv_get(rec, "pass_salt", u->pass_salt, sizeof(u->pass_salt));
        kv_get(rec, "pass_hash", u->pass_hash, sizeof(u->pass_hash));
        kv_get(rec, "pin_salt", u->pin_salt, sizeof(u->pin_salt));
        kv_get(rec, "pin_hash", u->pin_hash, sizeof(u->pin_hash));
        kv_get(rec, "voice", u->voice, sizeof(u->voice));
        kv_get(rec, "old_salt", u->old_salt, sizeof(u->old_salt));
        kv_get(rec, "old_hash", u->old_hash, sizeof(u->old_hash));
        if (u->name[0] && u->uid >= 1000) {
            if (!u->home[0]) home_name(u->name, u->home, sizeof(u->home));
            if (u->uid >= g_next_uid) g_next_uid = u->uid + 1;
            g_user_count++;
        }
        if (next) *next = keep;
        p = next;
    }
    return g_user_count > 0;
}

/* A disk used both before and after the rename holds two records of the
 * same person: /jarvis/system/account.conf (older) and the current one. The
 * older record's password keeps working, and its PIN and spoken passphrase
 * are taken over where the current record has none. Done once. */
static void adopt_legacy_credentials(void)
{
    static char buf[1024];
    char name[32], v[96];
    if (g_legacy_checked) return;
    g_legacy_checked = 1;
    int n = vfs_read_file(LEGACY_ACCOUNT_J, buf, sizeof(buf));
    if (n > 0 && kv_get(buf, "name", name, sizeof(name))) {
        int i = user_find(name);
        if (i >= 0) {
            user_t *u = &g_users[i];
            char salt[33], hash[65];
            kv_get(buf, "pass_salt", salt, sizeof(salt));
            kv_get(buf, "pass_hash", hash, sizeof(hash));
            if (hash[0] && strcmp(hash, u->pass_hash)) {
                snprintf(u->old_salt, sizeof(u->old_salt), "%s", salt);
                snprintf(u->old_hash, sizeof(u->old_hash), "%s", hash);
                terminal_printf("[AUTH] %s: the password from before the rename is accepted as well\n", u->name);
            }
            if (!u->pin_hash[0]) {
                kv_get(buf, "pin_salt", u->pin_salt, sizeof(u->pin_salt));
                kv_get(buf, "pin_hash", u->pin_hash, sizeof(u->pin_hash));
            }
            if (!u->voice[0] && kv_get(buf, "voice", v, sizeof(v)) && v[0]) {
                snprintf(u->voice, sizeof(u->voice), "%s", v);
                terminal_printf("[AUTH] %s: spoken passphrase restored from the earlier record\n", u->name);
            }
        }
    }
    accounts_save();
}

void accounts_init(void)
{
    memset(g_users, 0, sizeof(g_users));
    g_user_count = 0;
    g_selected = 0;
    g_session = -1;
    vfs_set_session(NULL, 1);
    if (!load_users() && (load_legacy(LEGACY_ACCOUNT) || load_legacy(LEGACY_ACCOUNT_J))) {
        user_make_home(&g_users[0]);
        adopt_legacy_files(&g_users[0]);
        accounts_save();
    }
    for (int i = 0; i < g_user_count; i++) user_make_home(&g_users[i]);
    if (g_user_count) adopt_legacy_credentials();
    reg_init();                     /* settings live in the registry (kernel/registry.c) */
    sync_kstate();
    if (!g_user_count) terminal_puts("[AUTH] No account yet: first-run setup\n");
    else terminal_printf("[AUTH] %d user account(s); %s\n", g_user_count,
                         g_user_count > 1 || account_has_sign_in() ? "sign-in required" : "no sign-in set");
}

/* ---- Settings ------------------------------------------------------------ */
/* Settings are the values of HKCU\Software\Kryzen OS\Settings. */
int setting_get(const char *key, char *out, size_t n)
{
    return reg_setting_get(key, out, n);
}

int setting_get_int(const char *key, int dflt)
{
    char v[16];
    if (!reg_setting_get(key, v, sizeof(v)) || !v[0]) return dflt;
    int r = 0, neg = v[0] == '-';
    for (const char *p = v + neg; *p >= '0' && *p <= '9'; p++) r = r * 10 + (*p - '0');
    return neg ? -r : r;
}

void setting_set(const char *key, const char *value)
{
    reg_setting_set(key, value, 0);
}

void setting_set_int(const char *key, int value)
{
    char v[16];
    snprintf(v, sizeof(v), "%d", value);
    reg_setting_set(key, v, 1);
}

/* ---- Account queries (the selected user) --------------------------------- */
int account_exists(void)       { return g_user_count > 0; }
const char *account_name(void) { return g_user_count ? cur()->name : "User"; }
int account_has_password(void) { return cur()->pass_hash[0] != 0 || cur()->old_hash[0] != 0; }
int account_has_pin(void)      { return cur()->pin_hash[0] != 0; }
int account_has_voice(void)    { return cur()->voice[0] != 0; }
int account_has_sign_in(void)  { return account_has_password() || account_has_pin() || account_has_voice(); }

/* ---- Users ------------------------------------------------------------------ */
int user_count(void)    { return g_user_count; }
int user_selected(void) { return g_user_count ? g_selected : -1; }
int user_current(void)  { return g_session; }

int user_info(int index, user_info_t *out)
{
    if (index < 0 || index >= g_user_count || !out) return -1;
    user_t *u = &g_users[index];
    memset(out, 0, sizeof(*out));
    out->uid = u->uid;
    out->admin = u->admin;
    snprintf(out->name, sizeof(out->name), "%s", u->name);
    snprintf(out->home, sizeof(out->home), "/kryzen/users/%s", u->home);
    out->has_password = u->pass_hash[0] != 0;
    out->has_pin = u->pin_hash[0] != 0;
    out->has_voice = u->voice[0] != 0;
    out->signed_in = index == g_session;
    return 0;
}

/* Chooses whose sign-in the lock screen (and account_*) deals with. While a
 * session is running, Settings keeps pointing at the signed-in user. */
int user_select(int index)
{
    if (index < 0 || index >= g_user_count) return -1;
    g_selected = index;
    sync_kstate();
    return 0;
}

/* Nobody signed in counts as the system itself (boot, installer, live). */
int user_is_admin(void)
{
    return g_session < 0 || g_users[g_session].admin;
}

int user_uid(void)
{
    return g_session < 0 ? 0 : g_users[g_session].uid;
}

static int admin_count(void)
{
    int n = 0;
    for (int i = 0; i < g_user_count; i++) n += g_users[i].admin;
    return n;
}

/* ---- Per-user settings: the HKCU hive --------------------------------------- */
static char g_hive_buf[192 * 1024];

static void hkcu_file(int uid, char *out, size_t n)
{
    snprintf(out, n, "/kryzen/system/hkcu-%d.reg", uid);
}

static void hkcu_save(int uid)
{
    char file[64];
    if (uid < 1000) return;
    int n = reg_export("HKCU", g_hive_buf, sizeof(g_hive_buf), 0);
    if (n <= 0) return;
    hkcu_file(uid, file, sizeof(file));
    vfs_write_file(file, g_hive_buf, (size_t)n);
}

/* Makes HKCU hold `uid`'s settings: the previous owner's are saved first. */
static void hkcu_switch(int uid)
{
    char file[64], path[200], err[96];
    if (g_hkcu_uid == uid) return;
    reg_flush();
    hkcu_save(g_hkcu_uid);
    int root = reg_open_key("HKCU", 0);
    for (int guard = 0; root >= 0 && guard < 4096; guard++) {       /* empty the hive */
        int child = reg_key_child(root, 0);
        if (child < 0 || reg_key_path(child, path, sizeof(path)) != 0 || reg_delete_key(path) != 0) break;
    }
    reg_settings_reset();
    hkcu_file(uid, file, sizeof(file));
    int n = vfs_read_file(file, g_hive_buf, sizeof(g_hive_buf));
    int nk = 0, nv = 0;
    if (n > 0 && reg_import(g_hive_buf, &nk, &nv, err, sizeof(err)) != 0)
        terminal_printf("[AUTH] Settings of uid %d: %s (using defaults)\n", uid, err);
    reg_open_key("HKCU\\Software\\Kryzen OS\\Settings", 1);
    reg_settings_reset();
    g_hkcu_uid = uid;
    accounts_save();
    reg_flush();
    terminal_printf("[AUTH] Settings of uid %d loaded (%d keys, %d values)\n", uid, nk, nv);
}

/* The selected user has authenticated: their session starts. Returns 1 if a
 * different user's session was ended to make room (the caller closes its
 * windows), 0 otherwise. */
int user_session_begin(void)
{
    if (!g_user_count) return 0;
    int changed = g_session >= 0 && g_session != g_selected;
    if (g_session == g_selected) return 0;
    user_t *u = &g_users[g_selected];
    char home[64];
    user_home_path(u, home, sizeof(home));
    user_make_home(u);
    g_session = g_selected;
    hkcu_switch(u->uid);
    vfs_set_session(home, u->admin);
    pkg_place_samples();                    /* sample packages in this user's Downloads */
    sync_kstate();
    terminal_printf("[AUTH] Session started: %s (uid %d, %s)\n", u->name, u->uid, u->admin ? "administrator" : "standard user");
    return changed;
}

/* Sign out: nobody is signed in afterwards. */
void user_session_end(void)
{
    if (g_session < 0) return;
    terminal_printf("[AUTH] Session ended: %s\n", g_users[g_session].name);
    reg_flush();
    hkcu_save(g_users[g_session].uid);
    g_session = -1;
    vfs_set_session(NULL, 1);
}

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

static int valid_name(const char *name, char *err, size_t errn)
{
    size_t l = name ? strlen(name) : 0;
    if (!l) { snprintf(err, errn, "Type a name."); return 0; }
    if (l > 30) { snprintf(err, errn, "A name can have up to 30 characters."); return 0; }
    for (const char *p = name; *p; p++)
        if (*p < 32 || *p == '/' || *p == '\\' || *p == '=' || *p == '[' || *p == ']') {
            snprintf(err, errn, "A name cannot contain / \\ = [ or ].");
            return 0;
        }
    return 1;
}

/* Adds an account. Returns its index, or -1 with a reason in err. */
int user_add(const char *name, const char *password, const char *pin, const char *voice,
             int admin, char *err, size_t errn)
{
    char norm[96], home[24];
    if (!valid_name(name, err, errn)) return -1;
    if (g_user_count >= USER_ACCOUNT_MAX) { snprintf(err, errn, "This PC already has %d users, the most it supports.", USER_ACCOUNT_MAX); return -1; }
    home_name(name, home, sizeof(home));
    for (int i = 0; i < g_user_count; i++)
        if (name_eq(g_users[i].name, name) || !strcmp(g_users[i].home, home)) {
            snprintf(err, errn, "There is already a user called %s.", g_users[i].name);
            return -1;
        }
    if (pin && pin[0] && !valid_pin(pin)) { snprintf(err, errn, "A PIN is 4 to 12 digits."); return -1; }
    if (voice && voice[0]) {
        normalise_spoken(voice, norm, sizeof(norm));
        if (strlen(norm) < 10) { snprintf(err, errn, "Use a spoken passphrase of at least three words."); return -1; }
    }
    user_t *u = &g_users[g_user_count];
    memset(u, 0, sizeof(*u));
    u->uid = g_next_uid++;
    u->admin = admin || g_user_count == 0;          /* the first account administers the PC */
    snprintf(u->name, sizeof(u->name), "%s", name);
    snprintf(u->home, sizeof(u->home), "%s", home);
    set_secret(u->pass_salt, u->pass_hash, password);
    set_secret(u->pin_salt, u->pin_hash, pin);
    if (voice && voice[0]) normalise_spoken(voice, u->voice, sizeof(u->voice));
    int index = g_user_count++;
    int session = g_session;
    user_make_home(u);
    if (session >= 0) {                             /* back to the signed-in user's view */
        char h[64];
        user_home_path(&g_users[session], h, sizeof(h));
        vfs_set_session(h, g_users[session].admin);
    }
    if (accounts_save() != 0) { g_user_count--; snprintf(err, errn, "The account could not be saved."); return -1; }
    terminal_printf("[AUTH] Account created for %s (uid %d, %s)\n", u->name, u->uid, u->admin ? "administrator" : "standard user");
    return index;
}

/* First-run setup: creates the first account and selects it. */
int account_create(const char *name, const char *password, const char *pin, const char *voice,
                   char *err, size_t errn)
{
    int index = user_add(name, password, pin, voice, 1, err, errn);
    if (index < 0) return -1;
    g_selected = index;
    if (g_user_count == 1) g_hkcu_uid = g_users[index].uid;   /* settings chosen during setup are theirs */
    accounts_save();
    sync_kstate();
    return 0;
}

/* Removes an account and its home folder. Administrators only; never the
 * signed-in user, never the last administrator. */
int user_remove(const char *name, char *err, size_t errn)
{
    int i = user_find(name);
    if (!user_is_admin()) { snprintf(err, errn, "Only an administrator can remove a user."); return -1; }
    if (i < 0) { snprintf(err, errn, "There is no user called %s.", name ? name : ""); return -1; }
    if (i == g_session) { snprintf(err, errn, "You cannot remove the account you are signed in with."); return -1; }
    if (g_users[i].admin && admin_count() <= 1) { snprintf(err, errn, "This is the only administrator; make another user an administrator first."); return -1; }
    char home[64], file[64];
    user_home_path(&g_users[i], home, sizeof(home));
    hkcu_file(g_users[i].uid, file, sizeof(file));
    vfs_delete(home);
    vfs_delete(file);
    terminal_printf("[AUTH] Account removed: %s\n", g_users[i].name);
    for (int k = i; k < g_user_count - 1; k++) g_users[k] = g_users[k + 1];
    g_user_count--;
    if (g_session > i) g_session--;
    if (g_selected > i) g_selected--;
    if (g_selected >= g_user_count) g_selected = 0;
    sync_kstate();
    return accounts_save();
}

int user_set_admin(const char *name, int admin, char *err, size_t errn)
{
    int i = user_find(name);
    if (!user_is_admin()) { snprintf(err, errn, "Only an administrator can change a user's role."); return -1; }
    if (i < 0) { snprintf(err, errn, "There is no user called %s.", name ? name : ""); return -1; }
    if (!admin && g_users[i].admin && admin_count() <= 1) { snprintf(err, errn, "This PC needs at least one administrator."); return -1; }
    g_users[i].admin = admin ? 1 : 0;
    if (i == g_session) {
        char h[64];
        user_home_path(&g_users[i], h, sizeof(h));
        vfs_set_session(h, g_users[i].admin);
    }
    return accounts_save();
}

/* An administrator sets (or clears, with "") another user's password. The
 * user's PIN and passphrase are cleared with it, so the new password is the
 * way in. */
int user_reset_password(const char *name, const char *password, char *err, size_t errn)
{
    int i = user_find(name);
    if (i < 0) { snprintf(err, errn, "There is no user called %s.", name ? name : ""); return -1; }
    if (!user_is_admin() && i != g_session) { snprintf(err, errn, "Only an administrator can change another user's password."); return -1; }
    user_t *u = &g_users[i];
    set_secret(u->pass_salt, u->pass_hash, password);
    u->old_salt[0] = u->old_hash[0] = 0;
    if (i != g_session) { u->pin_salt[0] = u->pin_hash[0] = 0; u->voice[0] = 0; }
    u->typed_failures = 0; u->retry_after = 0; u->voice_misses = 0;
    sync_kstate();
    return accounts_save();
}

int account_set_name(const char *name)
{
    char err[64];
    if (!g_user_count || !valid_name(name, err, sizeof(err))) return -1;
    for (int i = 0; i < g_user_count; i++)
        if (i != g_selected && name_eq(g_users[i].name, name)) return -1;
    snprintf(cur()->name, sizeof(cur()->name), "%s", name);     /* the home folder keeps its name */
    sync_kstate();
    return accounts_save();
}

int account_set_password(const char *password)
{
    if (!g_user_count) return -1;
    set_secret(cur()->pass_salt, cur()->pass_hash, password);
    cur()->old_salt[0] = cur()->old_hash[0] = 0;       /* one password from now on */
    sync_kstate();
    return accounts_save();
}

int account_set_pin(const char *pin, char *err, size_t errn)
{
    if (!g_user_count) return -1;
    if (pin && pin[0] && !valid_pin(pin)) { snprintf(err, errn, "A PIN is 4 to 12 digits."); return -1; }
    set_secret(cur()->pin_salt, cur()->pin_hash, pin);
    sync_kstate();
    return accounts_save();
}

int account_set_voice(const char *phrase, char *err, size_t errn)
{
    char norm[96] = "";
    if (!g_user_count) return -1;
    if (phrase && phrase[0]) {
        normalise_spoken(phrase, norm, sizeof(norm));
        if (strlen(norm) < 10) { snprintf(err, errn, "Use at least three words."); return -1; }
    }
    snprintf(cur()->voice, sizeof(cur()->voice), "%s", norm);
    cur()->voice_misses = 0;
    sync_kstate();
    return accounts_save();
}

/* ---- Checking ------------------------------------------------------------ */
/* Seconds before another typed attempt is allowed (0 = now). */
int auth_wait_seconds(void)
{
    if (g_kstate.uptime_ticks >= cur()->retry_after) return 0;
    return (int)((cur()->retry_after - g_kstate.uptime_ticks + 99) / 100);
}

static void typed_result(user_t *u, int ok)
{
    if (ok) {
        u->typed_failures = 0;
        u->voice_misses = 0;                     /* a typed sign-in re-enables voice */
        return;
    }
    if (++u->typed_failures >= 5) {
        int over = u->typed_failures - 5;
        uint64_t wait = 3000ULL << (over < 5 ? over : 5);          /* 30 s, 60 s, ... 16 min */
        u->retry_after = g_kstate.uptime_ticks + wait;
    }
}

static int secret_matches(const user_t *u, const char *secret)
{
    char h[65];
    if (u->pass_hash[0]) {
        secret_hash(u->pass_salt, secret, h);
        if (!strcmp(h, u->pass_hash)) return 1;
    }
    if (u->pin_hash[0] && valid_pin(secret)) {
        secret_hash(u->pin_salt, secret, h);
        if (!strcmp(h, u->pin_hash)) return 1;
    }
    if (u->old_hash[0]) {
        secret_hash(u->old_salt, secret, h);
        if (!strcmp(h, u->old_hash)) return 1;
    }
    return 0;
}

/* Typed secret of the selected user: the password, or the PIN if it is
 * digits. The log records only that an attempt failed, never what was typed. */
int auth_check_typed(const char *secret)
{
    if (!g_user_count || auth_wait_seconds() || !secret) return 0;
    int ok = secret_matches(cur(), secret);
    typed_result(cur(), ok);
    if (!ok) terminal_printf("[AUTH] Typed sign-in for %s rejected (%d characters; attempt %d)\n",
                             cur()->name, (int)strlen(secret), cur()->typed_failures);
    return ok;
}

/* Any administrator's password or PIN (for confirming system changes). */
int auth_check_admin(const char *secret)
{
    if (!secret || !secret[0]) return 0;
    for (int i = 0; i < g_user_count; i++)
        if (g_users[i].admin && secret_matches(&g_users[i], secret)) return 1;
    return 0;
}

/* Recovery (Safe Mode): removes every sign-in secret of a user so that they
 * can get back in and choose new ones. */
int auth_recovery_clear(const char *name)
{
    int i = user_find(name);
    if (i < 0 || !kernel_safe_mode()) return -1;
    user_t *u = &g_users[i];
    u->pass_salt[0] = u->pass_hash[0] = 0;
    u->pin_salt[0] = u->pin_hash[0] = 0;
    u->old_salt[0] = u->old_hash[0] = 0;
    u->voice[0] = 0;
    u->typed_failures = 0; u->retry_after = 0; u->voice_misses = 0;
    sync_kstate();
    terminal_printf("[AUTH] Recovery: sign-in secrets of %s cleared\n", u->name);
    return accounts_save();
}

/* Folds letters speech recognition confuses ("ph"/"f", "c"/"k", "z"/"s",
 * doubled letters, silent "h" after a consonant), so "Kristofer" and
 * "Christopher" compare as the same word. */
static void fold_sounds(const char *in, char *out, size_t n)
{
    size_t k = 0;
    for (size_t i = 0; in[i] && k < n - 1; i++) {
        char c = in[i], nx = in[i + 1];
        if (c == 'p' && nx == 'h') { c = 'f'; i++; }
        else if (c == 'c' && nx == 'k') { c = 'k'; i++; }
        else if (c == 'c' && nx == 'h') { c = 'k'; i++; }
        else if (c == 'c') c = (nx == 'e' || nx == 'i' || nx == 'y') ? 's' : 'k';
        else if (c == 'q') c = 'k';
        else if (c == 'z') c = 's';
        else if (c == 'y') c = 'i';
        else if (c == 'w' && nx == 'h') { c = 'w'; i++; }
        else if (c == 'h' && k && !(out[k - 1] == 'a' || out[k - 1] == 'e' || out[k - 1] == 'i' || out[k - 1] == 'o' || out[k - 1] == 'u')) continue;
        if (k && out[k - 1] == c) continue;                  /* doubled letters */
        out[k++] = c;
    }
    out[k] = 0;
}

/* Fewest edits that turn `want` into some stretch of `said`: words before
 * or after the passphrase ("okay, <passphrase>, thanks") cost nothing. */
static int best_window_distance(const char *said, const char *want)
{
    int ls = (int)strlen(said), lw = (int)strlen(want);
    if (ls > 190 || lw > 95) return 1000;
    static int prev[192], cur[192];
    for (int j = 0; j <= ls; j++) prev[j] = 0;                /* start anywhere in what was said */
    for (int i = 1; i <= lw; i++) {
        cur[0] = i;
        for (int j = 1; j <= ls; j++) {
            int sub = prev[j - 1] + (want[i - 1] != said[j - 1]);
            int del = prev[j] + 1, ins = cur[j - 1] + 1;
            cur[j] = sub < del ? (sub < ins ? sub : ins) : (del < ins ? del : ins);
        }
        memcpy(prev, cur, sizeof(int) * (size_t)(ls + 1));
    }
    int best = 1000;
    for (int j = 0; j <= ls; j++) if (prev[j] < best) best = prev[j];   /* end anywhere */
    return best;
}

/* Spoken attempt. Returns 1 accepted, 0 not close enough, -1 voice sign-in
 * paused (too many misses: type once to re-enable), -2 ignored. Fragments far shorter
 * than the passphrase (a cough, "thank you", the TV) are ignored instead
 * of counting as misses, so background noise cannot pause voice sign-in. */
int auth_check_voice(const char *transcript)
{
    char said[192], fs[192], fw[96];
    if (!account_has_voice()) return 0;
    user_t *u = cur();
    if (u->voice_misses >= 10) return -1;
    normalise_spoken(transcript, said, sizeof(said));
    int len = (int)strlen(u->voice), sl = (int)strlen(said);
    if (len < 10) return 0;
    if (sl * 2 < len) {
        terminal_printf("[AUTH] Voice attempt ignored: too short to be the passphrase\n");
        return -2;
    }
    /* whole transcript as said, then the best stretch of it, then by sound */
    int dist = edit_distance(said, u->voice);
    int win = sl <= len * 3 ? best_window_distance(said, u->voice) : 1000;
    fold_sounds(said, fs, sizeof(fs));
    fold_sounds(u->voice, fw, sizeof(fw));
    int fl = (int)strlen(fw);
    int fdist = (int)strlen(fs) <= fl * 3 ? best_window_distance(fs, fw) : 1000;
    /* close enough: about 1 wrong letter in 5 as spelled, 1 in 6 by sound */
    int ok = dist * 5 <= len || win * 5 <= len || (fl >= 8 && fdist * 6 <= fl);
    int pct = len ? 100 - (dist < win ? dist : win) * 100 / len : 0;
    terminal_printf("[AUTH] Voice attempt: %d%% match%s\n", pct < 0 ? 0 : pct, ok ? " (accepted)" : "");
    if (ok) u->voice_misses = 0; else u->voice_misses++;
    return ok;
}
