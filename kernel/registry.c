/*
 * =============================================================================
 * JARVIS AI OS — Registry (kernel/registry.c)
 * Language: C17
 *
 * A hierarchical configuration database in the style of the Windows
 * registry: keys containing sub-keys and typed values.
 *
 *   HKEY_CLASSES_ROOT    file types: which app opens .txt, .png, .html ...
 *   HKEY_CURRENT_USER    Software\JARVIS OS\Settings (every system
 *                        setting: theme, wallpaper, brightness ...),
 *                        Environment (Terminal variables, setx)
 *   HKEY_LOCAL_MACHINE   SOFTWARE\JARVIS OS\CurrentVersion, and, built
 *                        live from the hardware (not saved):
 *                        HARDWARE\DESCRIPTION, HARDWARE\DEVICEMAP,
 *                        SYSTEM\CurrentControlSet\Services (drivers),
 *                        SOFTWARE\JARVIS OS\Installed Apps
 *   HKEY_CURRENT_CONFIG  the display mode in use (live)
 *
 * Value types use the Windows numbering: REG_SZ 1, REG_EXPAND_SZ 2,
 * REG_BINARY 3, REG_DWORD 4, REG_MULTI_SZ 7, REG_QWORD 11.
 *
 * Saved keys are written to /jarvis/system/registry.reg (the protected
 * system folder) in the same text format as "reg export" files, a couple
 * of seconds after a change. The settings store (setting_get/set) is a
 * view of HKCU\Software\JARVIS OS\Settings.
 * =============================================================================
 */

#include "kernel.h"
#include <string.h>
#include <stdio.h>

#define REG_FILE        "/jarvis/system/registry.reg"
#define REG_OLD_SETTINGS "/jarvis/system/settings.conf"
#define REG_MAX_KEYS    1024
#define REG_MAX_VALUES  4096
#define REG_NAME_MAX    64

typedef struct { int used, parent, vol; char name[REG_NAME_MAX]; } rkey_t;
typedef struct { int used, key, type; uint32_t len; uint8_t *data; char name[REG_NAME_MAX]; } rval_t;

static rkey_t g_keys[REG_MAX_KEYS];
static rval_t g_vals[REG_MAX_VALUES];
static int g_ready, g_dirty, g_building;
volatile uint32_t g_reg_generation;

static const char *const g_hives[REG_HIVES] = { "HKEY_CLASSES_ROOT", "HKEY_CURRENT_USER", "HKEY_LOCAL_MACHINE", "HKEY_CURRENT_CONFIG" };
static const char *const g_hive_short[REG_HIVES] = { "HKCR", "HKCU", "HKLM", "HKCC" };

static int ci_eq(const char *a, const char *b, size_t bl)
{
    size_t i = 0;
    for (; i < bl; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (!x || x != y) return 0;
    }
    return a[i] == 0;
}

static void *rmalloc(size_t n) { return n ? kmalloc(n) : NULL; }

static void changed(int key)
{
    g_reg_generation++;
    int k = key;
    while (k >= 0 && !g_keys[k].vol) k = g_keys[k].parent;      /* volatile ancestor? */
    if (k < 0 && !g_building) g_dirty = 1;
}

/* ---- keys ------------------------------------------------------------------ */
static int child_find(int parent, const char *name, size_t len)
{
    for (int i = REG_HIVES; i < REG_MAX_KEYS; i++)
        if (g_keys[i].used && g_keys[i].parent == parent && ci_eq(g_keys[i].name, name, len)) return i;
    return -1;
}

static int child_add(int parent, const char *name, size_t len)
{
    if (len == 0 || len >= REG_NAME_MAX) return -1;
    for (int i = REG_HIVES; i < REG_MAX_KEYS; i++)
        if (!g_keys[i].used) {
            g_keys[i].used = 1;
            g_keys[i].parent = parent;
            g_keys[i].vol = g_building;
            memcpy(g_keys[i].name, name, len);
            g_keys[i].name[len] = 0;
            changed(i);
            return i;
        }
    return -1;
}

/* "HKEY_CURRENT_USER\Software\X", "HKCU/Software/X" ... -> key index. */
int reg_open_key(const char *path, int create)
{
    if (!g_ready || !path) return -1;
    while (*path == '\\' || *path == '/' || *path == ' ') path++;
    const char *p = path;
    while (*p && *p != '\\' && *p != '/') p++;
    size_t hl = (size_t)(p - path);
    int k = -1;
    for (int h = 0; h < REG_HIVES; h++)
        if (ci_eq(g_hives[h], path, hl) || ci_eq(g_hive_short[h], path, hl)) k = h;
    if (!strncmp(path, "Computer", 8) && (path[8] == '\\' || path[8] == '/')) return reg_open_key(path + 9, create);
    if (k < 0) return -1;
    while (*p) {
        while (*p == '\\' || *p == '/') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != '\\' && *p != '/') p++;
        int c = child_find(k, s, (size_t)(p - s));
        if (c < 0) {
            if (!create) return -1;
            c = child_add(k, s, (size_t)(p - s));
            if (c < 0) return -1;
        }
        k = c;
    }
    return k;
}

