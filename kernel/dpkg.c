/*
 * =============================================================================
 * JARVIS AI OS — dpkg and apt for the Linux layer (kernel/dpkg.c)
 * Language: C17
 *
 * Installs real Debian packages (.deb) into the Linux root /jarvis/linux,
 * where the Linux compatibility layer (kernel/linux.c) runs them.
 *
 *   dpkg -i file.deb     unpack and register a package
 *   dpkg -r|-P name      remove it (files, links, registration)
 *   dpkg -l / -L / -s    list packages, a package's files, its status
 *   dpkg -c / -I f.deb   contents / control information of a .deb
 *   apt update           download the package index (Packages.xz) named in
 *                        /etc/apt/sources.list (Debian 12 by default)
 *   apt install names    resolve dependencies, download over HTTPS, check
 *                        size and SHA-256, unpack in dependency order
 *   apt remove|purge     remove packages and the ones that depend on them
 *   apt search|show|list|upgrade|policy
 *
 * The package database has the Debian layout (/var/lib/dpkg/status,
 * /var/lib/dpkg/info/<name>.list ...). Files listed in
 * /etc/dpkg/dpkg.cfg.d/excludes (manuals, translations, docs) are skipped,
 * as on Debian's minimal images.
 *
 * Honest limits: package maintainer scripts (preinst/postinst ...) are not
 * run, because the Linux layer cannot start child processes (fork/exec) yet;
 * apt reports which packages had one. Release files are fetched over HTTPS
 * from the configured mirror and each .deb is checked against the SHA-256
 * in the index, but the index's GPG signature is not verified.
 * =============================================================================
 */

#include "kernel.h"
#include "crypto.h"
#include "xz.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#define LXR          "/jarvis/linux"
#define DPKG_DIR     LXR "/var/lib/dpkg"
#define STATUS_FILE  DPKG_DIR "/status"
#define APT_LISTS    LXR "/var/lib/apt/lists"
#define SOURCES      LXR "/etc/apt/sources.list"
#define EXCLUDES     LXR "/etc/dpkg/dpkg.cfg.d/excludes"

/* ---- helpers ------------------------------------------------------------------------ */
static void *big_alloc(size_t n) { return n >= LARGE_ALLOC_THRESHOLD ? kmalloc_large(n) : kmalloc(n ? n : 1); }
static void big_free(void *p, size_t n) { if (!p) return; if (n >= LARGE_ALLOC_THRESHOLD) kfree_large(p, n); else kfree(p); }

static void say(const char *fmt, ...)
{
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    user_console_write(b, strlen(b));
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int is_alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

static int ci_contains(const char *hay, size_t hl, const char *needle)
{
    size_t nl = strlen(needle);
    if (!nl) return 1;
    for (size_t i = 0; i + nl <= hl; i++) {
        size_t k = 0;
        while (k < nl && lower(hay[i + k]) == lower(needle[k])) k++;
        if (k == nl) return 1;
    }
    return 0;
}

/* Reads a whole JARVIS file into a fresh buffer (NUL-terminated). */
static char *read_all(const char *path, size_t *len)
{
    long size = vfs_size(path);
    if (size < 0 || vfs_is_dir(path)) return NULL;
    char *b = (char *)big_alloc((size_t)size + 1);
    if (!b) return NULL;
    int n = vfs_read_file(path, b, (size_t)size + 1);
    if (n < 0) { big_free(b, (size_t)size + 1); return NULL; }
    b[n] = 0;
    if (len) *len = (size_t)size;
    return b;
}

static uint64_t dec(const char *s)
{
    uint64_t v = 0;
    while (*s == ' ') s++;
    for (; is_digit(*s); s++) v = v * 10 + (uint64_t)(*s - '0');
    return v;
}

static void human(uint64_t bytes, char *out, size_t n)
{
    if (bytes >= 10ull << 20) snprintf(out, n, "%u MB", (unsigned)(bytes >> 20));
    else if (bytes >= 1024) snprintf(out, n, "%u kB", (unsigned)((bytes + 512) / 1024));
    else snprintf(out, n, "%u B", (unsigned)bytes);
}

/* JARVIS path of a Linux path (merged /usr and links in the folders). */
static void jpath(const char *linux_path, char *out, size_t n, int follow_last)
{
    char c[256];
    linux_canon(linux_path, c, follow_last);
    snprintf(out, n, "%s%s", LXR, strcmp(c, "/") ? c : "");
}

static void mkdirs(const char *jp)
{
    char p[256];
    snprintf(p, sizeof(p), "%s", jp);
    for (char *s = p + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        if (!vfs_is_dir(p) && !vfs_exists(p)) vfs_mkdir(p);
        *s = '/';
    }
    if (!vfs_is_dir(p) && !vfs_exists(p)) vfs_mkdir(p);
}

/* ---- Debian control fields ---------------------------------------------------------- */
/* Value of `key` in a stanza (first line plus continuation lines joined). */
static int field(const char *st, size_t len, const char *key, char *out, size_t n)
{
    size_t kl = strlen(key);
    out[0] = 0;
    const char *end = st + len;
    for (const char *p = st; p < end;) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol) eol = end;
        if ((size_t)(eol - p) > kl && p[kl] == ':') {
            size_t k = 0;
            while (k < kl && lower(p[k]) == lower(key[k])) k++;
            if (k == kl) {
                const char *v = p + kl + 1;
                while (v < eol && (*v == ' ' || *v == '\t')) v++;
                size_t o = 0;
                for (; v < eol && o + 1 < n; v++) out[o++] = *v;
                /* continuation lines (not for Description) */
                if (strcmp(key, "Description"))
                    while (eol < end && eol + 1 < end && (eol[1] == ' ' || eol[1] == '\t')) {
                        const char *q = eol + 1;
                        eol = memchr(q, '\n', (size_t)(end - q));
                        if (!eol) eol = end;
                        while (q < eol && (*q == ' ' || *q == '\t')) q++;
                        if (o + 2 < n) out[o++] = ' ';
                        for (; q < eol && o + 1 < n; q++) out[o++] = *q;
                    }
                out[o] = 0;
                return 1;
            }
        }
        p = eol < end ? eol + 1 : end;
    }
    return 0;
}

/* ---- versions (Debian policy 5.6.12) --------------------------------------------------- */
static int vorder(int c)
{
    if (is_digit(c)) return 0;
    if (is_alpha(c)) return c;
    if (c == '~') return -1;
    if (c) return c + 256;
    return 0;
}

static int verrevcmp(const char *a, const char *ae, const char *b, const char *be)
{
    while (a < ae || b < be) {
        int first_diff = 0;
        while ((a < ae && !is_digit(*a)) || (b < be && !is_digit(*b))) {
            int ac = a < ae ? vorder(*a) : 0, bc = b < be ? vorder(*b) : 0;
            if (ac != bc) return ac - bc;
            a++; b++;
        }
        while (a < ae && *a == '0') a++;
        while (b < be && *b == '0') b++;
        while (a < ae && b < be && is_digit(*a) && is_digit(*b)) {
            if (!first_diff) first_diff = *a - *b;
            a++; b++;
        }
        if (a < ae && is_digit(*a)) return 1;
        if (b < be && is_digit(*b)) return -1;
        if (first_diff) return first_diff;
    }
    return 0;
}

int dpkg_vercmp(const char *a, const char *b)
{
    long ea = 0, eb = 0;
    const char *ca = strchr(a, ':'), *cb = strchr(b, ':');
    if (ca) { for (const char *p = a; p < ca; p++) ea = ea * 10 + (*p - '0'); a = ca + 1; }
    if (cb) { for (const char *p = b; p < cb; p++) eb = eb * 10 + (*p - '0'); b = cb + 1; }
    if (ea != eb) return ea < eb ? -1 : 1;
    const char *ra = strrchr(a, '-'), *rb = strrchr(b, '-');
    const char *ua_end = ra ? ra : a + strlen(a), *ub_end = rb ? rb : b + strlen(b);
    int r = verrevcmp(a, ua_end, b, ub_end);
    if (r) return r;
    const char *rae = a + strlen(a), *rbe = b + strlen(b);
    return verrevcmp(ra ? ra + 1 : rae, rae, rb ? rb + 1 : rbe, rbe);
}

static int ver_ok(const char *have, const char *op, const char *want)
{
    if (!op[0]) return 1;
    int c = dpkg_vercmp(have, want);
    if (!strcmp(op, ">=")) return c >= 0;
    if (!strcmp(op, "<=")) return c <= 0;
    if (!strcmp(op, ">>") || !strcmp(op, ">")) return c > 0;
    if (!strcmp(op, "<<") || !strcmp(op, "<")) return c < 0;
    if (!strcmp(op, "=")) return c == 0;
    return 1;
}

/* One alternative of a dependency: "name:any (>= 1.2)". alt: another
 * alternative of the same group follows ("a | b"). */
typedef struct { char name[96], op[4], ver[64]; int alt; } dep_t;

static const char *parse_dep(const char *p, const char *end, dep_t *d)
{
    memset(d, 0, sizeof(*d));
    while (p < end && *p == ' ') p++;
    size_t o = 0;
    while (p < end && *p != ' ' && *p != '(' && *p != ',' && *p != '|' && *p != ':' && *p != '[' && *p != '<' && o + 1 < sizeof(d->name)) d->name[o++] = *p++;
    d->name[o] = 0;
    while (p < end && *p != ',' && *p != '|') {
        if (*p == '(') {
            p++;
            while (p < end && *p == ' ') p++;
            o = 0;
            while (p < end && (*p == '<' || *p == '>' || *p == '=') && o < 2) d->op[o++] = *p++;
            d->op[o] = 0;
            while (p < end && *p == ' ') p++;
            o = 0;
            while (p < end && *p != ')' && *p != ' ' && o + 1 < sizeof(d->ver)) d->ver[o++] = *p++;
            d->ver[o] = 0;
            while (p < end && *p != ')') p++;
        }
        if (p < end) p++;
    }
    if (p < end) { d->alt = *p == '|'; p++; }              /* past the separator */
    return p;
}

/* ---- installed packages (/var/lib/dpkg/status) ------------------------------------------- */
#define MAX_INST 4096
typedef struct { char name[96], ver[64]; char *stanza; size_t len; } inst_t;
static inst_t g_inst[MAX_INST];
static int g_ninst;
static char *g_status;
static size_t g_status_len;

