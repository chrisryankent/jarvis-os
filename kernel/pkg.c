/*
 * =============================================================================
 * JARVIS AI OS — Package manager (kernel/pkg.c)
 * Language: C17
 *
 * Installs, lists, runs and removes JARVIS packages (.jpk files, built on a
 * PC with tools/mkjpk.py).
 *
 * Package file layout (all integers little-endian):
 *   "JPK1"                    magic
 *   u32  manifest length
 *   u32  CRC-32 of everything that follows this header
 *   manifest                  text, one "key=value" per line:
 *                               name         id: a-z, 0-9, '-' (required)
 *                               title        display name
 *                               version, publisher, description
 *                               exec         program to run, relative path (required)
 *   entries, until the end:   u16 path length, path (relative), u32 size, data
 *
 * Installing checks the checksum and every path *before* writing anything,
 * then writes the files to /jarvis/apps/<name>/ and the manifest to
 * /jarvis/apps/<name>/package.info. Installing a package that is already
 * installed replaces it (upgrade). Programs inside run in user mode, like
 * any other program.
 *
 * Packages are not signed yet: install only packages from sources you trust.
 * =============================================================================
 */

#include "kernel.h"
#include <string.h>
#include <stdio.h>

#define PKG_ROOT      "/jarvis/apps"
#define PKG_MAX_SIZE  (64u << 20)