int reg_key_path(int k, char *out, size_t n)
{
    if (k < 0 || k >= REG_MAX_KEYS || !g_keys[k].used) { if (n) out[0] = 0; return -1; }
    int chain[64], d = 0;
    for (int x = k; x >= 0 && d < 64; x = g_keys[x].parent) chain[d++] = x;
    size_t o = 0;
    out[0] = 0;
    for (int i = d - 1; i >= 0; i--) {
        int w = snprintf(out + o, n - o, "%s%s", i == d - 1 ? "" : "\\", g_keys[chain[i]].name);
        if (w < 0 || o + (size_t)w >= n) break;
        o += (size_t)w;
    }
    return 0;
}

const char *reg_key_name(int k) { return k >= 0 && k < REG_MAX_KEYS && g_keys[k].used ? g_keys[k].name : ""; }
int reg_key_parent(int k)       { return k >= 0 && k < REG_MAX_KEYS ? g_keys[k].parent : -1; }
int reg_key_volatile(int k)     { while (k >= 0) { if (g_keys[k].vol) return 1; k = g_keys[k].parent; } return 0; }

/* i-th sub-key of k in alphabetical order (-1 when there are no more). */
int reg_key_child(int k, int i)
{
    static int cache_parent = -2, cache_n;
    static uint32_t cache_gen;
    static int sorted[REG_MAX_KEYS];
    if (k == -1) return i >= 0 && i < REG_HIVES ? i : -1;
    if (cache_parent != k || cache_gen != g_reg_generation) {
        cache_n = 0;
        for (int c = REG_HIVES; c < REG_MAX_KEYS; c++)
            if (g_keys[c].used && g_keys[c].parent == k) sorted[cache_n++] = c;
        for (int a = 1; a < cache_n; a++) {
            int t = sorted[a], b = a - 1;
            while (b >= 0) {
                const char *x = g_keys[sorted[b]].name, *y = g_keys[t].name;
                int cmp = 0;
                for (;; x++, y++) {
                    char cx = *x >= 'A' && *x <= 'Z' ? (char)(*x + 32) : *x, cy = *y >= 'A' && *y <= 'Z' ? (char)(*y + 32) : *y;
                    if (cx != cy || !cx) { cmp = cx - cy; break; }
                }
                if (cmp <= 0) break;
                sorted[b + 1] = sorted[b];
                b--;
            }
            sorted[b + 1] = t;
        }
        cache_parent = k;
        cache_gen = g_reg_generation;
    }
    return i >= 0 && i < cache_n ? sorted[i] : -1;
}

int reg_key_has_children(int k)
{
    for (int c = REG_HIVES; c < REG_MAX_KEYS; c++) if (g_keys[c].used && g_keys[c].parent == k) return 1;
    return 0;
}

static void val_free(int v)
{
    if (g_vals[v].data) kfree(g_vals[v].data);
    memset(&g_vals[v], 0, sizeof(g_vals[v]));
}

static void key_delete_tree(int k)
{
    for (int c = REG_HIVES; c < REG_MAX_KEYS; c++)
        if (g_keys[c].used && g_keys[c].parent == k) key_delete_tree(c);
    for (int v = 0; v < REG_MAX_VALUES; v++)
        if (g_vals[v].used && g_vals[v].key == k) val_free(v);
    changed(k);
    g_keys[k].used = 0;
}

int reg_delete_key(const char *path)
{
    int k = reg_open_key(path, 0);
    if (k < REG_HIVES) return -1;                       /* never a hive root */
    key_delete_tree(k);
    return 0;
}

int reg_rename_key(const char *path, const char *new_name)
{
    int k = reg_open_key(path, 0);
    size_t l = strlen(new_name);
    if (k < REG_HIVES || !l || l >= REG_NAME_MAX || strchr(new_name, '\\')) return -1;
    if (child_find(g_keys[k].parent, new_name, l) >= 0) return -1;
    snprintf(g_keys[k].name, sizeof(g_keys[k].name), "%s", new_name);
    changed(k);
    return 0;
}

/* ---- values ------------------------------------------------------------------ */
static int val_find(int k, const char *name)
{
    for (int v = 0; v < REG_MAX_VALUES; v++)
        if (g_vals[v].used && g_vals[v].key == k && ci_eq(g_vals[v].name, name, strlen(name))) return v;
    return -1;
}