static void status_load(void)
{
    if (g_status) big_free(g_status, g_status_len + 1);
    g_status = read_all(STATUS_FILE, &g_status_len);
    if (!g_status) { g_status = (char *)big_alloc(1); g_status[0] = 0; g_status_len = 0; }
    g_ninst = 0;
    char *p = g_status, *end = g_status + g_status_len;
    while (p < end && g_ninst < MAX_INST) {
        while (p < end && *p == '\n') p++;
        if (p >= end) break;
        char *s = p;
        while (p < end && !(p[0] == '\n' && (p + 1 >= end || p[1] == '\n'))) p++;
        if (p < end) p++;
        inst_t *e = &g_inst[g_ninst];
        char status[64];
        field(s, (size_t)(p - s), "Status", status, sizeof(status));
        if (!field(s, (size_t)(p - s), "Package", e->name, sizeof(e->name)) || !strstr(status, "installed") || strstr(status, "not-installed")) continue;
        field(s, (size_t)(p - s), "Version", e->ver, sizeof(e->ver));
        e->stanza = s;
        e->len = (size_t)(p - s);
        g_ninst++;
    }
}

static inst_t *inst_find(const char *name)
{
    for (int i = 0; i < g_ninst; i++) if (!strcmp(g_inst[i].name, name)) return &g_inst[i];
    return NULL;
}

/* Some installed package provides `name` (virtual package)? */
static int inst_provides(const char *name)
{
    static char prov[4096];
    for (int i = 0; i < g_ninst; i++) {
        if (!field(g_inst[i].stanza, g_inst[i].len, "Provides", prov, sizeof(prov))) continue;
        const char *p = prov, *end = prov + strlen(prov);
        while (p < end) {
            dep_t d;
            p = parse_dep(p, end, &d);
            if (!strcmp(d.name, name)) return 1;
        }
    }
    return 0;
}

/* Rewrites the status file: drops `name`, then appends `add` (if any). */
static int status_write(const char *name, const char *add, size_t add_len)
{
    size_t cap = g_status_len + add_len + 4;
    char *o = (char *)big_alloc(cap);
    if (!o) return -1;
    size_t n = 0;
    for (int i = 0; i < g_ninst; i++) {
        if (!strcmp(g_inst[i].name, name)) continue;
        memcpy(o + n, g_inst[i].stanza, g_inst[i].len);
        n += g_inst[i].len;
        if (n && o[n - 1] != '\n') o[n++] = '\n';
        o[n++] = '\n';
    }
    if (add_len) {
        memcpy(o + n, add, add_len);
        n += add_len;
        if (o[n - 1] != '\n') o[n++] = '\n';
        o[n++] = '\n';
    }
    int rc = vfs_write_file(STATUS_FILE, o, n);
    big_free(o, cap);
    status_load();
    return rc;
}

/* ---- path exclusions (dpkg path-exclude / path-include) -------------------------------- */
static int glob(const char *pat, const char *s)
{
    while (*pat) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return 1;
            for (; *s; s++) if (glob(pat, s)) return 1;
            return glob(pat, s);
        }
        if (*pat == '?' ? !*s : *pat != *s) return 0;
        pat++; s++;
    }
    return !*s;
}

static char *g_excl;
static size_t g_excl_len;

static int excluded(const char *path)
{
    if (!g_excl) return 0;
    int ex = 0;
    for (char *p = g_excl; *p;) {
        char *eol = strchr(p, '\n');
        size_t l = eol ? (size_t)(eol - p) : strlen(p);
        char line[256];
        if (l < sizeof(line)) {
            memcpy(line, p, l);
            line[l] = 0;
            if (!strncmp(line, "path-exclude=", 13) && glob(line + 13, path)) ex = 1;
            else if (!strncmp(line, "path-include=", 13) && glob(line + 13, path)) ex = 0;
        }
        p = eol ? eol + 1 : p + l;
    }
    return ex;
}

/* ---- archives: ar, gzip, xz, tar ---------------------------------------------------------- */
typedef struct { const uint8_t *data; size_t len; char name[32]; } member_t;

static int ar_find(const uint8_t *a, size_t len, const char *prefix, member_t *m)
{
    if (len < 8 || memcmp(a, "!<arch>\n", 8)) return -1;
    size_t off = 8;
    while (off + 60 <= len) {
        const uint8_t *h = a + off;
        char name[17];
        memcpy(name, h, 16);
        name[16] = 0;
        for (int i = 15; i >= 0 && (name[i] == ' ' || name[i] == '/'); i--) name[i] = 0;
        size_t size = 0;
        for (int i = 48; i < 58 && h[i] >= '0' && h[i] <= '9'; i++) size = size * 10 + (size_t)(h[i] - '0');
        if (off + 60 + size > len) return -1;
        if (!strncmp(name, prefix, strlen(prefix))) {
            m->data = h + 60;
            m->len = size;
            snprintf(m->name, sizeof(m->name), "%s", name);
            return 0;
        }
        off += 60 + size + (size & 1);
    }
    return -1;
}

/* tar, fed in pieces */
enum { TE_FILE = 1, TE_DIR, TE_SYMLINK, TE_HARDLINK };
typedef int (*tar_entry_fn)(void *ctx, int type, const char *path, const char *link, const uint8_t *data, size_t size);

typedef struct {
    uint8_t hdr[512];
    size_t hfill;
    uint64_t remain, pad;
    int kind;                       /* 0 none, else the header type char */
    char path[512], link[512], longname[512], longlink[512];
    int has_longname, has_longlink;
    uint8_t *data;
    size_t dsize, dfill;
    int zeros, done;
    tar_entry_fn fn;
    void *ctx;
    char err[128];
} tar_t;

static uint64_t octal(const uint8_t *p, int n)
{
    if (p[0] & 0x80) {                                      /* base-256 */
        uint64_t v = p[0] & 0x7F;
        for (int i = 1; i < n; i++) v = (v << 8) | p[i];
        return v;
    }
    uint64_t v = 0;
    for (int i = 0; i < n && p[i]; i++) if (p[i] >= '0' && p[i] <= '7') v = v * 8 + (uint64_t)(p[i] - '0');
    return v;
}

static void clean_path(const char *in, char *out, size_t n)
{
    while (in[0] == '.' && in[1] == '/') in += 2;
    while (in[0] == '/') in++;
    if (!strcmp(in, ".")) in = "";
    snprintf(out, n, "/%s", in);
    size_t l = strlen(out);
    while (l > 1 && out[l - 1] == '/') out[--l] = 0;
}

static int tar_finish(tar_t *t)
{
    char k = (char)t->kind;
    if (k == 'L') { snprintf(t->longname, sizeof(t->longname), "%.*s", (int)t->dfill, (char *)t->data); t->has_longname = 1; }
    else if (k == 'K') { snprintf(t->longlink, sizeof(t->longlink), "%.*s", (int)t->dfill, (char *)t->data); t->has_longlink = 1; }
    else if (k == 'x') {                                    /* pax: "len key=value\n" */
        const char *p = (const char *)t->data, *end = p + t->dfill;
        while (p < end) {
            size_t rl = 0;
            const char *q = p;
            while (q < end && is_digit(*q)) rl = rl * 10 + (size_t)(*q++ - '0');
            if (!rl || p + rl > end) break;
            const char *kv = q + 1, *eq = memchr(kv, '=', (size_t)(p + rl - kv));
            if (eq) {
                size_t vl = (size_t)(p + rl - 1 - (eq + 1));
                if (!strncmp(kv, "path=", 5)) { snprintf(t->longname, sizeof(t->longname), "%.*s", (int)vl, eq + 1); t->has_longname = 1; }
                if (!strncmp(kv, "linkpath=", 9)) { snprintf(t->longlink, sizeof(t->longlink), "%.*s", (int)vl, eq + 1); t->has_longlink = 1; }
            }
            p += rl;
        }
    } else if (k == '0' || k == 0 || k == '7') {
        if (t->fn(t->ctx, TE_FILE, t->path, "", t->data, t->dfill)) return -1;
    }
    if (t->data) big_free(t->data, t->dsize + 1);
    t->data = NULL;
    t->dsize = t->dfill = 0;
    t->kind = 0;
    return 0;
}

static int tar_header(tar_t *t)
{
    const uint8_t *h = t->hdr;
    int zero = 1;
    for (int i = 0; i < 512; i++) if (h[i]) { zero = 0; break; }
    if (zero) { if (++t->zeros >= 2) t->done = 1; return 0; }
    t->zeros = 0;
    uint64_t sum = 0, want = octal(h + 148, 8);
    for (int i = 0; i < 512; i++) sum += (i >= 148 && i < 156) ? ' ' : h[i];
    if (sum != want) { snprintf(t->err, sizeof(t->err), "damaged tar header"); return -1; }
    char name[512];
    if (!memcmp(h + 257, "ustar", 5) && h[345]) snprintf(name, sizeof(name), "%.155s/%.100s", (const char *)h + 345, (const char *)h);
    else snprintf(name, sizeof(name), "%.100s", (const char *)h);
    char link[512];
    snprintf(link, sizeof(link), "%.100s", (const char *)h + 157);
    if (t->has_longname) { snprintf(name, sizeof(name), "%s", t->longname); t->has_longname = 0; }
    if (t->has_longlink) { snprintf(link, sizeof(link), "%s", t->longlink); t->has_longlink = 0; }
    char type = (char)h[156];
    uint64_t size = octal(h + 124, 12);
    clean_path(name, t->path, sizeof(t->path));
    snprintf(t->link, sizeof(t->link), "%s", link);
    t->remain = size;
    t->pad = (512 - size % 512) % 512;
    t->kind = type;
    if (type == '0' || type == 0 || type == '7' || type == 'L' || type == 'K' || type == 'x') {
        if (size > (256ull << 20)) { snprintf(t->err, sizeof(t->err), "a file in the package is too large"); return -1; }
        t->dsize = (size_t)size;
        t->dfill = 0;
        t->data = (uint8_t *)big_alloc(t->dsize + 1);
        if (!t->data) { snprintf(t->err, sizeof(t->err), "out of memory"); return -1; }
        if (!size) return tar_finish(t);
        return 0;
    }
    int rc = 0;
    if (type == '5') rc = t->fn(t->ctx, TE_DIR, t->path, "", NULL, 0);
    else if (type == '2') rc = t->fn(t->ctx, TE_SYMLINK, t->path, t->link, NULL, 0);
    else if (type == '1') { char lp[512]; clean_path(t->link, lp, sizeof(lp)); rc = t->fn(t->ctx, TE_HARDLINK, t->path, lp, NULL, 0); }
    t->kind = 0;                                            /* devices, fifos, pax globals: skipped */
    if (size) t->kind = 'g';
    return rc ? -1 : 0;
}

