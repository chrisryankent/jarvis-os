/*
 * =============================================================================
 * JARVIS AI OS — FAT file system, read-only (kernel/fatfs.c)
 * Language: C17
 *
 * Reads the FAT16 / FAT32 file systems that USB flash drives and memory
 * cards come formatted with (also from Windows, macOS, phones and cameras),
 * including long file names.
 *
 * Each FAT volume found on a USB block device appears in the file tree as
 * /jarvis/usb0, /jarvis/usb1, ... Folders can be browsed, files opened and
 * copied onto the JARVIS disk. The drive is never written to: writing FAT
 * safely (allocation, directory updates, long-name entries) is not
 * implemented yet, so every write to these paths is refused.
 *
 * Not supported: exFAT and NTFS (common on drives of 64 GB and up), FAT12,
 * sector sizes other than 512 bytes.
 * =============================================================================
 */

#include "kernel.h"
#include <string.h>
#include <stdio.h>

#define FAT_MAX_VOLS   4
#define FAT_NAME_MAX   128

typedef struct {
    int      used;
    int      dev;                   /* block device */
    char     mount[16];             /* "/jarvis/usb0" */
    char     label[12];
    int      fat32;
    uint64_t start;                 /* first sector of the volume */
    uint32_t sec_per_clus;
    uint64_t fat_start;             /* absolute sectors */
    uint64_t root_start;            /* FAT16 fixed root directory */
    uint32_t root_sectors;
    uint64_t data_start;
    uint32_t root_cluster;          /* FAT32 */
    uint32_t clusters;
    uint64_t total_bytes;
} fat_vol_t;

typedef struct {
    char     name[FAT_NAME_MAX];
    int      is_dir;
    uint32_t cluster;
    uint32_t size;
} fat_entry_t;