int reg_set_value_k(int k, const char *name, int type, const void *data, uint32_t len)
{
    if (k < 0 || !name || strlen(name) >= REG_NAME_MAX || len > 65536) return -1;
    int v = val_find(k, name);
    if (v >= 0 && g_vals[v].type == type && g_vals[v].len == len && (!len || !memcmp(g_vals[v].data, data, len))) return 0;
    if (v < 0) {
        for (int i = 0; i < REG_MAX_VALUES; i++) if (!g_vals[i].used) { v = i; break; }
        if (v < 0) return -1;
        g_vals[v].used = 1;
        g_vals[v].key = k;
        snprintf(g_vals[v].name, sizeof(g_vals[v].name), "%s", name);
    } else if (g_vals[v].data) { kfree(g_vals[v].data); g_vals[v].data = NULL; }
    g_vals[v].type = type;
    g_vals[v].len = len;
    if (len) {
        g_vals[v].data = (uint8_t *)rmalloc(len);
        if (!g_vals[v].data) { val_free(v); return -1; }
        memcpy(g_vals[v].data, data, len);
    }
    changed(k);
    return 0;
}

int reg_set_value(const char *path, const char *name, int type, const void *data, uint32_t len)
{
    return reg_set_value_k(reg_open_key(path, 1), name, type, data, len);
}

int reg_set_sz(const char *path, const char *name, const char *s)
{
    return reg_set_value(path, name, REG_SZ, s, (uint32_t)strlen(s) + 1);
}

int reg_set_dword(const char *path, const char *name, uint32_t d)
{
    return reg_set_value(path, name, REG_DWORD, &d, 4);
}

/* Value as text: strings as they are, numbers in decimal. */
static int val_text(int v, char *out, size_t n)
{
    rval_t *r = &g_vals[v];
    if (!n) return -1;
    out[0] = 0;
    switch (r->type) {
    case REG_DWORD: { uint32_t d = 0; memcpy(&d, r->data, r->len < 4 ? r->len : 4); snprintf(out, n, "%u", d); return 0; }
    case REG_QWORD: { uint64_t q = 0; memcpy(&q, r->data, r->len < 8 ? r->len : 8); snprintf(out, n, "%llu", (unsigned long long)q); return 0; }
    case REG_SZ: case REG_EXPAND_SZ: case REG_MULTI_SZ: {
        size_t l = r->len < n - 1 ? r->len : n - 1;
        if (l) memcpy(out, r->data, l);
        out[l] = 0;
        if (r->type == REG_MULTI_SZ)                        /* one item per line */
            for (size_t i = 0; i + 1 < l; i++) if (!out[i]) out[i] = '\n';
        return 0;
    }
    default: {
        size_t o = 0;
        for (uint32_t i = 0; i < r->len && o + 4 < n; i++) o += (size_t)snprintf(out + o, n - o, "%s%02x", i ? " " : "", r->data[i]);
        return 0;
    }
    }
}

int reg_get_text_k(int k, const char *name, char *out, size_t n)
{
    int v = k >= 0 ? val_find(k, name) : -1;
    if (v < 0) { if (n) out[0] = 0; return -1; }
    return val_text(v, out, n);
}

int reg_get_text(const char *path, const char *name, char *out, size_t n)
{
    return reg_get_text_k(reg_open_key(path, 0), name, out, n);
}

int reg_delete_value(const char *path, const char *name)
{
    int k = reg_open_key(path, 0), v = k >= 0 ? val_find(k, name) : -1;
    if (v < 0) return -1;
    changed(k);
    val_free(v);
    return 0;
}

int reg_rename_value(const char *path, const char *old, const char *nw)
{
    int k = reg_open_key(path, 0), v = k >= 0 ? val_find(k, old) : -1;
    if (v < 0 || strlen(nw) >= REG_NAME_MAX || val_find(k, nw) >= 0) return -1;
    snprintf(g_vals[v].name, sizeof(g_vals[v].name), "%s", nw);
    changed(k);
    return 0;
}