static int tar_feed(void *ctx, const uint8_t *p, size_t n)
{
    tar_t *t = (tar_t *)ctx;
    while (n && !t->done) {
        if (t->remain) {
            size_t k = n < t->remain ? n : (size_t)t->remain;
            if (t->data && t->dfill + k <= t->dsize) { memcpy(t->data + t->dfill, p, k); t->dfill += k; }
            p += k; n -= k; t->remain -= k;
            if (!t->remain && tar_finish(t)) return 1;
            continue;
        }
        if (t->pad) {
            size_t k = n < t->pad ? n : (size_t)t->pad;
            p += k; n -= k; t->pad -= k;
            continue;
        }
        size_t k = 512 - t->hfill;
        if (k > n) k = n;
        memcpy(t->hdr + t->hfill, p, k);
        t->hfill += k; p += k; n -= k;
        if (t->hfill == 512) {
            t->hfill = 0;
            if (tar_header(t)) return 1;
        }
    }
    return 0;
}

static void *xz_alloc(size_t n) { return big_alloc(n); }
static void xz_release(void *p, size_t n) { big_free(p, n); }

/* Decompresses a member (by its name: .xz, .gz or plain) into the tar parser. */
static int unpack_member(const member_t *m, tar_t *t, char *err, size_t en)
{
    const char *dot = strrchr(m->name, '.');
    int rc;
    if (dot && !strcmp(dot, ".xz")) rc = xz_decode(m->data, m->len, tar_feed, t, xz_alloc, xz_release, err, en);
    else if (dot && !strcmp(dot, ".gz")) {
        const uint8_t *g = m->data;
        if (m->len < 18 || g[0] != 0x1F || g[1] != 0x8B || g[2] != 8) { snprintf(err, en, "damaged gzip data"); return -1; }
        size_t p = 10;
        uint8_t fl = g[3];
        if (fl & 4) p += 2 + (size_t)(g[p] | g[p + 1] << 8);
        if (fl & 8) { while (p < m->len && g[p]) p++; p++; }
        if (fl & 16) { while (p < m->len && g[p]) p++; p++; }
        if (fl & 2) p += 2;
        size_t isize = (size_t)(g[m->len - 4] | g[m->len - 3] << 8 | g[m->len - 2] << 16 | (uint32_t)g[m->len - 1] << 24);
        uint8_t *out = (uint8_t *)big_alloc(isize + 1);
        if (!out) { snprintf(err, en, "out of memory"); return -1; }
        size_t got = 0;
        if (p >= m->len || inflate_raw(g + p, m->len - 8 - p, out, isize, &got) || got != isize) { big_free(out, isize + 1); snprintf(err, en, "damaged gzip data"); return -1; }
        if (xz_crc32(out, isize, 0) != (uint32_t)(g[m->len - 8] | g[m->len - 7] << 8 | g[m->len - 6] << 16 | (uint32_t)g[m->len - 5] << 24)) {
            big_free(out, isize + 1); snprintf(err, en, "gzip CRC mismatch: the package is damaged"); return -1;
        }
        rc = tar_feed(t, out, isize) ? -1 : 0;
        big_free(out, isize + 1);
    } else if (dot && !strcmp(dot, ".zst")) { snprintf(err, en, "zstd-compressed package (Ubuntu builds); use Debian packages"); return -1; }
    else if (!strcmp(m->name, "data.tar") || !strcmp(m->name, "control.tar")) rc = tar_feed(t, m->data, m->len) ? -1 : 0;
    else { snprintf(err, en, "unknown compression: %s", m->name); return -1; }
    if (t->err[0]) { snprintf(err, en, "%s", t->err); rc = -1; }
    if (t->data) { big_free(t->data, t->dsize + 1); t->data = NULL; }
    return rc;
}

/* ---- dpkg: control information --------------------------------------------------------------- */
typedef struct {
    char control[16384];
    size_t control_len;
    char conffiles[4096];
    char md5sums[65536];
    size_t md5_len;
    char scripts[96];               /* names of maintainer scripts present */
    char script_text[5][8192];
    size_t script_len[5];
} ctl_t;

static const char *const g_script_names[5] = { "preinst", "postinst", "prerm", "postrm", "config" };

static int ctl_entry(void *ctx, int type, const char *path, const char *link, const uint8_t *data, size_t size)
{
    ctl_t *c = (ctl_t *)ctx;
    (void)link;
    if (type != TE_FILE) return 0;
    const char *n = path + 1;
    if (!strcmp(n, "control") && size < sizeof(c->control)) { memcpy(c->control, data, size); c->control[size] = 0; c->control_len = size; }
    else if (!strcmp(n, "conffiles") && size < sizeof(c->conffiles)) { memcpy(c->conffiles, data, size); c->conffiles[size] = 0; }
    else if (!strcmp(n, "md5sums") && size < sizeof(c->md5sums)) { memcpy(c->md5sums, data, size); c->md5sums[size] = 0; c->md5_len = size; }
    else
        for (int i = 0; i < 5; i++)
            if (!strcmp(n, g_script_names[i])) {
                size_t l = size < sizeof(c->script_text[i]) ? size : sizeof(c->script_text[i]) - 1;
                memcpy(c->script_text[i], data, l);
                c->script_text[i][l] = 0;
                c->script_len[i] = l;
                if (strlen(c->scripts) + strlen(n) + 2 < sizeof(c->scripts)) {
                    if (c->scripts[0]) strcat(c->scripts, " ");
                    strcat(c->scripts, n);
                }
            }
    return 0;
}

static ctl_t g_ctl;

static int read_control(const uint8_t *deb, size_t len, ctl_t *c, char *err, size_t en)
{
    memset(c, 0, sizeof(*c));
    member_t m;
    if (len < 8 || memcmp(deb, "!<arch>\n", 8)) { snprintf(err, en, "not a Debian package (.deb)"); return -1; }
    if (ar_find(deb, len, "debian-binary", &m) || m.len < 3 || memcmp(m.data, "2.", 2)) { snprintf(err, en, "unsupported .deb format"); return -1; }
    if (ar_find(deb, len, "control.tar", &m)) { snprintf(err, en, "the package has no control information"); return -1; }
    static tar_t t;
    memset(&t, 0, sizeof(t));
    t.fn = ctl_entry;
    t.ctx = c;
    if (unpack_member(&m, &t, err, en)) return -1;
    if (!c->control_len) { snprintf(err, en, "the package has no control file"); return -1; }
    return 0;
}

/* ---- dpkg: unpacking ---------------------------------------------------------------------------- */
typedef struct {
    char *list;                     /* Linux paths, one per line */
    size_t len, cap;
    int files, links, skipped;
    uint64_t bytes;
    char err[160];
} unpack_t;

static void list_add(unpack_t *u, const char *p)
{
    size_t l = strlen(p);
    if (u->len + l + 2 > u->cap) {
        size_t nc = u->cap ? u->cap * 2 : 65536;
        while (nc < u->len + l + 2) nc *= 2;
        char *n = (char *)big_alloc(nc);
        if (!n) return;
        if (u->list) { memcpy(n, u->list, u->len); big_free(u->list, u->cap); }
        u->list = n;
        u->cap = nc;
    }
    memcpy(u->list + u->len, p, l);
    u->len += l;
    u->list[u->len++] = '\n';
    u->list[u->len] = 0;
}

static int data_entry(void *ctx, int type, const char *path, const char *link, const uint8_t *data, size_t size)
{
    unpack_t *u = (unpack_t *)ctx;
    if (!strcmp(path, "/")) return 0;
    char canon[256], jp[256];
    linux_canon(path, canon, 0);
    snprintf(jp, sizeof(jp), "%s%s", LXR, canon);
    if (strlen(jp) >= 250) { u->skipped++; return 0; }
    switch (type) {
    case TE_DIR:
        if (!vfs_is_dir(jp)) mkdirs(jp);
        list_add(u, canon);
        return 0;
    case TE_FILE:
        if (excluded(path)) { u->skipped++; return 0; }
        linux_link_remove(canon);
        {
            char parent[256];
            snprintf(parent, sizeof(parent), "%s", jp);
            char *s = strrchr(parent, '/');
            if (s && s != parent) { *s = 0; if (!vfs_is_dir(parent)) mkdirs(parent); }
        }
        if (vfs_is_dir(jp)) { snprintf(u->err, sizeof(u->err), "%s is a folder, the package has a file there", canon); return 1; }
        if (vfs_write_file(jp, data, size) != 0) { snprintf(u->err, sizeof(u->err), "could not write %s (disk full or too many files?)", canon); return 1; }
        u->files++;
        u->bytes += size;
        list_add(u, canon);
        return 0;
    case TE_SYMLINK:
        if (excluded(path)) { u->skipped++; return 0; }
        if (vfs_exists(jp) && !vfs_is_dir(jp)) vfs_delete(jp);
        if (linux_link_set(canon, link)) { snprintf(u->err, sizeof(u->err), "too many symbolic links"); return 1; }
        u->links++;
        list_add(u, canon);
        return 0;
    case TE_HARDLINK: {
        if (excluded(path)) { u->skipped++; return 0; }
        char src[256];
        jpath(link, src, sizeof(src), 1);
        size_t l = 0;
        char *b = read_all(src, &l);
        if (!b) { snprintf(u->err, sizeof(u->err), "hard link target %s is missing", link); return 1; }
        int rc = vfs_write_file(jp, b, l);
        big_free(b, l + 1);
        if (rc) { snprintf(u->err, sizeof(u->err), "could not write %s", canon); return 1; }
        u->files++;
        list_add(u, canon);
        return 0;
    }
    }
    return 0;
}

static void info_path(const char *pkg, const char *ext, char *out, size_t n) { snprintf(out, n, "%s/info/%s.%s", DPKG_DIR, pkg, ext); }

/* Removes the files of an old list that the new list no longer has (or
 * all of them when new_list is NULL). Folders go only when empty, and the
 * top two levels (/usr/bin, /usr/lib ...) always stay. */
static int list_has(const char *list, const char *p)
{
    size_t l = strlen(p);
    for (const char *q = list; (q = strstr(q, p)) != NULL; q++)
        if ((q == list || q[-1] == '\n') && (q[l] == '\n' || q[l] == 0)) return 1;
    return 0;
}

static void remove_files(const char *old_list, const char *new_list)
{
    size_t n = strlen(old_list);
    while (n) {                                             /* backwards: files before their folders */
        size_t e = n;
        while (e && old_list[e - 1] == '\n') e--;
        size_t s = e;
        while (s && old_list[s - 1] != '\n') s--;
        n = s;
        if (s == e || e - s >= 250) continue;
        char p[256];
        memcpy(p, old_list + s, e - s);
        p[e - s] = 0;
        if (new_list && list_has(new_list, p)) continue;
        if (linux_link_remove(p) == 0) continue;
        char jp[256];
        snprintf(jp, sizeof(jp), "%s%s", LXR, p);
        if (vfs_is_dir(jp)) {
            int depth = 0;
            for (const char *c = p; *c; c++) if (*c == '/') depth++;
            if (depth > 2 && vfs_count_children(jp) == 0) vfs_delete(jp);
        } else if (vfs_exists(jp)) vfs_delete(jp);
    }
}