static fat_vol_t g_vols[FAT_MAX_VOLS];
static uint8_t   g_sec[512];
static uint8_t   g_fatsec[512];
static int       g_fatsec_dev = -1;
static uint64_t  g_fatsec_lba = ~0ULL;
static int       g_lock = -1;

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static char lower_c(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int name_eq(const char *a, const char *b, size_t blen)
{
    size_t i = 0;
    for (; i < blen; i++)
        if (!a[i] || lower_c(a[i]) != lower_c(b[i])) return 0;
    return a[i] == 0;
}

/* ---- Cluster chains ------------------------------------------------------ */
static uint32_t fat_next(fat_vol_t *v, uint32_t clus)
{
    uint64_t off = (uint64_t)clus * (v->fat32 ? 4 : 2);
    uint64_t lba = v->fat_start + off / 512;
    if (g_fatsec_dev != v->dev || g_fatsec_lba != lba) {
        if (block_read(v->dev, lba, 1, g_fatsec) != 0) return 0x0FFFFFFF;
        g_fatsec_dev = v->dev;
        g_fatsec_lba = lba;
    }
    if (v->fat32) return le32(g_fatsec + off % 512) & 0x0FFFFFFF;
    uint32_t n = le16(g_fatsec + off % 512);
    return n >= 0xFFF8 ? 0x0FFFFFFF : n;
}

static int clus_valid(fat_vol_t *v, uint32_t c) { return c >= 2 && c < v->clusters + 2; }

static uint64_t clus_lba(fat_vol_t *v, uint32_t c) { return v->data_start + (uint64_t)(c - 2) * v->sec_per_clus; }

/* ---- Directories ----------------------------------------------------------
 * Calls fn for every entry of the directory starting at `cluster` (0 = the
 * FAT16 root). Stops early if fn returns non-zero; returns that value.
 */
typedef int (*dir_fn)(const fat_entry_t *e, void *ctx);

static int dir_walk(fat_vol_t *v, uint32_t cluster, dir_fn fn, void *ctx)
{
    uint16_t lfn[260];
    int lfn_len = 0;
    uint64_t lba;
    uint32_t sectors_left;
    int guard = 0;

    if (cluster == 0 && !v->fat32) { lba = v->root_start; sectors_left = v->root_sectors; }
    else {
        if (cluster == 0) cluster = v->root_cluster;
        if (!clus_valid(v, cluster)) return 0;
        lba = clus_lba(v, cluster); sectors_left = v->sec_per_clus;
    }

    for (;;) {
        if (block_read(v->dev, lba, 1, g_sec) != 0) return -1;
        for (int i = 0; i < 512; i += 32) {
            const uint8_t *d = g_sec + i;
            if (d[0] == 0) return 0;                        /* end of directory */
            if (d[0] == 0xE5) { lfn_len = 0; continue; }    /* deleted */

            if (d[11] == 0x0F) {                            /* long-name piece */
                int seq = d[0] & 0x1F;
                if (seq < 1 || seq > 20) { lfn_len = 0; continue; }
                static const int pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
                for (int k = 0; k < 13; k++) {
                    int at = (seq - 1) * 13 + k;
                    if (at < 260) lfn[at] = le16(d + pos[k]);
                }
                if (d[0] & 0x40) {                          /* last piece comes first */
                    lfn_len = seq * 13;
                    if (lfn_len > 259) lfn_len = 259;
                }
                continue;
            }
            if (d[11] & 0x08) { lfn_len = 0; continue; }    /* volume label */

            fat_entry_t e;
            memset(&e, 0, sizeof(e));
            if (lfn_len) {
                int n = 0;
                for (int k = 0; k < lfn_len && n < FAT_NAME_MAX - 1; k++) {
                    uint16_t c = lfn[k];
                    if (c == 0 || c == 0xFFFF) break;
                    e.name[n++] = (c < 128 && c >= 32) ? (char)c : '_';
                }
                e.name[n] = 0;
            } else {                                        /* 8.3 name */
                int n = 0;
                for (int k = 0; k < 8 && d[k] != ' '; k++) e.name[n++] = lower_c((char)d[k]);
                if (d[8] != ' ') {
                    e.name[n++] = '.';
                    for (int k = 8; k < 11 && d[k] != ' '; k++) e.name[n++] = lower_c((char)d[k]);
                }
                e.name[n] = 0;
            }
            lfn_len = 0;
            if (!strcmp(e.name, ".") || !strcmp(e.name, "..") || !e.name[0]) continue;

            e.is_dir = (d[11] & 0x10) != 0;
            e.cluster = ((uint32_t)le16(d + 20) << 16) | le16(d + 26);
            e.size = le32(d + 28);
            int rc = fn(&e, ctx);
            if (rc) return rc;
        }

        lba++;
        if (--sectors_left == 0) {
            if (cluster == 0 && !v->fat32) return 0;       /* fixed root ended */
            cluster = fat_next(v, cluster);
            if (!clus_valid(v, cluster) || ++guard > 65536) return 0;
            lba = clus_lba(v, cluster);
            sectors_left = v->sec_per_clus;
        }
    }
}

/* ---- Path lookup --------------------------------------------------------- */
typedef struct { const char *want; size_t len; fat_entry_t found; } find_ctx_t;

static int find_cb(const fat_entry_t *e, void *c)
{
    find_ctx_t *f = (find_ctx_t *)c;
    if (name_eq(e->name, f->want, f->len)) { f->found = *e; return 1; }
    return 0;
}

static fat_vol_t *vol_for(const char *path, const char **rel)
{
    for (int i = 0; i < FAT_MAX_VOLS; i++) {
        size_t n = strlen(g_vols[i].mount);
        if (g_vols[i].used && strncmp(path, g_vols[i].mount, n) == 0 &&
            (path[n] == 0 || path[n] == '/')) {
            *rel = path + n;
            return &g_vols[i];
        }
    }
    return NULL;
}

/* Resolve a path inside the volume. 0 on success. */
static int lookup(fat_vol_t *v, const char *rel, fat_entry_t *out)
{
    memset(out, 0, sizeof(*out));
    out->is_dir = 1;                                        /* the root */
    while (*rel == '/') rel++;
    while (*rel) {
        const char *end = rel;
        while (*end && *end != '/') end++;
        if (!out->is_dir) return -1;
        find_ctx_t f = { rel, (size_t)(end - rel), { {0}, 0, 0, 0 } };
        if (dir_walk(v, out->cluster, find_cb, &f) != 1) return -1;
        *out = f.found;
        rel = end;
        while (*rel == '/') rel++;
    }
    return 0;
}

/* ---- Public interface (called by kernel/vfs.c) --------------------------- */
int fatfs_owns(const char *path)
{
    const char *rel;
    return vol_for(path, &rel) != NULL;
}

int fatfs_stat(const char *path, int *is_dir, uint32_t *size)
{
    const char *rel;
    fat_vol_t *v = vol_for(path, &rel);
    fat_entry_t e;
    if (!v) return -1;
    kmutex_lock(g_lock);
    int rc = lookup(v, rel, &e);
    kmutex_unlock(g_lock);
    if (rc != 0) return -1;
    if (is_dir) *is_dir = e.is_dir;
    if (size) *size = e.is_dir ? 0 : e.size;
    return 0;
}

typedef struct { char *buf; size_t max, out; const char *dir; } list_ctx_t;

static int list_cb(const fat_entry_t *e, void *c)
{
    list_ctx_t *l = (list_ctx_t *)c;
    const char *parts[] = { e->is_dir ? "[dir] " : "[file] ", l->dir, "/", e->name, "\n" };
    for (int p = 0; p < 5; p++)
        for (const char *s = parts[p]; *s && l->out + 1 < l->max; s++)
            l->buf[l->out++] = *s;
    return 0;
}

/* Same format as vfs_list(): one "[dir] /path" or "[file] /path" per line,
 * for the direct contents of the folder. */
int fatfs_list(const char *path, char *buf, size_t max)
{
    const char *rel;
    fat_vol_t *v = vol_for(path, &rel);
    fat_entry_t e;
    if (!v || max == 0) return -1;

    char dir[256];
    snprintf(dir, sizeof(dir), "%s", path);
    size_t dl = strlen(dir);
    while (dl > 1 && dir[dl - 1] == '/') dir[--dl] = 0;

    kmutex_lock(g_lock);
    list_ctx_t l = { buf, max, 0, dir };
    int rc = lookup(v, rel, &e) == 0 && e.is_dir ? dir_walk(v, e.cluster, list_cb, &l) : -1;
    kmutex_unlock(g_lock);
    buf[l.out] = 0;
    return rc < 0 ? -1 : (int)l.out;
}

/* Read up to len bytes of a file starting at offset. Returns bytes read. */
int fatfs_read(const char *path, uint64_t offset, void *buf, size_t len)
{
    const char *rel;
    fat_vol_t *v = vol_for(path, &rel);
    fat_entry_t e;
    if (!v) return -1;

    kmutex_lock(g_lock);
    if (lookup(v, rel, &e) != 0 || e.is_dir) { kmutex_unlock(g_lock); return -1; }
    if (offset >= e.size) { kmutex_unlock(g_lock); return 0; }
    if (len > e.size - offset) len = (size_t)(e.size - offset);

    uint32_t clus_bytes = v->sec_per_clus * 512;
    uint32_t c = e.cluster;
    for (uint64_t skip = offset / clus_bytes; skip && clus_valid(v, c); skip--)
        c = fat_next(v, c);

    uint8_t *out = (uint8_t *)buf;
    size_t done = 0;
    uint32_t in_clus = (uint32_t)(offset % clus_bytes);    /* position inside cluster c */
    while (done < len && clus_valid(v, c)) {
        while (done < len && in_clus < clus_bytes) {
            uint64_t lba = clus_lba(v, c) + in_clus / 512;
            uint32_t sec_off = in_clus % 512;
            if (block_read(v->dev, lba, 1, g_sec) != 0) { kmutex_unlock(g_lock); return -1; }
            size_t n = 512 - sec_off;
            if (n > len - done) n = len - done;
            memcpy(out + done, g_sec + sec_off, n);
            done += n;
            in_clus += (uint32_t)n;
        }
        c = fat_next(v, c);
        in_clus = 0;
    }
    kmutex_unlock(g_lock);
    return (int)done;
}

int fatfs_volume_info(int i, char *mount, size_t mlen, char *label, size_t llen, uint64_t *bytes)
{
    if (i < 0 || i >= FAT_MAX_VOLS || !g_vols[i].used) return -1;
    snprintf(mount, mlen, "%s", g_vols[i].mount);
    snprintf(label, llen, "%s", g_vols[i].label);
    *bytes = g_vols[i].total_bytes;
    return 0;
}

/* ---- Mounting ------------------------------------------------------------ */
static int try_volume(int dev, uint64_t start, const char *mount)
{
    uint8_t b[512];
    if (block_read(dev, start, 1, b) != 0 || b[510] != 0x55 || b[511] != 0xAA) return -1;
    if (le16(b + 11) != 512 || !b[13] || !b[16] || !le16(b + 14)) return -1;

    fat_vol_t v;
    memset(&v, 0, sizeof(v));
    uint32_t spc = b[13], reserved = le16(b + 14), nfats = b[16], root_ents = le16(b + 17);
    uint32_t fatsz = le16(b + 22) ? le16(b + 22) : le32(b + 36);
    uint32_t total = le16(b + 19) ? le16(b + 19) : le32(b + 32);
    v.root_sectors = (root_ents * 32 + 511) / 512;
    uint32_t data_secs = total - (reserved + nfats * fatsz + v.root_sectors);
    v.clusters = data_secs / spc;
    if (v.clusters < 4085) return -2;                       /* FAT12 */

    v.dev = dev;
    v.start = start;
    v.sec_per_clus = spc;
    v.fat32 = v.clusters >= 65525;
    v.fat_start = start + reserved;
    v.root_start = v.fat_start + (uint64_t)nfats * fatsz;
    v.data_start = v.root_start + v.root_sectors;
    v.root_cluster = v.fat32 ? le32(b + 44) : 0;
    v.total_bytes = (uint64_t)v.clusters * spc * 512;
    const uint8_t *lab = b + (v.fat32 ? 71 : 43);
    memcpy(v.label, lab, 11);
    for (int i = 10; i >= 0 && (v.label[i] == ' ' || !v.label[i]); i--) v.label[i] = 0;
    if (!strcmp(v.label, "NO NAME")) v.label[0] = 0;
    snprintf(v.mount, sizeof(v.mount), "%s", mount);
    v.used = 1;

    for (int i = 0; i < FAT_MAX_VOLS; i++)
        if (!g_vols[i].used) { g_vols[i] = v; return 0; }
    return -1;
}

void fatfs_mount_all(void)
{
    g_lock = kmutex_create("fatfs");
    int n = 0;

    for (int dev = 0; dev < block_count() && n < FAT_MAX_VOLS; dev++) {
        block_info_t info;
        if (block_get_info(dev, &info) != 0 || strncmp(info.name, "usb", 3) != 0) continue;

        char mount[16];
        snprintf(mount, sizeof(mount), "/jarvis/%s", info.name);
        uint8_t mbr[512];
        if (block_read(dev, 0, 1, mbr) != 0) continue;

        int rc = try_volume(dev, 0, mount);                 /* whole drive is one volume */
        if (rc == -1 && mbr[510] == 0x55 && mbr[511] == 0xAA) {
            for (int p = 0; p < 4 && rc != 0; p++) {        /* first FAT partition */
                const uint8_t *e = mbr + 446 + p * 16;
                uint8_t type = e[4];
                if (type == 0x07) {
                    terminal_printf("[FAT] %s: exFAT/NTFS partition (not supported yet)\n", info.name);
                    rc = -3;
                } else if (type == 0x04 || type == 0x06 || type == 0x0E || type == 0x0B || type == 0x0C) {
                    rc = try_volume(dev, le32(e + 8), mount);
                }
            }
        }
        if (rc == 0) {
            fat_vol_t *v = &g_vols[n++];
            vfs_mount_point(mount);
            terminal_printf("[FAT] %s: %s volume%s%s, %u MB, mounted at %s (read-only)\n", info.name,
                v->fat32 ? "FAT32" : "FAT16", v->label[0] ? " " : "", v->label,
                (unsigned)(v->total_bytes >> 20), mount);
        } else if (rc == -1 || rc == -2) {
            terminal_printf("[FAT] %s: no FAT file system found\n", info.name);
        }
    }
}