/* i-th value of key k (sorted by name, "(Default)" = "" first). */
int reg_value_at(int k, int i, char *name, size_t nn, int *type, char *text, size_t tn, uint32_t *len)
{
    static int cache_key = -2, cache_n, sorted[REG_MAX_VALUES];
    static uint32_t cache_gen;
    if (cache_key != k || cache_gen != g_reg_generation) {
        cache_n = 0;
        for (int v = 0; v < REG_MAX_VALUES; v++) if (g_vals[v].used && g_vals[v].key == k) sorted[cache_n++] = v;
        for (int a = 1; a < cache_n; a++) {
            int t = sorted[a], b = a - 1;
            while (b >= 0 && strcmp(g_vals[sorted[b]].name, g_vals[t].name) > 0) {
                sorted[b + 1] = sorted[b];
                b--;
            }
            sorted[b + 1] = t;
        }
        cache_key = k;
        cache_gen = g_reg_generation;
    }
    if (i < 0 || i >= cache_n) return -1;
    int v = sorted[i];
    if (name) snprintf(name, nn, "%s", g_vals[v].name);
    if (type) *type = g_vals[v].type;
    if (len) *len = g_vals[v].len;
    if (text) val_text(v, text, tn);
    return 0;
}

/* Raw bytes of a value (binary editing). */
int reg_get_raw_k(int k, const char *name, int *type, void *buf, uint32_t cap)
{
    int v = k >= 0 ? val_find(k, name) : -1;
    if (v < 0) return -1;
    if (type) *type = g_vals[v].type;
    uint32_t l = g_vals[v].len < cap ? g_vals[v].len : cap;
    if (l) memcpy(buf, g_vals[v].data, l);
    return (int)g_vals[v].len;
}

const char *reg_type_name(int t)
{
    switch (t) {
    case REG_SZ: return "REG_SZ"; case REG_EXPAND_SZ: return "REG_EXPAND_SZ"; case REG_BINARY: return "REG_BINARY";
    case REG_DWORD: return "REG_DWORD"; case REG_MULTI_SZ: return "REG_MULTI_SZ"; case REG_QWORD: return "REG_QWORD";
    default: return "REG_NONE";
    }
}

int reg_type_parse(const char *s)
{
    static const struct { const char *n; int t; } m[] = {
        { "REG_SZ", REG_SZ }, { "REG_EXPAND_SZ", REG_EXPAND_SZ }, { "REG_BINARY", REG_BINARY }, { "REG_DWORD", REG_DWORD },
        { "REG_MULTI_SZ", REG_MULTI_SZ }, { "REG_QWORD", REG_QWORD }, { "SZ", REG_SZ }, { "DWORD", REG_DWORD },
        { "QWORD", REG_QWORD }, { "BINARY", REG_BINARY }, { "STRING", REG_SZ }, { "MULTI_SZ", REG_MULTI_SZ }, { "EXPAND_SZ", REG_EXPAND_SZ },
    };
    for (size_t i = 0; i < sizeof(m) / sizeof(m[0]); i++) if (ci_eq(s, m[i].n, strlen(m[i].n))) return m[i].t;
    return -1;
}

/* Text -> value bytes for a type (numbers decimal or 0x hex; binary as hex
 * bytes; multi-string items separated by newlines or \0). */
int reg_set_from_text(int k, const char *name, int type, const char *text, char *err, size_t en)
{
    uint8_t buf[2048];
    uint32_t len = 0;
    if (type == REG_DWORD || type == REG_QWORD) {
        uint64_t v = 0;
        const char *p = text;
        while (*p == ' ') p++;
        int hex = p[0] == '0' && (p[1] == 'x' || p[1] == 'X');
        if (hex) p += 2;
        if (!*p) { snprintf(err, en, "type a number"); return -1; }
        for (; *p; p++) {
            int d = *p >= '0' && *p <= '9' ? *p - '0' : hex && *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 : hex && *p >= 'A' && *p <= 'F' ? *p - 'A' + 10 : -1;
            if (d < 0) { if (*p == ' ') break; snprintf(err, en, "'%c' is not a %s digit", *p, hex ? "hex" : "decimal"); return -1; }
            v = v * (hex ? 16 : 10) + (uint64_t)d;
        }
        if (type == REG_DWORD && v > 0xFFFFFFFFull) { snprintf(err, en, "too big for a DWORD (32-bit)"); return -1; }
        len = type == REG_DWORD ? 4 : 8;
        memcpy(buf, &v, len);
    } else if (type == REG_BINARY) {
        int hi = -1;
        for (const char *p = text; *p; p++) {
            int d = *p >= '0' && *p <= '9' ? *p - '0' : *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 : *p >= 'A' && *p <= 'F' ? *p - 'A' + 10 : -1;
            if (d < 0) { if (*p == ' ' || *p == ',') continue; snprintf(err, en, "binary data is hex bytes, e.g. 0a ff 10"); return -1; }
            if (hi < 0) hi = d; else { if (len < sizeof(buf)) buf[len++] = (uint8_t)(hi * 16 + d); hi = -1; }
        }
        if (hi >= 0) { snprintf(err, en, "odd number of hex digits"); return -1; }
    } else {
        size_t l = strlen(text);
        if (l + 2 > sizeof(buf)) { snprintf(err, en, "text too long"); return -1; }
        memcpy(buf, text, l + 1);
        len = (uint32_t)l + 1;
        if (type == REG_MULTI_SZ) {
            for (size_t i = 0; i < l; i++) if (buf[i] == '\n') buf[i] = 0;
            buf[len++] = 0;                                 /* list ends with an empty string */
        }
    }
    if (reg_set_value_k(k, name, type, buf, len) != 0) { snprintf(err, en, "the registry is full"); return -1; }
    return 0;
}