static void deps_missing(const char *control, size_t len, char *out, size_t on)
{
    static char deps[8192];
    out[0] = 0;
    const char *keys[2] = { "Pre-Depends", "Depends" };
    for (int k = 0; k < 2; k++) {
        if (!field(control, len, keys[k], deps, sizeof(deps))) continue;
        const char *p = deps, *end = deps + strlen(deps);
        while (p < end) {
            int ok = 0;
            char first[96] = "";
            for (;;) {
                dep_t d;
                p = parse_dep(p, end, &d);
                if (!first[0]) snprintf(first, sizeof(first), "%s", d.name);
                inst_t *i = inst_find(d.name);
                if ((i && ver_ok(i->ver, d.op, d.ver)) || (!d.op[0] && inst_provides(d.name))) ok = 1;
                if (d.alt && p < end) continue;
                break;
            }
            if (!ok && first[0] && strlen(out) + strlen(first) + 3 < on) { if (out[0]) strcat(out, ", "); strcat(out, first); }
        }
    }
}

/* Installs one .deb held in memory. quiet: apt prints its own lines. */
static int dpkg_install_buf(const uint8_t *deb, size_t len, int check_deps, char *pkg_out, size_t pn)
{
    char err[160];
    if (read_control(deb, len, &g_ctl, err, sizeof(err))) { say("dpkg: error: %s\n", err); return -1; }
    char pkg[96], ver[64], arch[32];
    field(g_ctl.control, g_ctl.control_len, "Package", pkg, sizeof(pkg));
    field(g_ctl.control, g_ctl.control_len, "Version", ver, sizeof(ver));
    field(g_ctl.control, g_ctl.control_len, "Architecture", arch, sizeof(arch));
    if (pkg_out) snprintf(pkg_out, pn, "%s", pkg);
    if (!pkg[0]) { say("dpkg: error: the package has no name\n"); return -1; }
    if (strcmp(arch, "amd64") && strcmp(arch, "all")) { say("dpkg: error: %s is built for %s, not amd64 (x86-64)\n", pkg, arch); return -1; }
    status_load();
    inst_t *old = inst_find(pkg);
    char oldver[64] = "";
    if (old) snprintf(oldver, sizeof(oldver), "%s", old->ver);
    if (check_deps) {
        char miss[512];
        deps_missing(g_ctl.control, g_ctl.control_len, miss, sizeof(miss));
        if (miss[0]) say("dpkg: warning: %s depends on %s, which %s not installed (apt install fetches dependencies)\n", pkg, miss, strchr(miss, ',') ? "are" : "is");
    }
    if (old) say("Preparing to unpack %s (%s) over (%s) ...\n", pkg, ver, oldver);
    else say("Selecting previously unselected package %s.\n", pkg);
    say("Unpacking %s (%s) ...\n", pkg, ver);

    member_t m;
    if (ar_find(deb, len, "data.tar", &m)) { say("dpkg: error: %s has no data.tar\n", pkg); return -1; }
    g_excl = read_all(EXCLUDES, &g_excl_len);
    static unpack_t u;
    memset(&u, 0, sizeof(u));
    static tar_t t;
    memset(&t, 0, sizeof(t));
    t.fn = data_entry;
    t.ctx = &u;
    int rc = unpack_member(&m, &t, err, sizeof(err));
    if (g_excl) { big_free(g_excl, g_excl_len + 1); g_excl = NULL; }
    if (rc) {
        say("dpkg: error processing %s: %s\n", pkg, u.err[0] ? u.err : err);
        linux_links_save();
        if (u.list) big_free(u.list, u.cap);
        return -1;
    }
    /* an upgrade: files the new version no longer has */
    char lp[256];
    info_path(pkg, "list", lp, sizeof(lp));
    if (old) {
        size_t ol;
        char *oldlist = read_all(lp, &ol);
        if (oldlist) { remove_files(oldlist, u.list ? u.list : ""); big_free(oldlist, ol + 1); }
    }
    linux_links_save();
    vfs_write_file(lp, u.list ? u.list : "", u.len);
    if (u.list) big_free(u.list, u.cap);
    if (g_ctl.md5_len) { info_path(pkg, "md5sums", lp, sizeof(lp)); vfs_write_file(lp, g_ctl.md5sums, g_ctl.md5_len); }
    if (g_ctl.conffiles[0]) { info_path(pkg, "conffiles", lp, sizeof(lp)); vfs_write_file(lp, g_ctl.conffiles, strlen(g_ctl.conffiles)); }
    for (int i = 0; i < 5; i++)
        if (g_ctl.script_len[i]) { info_path(pkg, g_script_names[i], lp, sizeof(lp)); vfs_write_file(lp, g_ctl.script_text[i], g_ctl.script_len[i]); }

    /* registration: the control stanza with a Status line after Package */
    static char st[17000];
    size_t o = 0;
    const char *c = g_ctl.control, *end = c + g_ctl.control_len;
    while (end > c && (end[-1] == '\n')) end--;
    const char *eol = memchr(c, '\n', (size_t)(end - c));
    if (!eol) eol = end;
    o += (size_t)snprintf(st + o, sizeof(st) - o, "%.*s\nStatus: install ok installed\n", (int)(eol - c), c);
    if (eol < end) {
        size_t rest = (size_t)(end - eol - 1);
        if (o + rest + 2 < sizeof(st)) { memcpy(st + o, eol + 1, rest); o += rest; st[o++] = '\n'; }
    }
    status_write(pkg, st, o);
    say("Setting up %s (%s) ...\n", pkg, ver);
    if (u.skipped) say("  (%d file(s) not unpacked: manuals/translations/docs, see /etc/dpkg/dpkg.cfg.d/excludes)\n", u.skipped);
    if (g_ctl.scripts[0] && strstr(g_ctl.scripts, "postinst"))
        say("  note: %s's configuration script (postinst) was saved but not run: the Linux layer cannot start shell scripts yet\n", pkg);
    g_storage_generation++;
    return 0;
}

static int dpkg_remove(const char *pkg, int purge, int force)
{
    status_load();
    inst_t *e = inst_find(pkg);
    if (!e) { say("dpkg: warning: ignoring request to remove %s which isn't installed\n", pkg); return -1; }
    if (!force) {
        /* refuse if something installed depends on it */
        static char deps[8192];
        for (int i = 0; i < g_ninst; i++) {
            if (!strcmp(g_inst[i].name, pkg)) continue;
            const char *keys[2] = { "Pre-Depends", "Depends" };
            for (int k = 0; k < 2; k++) {
                if (!field(g_inst[i].stanza, g_inst[i].len, keys[k], deps, sizeof(deps))) continue;
                const char *p = deps, *end = deps + strlen(deps);
                while (p < end) {
                    dep_t d;
                    int n = 0, hit = 0;
                    do { p = parse_dep(p, end, &d); n++; if (!strcmp(d.name, pkg)) hit = 1; } while (d.alt && p < end);
                    if (hit && n == 1) {
                        say("dpkg: dependency problems prevent removal of %s:\n %s depends on %s.\n", pkg, g_inst[i].name, pkg);
                        return -1;
                    }
                }
            }
        }
    }
    char ver[64];
    snprintf(ver, sizeof(ver), "%s", e->ver);
    say("Removing %s (%s) ...\n", pkg, ver);
    char lp[256];
    info_path(pkg, "list", lp, sizeof(lp));
    size_t ll;
    char *list = read_all(lp, &ll);
    if (list) { remove_files(list, NULL); big_free(list, ll + 1); }
    linux_links_save();
    const char *exts[] = { "list", "md5sums", "conffiles", "preinst", "postinst", "prerm", "postrm", "config" };
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        info_path(pkg, exts[i], lp, sizeof(lp));
        if (vfs_exists(lp)) vfs_delete(lp);
    }
    status_write(pkg, NULL, 0);
    if (purge) say("Purging configuration files for %s (%s) ...\n", pkg, ver);
    g_storage_generation++;
    return 0;
}

/* ---- dpkg: queries --------------------------------------------------------------------------- */
static int list_contents(void *ctx, int type, const char *path, const char *link, const uint8_t *data, size_t size)
{
    (void)ctx; (void)data;
    char line[600];
    if (type == TE_DIR) snprintf(line, sizeof(line), "drwxr-xr-x  %10u  .%s/\n", 0u, strcmp(path, "/") ? path : "");
    else if (type == TE_SYMLINK) snprintf(line, sizeof(line), "lrwxrwxrwx  %10u  .%s -> %s\n", 0u, path, link);
    else if (type == TE_HARDLINK) snprintf(line, sizeof(line), "hrw-r--r--  %10u  .%s link to .%s\n", 0u, path, link);
    else snprintf(line, sizeof(line), "-rw-r--r--  %10u  .%s\n", (unsigned)size, path);
    user_console_write(line, strlen(line));
    return 0;
}

static uint8_t *load_deb(const char *path, size_t *len)
{
    long size = vfs_size(path);
    if (size <= 0) return NULL;
    uint8_t *b = (uint8_t *)big_alloc((size_t)size + 1);
    if (!b) return NULL;
    if (vfs_read_file(path, (char *)b, (size_t)size + 1) != size) { big_free(b, (size_t)size + 1); return NULL; }
    *len = (size_t)size;
    return b;
}