volatile uint32_t g_pkg_generation = 1;     /* bumped on install / uninstall */

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t crc32_buf(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

static void set_err(char *err, size_t n, const char *msg)
{
    if (err && n) snprintf(err, n, "%s", msg);
}

/* Value of `key` in manifest text, copied into out. */
static void manifest_get(const char *m, size_t mlen, const char *key, char *out, size_t n)
{
    size_t klen = strlen(key);
    out[0] = 0;
    for (size_t i = 0; i < mlen;) {
        size_t e = i;
        while (e < mlen && m[e] != '\n') e++;
        if (e - i > klen && !strncmp(m + i, key, klen) && m[i + klen] == '=') {
            size_t vlen = e - i - klen - 1;
            if (vlen >= n) vlen = n - 1;
            memcpy(out, m + i + klen + 1, vlen);
            out[vlen] = 0;
            while (vlen && (out[vlen - 1] == '\r' || out[vlen - 1] == ' ')) out[--vlen] = 0;
            return;
        }
        i = e + 1;
    }
}

static int valid_name(const char *s)
{
    if (!*s || strlen(s) > 31) return 0;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-')) return 0;
    return 1;
}

/* A path inside the package must stay inside the app's folder. */
static int valid_rel_path(const char *p, size_t len)
{
    if (!len || len > 120 || p[0] == '/') return 0;
    for (size_t i = 0; i < len; i++) {
        char c = p[i];
        if (c < 32 || c > 126 || c == '\\') return 0;
        if (c == '.' && (i == 0 || p[i - 1] == '/') && i + 1 < len && p[i + 1] == '.') return 0;   /* ".." */
    }
    return 1;
}

static void fill_info(pkg_info_t *o, const char *m, size_t mlen)
{
    manifest_get(m, mlen, "name", o->name, sizeof(o->name));
    manifest_get(m, mlen, "title", o->title, sizeof(o->title));
    manifest_get(m, mlen, "version", o->version, sizeof(o->version));
    manifest_get(m, mlen, "publisher", o->publisher, sizeof(o->publisher));
    manifest_get(m, mlen, "description", o->description, sizeof(o->description));
    manifest_get(m, mlen, "exec", o->exec, sizeof(o->exec));
    if (!o->title[0]) snprintf(o->title, sizeof(o->title), "%s", o->name);
}

/* Read a whole package into memory. Caller frees with pkg_free(). */
static uint8_t *load(const char *path, size_t *size, char *err, size_t errn)
{
    long sz = vfs_size(path);
    if (sz < 12 || vfs_is_dir(path)) { set_err(err, errn, "Not a package file."); return NULL; }
    if ((uint32_t)sz > PKG_MAX_SIZE) { set_err(err, errn, "Package is larger than 64 MB."); return NULL; }
    size_t n = (size_t)sz + 1;
    uint8_t *buf = n >= LARGE_ALLOC_THRESHOLD ? (uint8_t *)kmalloc_large(n) : (uint8_t *)kmalloc(n);
    if (!buf) { set_err(err, errn, "Not enough memory."); return NULL; }
    if (vfs_read_file(path, (char *)buf, n) != sz) {
        if (n >= LARGE_ALLOC_THRESHOLD) kfree_large(buf, n); else kfree(buf);
        set_err(err, errn, "The package could not be read.");
        return NULL;
    }
    *size = (size_t)sz;
    return buf;
}

static void pkg_free(uint8_t *buf, size_t size)
{
    size_t n = size + 1;
    if (n >= LARGE_ALLOC_THRESHOLD) kfree_large(buf, n); else kfree(buf);
}

/* Check the whole package; fills info. Returns the manifest length or -1. */
static int verify(const uint8_t *b, size_t size, pkg_info_t *info, char *err, size_t errn)
{
    if (memcmp(b, "JPK1", 4) != 0) { set_err(err, errn, "Not a JARVIS package."); return -1; }
    uint32_t mlen = rd32le(b + 4);
    if (12 + (size_t)mlen > size || mlen > 4096) { set_err(err, errn, "The package is damaged."); return -1; }
    if (crc32_buf(b + 12, size - 12) != rd32le(b + 8)) {
        set_err(err, errn, "The package is damaged (checksum does not match).");
        return -1;
    }

    memset(info, 0, sizeof(*info));
    fill_info(info, (const char *)b + 12, mlen);
    if (!valid_name(info->name)) { set_err(err, errn, "The package has no valid name."); return -1; }
    if (!info->exec[0]) { set_err(err, errn, "The package does not say which program to run."); return -1; }

    int has_exec = 0;
    for (size_t off = 12 + mlen; off < size;) {
        if (off + 2 > size) { set_err(err, errn, "The package is damaged."); return -1; }
        size_t plen = (size_t)(b[off] | (b[off + 1] << 8));
        if (off + 2 + plen + 4 > size || !valid_rel_path((const char *)b + off + 2, plen)) {
            set_err(err, errn, "The package contains an unsafe file path.");
            return -1;
        }
        uint32_t fsize = rd32le(b + off + 2 + plen);
        if (off + 6 + plen + fsize > size) { set_err(err, errn, "The package is damaged."); return -1; }
        if (strlen(info->exec) == plen && !memcmp(info->exec, b + off + 2, plen)) has_exec = 1;
        info->files++;
        info->bytes += fsize;
        off += 6 + plen + fsize;
    }
    if (!has_exec) { set_err(err, errn, "The program named in the package is missing."); return -1; }
    return (int)mlen;
}

int pkg_inspect(const char *path, pkg_info_t *info, char *err, size_t errn)
{
    size_t size;
    uint8_t *b = load(path, &size, err, errn);
    if (!b) return -1;
    int rc = verify(b, size, info, err, errn) < 0 ? -1 : 0;
    if (rc == 0) {
        char ipath[96];
        snprintf(ipath, sizeof(ipath), PKG_ROOT "/%s/package.info", info->name);
        info->installed = vfs_exists(ipath);
    }
    pkg_free(b, size);
    return rc;
}

int pkg_install(const char *path, pkg_info_t *info, char *err, size_t errn)
{
    size_t size;
    pkg_info_t tmp;
    if (!info) info = &tmp;
    uint8_t *b = load(path, &size, err, errn);
    if (!b) return -1;
    int mlen = verify(b, size, info, err, errn);
    if (mlen < 0) { pkg_free(b, size); return -1; }

    char dir[64], file[200];
    snprintf(dir, sizeof(dir), PKG_ROOT "/%s", info->name);
    vfs_mkdir(PKG_ROOT);
    if (vfs_exists(dir)) vfs_delete(dir);                  /* upgrade: replace */
    vfs_mkdir(dir);

    int rc = 0;
    for (size_t off = 12 + (size_t)mlen; off < size && rc == 0;) {
        size_t plen = (size_t)(b[off] | (b[off + 1] << 8));
        uint32_t fsize = rd32le(b + off + 2 + plen);
        /* (the kernel's printf has no %.*s, so build the path by hand) */
        size_t dl = strlen(dir);
        memcpy(file, dir, dl);
        file[dl] = '/';
        memcpy(file + dl + 1, b + off + 2, plen);        /* plen <= 120: fits */
        file[dl + 1 + plen] = 0;
        for (char *s = file + strlen(dir) + 1; *s; s++) {  /* create sub-folders */
            if (*s == '/') { *s = 0; vfs_mkdir(file); *s = '/'; }
        }
        if (vfs_write_file(file, b + off + 6 + plen, fsize) != 0) rc = -1;
        off += 6 + plen + fsize;
    }

    snprintf(file, sizeof(file), "%s/package.info", dir);
    if (rc == 0) rc = vfs_write_file(file, b + 12, (size_t)mlen);
    pkg_free(b, size);

    if (rc != 0) {
        vfs_delete(dir);
        set_err(err, errn, "Installing failed: the disk is full or the file table is full.");
        return -1;
    }
    info->installed = 1;
    g_pkg_generation++;
    terminal_printf("[PKG] Installed %s %s (%u files)\n", info->name, info->version, info->files);
    return 0;
}

int pkg_uninstall(const char *name)
{
    char dir[64];
    if (!valid_name(name)) return -1;
    snprintf(dir, sizeof(dir), PKG_ROOT "/%s", name);
    if (!vfs_exists(dir) || vfs_delete(dir) != 0) return -1;
    g_pkg_generation++;
    terminal_printf("[PKG] Uninstalled %s\n", name);
    return 0;
}

int pkg_list(pkg_info_t *out, int max)
{
    static char listing[8192];
    char manifest[4096], ipath[96];
    int n = 0;

    if (vfs_list(PKG_ROOT, listing, sizeof(listing)) < 0) return 0;
    size_t rlen = strlen(PKG_ROOT);
    for (const char *p = listing; *p && n < max;) {
        const char *eol = strchr(p, '\n');
        if (!eol) eol = p + strlen(p);
        const char *full = strchr(p, '/');
        if (!strncmp(p, "[dir] ", 6) && full && full < eol && (size_t)(eol - full) > rlen + 1 &&
            !strncmp(full, PKG_ROOT "/", rlen + 1) && !memchr(full + rlen + 1, '/', (size_t)(eol - full) - rlen - 1)) {
            size_t fl = (size_t)(eol - full);
            if (fl > sizeof(ipath) - 16) { p = *eol ? eol + 1 : eol; continue; }
            memcpy(ipath, full, fl);
            memcpy(ipath + fl, "/package.info", 14);
            int len = vfs_read_file(ipath, manifest, sizeof(manifest));
            if (len > 0) {
                memset(&out[n], 0, sizeof(out[n]));
                fill_info(&out[n], manifest, (size_t)len);
                out[n].installed = 1;
                if (valid_name(out[n].name)) n++;
            }
        }
        p = *eol ? eol + 1 : eol;
    }
    return n;
}

/* Run an installed package's program. Returns the PID, or 0. */
uint32_t pkg_run(const char *name)
{
    pkg_info_t list[32];
    int n = pkg_list(list, 32);
    for (int i = 0; i < n; i++) {
        if (strcmp(list[i].name, name)) continue;
        char path[200];
        snprintf(path, sizeof(path), PKG_ROOT "/%s/%s", list[i].name, list[i].exec);
        return user_exec(path);
    }
    return 0;
}

/* Sample packages built from user/ are placed in Downloads at boot, so there
 * is something to install. They are not installed automatically. */
extern const uint8_t user_pkg_hello_start[], user_pkg_hello_end[];
extern const uint8_t user_pkg_counter_start[], user_pkg_counter_end[];

void pkg_place_samples(void)
{
    const struct { const char *file; const uint8_t *s, *e; } samples[] = {
        { "/jarvis/data/Downloads/hello-1.0.jpk",   user_pkg_hello_start,   user_pkg_hello_end },
        { "/jarvis/data/Downloads/counter-1.0.jpk", user_pkg_counter_start, user_pkg_counter_end },
    };
    vfs_mkdir("/jarvis/data/Downloads");
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++)
        if (!vfs_exists(samples[i].file))
            vfs_write_file(samples[i].file, samples[i].s, (size_t)(samples[i].e - samples[i].s));
}