/* ---- .reg text format ------------------------------------------------------------ */
static size_t esc_str(char *o, size_t n, const uint8_t *d, uint32_t len)
{
    size_t w = 0;
    for (uint32_t i = 0; i < len && d[i] && w + 3 < n; i++) {
        if (d[i] == '"' || d[i] == '\\') o[w++] = '\\';
        o[w++] = (char)d[i];
    }
    o[w] = 0;
    return w;
}

static size_t export_key(int k, char *out, size_t n, size_t o, int include_volatile)
{
    if (o >= n || (!include_volatile && g_keys[k].vol)) return o;
    char path[512];
    reg_key_path(k, path, sizeof(path));
    o += (size_t)snprintf(out + o, n - o, "\r\n[%s]\r\n", path);
    for (int i = 0; o < n; i++) {
        char nm[REG_NAME_MAX];
        int type;
        uint32_t len;
        if (reg_value_at(k, i, nm, sizeof(nm), &type, NULL, 0, &len) != 0) break;
        static uint8_t raw[65536];
        reg_get_raw_k(k, nm, NULL, raw, sizeof(raw));
        char ename[REG_NAME_MAX * 2];
        esc_str(ename, sizeof(ename), (const uint8_t *)nm, (uint32_t)strlen(nm));
        if (nm[0]) o += (size_t)snprintf(out + o, n - o, "\"%s\"=", ename);
        else o += (size_t)snprintf(out + o, n - o, "@=");
        if (type == REG_SZ) {
            static char e[4096];
            esc_str(e, sizeof(e), raw, len);
            o += (size_t)snprintf(out + o, n - o, "\"%s\"\r\n", e);
        } else if (type == REG_DWORD) {
            uint32_t d = 0;
            memcpy(&d, raw, 4);
            o += (size_t)snprintf(out + o, n - o, "dword:%08x\r\n", d);
        } else {
            const char *pre = type == REG_BINARY ? "hex" : type == REG_QWORD ? "hex(b)" : type == REG_MULTI_SZ ? "hex(7)" : type == REG_EXPAND_SZ ? "hex(2)" : "hex(0)";
            o += (size_t)snprintf(out + o, n - o, "%s:", pre);
            for (uint32_t i2 = 0; i2 < len && o + 8 < n; i2++) o += (size_t)snprintf(out + o, n - o, "%s%02x", i2 ? "," : "", raw[i2]);
            o += (size_t)snprintf(out + o, n - o, "\r\n");
        }
    }
    for (int i = 0; o < n; i++) {
        int c = reg_key_child(k, i);
        if (c < 0) break;
        o = export_key(c, out, n, o, include_volatile);   /* (the child cache is rebuilt per parent) */
    }
    return o;
}

/* Writes "path" and everything below it as .reg text. Returns length or -1. */
int reg_export(const char *path, char *out, size_t n, int include_volatile)
{
    int k = reg_open_key(path, 0);
    if (k < 0 || n < 64) return -1;
    size_t o = (size_t)snprintf(out, n, "Windows Registry Editor Version 5.00\r\n");
    /* children are visited in sorted order; the cache is keyed by parent, so
     * walk without relying on it across recursion */
    o = export_key(k, out, n, o, include_volatile);
    if (o >= n) o = n - 1;
    out[o] = 0;
    return (int)o;
}

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

/* Imports .reg text: [key], [-key] (delete), "name"="text", "name"=dword:,
 * hex:, hex(2|7|b):, "name"=- (delete), @= (default value). */