static int cmd_dpkg(int argc, char **argv, const char *cwd)
{
    if (argc < 2) { say("usage: dpkg -i <file.deb> | -r <pkg> | -P <pkg> | -l [pattern] | -L <pkg> | -s <pkg> | -c <file.deb> | -I <file.deb>\n"); return 1; }
    const char *op = argv[1];
    int force = 0;
    for (int i = 1; i < argc; i++) if (!strncmp(argv[i], "--force", 7)) force = 1;
    if (!strcmp(op, "-l") || !strcmp(op, "--list")) {
        status_load();
        say("Desired=Unknown/Install/Remove/Purge/Hold\n| Status=Not/Inst/Conf-files/Unpacked/halF-conf/Half-inst/trig-aWait/Trig-pend\n");
        say("||/ %-30s %-24s %-6s %s\n", "Name", "Version", "Arch", "Description");
        say("+++-%s\n", "==============================-========================-======-==========================");
        int shown = 0;
        for (int i = 0; i < g_ninst; i++) {
            if (argc > 2 && !glob(argv[2], g_inst[i].name)) continue;
            char arch[16], desc[80];
            field(g_inst[i].stanza, g_inst[i].len, "Architecture", arch, sizeof(arch));
            field(g_inst[i].stanza, g_inst[i].len, "Description", desc, sizeof(desc));
            say("ii  %-30s %-24s %-6s %s\n", g_inst[i].name, g_inst[i].ver, arch, desc);
            shown++;
        }
        if (!shown) say(argc > 2 ? "dpkg-query: no packages found matching %s\n" : "(no packages installed: try apt update, then apt install <name>)\n", argc > 2 ? argv[2] : "");
        return 0;
    }
    if (argc < 3) { say("dpkg: error: %s needs an argument\n", op); return 1; }
    int rc = 0;
    for (int a = 2; a < argc; a++) {
        const char *arg = argv[a];
        if (!strncmp(arg, "--", 2)) continue;
        if (!strcmp(op, "-L") || !strcmp(op, "--listfiles")) {
            char lp[256];
            info_path(arg, "list", lp, sizeof(lp));
            size_t l;
            char *list = read_all(lp, &l);
            if (!list) { say("dpkg-query: package '%s' is not installed\n", arg); rc = 1; continue; }
            user_console_write(list, l);
            big_free(list, l + 1);
        } else if (!strcmp(op, "-s") || !strcmp(op, "--status")) {
            status_load();
            inst_t *e = inst_find(arg);
            if (!e) { say("dpkg-query: package '%s' is not installed and no information is available\n", arg); rc = 1; continue; }
            user_console_write(e->stanza, e->len);
            say("\n");
        } else if (!strcmp(op, "-r") || !strcmp(op, "--remove") || !strcmp(op, "-P") || !strcmp(op, "--purge")) {
            if (dpkg_remove(arg, op[1] == 'P' || !strcmp(op, "--purge"), force)) rc = 1;
        } else if (!strcmp(op, "-i") || !strcmp(op, "--install") || !strcmp(op, "-c") || !strcmp(op, "--contents") || !strcmp(op, "-I") || !strcmp(op, "--info")) {
            char p[256];
            if (arg[0] == '/') snprintf(p, sizeof(p), "%s", arg);
            else snprintf(p, sizeof(p), "%s/%s", cwd, arg);
            size_t len;
            uint8_t *deb = load_deb(p, &len);
            if (!deb) { say("dpkg: error: cannot access archive '%s': No such file\n", arg); rc = 1; continue; }
            char err[160];
            if (op[1] == 'i' || !strcmp(op, "--install")) {
                if (dpkg_install_buf(deb, len, 1, NULL, 0)) rc = 1;
            } else if (op[1] == 'I' || !strcmp(op, "--info")) {
                if (read_control(deb, len, &g_ctl, err, sizeof(err))) { say("dpkg-deb: error: %s\n", err); rc = 1; }
                else {
                    say(" new Debian package, version 2.0.\n size %u bytes.\n", (unsigned)len);
                    if (g_ctl.scripts[0]) say(" maintainer scripts: %s\n", g_ctl.scripts);
                    user_console_write(g_ctl.control, g_ctl.control_len);
                }
            } else {
                member_t m;
                static tar_t t;
                memset(&t, 0, sizeof(t));
                t.fn = list_contents;
                if (ar_find(deb, len, "data.tar", &m) || unpack_member(&m, &t, err, sizeof(err))) { say("dpkg-deb: error: %s\n", err[0] ? err : "no data.tar"); rc = 1; }
            }
            big_free(deb, len + 1);
        } else { say("dpkg: error: unknown option %s\n", op); return 1; }
    }
    return rc;
}

/* ---- apt: sources and the package index ------------------------------------------------------ */
typedef struct { char uri[200], suite[48], comp[48]; } src_t;

static int sources(src_t *out, int max)
{
    size_t l;
    char *s = read_all(SOURCES, &l);
    if (!s) return 0;
    int n = 0;
    for (char *p = s; *p && n < max;) {
        char *eol = strchr(p, '\n');
        if (eol) *eol = 0;
        char *w[16];
        int nw = 0;
        for (char *q = p; *q && nw < 16;) {
            while (*q == ' ' || *q == '\t') q++;
            if (!*q || *q == '#') break;
            w[nw++] = q;
            if (*q == '[') { while (*q && *q != ']') q++; if (*q) q++; nw--; }   /* [options] */
            else while (*q && *q != ' ' && *q != '\t') q++;
            if (*q) *q++ = 0;
        }
        if (nw >= 4 && !strcmp(w[0], "deb"))
            for (int c = 3; c < nw && n < max; c++) {
                snprintf(out[n].uri, sizeof(out[n].uri), "%s", w[1]);
                size_t ul = strlen(out[n].uri);
                if (ul && out[n].uri[ul - 1] == '/') out[n].uri[ul - 1] = 0;
                snprintf(out[n].suite, sizeof(out[n].suite), "%s", w[2]);
                snprintf(out[n].comp, sizeof(out[n].comp), "%s", w[c]);
                n++;
            }
        if (!eol) break;
        p = eol + 1;
    }
    big_free(s, l + 1);
    return n;
}

static void list_name(const src_t *s, char *out, size_t n)
{
    char u[200];
    snprintf(u, sizeof(u), "%s", s->uri);
    for (char *c = u; *c; c++) if (*c == '/' || *c == ':') *c = '_';
    snprintf(out, n, "%s/%s_%s_%s_amd64.idx", APT_LISTS, u, s->suite, s->comp);
}

/* Downloads a URL into memory, printing progress. 0 = ok. */
static http_req_t g_req;
static int fetch(const char *url, int show_progress)
{
    memset(&g_req, 0, sizeof(g_req));
    snprintf(g_req.url, sizeof(g_req.url), "%s", url);
    if (http_start(&g_req) != 0) { say("E: could not start the download of %s\n", url); return -1; }
    int last = -1;
    while (g_req.state != HTTP_DONE && g_req.state != HTTP_FAILED) {
        kthread_sleep(10);
        if (show_progress && g_req.expected > 2000000) {
            int pct = (int)(g_req.len * 100 / g_req.expected);
            if (pct / 20 != last / 20) { say("  %d%% [%u of %u kB]\n", pct, (unsigned)(g_req.len / 1024), (unsigned)(g_req.expected / 1024)); last = pct; }
        }
    }
    if (g_req.state == HTTP_FAILED) { say("E: Failed to fetch %s  %s\n", url, g_req.err); http_free(&g_req); return -1; }
    if (g_req.status != 200) { say("E: Failed to fetch %s  %d %s\n", url, g_req.status, g_req.status == 404 ? "Not Found" : "error"); http_free(&g_req); return -1; }
    return 0;
}

/* Packages -> one line per package:
 * name \t version \t arch \t pre-depends \t depends \t provides \t filename \t size \t sha256 \t installed-size \t description \t section */
enum { F_NAME, F_VER, F_ARCH, F_PREDEP, F_DEP, F_PROV, F_FILE, F_SIZE, F_SHA, F_ISIZE, F_DESC, F_SECT, F_COUNT };
static const char *const g_idx_keys[F_COUNT] = { "Package", "Version", "Architecture", "Pre-Depends", "Depends", "Provides", "Filename",
                                                 "Size", "SHA256", "Installed-Size", "Description", "Section" };

typedef struct {
    char *out;
    size_t len, cap;
    char line[32768];
    size_t ll;
    char fld[F_COUNT][1024];
    int have, count, fail;
    uint64_t in_bytes;
} idxb_t;

static void idx_emit(idxb_t *b)
{
    if (!b->have || !b->fld[F_NAME][0]) { b->have = 0; return; }
    size_t need = 0;
    for (int i = 0; i < F_COUNT; i++) need += strlen(b->fld[i]) + 1;
    if (b->len + need + 1 > b->cap) {
        size_t nc = b->cap * 2;
        while (nc < b->len + need + 1) nc *= 2;
        char *n = (char *)big_alloc(nc);
        if (!n) { b->fail = 1; return; }
        memcpy(n, b->out, b->len);
        big_free(b->out, b->cap);
        b->out = n;
        b->cap = nc;
    }
    for (int i = 0; i < F_COUNT; i++) {
        size_t l = strlen(b->fld[i]);
        for (size_t k = 0; k < l; k++) if (b->fld[i][k] == '\t') b->fld[i][k] = ' ';
        memcpy(b->out + b->len, b->fld[i], l);
        b->len += l;
        b->out[b->len++] = i == F_COUNT - 1 ? '\n' : '\t';
        b->fld[i][0] = 0;
    }
    b->count++;
    b->have = 0;
}

static void idx_line(idxb_t *b)
{
    b->line[b->ll] = 0;
    if (!b->ll) { idx_emit(b); return; }
    if (b->line[0] == ' ' || b->line[0] == '\t') return;   /* continuation (long description) */
    char *colon = strchr(b->line, ':');
    if (!colon) return;
    *colon = 0;
    for (int i = 0; i < F_COUNT; i++)
        if (!strcmp(b->line, g_idx_keys[i])) {
            const char *v = colon + 1;
            while (*v == ' ') v++;
            snprintf(b->fld[i], sizeof(b->fld[i]), "%s", v);
            if (i == F_FILE && !strncmp(b->fld[i], "pool/", 5)) memmove(b->fld[i], b->fld[i] + 5, strlen(b->fld[i]) - 4);
            if (i == F_DESC && strlen(b->fld[i]) > 90) b->fld[i][90] = 0;
            b->have = 1;
            break;
        }
}

static int idx_feed(void *ctx, const uint8_t *p, size_t n)
{
    idxb_t *b = (idxb_t *)ctx;
    b->in_bytes += n;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '\n') { idx_line(b); b->ll = 0; }
        else if (b->ll + 1 < sizeof(b->line)) b->line[b->ll++] = (char)p[i];
    }
    return b->fail;
}