int reg_import(const char *text, int *keys_out, int *vals_out, char *err, size_t en)
{
    int k = -1, nk = 0, nv = 0, line_no = 0;
    const char *p = text;
    char line[8192];
    while (*p) {
        size_t l = 0;
        /* join continuation lines ending in backslash */
        for (;;) {
            while (*p && *p != '\n' && l < sizeof(line) - 1) { if (*p != '\r') line[l++] = *p; p++; }
            if (*p == '\n') p++;
            line_no++;
            if (l && line[l - 1] == '\\' && *p) { l--; while (*p == ' ') p++; continue; }
            break;
        }
        line[l] = 0;
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == ';' || !strncmp(s, "Windows Registry", 16) || !strncmp(s, "REGEDIT", 7)) continue;
        if (*s == '[') {
            char *e = strrchr(s, ']');
            if (!e) { snprintf(err, en, "line %d: missing ]", line_no); return -1; }
            *e = 0;
            if (s[1] == '-') { reg_delete_key(s + 2); k = -1; continue; }
            k = reg_open_key(s + 1, 1);
            if (k < 0) { snprintf(err, en, "line %d: unknown hive in %s", line_no, s + 1); return -1; }
            nk++;
            continue;
        }
        if (k < 0) { snprintf(err, en, "line %d: value outside a [key]", line_no); return -1; }
        char name[REG_NAME_MAX] = "";
        if (*s == '@') s++;
        else if (*s == '"') {
            s++;
            size_t nl = 0;
            while (*s && *s != '"' && nl < sizeof(name) - 1) { if (*s == '\\' && s[1]) s++; name[nl++] = *s++; }
            name[nl] = 0;
            if (*s == '"') s++;
        } else { snprintf(err, en, "line %d: expected \"name\"=", line_no); return -1; }
        while (*s == ' ') s++;
        if (*s != '=') { snprintf(err, en, "line %d: expected =", line_no); return -1; }
        s++;
        while (*s == ' ') s++;
        static uint8_t buf[8192];
        uint32_t bl = 0;
        int type;
        if (*s == '-') {
            int v = val_find(k, name);
            if (v >= 0) { changed(k); val_free(v); }
            continue;
        } else if (*s == '"') {
            s++;
            while (*s && *s != '"' && bl < sizeof(buf) - 2) { if (*s == '\\' && s[1]) s++; buf[bl++] = (uint8_t)*s++; }
            buf[bl++] = 0;
            type = REG_SZ;
        } else if (!strncmp(s, "dword:", 6)) {
            uint32_t d = 0;
            for (s += 6; hexval(*s) >= 0; s++) d = d * 16 + (uint32_t)hexval(*s);
            memcpy(buf, &d, 4);
            bl = 4;
            type = REG_DWORD;
        } else if (!strncmp(s, "hex", 3)) {
            s += 3;
            type = REG_BINARY;
            if (*s == '(') {
                int t = 0;
                for (s++; hexval(*s) >= 0; s++) t = t * 16 + hexval(*s);
                type = t;
                if (*s == ')') s++;
            }
            if (*s == ':') s++;
            while (*s && bl < sizeof(buf)) {
                if (hexval(s[0]) >= 0 && hexval(s[1]) >= 0) { buf[bl++] = (uint8_t)(hexval(s[0]) * 16 + hexval(s[1])); s += 2; }
                else if (*s == ',' || *s == ' ') s++;
                else break;
            }
        } else { snprintf(err, en, "line %d: unknown value format", line_no); return -1; }
        if (reg_set_value_k(k, name, type, buf, bl) != 0) { snprintf(err, en, "line %d: the registry is full", line_no); return -1; }
        nv++;
    }
    if (keys_out) *keys_out = nk;
    if (vals_out) *vals_out = nv;
    return 0;
}

/* ---- persistence ----------------------------------------------------------------- */
void reg_flush(void)
{
    if (!g_ready || !g_dirty) return;
    g_dirty = 0;
    size_t cap = 512 * 1024;
    char *b = (char *)kmalloc_large(cap);
    if (!b) { g_dirty = 1; return; }
    size_t o = (size_t)snprintf(b, cap, "Windows Registry Editor Version 5.00\r\n");
    for (int h = 0; h < REG_HIVES; h++) {
        if (h == REG_HKCC) continue;                         /* current config is live only */
        o = export_key(h, b, cap, o, 0);
    }
    if (o >= cap) o = cap - 1;
    vfs_mkdir("/jarvis/system");
    vfs_write_file(REG_FILE, b, o);
    kfree_large(b, cap);
}

/* ---- live (volatile) keys ------------------------------------------------------- */
void sys_cpu_name(char *out, size_t n);         /* desktop/sysutil.c */

static void vset_sz(int k, const char *name, const char *s) { reg_set_value_k(k, name, REG_SZ, s, (uint32_t)strlen(s) + 1); }
static void vset_dw(int k, const char *name, uint32_t d)    { reg_set_value_k(k, name, REG_DWORD, &d, 4); }