static int apt_update(void)
{
    static src_t src[16];
    int ns = sources(src, 16);
    if (!ns) { say("E: no sources in /etc/apt/sources.list (add a line like: deb https://deb.debian.org/debian bookworm main)\n"); return 1; }
    int total = 0, failed = 0;
    uint64_t fetched = 0;
    for (int i = 0; i < ns; i++) {
        char url[400], ln[300];
        list_name(&src[i], ln, sizeof(ln));
        /* InRelease lists the SHA-256 of every index: an unchanged index is
         * not downloaded again, and a changed one is checked against it */
        char want_sha[65] = "", rel[400], key[160];
        uint64_t want_size = 0;
        snprintf(rel, sizeof(rel), "%s/dists/%s/InRelease", src[i].uri, src[i].suite);
        say("Get:%d %s %s InRelease\n", i + 1, src[i].uri, src[i].suite);
        if (fetch(rel, 0)) { failed++; continue; }
        fetched += g_req.len;
        snprintf(key, sizeof(key), " %s/binary-amd64/Packages.xz", src[i].comp);
        const char *sha = strstr((const char *)g_req.data, "\nSHA256:");
        for (const char *p = sha; p && *p;) {
            const char *eol = strchr(p + 1, '\n');
            if (!eol) break;
            const char *line = p + 1;
            if (line[0] != ' ' && line != sha + 1) break;           /* end of the SHA256 section */
            size_t kl = strlen(key);
            if ((size_t)(eol - line) > kl && !strncmp(eol - kl, key, kl) && eol - line > 66) {
                memcpy(want_sha, line + 1, 64);
                want_sha[64] = 0;
                want_size = dec(line + 66);
                break;
            }
            p = eol;
        }
        http_free(&g_req);
        if (!want_sha[0]) { say("E: %s/dists/%s has no %s/binary-amd64/Packages.xz (check /etc/apt/sources.list)\n", src[i].uri, src[i].suite, src[i].comp); failed++; continue; }
        {   /* the saved index records the hash it was built from */
            char head[512];
            int hn = vfs_read_file(ln, head, sizeof(head));
            if (hn > 0) {
                char *e = strchr(head, '\n');
                if (e) *e = 0;
                const char *h = strstr(head, want_sha);
                if (h && !strncmp(head, "#source\t", 8)) {
                    const char *cnt = strrchr(head, '\t');
                    say("Hit:%d %s %s/%s amd64 Packages (unchanged)\n", i + 1, src[i].uri, src[i].suite, src[i].comp);
                    total += (int)dec(cnt + 1);
                    continue;
                }
            }
        }
        snprintf(url, sizeof(url), "%s/dists/%s/%s/binary-amd64/Packages.xz", src[i].uri, src[i].suite, src[i].comp);
        char sz[24];
        human(want_size, sz, sizeof(sz));
        say("Get:%d %s %s/%s amd64 Packages [%s]\n", i + 1, src[i].uri, src[i].suite, src[i].comp, sz);
        if (fetch(url, 1)) { failed++; continue; }
        fetched += g_req.len;
        {
            uint8_t d[32];
            char hex[65];
            hash_once(HASH_SHA256, g_req.data, g_req.len, d);
            for (int k = 0; k < 32; k++) snprintf(hex + k * 2, 3, "%02x", d[k]);
            if (strcmp(hex, want_sha) || g_req.len != want_size) {
                say("E: Failed to fetch %s  Hash Sum mismatch\n", url);
                http_free(&g_req);
                failed++;
                continue;
            }
        }
        say("Reading package lists (decompressing %u kB) ...\n", (unsigned)(g_req.len / 1024));
        static idxb_t b;
        memset(&b, 0, sizeof(b));
        b.cap = 4u << 20;
        b.out = (char *)big_alloc(b.cap);
        if (!b.out) { say("E: out of memory\n"); http_free(&g_req); return 1; }
        /* header: #source uri suite comp sha256 count (count filled in below) */
        b.len = (size_t)snprintf(b.out, b.cap, "#source\t%s\t%s\t%s\t%s\t%10u\n", src[i].uri, src[i].suite, src[i].comp, want_sha, 0u);
        char err[128];
        int rc = xz_decode(g_req.data, g_req.len, idx_feed, &b, xz_alloc, xz_release, err, sizeof(err));
        http_free(&g_req);
        if (!rc) { b.ll = 0; idx_emit(&b); }
        if (rc || b.fail) { say("E: the package list from %s is damaged: %s\n", src[i].uri, b.fail ? "out of memory" : err); big_free(b.out, b.cap); failed++; continue; }
        {
            char cnt[16];
            snprintf(cnt, sizeof(cnt), "%10u", (unsigned)b.count);
            char *nl = memchr(b.out, '\n', b.len);
            if (nl) memcpy(nl - 10, cnt, 10);
        }
        if (vfs_write_file(ln, b.out, b.len) != 0) { say("E: could not save the package list (disk full?)\n"); big_free(b.out, b.cap); failed++; continue; }
        big_free(b.out, b.cap);
        total += b.count;
    }
    char h[32];
    human(fetched, h, sizeof(h));
    say("Fetched %s\n", h);
    if (total) say("Reading package lists... Done\n%d packages available.\n", total);
    return failed ? 1 : 0;
}

/* The loaded index: all .idx files, with a hash from name to record. */
static char *g_idx;
static size_t g_idx_len;
static uint32_t *g_idx_hash;          /* offset + 1 */
#define IDX_HASH 262144
typedef struct { const char *f[F_COUNT]; size_t l[F_COUNT]; char uri[200]; } rec_t;
static struct { size_t off; char uri[200]; } g_src[16];
static int g_nsrc;

static uint32_t name_hash(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h & (IDX_HASH - 1);
}

static int idx_load(void)
{
    if (g_idx) return 0;
    static char listing[8192];
    vfs_list(APT_LISTS, listing, sizeof(listing));
    size_t total = 0;
    for (char *p = listing; *p;) {
        char *eol = strchr(p, '\n');
        if (!eol) eol = p + strlen(p);
        char *full = strchr(p, '/');
        if (full && full < eol && eol - full > 4 && !strncmp(eol - 4, ".idx", 4)) {
            char fp[300];
            snprintf(fp, sizeof(fp), "%.*s", (int)(eol - full), full);
            long s = vfs_size(fp);
            if (s > 0) total += (size_t)s + 1;
        }
        p = *eol ? eol + 1 : eol;
    }
    if (!total) return -1;
    g_idx = (char *)big_alloc(total + 1);
    g_idx_hash = (uint32_t *)big_alloc(IDX_HASH * sizeof(uint32_t));
    if (!g_idx || !g_idx_hash) return -1;
    memset(g_idx_hash, 0, IDX_HASH * sizeof(uint32_t));
    g_idx_len = 0;
    for (char *p = listing; *p;) {
        char *eol = strchr(p, '\n');
        if (!eol) eol = p + strlen(p);
        char *full = strchr(p, '/');
        if (full && full < eol && eol - full > 4 && !strncmp(eol - 4, ".idx", 4)) {
            char fp[300];
            snprintf(fp, sizeof(fp), "%.*s", (int)(eol - full), full);
            int n = vfs_read_file(fp, g_idx + g_idx_len, total + 1 - g_idx_len);
            if (n > 0) { g_idx_len += (size_t)n; if (g_idx[g_idx_len - 1] != '\n') g_idx[g_idx_len++] = '\n'; }
        }
        p = *eol ? eol + 1 : eol;
    }
    g_idx[g_idx_len] = 0;
    /* hash names (the newest version wins); note where each source starts */
    g_nsrc = 0;
    for (size_t o = 0; o < g_idx_len;) {
        const char *line = g_idx + o;
        const char *tab = strchr(line, '\t'), *eol = strchr(line, '\n');
        if (!eol) break;
        if (!strncmp(line, "#source\t", 8) && g_nsrc < 16) {
            const char *u = line + 8, *t = strchr(u, '\t');
            g_src[g_nsrc].off = o;
            snprintf(g_src[g_nsrc].uri, sizeof(g_src[0].uri), "%.*s", (int)((t && t < eol ? t : eol) - u), u);
            g_nsrc++;
        }
        if (line[0] != '#' && tab && tab < eol) {
            uint32_t h = name_hash(line, (size_t)(tab - line));
            for (uint32_t k = h;; k = (k + 1) & (IDX_HASH - 1)) {
                if (!g_idx_hash[k]) { g_idx_hash[k] = (uint32_t)o + 1; break; }
                const char *other = g_idx + g_idx_hash[k] - 1;
                if (!strncmp(other, line, (size_t)(tab - line) + 1)) {
                    /* same name twice: keep the newer version */
                    const char *ov = strchr(other, '\t') + 1, *nv = tab + 1;
                    char a[64], b[64];
                    snprintf(a, sizeof(a), "%.*s", (int)(strchr(ov, '\t') - ov), ov);
                    snprintf(b, sizeof(b), "%.*s", (int)(strchr(nv, '\t') - nv), nv);
                    if (dpkg_vercmp(b, a) > 0) g_idx_hash[k] = (uint32_t)o + 1;
                    break;
                }
            }
        }
        o = (size_t)(eol - g_idx) + 1;
    }
    return 0;
}

static void rec_split(size_t off, rec_t *r)
{
    const char *p = g_idx + off;
    for (int i = 0; i < F_COUNT; i++) {
        const char *e = p;
        while (*e && *e != '\t' && *e != '\n') e++;
        r->f[i] = p;
        r->l[i] = (size_t)(e - p);
        p = *e == '\t' ? e + 1 : e;
    }
    /* the source this record came from */
    r->uri[0] = 0;
    for (int i = 0; i < g_nsrc; i++) if (g_src[i].off <= off) snprintf(r->uri, sizeof(r->uri), "%s", g_src[i].uri);
}

static int rec_find(const char *name, rec_t *r)
{
    if (!g_idx) return 0;
    size_t nl = strlen(name);
    for (uint32_t k = name_hash(name, nl);; k = (k + 1) & (IDX_HASH - 1)) {
        if (!g_idx_hash[k]) return 0;
        const char *line = g_idx + g_idx_hash[k] - 1;
        if (!strncmp(line, name, nl) && line[nl] == '\t') { rec_split(g_idx_hash[k] - 1, r); return 1; }
    }
}

static void fcopy(const rec_t *r, int f, char *out, size_t n) { snprintf(out, n, "%.*s", (int)r->l[f], r->f[f]); }

/* First package that provides `name`. */
static int rec_provider(const char *name, rec_t *r)
{
    size_t nl = strlen(name);
    for (size_t o = 0; o < g_idx_len;) {
        const char *eol = strchr(g_idx + o, '\n');
        if (!eol) break;
        if (g_idx[o] != '#') {
            rec_t t;
            rec_split(o, &t);
            for (size_t i = 0; i + nl <= t.l[F_PROV]; i++)
                if (!strncmp(t.f[F_PROV] + i, name, nl) && (i == 0 || t.f[F_PROV][i - 1] == ' ') &&
                    (i + nl == t.l[F_PROV] || t.f[F_PROV][i + nl] == ',' || t.f[F_PROV][i + nl] == ' ')) { *r = t; return 1; }
        }
        o = (size_t)(eol - g_idx) + 1;
    }
    return 0;
}

/* ---- apt: resolving and installing --------------------------------------------------------------- */
#define PLAN_MAX 512
typedef struct { rec_t r; char name[96]; int requested; } plan_t;
static plan_t g_plan[PLAN_MAX];
static int g_nplan;
static char g_visiting[PLAN_MAX][96];
static int g_nvisit;

static int planned(const char *name)
{
    for (int i = 0; i < g_nplan; i++) if (!strcmp(g_plan[i].name, name)) return 1;
    for (int i = 0; i < g_nvisit; i++) if (!strcmp(g_visiting[i], name)) return 1;
    return 0;
}