/* Rebuilds the keys that describe the machine as it is now. */
void reg_refresh_live(void)
{
    if (!g_ready) return;
    g_building = 1;
    reg_delete_key("HKLM\\HARDWARE");
    reg_delete_key("HKLM\\SYSTEM\\CurrentControlSet\\Services");
    reg_delete_key("HKLM\\SOFTWARE\\JARVIS OS\\Installed Apps");
    reg_delete_key("HKCC\\System");

    char b[96];
    int k = reg_open_key("HKLM\\HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 1);
    sys_cpu_name(b, sizeof(b));
    vset_sz(k, "ProcessorNameString", b);
    vset_sz(k, "Identifier", "x86-64");
    uint64_t total = 0, freep = 0;
    pmm_get_stats(&total, &freep);
    k = reg_open_key("HKLM\\HARDWARE\\DESCRIPTION\\System", 1);
    vset_dw(k, "MemoryMB", (uint32_t)(total >> 8));
    vset_sz(k, "SystemBiosVersion", "Multiboot2 / GRUB");

    static dev_info_t d[48];
    int n = devmgr_list(d, 48);
    for (int i = 0; i < n; i++) {
        char path[160], nm[56];
        snprintf(nm, sizeof(nm), "%s", d[i].name);
        for (char *c = nm; *c; c++) if (*c == '\\' || *c == '/') *c = '-';
        snprintf(path, sizeof(path), "HKLM\\HARDWARE\\DEVICEMAP\\%s\\%02d %s", d[i].type, d[i].id, nm);
        int dk = reg_open_key(path, 1);
        vset_sz(dk, "DeviceDesc", d[i].name);
        vset_sz(dk, "Location", d[i].location);
        vset_sz(dk, "Driver", d[i].driver);
        if (d[i].vendor[0]) vset_sz(dk, "Mfg", d[i].vendor);
        snprintf(b, sizeof(b), "%04x:%04x", d[i].vendor_id, d[i].device_id);
        vset_sz(dk, "HardwareID", b);
        vset_dw(dk, "Status", (uint32_t)d[i].state);
        if (d[i].irq >= 0) vset_dw(dk, "IRQ", (uint32_t)d[i].irq);
        if (d[i].driver[0]) {
            snprintf(path, sizeof(path), "HKLM\\SYSTEM\\CurrentControlSet\\Services\\%s", d[i].driver);
            int sk = reg_open_key(path, 1);
            vset_dw(sk, "Start", 0);                         /* boot */
            vset_dw(sk, "Type", 1);                          /* kernel driver */
            vset_sz(sk, "ImagePath", "built into the kernel");
            vset_sz(sk, "DisplayName", d[i].driver);
        }
    }
    static pkg_info_t pk[32];
    int np = pkg_list(pk, 32);
    for (int i = 0; i < np; i++) {
        char path[160];
        snprintf(path, sizeof(path), "HKLM\\SOFTWARE\\JARVIS OS\\Installed Apps\\%s", pk[i].name);
        int ak = reg_open_key(path, 1);
        vset_sz(ak, "DisplayName", pk[i].title);
        vset_sz(ak, "DisplayVersion", pk[i].version);
        vset_sz(ak, "Publisher", pk[i].publisher);
    }
    extern uint32_t fb_get_width(void), fb_get_height(void);
    k = reg_open_key("HKCC\\System\\CurrentControlSet\\Control\\Video", 1);
    vset_dw(k, "XResolution", fb_get_width());
    vset_dw(k, "YResolution", fb_get_height());
    vset_dw(k, "BitsPerPel", 32);
    /* mark the generated subtrees volatile */
    static const char *const roots[] = { "HKLM\\HARDWARE", "HKLM\\SYSTEM\\CurrentControlSet\\Services", "HKLM\\SOFTWARE\\JARVIS OS\\Installed Apps", "HKCC\\System" };
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) { int r = reg_open_key(roots[i], 1); if (r >= 0) g_keys[r].vol = 1; }
    g_building = 0;
    g_reg_generation++;
}

/* ---- defaults and start-up -------------------------------------------------------- */
static void defaults(void)
{
    reg_set_sz("HKLM\\SOFTWARE\\JARVIS OS\\CurrentVersion", "ProductName", "JARVIS OS");
    reg_set_sz("HKLM\\SOFTWARE\\JARVIS OS\\CurrentVersion", "CurrentVersion", "1.0");
    reg_set_sz("HKLM\\SOFTWARE\\JARVIS OS\\CurrentVersion", "CurrentBuild", __DATE__);
    reg_set_sz("HKLM\\SOFTWARE\\JARVIS OS\\CurrentVersion", "SystemRoot", "/jarvis");
    reg_set_sz("HKLM\\SOFTWARE\\JARVIS OS\\CurrentVersion", "RegisteredOwner", account_name());
    char t[24];
    snprintf(t, sizeof(t), "%ld", (long)sys_unix_time());
    reg_set_sz("HKLM\\SOFTWARE\\JARVIS OS\\CurrentVersion", "InstallDate", t);
    static const char *const assoc[][2] = {
        { ".txt", "Notepad" }, { ".log", "Notepad" }, { ".md", "Notepad" }, { ".conf", "Notepad" }, { ".ini", "Notepad" },
        { ".c", "Notepad" }, { ".h", "Notepad" }, { ".py", "Notepad" }, { ".json", "Notepad" }, { ".csv", "Notepad" },
        { ".png", "Photos" }, { ".jpg", "Photos" }, { ".jpeg", "Photos" }, { ".bmp", "Photos" },
        { ".html", "Browser" }, { ".htm", "Browser" }, { ".jpk", "Apps" }, { ".reg", "Registry Editor" },
    };
    for (size_t i = 0; i < sizeof(assoc) / sizeof(assoc[0]); i++) {
        char p[64];
        snprintf(p, sizeof(p), "HKCR\\%s", assoc[i][0]);
        reg_set_sz(p, "", assoc[i][1]);
    }
    reg_open_key("HKCU\\Environment", 1);
    reg_open_key("HKCU\\Software\\JARVIS OS\\Settings", 1);
}