static int plan_provides(const char *name)
{
    for (int i = 0; i < g_nplan; i++) {
        const rec_t *r = &g_plan[i].r;
        size_t nl = strlen(name);
        for (size_t k = 0; k + nl <= r->l[F_PROV]; k++)
            if (!strncmp(r->f[F_PROV] + k, name, nl) && (k == 0 || r->f[F_PROV][k - 1] == ' ') &&
                (k + nl == r->l[F_PROV] || r->f[F_PROV][k + nl] == ',' || r->f[F_PROV][k + nl] == ' ')) return 1;
    }
    return 0;
}

static int resolve(const char *name, const dep_t *want, int requested, int upgrade, int depth, char *why, size_t wn)
{
    if (depth > 40) { snprintf(why, wn, "dependencies nest too deeply at %s", name); return -1; }
    inst_t *in = inst_find(name);
    if (in && !requested && (!want || ver_ok(in->ver, want->op, want->ver))) return 0;
    if (planned(name)) return 0;
    rec_t r;
    if (!rec_find(name, &r)) {
        if (!want || !want->op[0]) {
            if (inst_provides(name) || plan_provides(name)) return 0;
            if (rec_provider(name, &r)) {
                char pn[96];
                fcopy(&r, F_NAME, pn, sizeof(pn));
                return resolve(pn, NULL, requested, upgrade, depth + 1, why, wn);
            }
        }
        snprintf(why, wn, requested ? "Unable to locate package %s" : "%s is not available (needed by another package)", name);
        return -1;
    }
    char ver[64];
    fcopy(&r, F_VER, ver, sizeof(ver));
    if (in && requested && !upgrade && !dpkg_vercmp(in->ver, ver)) return 0;          /* already newest */
    if (in && dpkg_vercmp(in->ver, ver) >= 0 && (!want || ver_ok(in->ver, want->op, want->ver))) return 0;
    if (want && !ver_ok(ver, want->op, want->ver)) {
        snprintf(why, wn, "%s %s is available, but %s %s is needed", name, ver, want->op, want->ver);
        return -1;
    }
    if (g_nvisit >= PLAN_MAX || g_nplan >= PLAN_MAX) { snprintf(why, wn, "too many packages"); return -1; }
    snprintf(g_visiting[g_nvisit++], 96, "%s", name);
    static char deps[16384];
    for (int f = F_PREDEP; f <= F_DEP; f++) {
        snprintf(deps, sizeof(deps), "%.*s", (int)r.l[f], r.f[f]);
        const char *p = deps, *end = deps + strlen(deps);
        while (p < end) {
            dep_t alts[8];
            int na = 0;
            for (;;) {
                dep_t d;
                p = parse_dep(p, end, &d);
                if (na < 8) alts[na++] = d;
                if (d.alt && p < end) continue;
                break;
            }
            int ok = 0;
            for (int a = 0; a < na && !ok; a++) {
                inst_t *i = inst_find(alts[a].name);
                if ((i && ver_ok(i->ver, alts[a].op, alts[a].ver)) || planned(alts[a].name) ||
                    (!alts[a].op[0] && (inst_provides(alts[a].name) || plan_provides(alts[a].name)))) ok = 1;
            }
            for (int a = 0; a < na && !ok; a++) {
                if (!alts[a].name[0]) { ok = 1; break; }
                char sub[160];
                if (resolve(alts[a].name, &alts[a], 0, 0, depth + 1, sub, sizeof(sub)) == 0) ok = 1;
                else if (a == na - 1) { snprintf(why, wn, "%s depends on %s: %s", name, alts[0].name, sub); g_nvisit--; return -1; }
            }
        }
    }
    g_nvisit--;
    plan_t *pl = &g_plan[g_nplan++];
    pl->r = r;
    snprintf(pl->name, sizeof(pl->name), "%s", name);
    pl->requested = requested;
    return 0;
}

static int ask_yes(int assume_yes)
{
    if (assume_yes) return 1;
    say("Do you want to continue? [Y/n] ");
    char line[16];
    int n = user_stdin_read(line, sizeof(line) - 1);
    if (n <= 0) { say("\nAbort (no answer; use -y to answer yes automatically).\n"); return 0; }
    line[n] = 0;
    return line[0] == '\n' || line[0] == 'y' || line[0] == 'Y';
}

static int apt_install(int nnames, char **names, int assume_yes, int upgrade_all)
{
    if (idx_load()) { say("E: no package lists yet. Run: apt update\n"); return 1; }
    say("Reading package lists... Done\nBuilding dependency tree... Done\n");
    status_load();
    g_nplan = 0;
    g_nvisit = 0;
    char why[256];
    int bad = 0;
    if (upgrade_all) {
        for (int i = 0; i < g_ninst; i++) {
            rec_t r;
            char v[64];
            if (!rec_find(g_inst[i].name, &r)) continue;
            fcopy(&r, F_VER, v, sizeof(v));
            if (dpkg_vercmp(v, g_inst[i].ver) > 0 && resolve(g_inst[i].name, NULL, 1, 1, 0, why, sizeof(why))) { say("E: %s\n", why); bad = 1; }
        }
    }
    for (int i = 0; i < nnames; i++) {
        char nm[96];
        snprintf(nm, sizeof(nm), "%s", names[i]);
        char *colon = strchr(nm, ':');
        if (colon) *colon = 0;                              /* name:amd64 */
        if (resolve(nm, NULL, 1, 0, 0, why, sizeof(why))) { say("E: %s\n", why); bad = 1; continue; }
        inst_t *in = inst_find(nm);
        if (in && !planned(nm)) say("%s is already the newest version (%s).\n", nm, in->ver);
    }
    if (bad) return 1;
    if (!g_nplan) { say("0 upgraded, 0 newly installed, 0 to remove.\n"); return 0; }
    uint64_t dl = 0, inst_size = 0;
    int extra = 0, upgrades = 0;
    for (int i = 0; i < g_nplan; i++) {
        char s[24], is[24];
        fcopy(&g_plan[i].r, F_SIZE, s, sizeof(s));
        fcopy(&g_plan[i].r, F_ISIZE, is, sizeof(is));
        uint64_t sz = 0, isz = 0;
        for (char *c = s; is_digit(*c); c++) sz = sz * 10 + (uint64_t)(*c - '0');
        for (char *c = is; is_digit(*c); c++) isz = isz * 10 + (uint64_t)(*c - '0');
        dl += sz;
        inst_size += isz * 1024;
        if (!g_plan[i].requested) extra++;
        if (inst_find(g_plan[i].name)) upgrades++;
    }
    if (extra) {
        say("The following additional packages will be installed:\n ");
        for (int i = 0; i < g_nplan; i++) if (!g_plan[i].requested) say(" %s", g_plan[i].name);
        say("\n");
    }
    if (g_nplan - upgrades) {
        say("The following NEW packages will be installed:\n ");
        for (int i = 0; i < g_nplan; i++) if (!inst_find(g_plan[i].name)) say(" %s", g_plan[i].name);
        say("\n");
    }
    if (upgrades) {
        say("The following packages will be upgraded:\n ");
        for (int i = 0; i < g_nplan; i++) if (inst_find(g_plan[i].name)) say(" %s", g_plan[i].name);
        say("\n");
    }
    char a[24], b[24];
    human(dl, a, sizeof(a));
    human(inst_size, b, sizeof(b));
    say("%d upgraded, %d newly installed, 0 to remove.\nNeed to get %s of archives.\nAfter this operation, %s of additional disk space will be used.\n",
        upgrades, g_nplan - upgrades, a, b);
    if ((extra || upgrade_all) && !ask_yes(assume_yes)) return 1;

    /* download everything first, like apt */
    static uint8_t *debs[PLAN_MAX];
    static size_t deb_len[PLAN_MAX];
    int rc = 0;
    for (int i = 0; i < g_nplan; i++) {
        rec_t *r = &g_plan[i].r;
        char file[300], url[600], ver[64], s[24];
        fcopy(r, F_FILE, file, sizeof(file));
        fcopy(r, F_VER, ver, sizeof(ver));
        fcopy(r, F_SIZE, s, sizeof(s));
        snprintf(url, sizeof(url), "%s/pool/%s", r->uri, file);
        human((uint64_t)dec(s), a, sizeof(a));
        say("Get:%d %s %s amd64 %s [%s]\n", i + 1, r->uri, g_plan[i].name, ver, a);
        if (fetch(url, 1)) { rc = 1; break; }
        /* integrity: size and SHA-256 from the index */
        char sha[80], hex[65];
        fcopy(r, F_SHA, sha, sizeof(sha));
        uint8_t d[32];
        hash_once(HASH_SHA256, g_req.data, g_req.len, d);
        for (int k = 0; k < 32; k++) snprintf(hex + k * 2, 3, "%02x", d[k]);
        if ((uint64_t)g_req.len != dec(s) || (sha[0] && strcmp(sha, hex))) {
            say("E: Failed to fetch %s  Hash Sum mismatch\n   Expected SHA256 %s, got %s\n", url, sha, hex);
            http_free(&g_req);
            rc = 1;
            break;
        }
        debs[i] = (uint8_t *)big_alloc(g_req.len + 1);
        if (!debs[i]) { say("E: out of memory\n"); http_free(&g_req); rc = 1; break; }
        memcpy(debs[i], g_req.data, g_req.len);
        deb_len[i] = g_req.len;
        http_free(&g_req);
    }
    if (!rc) {
        human(dl, a, sizeof(a));
        say("Fetched %s\n", a);
        int scripts = 0;
        for (int i = 0; i < g_nplan && !rc; i++) {
            char got[96];
            if (dpkg_install_buf(debs[i], deb_len[i], 0, got, sizeof(got))) rc = 1;
            if (g_ctl.scripts[0] && strstr(g_ctl.scripts, "postinst")) scripts++;
        }
        (void)scripts;
    }
    for (int i = 0; i < g_nplan; i++) if (debs[i]) { big_free(debs[i], deb_len[i] + 1); debs[i] = NULL; }
    return rc;
}

static int apt_remove(int nnames, char **names, int assume_yes, int purge)
{
    status_load();
    static char victims[PLAN_MAX][96];
    int nv = 0;
    for (int i = 0; i < nnames; i++) {
        if (!inst_find(names[i])) { say("Package '%s' is not installed, so not removed\n", names[i]); continue; }
        if (nv < PLAN_MAX) snprintf(victims[nv++], 96, "%s", names[i]);
    }
    if (!nv) { say("0 upgraded, 0 newly installed, 0 to remove.\n"); return 0; }
    /* everything installed that (transitively) depends on them goes too */
    static char deps[8192];
    int requested = nv;
    for (int changed = 1; changed && nv < PLAN_MAX;) {
        changed = 0;
        for (int i = 0; i < g_ninst && nv < PLAN_MAX; i++) {
            int already = 0;
            for (int v = 0; v < nv; v++) if (!strcmp(victims[v], g_inst[i].name)) already = 1;
            if (already) continue;
            int hit = 0;
            for (int k = 0; k < 2 && !hit; k++) {
                if (!field(g_inst[i].stanza, g_inst[i].len, k ? "Depends" : "Pre-Depends", deps, sizeof(deps))) continue;
                const char *p = deps, *end = deps + strlen(deps);
                while (p < end && !hit) {
                    /* a group is broken only if every alternative is going away */
                    int all_gone = 1, any = 0;
                    for (;;) {
                        dep_t d;
                        p = parse_dep(p, end, &d);
                        any = 1;
                        int gone = 0;
                        for (int v = 0; v < nv; v++) if (!strcmp(victims[v], d.name)) gone = 1;
                        if (!gone) all_gone = 0;
                        if (d.alt && p < end) continue;
                        break;
                    }
                    if (any && all_gone) hit = 1;
                }
            }
            if (hit) { snprintf(victims[nv++], 96, "%s", g_inst[i].name); changed = 1; }
        }
    }
    say("Reading package lists... Done\nBuilding dependency tree... Done\nThe following packages will be REMOVED:\n ");
    for (int v = 0; v < nv; v++) say(" %s%s", victims[v], purge ? "*" : "");
    say("\n0 upgraded, 0 newly installed, %d to remove.\n", nv);
    if (nv > requested && !ask_yes(assume_yes)) return 1;
    int rc = 0;
    for (int v = nv - 1; v >= 0; v--) if (dpkg_remove(victims[v], purge, 1)) rc = 1;
    return rc;
}

static int cmd_apt(int argc, char **argv)
{
    const char *op = NULL;
    int yes = 0, installed = 0, upgradable = 0;
    static char *names[64];
    int nn = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-y") || !strcmp(argv[i], "--yes") || !strcmp(argv[i], "--assume-yes")) yes = 1;
        else if (!strcmp(argv[i], "--installed")) installed = 1;
        else if (!strcmp(argv[i], "--upgradable")) upgradable = 1;
        else if (argv[i][0] == '-') continue;
        else if (!op) op = argv[i];
        else if (nn < 64) names[nn++] = argv[i];
    }
    if (!op) {
        say("apt 2.6 (JARVIS OS Linux layer)\nUsage: apt [options] command\n\n"
            "  update      download the package lists\n  install     install packages (and what they depend on)\n"
            "  remove      remove packages\n  purge       remove packages and their configuration\n"
            "  upgrade     upgrade installed packages\n  search      search package names and descriptions\n"
            "  show        show package details\n  list        list packages (--installed, --upgradable)\n"
            "  policy      show installed and candidate versions\n\nPrograms installed this way run from the Terminal by name, e.g. after\n'apt install hello' type: hello\n");
        return 0;
    }
    if (!strcmp(op, "update")) return apt_update();
    if (!strcmp(op, "install") || !strcmp(op, "reinstall")) {
        if (!nn) { say("E: give at least one package name, e.g. apt install hello\n"); return 1; }
        return apt_install(nn, names, yes, 0);
    }
    if (!strcmp(op, "upgrade") || !strcmp(op, "full-upgrade") || !strcmp(op, "dist-upgrade")) return apt_install(0, names, yes, 1);
    if (!strcmp(op, "remove") || !strcmp(op, "purge") || !strcmp(op, "autoremove")) {
        if (!strcmp(op, "autoremove")) { say("0 upgraded, 0 newly installed, 0 to remove.\n"); return 0; }
        if (!nn) { say("E: give at least one package name\n"); return 1; }
        return apt_remove(nn, names, yes, !strcmp(op, "purge"));
    }
    if (!strcmp(op, "clean") || !strcmp(op, "autoclean")) { say("(packages are installed straight from memory; nothing is cached)\n"); return 0; }
    if (!strcmp(op, "list") && installed) {
        status_load();
        say("Listing... Done\n");
        for (int i = 0; i < g_ninst; i++) {
            char arch[16];
            field(g_inst[i].stanza, g_inst[i].len, "Architecture", arch, sizeof(arch));
            if (nn && !glob(names[0], g_inst[i].name)) continue;
            say("%s/now %s %s [installed]\n", g_inst[i].name, g_inst[i].ver, arch);
        }
        return 0;
    }
    if (idx_load()) { say("E: no package lists yet. Run: apt update\n"); return 1; }
    status_load();
    if (!strcmp(op, "list")) {
        say("Listing... Done\n");
        int shown = 0;
        for (size_t o = 0; o < g_idx_len && shown < 200;) {
            const char *eol = strchr(g_idx + o, '\n');
            if (!eol) break;
            if (g_idx[o] != '#') {
                rec_t r;
                rec_split(o, &r);
                char nm[96], v[64], ar[16];
                fcopy(&r, F_NAME, nm, sizeof(nm));
                fcopy(&r, F_VER, v, sizeof(v));
                fcopy(&r, F_ARCH, ar, sizeof(ar));
                inst_t *in = inst_find(nm);
                int up = in && dpkg_vercmp(v, in->ver) > 0;
                if ((!upgradable || up) && (!nn || glob(names[0], nm))) {
                    say("%s %s %s%s\n", nm, v, ar, in ? (up ? " [upgradable]" : " [installed]") : "");
                    shown++;
                }
            }
            o = (size_t)(eol - g_idx) + 1;
        }
        if (!nn && !upgradable) say("(showing the first 200; give a pattern such as: apt list 'lib*ssl*')\n");
        return 0;
    }
    if (!strcmp(op, "search")) {
        if (!nn) { say("E: give a word to search for\n"); return 1; }
        int shown = 0;
        for (size_t o = 0; o < g_idx_len && shown < 100;) {
            const char *eol = strchr(g_idx + o, '\n');
            if (!eol) break;
            if (g_idx[o] != '#') {
                rec_t r;
                rec_split(o, &r);
                int all = 1;
                for (int k = 0; k < nn; k++)
                    if (!ci_contains(r.f[F_NAME], r.l[F_NAME], names[k]) && !ci_contains(r.f[F_DESC], r.l[F_DESC], names[k])) all = 0;
                if (all) {
                    char nm[96], v[64], d[100];
                    fcopy(&r, F_NAME, nm, sizeof(nm));
                    fcopy(&r, F_VER, v, sizeof(v));
                    fcopy(&r, F_DESC, d, sizeof(d));
                    say("%s/%s %s amd64%s\n  %s\n\n", nm, "stable", v, inst_find(nm) ? " [installed]" : "", d);
                    shown++;
                }
            }
            o = (size_t)(eol - g_idx) + 1;
        }
        if (shown == 100) say("(first 100 matches; add more words to narrow it down)\n");
        if (!shown) say("No packages found\n");
        return 0;
    }
    if (!strcmp(op, "show") || !strcmp(op, "policy")) {
        for (int k = 0; k < nn; k++) {
            rec_t r;
            if (!rec_find(names[k], &r)) { say("N: Unable to locate package %s\n", names[k]); continue; }
            inst_t *in = inst_find(names[k]);
            if (!strcmp(op, "policy")) {
                char v[64];
                fcopy(&r, F_VER, v, sizeof(v));
                say("%s:\n  Installed: %s\n  Candidate: %s\n", names[k], in ? in->ver : "(none)", v);
                continue;
            }
            const char *labels[F_COUNT] = { "Package", "Version", "Architecture", "Pre-Depends", "Depends", "Provides", "Filename", "Download-Size",
                                            "SHA256", "Installed-Size", "Description", "Section" };
            for (int f = 0; f < F_COUNT; f++) {
                if (!r.l[f] || f == F_SHA) continue;
                if (f == F_FILE) say("APT-Sources: %s\n", r.uri);
                else if (f == F_ISIZE) say("Installed-Size: %.*s kB\n", (int)r.l[f], r.f[f]);
                else say("%s: %.*s\n", labels[f], (int)r.l[f], r.f[f]);
            }
            say("Installed: %s\n\n", in ? in->ver : "no");
        }
        return 0;
    }
    say("E: Invalid operation %s\n", op);
    return 1;
}

/* ---- the worker process ------------------------------------------------------------------------ */
static char g_args[4096];
static char *g_argv[64];
static int g_argc;
static char g_cwd[256];
static volatile uint32_t g_worker;

static void dpkg_worker(void)
{
    const char *cmd = g_argv[0];
    int rc;
    if (!strcmp(cmd, "dpkg") || !strcmp(cmd, "dpkg-query") || !strcmp(cmd, "dpkg-deb")) {
        if (!strcmp(cmd, "dpkg-deb") && g_argc > 1 && !strcmp(g_argv[1], "-x")) { say("dpkg-deb -x is not supported; use dpkg -i\n"); rc = 1; }
        else rc = cmd_dpkg(g_argc, g_argv, g_cwd);
    } else rc = cmd_apt(g_argc, g_argv);
    if (g_idx) { big_free(g_idx, g_idx_len + 1); g_idx = NULL; }
    if (g_idx_hash) { big_free(g_idx_hash, IDX_HASH * sizeof(uint32_t)); g_idx_hash = NULL; }
    g_worker = 0;
    kthread_exit((uint64_t)rc);
}

/* Starts dpkg/apt/apt-get/apt-cache (argv[0]) as its own process; output
 * goes to the Terminal. Returns the PID, or 0 if one is already running. */
uint32_t dpkg_start(int argc, char **argv, const char *cwd)
{
    if (g_worker && proc_exists(g_worker)) {
        say("E: Could not get lock /var/lib/dpkg/lock-frontend. It is held by process %u (apt)\n", (unsigned)g_worker);
        return 0;
    }
    size_t o = 0;
    g_argc = 0;
    for (int i = 0; i < argc && g_argc < 63; i++) {
        size_t l = strlen(argv[i]);
        if (o + l + 1 > sizeof(g_args)) break;
        memcpy(g_args + o, argv[i], l + 1);
        g_argv[g_argc++] = g_args + o;
        o += l + 1;
    }
    g_argv[g_argc] = NULL;
    if (!strcmp(g_argv[0], "apt-get") || !strcmp(g_argv[0], "apt-cache")) g_argv[0] = (char *)"apt";
    snprintf(g_cwd, sizeof(g_cwd), "%s", cwd ? cwd : "/jarvis/data");
    uint64_t irq;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(irq) :: "memory");
    g_worker = proc_spawn(!strcmp(g_argv[0], "apt") ? "apt" : "dpkg", dpkg_worker, PRIO_USER, 0);
    if (irq & 0x200) __asm__ volatile ("sti");
    return g_worker;
}