void reg_init(void)
{
    memset(g_keys, 0, sizeof(g_keys));
    memset(g_vals, 0, sizeof(g_vals));
    for (int h = 0; h < REG_HIVES; h++) {
        g_keys[h].used = 1;
        g_keys[h].parent = -1;
        snprintf(g_keys[h].name, sizeof(g_keys[h].name), "%s", g_hives[h]);
    }
    g_ready = 1;
    long size = vfs_size(REG_FILE);
    int loaded = 0;
    if (size > 0) {
        size_t cap = (size_t)size + 1;
        char *b = (char *)(cap >= LARGE_ALLOC_THRESHOLD ? kmalloc_large(cap) : kmalloc(cap));
        if (b && vfs_read_file(REG_FILE, b, cap) > 0) {
            char err[96];
            int nk, nv;
            if (reg_import(b, &nk, &nv, err, sizeof(err)) == 0) { loaded = 1; terminal_printf("[REG] Loaded %d keys, %d values\n", nk, nv); }
            else terminal_printf("[REG] registry.reg: %s (starting from defaults)\n", err);
        }
        if (b) { if (cap >= LARGE_ALLOC_THRESHOLD) kfree_large(b, cap); else kfree(b); }
    }
    if (!loaded) {
        defaults();
        /* first start with a registry: bring over the old settings file */
        static char old[4096];
        int n = vfs_read_file(REG_OLD_SETTINGS, old, sizeof(old));
        int moved = 0;
        for (char *p = n > 0 ? old : (char *)""; *p;) {
            char *e = strchr(p, '\n');
            if (e) *e = 0;
            char *eq = strchr(p, '=');
            if (eq) {
                *eq = 0;
                const char *v = eq + 1;
                int num = *v && strlen(v) < 10;
                for (const char *c = v + (*v == '-'); *c && num; c++) if (*c < '0' || *c > '9') num = 0;
                if (num && *v != '-') {
                    uint32_t d = 0;
                    for (const char *c = v; *c; c++) d = d * 10 + (uint32_t)(*c - '0');
                    reg_set_dword("HKCU\\Software\\JARVIS OS\\Settings", p, d);
                } else reg_set_sz("HKCU\\Software\\JARVIS OS\\Settings", p, v);
                moved++;
            }
            if (!e) break;
            p = e + 1;
        }
        terminal_printf("[REG] New registry created%s%d setting(s) moved in\n", moved ? ", " : "", moved);
        g_dirty = 1;
        reg_flush();
    }
    g_dirty = 0;
}

/* ---- settings view ----------------------------------------------------------------- */
#define SETTINGS_KEY "HKCU\\Software\\JARVIS OS\\Settings"
static int g_settings_key = -1;

static int settings_key(void)
{
    if (g_settings_key < 0 || !g_keys[g_settings_key].used) g_settings_key = reg_open_key(SETTINGS_KEY, 1);
    return g_settings_key;
}

int reg_setting_get(const char *key, char *out, size_t n)
{
    if (!g_ready) { if (n) out[0] = 0; return 0; }
    return reg_get_text_k(settings_key(), key, out, n) == 0;
}

void reg_setting_set(const char *key, const char *value, int numeric)
{
    if (!g_ready) return;
    int k = settings_key();
    if (numeric) {
        long v = 0;
        int neg = *value == '-';
        for (const char *c = value + neg; *c >= '0' && *c <= '9'; c++) v = v * 10 + (*c - '0');
        if (!neg) { uint32_t d = (uint32_t)v; reg_set_value_k(k, key, REG_DWORD, &d, 4); return; }
    }
    reg_set_value_k(k, key, REG_SZ, value, (uint32_t)strlen(value) + 1);
}
