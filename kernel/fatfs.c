/*
 * =============================================================================
 * JARVIS AI OS — FAT file system, read and write (kernel/fatfs.c)
 * Language: C17
 *
 * FAT16 and FAT32, as USB flash drives and memory cards come formatted
 * (from Windows, macOS, phones and cameras), with long file names.
 *
 * Every FAT volume on a disk other than the system disk is mounted: the
 * first one as /jarvis/<disk> (usb0, sd1 ...), further partitions as
 * /jarvis/<disk>p2, p3 ... Files and folders can be read, created,
 * written, appended to, renamed, moved and deleted.
 *
 * Writing keeps the volume consistent the way other systems expect:
 *   - every FAT copy is updated, cluster chains are freed on delete
 *   - long names get matching 8.3 aliases (NAME~1.TXT) with checksums
 *   - directories grow by a cluster when full (FAT16's fixed root cannot)
 *   - folders get "." and ".." entries; a moved folder's ".." is updated
 *   - the FAT32 FSInfo free count is kept current
 *   - date and time stamps come from the real-time clock
 * Writes go straight to the device (no write-back cache), so pulling the
 * drive out after an operation finished loses nothing; Eject still flushes.
 *
 * Not supported: exFAT and NTFS, FAT12, sector sizes other than 512 bytes.
 * =============================================================================
 */

#include "kernel.h"
#include <string.h>
#include <stdio.h>

#define FAT_MAX_VOLS   8
#define FAT_NAME_MAX   128
#define FAT_EOC        0x0FFFFFFF

typedef struct {
    int      used;
    int      dev;                   /* block device */
    int      part;                  /* partition number (1-4), 0 = whole disk */
    char     mount[24];             /* "/jarvis/usb0" */
    char     label[12];
    int      fat32;
    uint64_t start;                 /* first sector of the volume */
    uint32_t sec_per_clus;
    uint32_t nfats, fatsz;          /* FAT copies, sectors per FAT */
    uint64_t fat_start;             /* absolute sectors */
    uint64_t root_start;            /* FAT16 fixed root directory */
    uint32_t root_sectors;
    uint64_t data_start;
    uint32_t root_cluster;          /* FAT32 */
    uint32_t clusters;
    uint32_t fsinfo;                /* FAT32 FSInfo sector (relative), 0 = none */
    uint32_t free_clusters;
    uint32_t next_free;             /* allocation hint */
    uint64_t total_bytes;
    uint64_t free_bytes;
} fat_vol_t;

/* A directory entry, and where its slots are on disk (for updates). */
typedef struct {
    char     name[FAT_NAME_MAX];
    int      is_dir;
    uint32_t cluster;
    uint32_t size;
    int      nslots;                /* long-name pieces + the short entry */
    uint64_t slot_lba[21];
    uint16_t slot_off[21];
    uint8_t  short_name[11];
} fat_entry_t;

static fat_vol_t g_vols[FAT_MAX_VOLS];
static uint8_t   g_sec[512];
static uint8_t   g_fatsec[512];
static int       g_fatsec_dev = -1;
static uint64_t  g_fatsec_lba = ~0ULL;
static int       g_lock = -1;

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static char lower_c(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static char upper_c(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static int name_eq(const char *a, const char *b, size_t blen)
{
    size_t i = 0;
    for (; i < blen; i++)
        if (!a[i] || lower_c(a[i]) != lower_c(b[i])) return 0;
    return a[i] == 0;
}

static void fat_lock(void)   { if (g_lock < 0) g_lock = kmutex_create("fatfs"); kmutex_lock(g_lock); }
static void fat_unlock(void) { kmutex_unlock(g_lock); }

/* ---- Cluster chains ------------------------------------------------------ */
static uint32_t fat_get(fat_vol_t *v, uint32_t clus)
{
    uint64_t off = (uint64_t)clus * (v->fat32 ? 4 : 2);
    uint64_t lba = v->fat_start + off / 512;
    if (g_fatsec_dev != v->dev || g_fatsec_lba != lba) {
        if (block_read(v->dev, lba, 1, g_fatsec) != 0) return FAT_EOC;
        g_fatsec_dev = v->dev;
        g_fatsec_lba = lba;
    }
    if (v->fat32) return le32(g_fatsec + off % 512) & 0x0FFFFFFF;
    uint32_t n = le16(g_fatsec + off % 512);
    return n >= 0xFFF8 ? FAT_EOC : n;
}
#define fat_next fat_get

/* Write one FAT entry into every FAT copy. */
static int fat_set(fat_vol_t *v, uint32_t clus, uint32_t val)
{
    uint64_t off = (uint64_t)clus * (v->fat32 ? 4 : 2);
    for (uint32_t f = 0; f < v->nfats; f++) {
        uint64_t lba = v->fat_start + (uint64_t)f * v->fatsz + off / 512;
        static uint8_t s[512];
        if (block_read(v->dev, lba, 1, s) != 0) return -1;
        if (v->fat32) put32(s + off % 512, (le32(s + off % 512) & 0xF0000000u) | (val & 0x0FFFFFFF));
        else put16(s + off % 512, (uint16_t)(val >= 0x0FFFFFF8 ? 0xFFFF : val));
        if (block_write(v->dev, lba, 1, s) != 0) return -1;
        if (f == 0 && g_fatsec_dev == v->dev && g_fatsec_lba == lba) memcpy(g_fatsec, s, 512);
    }
    return 0;
}

static int clus_valid(fat_vol_t *v, uint32_t c) { return c >= 2 && c < v->clusters + 2; }

static uint64_t clus_lba(fat_vol_t *v, uint32_t c) { return v->data_start + (uint64_t)(c - 2) * v->sec_per_clus; }

static void vol_update_free(fat_vol_t *v)
{
    v->free_bytes = (uint64_t)v->free_clusters * v->sec_per_clus * 512;
}

static void fsinfo_write(fat_vol_t *v)
{
    if (!v->fat32 || !v->fsinfo) return;
    static uint8_t s[512];
    if (block_read(v->dev, v->start + v->fsinfo, 1, s) != 0) return;
    if (le32(s) != 0x41615252 || le32(s + 484) != 0x61417272) return;
    put32(s + 488, v->free_clusters);
    put32(s + 492, v->next_free);
    block_write(v->dev, v->start + v->fsinfo, 1, s);
}

static int clus_zero(fat_vol_t *v, uint32_t c)
{
    static uint8_t z[512];
    memset(z, 0, sizeof(z));
    for (uint32_t s = 0; s < v->sec_per_clus; s++)
        if (block_write(v->dev, clus_lba(v, c) + s, 1, z) != 0) return -1;
    return 0;
}

/* Allocate one cluster (marked end-of-chain), linked after prev if prev != 0. */
static uint32_t fat_alloc(fat_vol_t *v, uint32_t prev)
{
    if (!v->free_clusters) return 0;
    uint32_t start = clus_valid(v, v->next_free) ? v->next_free : 2;
    uint32_t c = start;
    for (uint32_t tries = 0; tries < v->clusters; tries++) {
        if (fat_get(v, c) == 0) {
            if (fat_set(v, c, FAT_EOC) != 0) return 0;
            if (prev && fat_set(v, prev, c) != 0) return 0;
            v->free_clusters--;
            v->next_free = c + 1;
            vol_update_free(v);
            return c;
        }
        if (++c >= v->clusters + 2) c = 2;
    }
    v->free_clusters = 0;
    return 0;
}

static void fat_free_chain(fat_vol_t *v, uint32_t c)
{
    for (int guard = 0; clus_valid(v, c) && guard < 4 * 1024 * 1024; guard++) {
        uint32_t next = fat_get(v, c);
        if (fat_set(v, c, 0) != 0) break;
        v->free_clusters++;
        if (c < v->next_free) v->next_free = c;
        c = next;
    }
    vol_update_free(v);
}

/* ---- Time stamps ----------------------------------------------------------- */
static void fat_now(uint16_t *date, uint16_t *time)
{
    int64_t t = sys_unix_time();
    if (t < 315532800) t = 315532800;           /* 1980-01-01, FAT's epoch */
    int64_t days = t / 86400, secs = t % 86400;
    int y = 1970;
    for (;;) {
        int yd = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
        if (days < yd) break;
        days -= yd;
        y++;
    }
    static const int md[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int m = 0;
    for (; m < 12; m++) {
        int d = md[m] + (m == 1 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)));
        if (days < d) break;
        days -= d;
    }
    *date = (uint16_t)(((y - 1980) << 9) | ((m + 1) << 5) | (int)(days + 1));
    *time = (uint16_t)(((secs / 3600) << 11) | (((secs / 60) % 60) << 5) | ((secs % 60) / 2));
}

/* ---- Directories ----------------------------------------------------------
 * Calls fn for every entry of the directory starting at `cluster` (0 = the
 * root). Stops early if fn returns non-zero; returns that value.
 */
typedef int (*dir_fn)(const fat_entry_t *e, void *ctx);

static int dir_walk(fat_vol_t *v, uint32_t cluster, dir_fn fn, void *ctx)
{
    uint16_t lfn[260];
    int lfn_len = 0;
    uint64_t lba;
    uint32_t sectors_left;
    int guard = 0;
    uint64_t pos_lba[21];
    uint16_t pos_off[21];
    int npos = 0;

    if (cluster == 0 && !v->fat32) { lba = v->root_start; sectors_left = v->root_sectors; }
    else {
        if (cluster == 0) cluster = v->root_cluster;
        if (!clus_valid(v, cluster)) return 0;
        lba = clus_lba(v, cluster); sectors_left = v->sec_per_clus;
    }

    for (;;) {
        static uint8_t sec[512];
        if (block_read(v->dev, lba, 1, sec) != 0) return -1;
        for (int i = 0; i < 512; i += 32) {
            const uint8_t *d = sec + i;
            if (d[0] == 0) return 0;                        /* end of directory */
            if (d[0] == 0xE5) { lfn_len = 0; npos = 0; continue; }

            if (d[11] == 0x0F) {                            /* long-name piece */
                int seq = d[0] & 0x1F;
                if (seq < 1 || seq > 20) { lfn_len = 0; npos = 0; continue; }
                if (d[0] & 0x40) { npos = 0; lfn_len = seq * 13; if (lfn_len > 259) lfn_len = 259; }
                static const int pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
                for (int k = 0; k < 13; k++) {
                    int at = (seq - 1) * 13 + k;
                    if (at < 260) lfn[at] = le16(d + pos[k]);
                }
                if (npos < 20) { pos_lba[npos] = lba; pos_off[npos] = (uint16_t)i; npos++; }
                continue;
            }
            if (d[11] & 0x08) { lfn_len = 0; npos = 0; continue; }    /* volume label */

            static fat_entry_t e;
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
                int lower_base = (d[12] & 0x08) != 0, lower_ext = (d[12] & 0x10) != 0;
                for (int k = 0; k < 8 && d[k] != ' '; k++) e.name[n++] = lower_base ? lower_c((char)d[k]) : (char)d[k];
                if (d[8] != ' ') {
                    e.name[n++] = '.';
                    for (int k = 8; k < 11 && d[k] != ' '; k++) e.name[n++] = lower_ext ? lower_c((char)d[k]) : (char)d[k];
                }
                e.name[n] = 0;
                if (!lower_base && !lower_ext)               /* old all-caps names: show them lower case */
                    for (int k = 0; k < n; k++) e.name[k] = lower_c(e.name[k]);
            }
            for (int k = 0; k < npos; k++) { e.slot_lba[k] = pos_lba[k]; e.slot_off[k] = pos_off[k]; }
            if (!lfn_len) npos = 0, e.nslots = 0;
            else e.nslots = npos;
            e.slot_lba[e.nslots] = lba;
            e.slot_off[e.nslots] = (uint16_t)i;
            e.nslots++;
            memcpy(e.short_name, d, 11);
            lfn_len = 0;
            npos = 0;
            if (!strcmp(e.name, ".") || !strcmp(e.name, "..") || !e.name[0]) continue;

            e.is_dir = (d[11] & 0x10) != 0;
            e.cluster = ((uint32_t)le16(d + 20) << 16) | le16(d + 26);
            if (!v->fat32) e.cluster &= 0xFFFF;
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
    fat_vol_t *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < FAT_MAX_VOLS; i++) {
        size_t n = strlen(g_vols[i].mount);
        if (g_vols[i].used && n > best_len && strncmp(path, g_vols[i].mount, n) == 0 &&
            (path[n] == 0 || path[n] == '/')) {
            best = &g_vols[i];
            best_len = n;
        }
    }
    if (best) *rel = path + best_len;
    return best;
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
        static find_ctx_t f;
        f.want = rel;
        f.len = (size_t)(end - rel);
        if (dir_walk(v, out->cluster, find_cb, &f) != 1) return -1;
        *out = f.found;
        rel = end;
        while (*rel == '/') rel++;
    }
    return 0;
}

/* Splits rel into parent folder (looked up) and leaf name. */
static int lookup_parent(fat_vol_t *v, const char *rel, fat_entry_t *parent, char *leaf, size_t n)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", rel);
    size_t l = strlen(tmp);
    while (l && tmp[l - 1] == '/') tmp[--l] = 0;
    char *s = strrchr(tmp, '/');
    const char *name = s ? s + 1 : tmp;
    if (!*name || strlen(name) >= n) return -1;
    snprintf(leaf, n, "%s", name);
    if (s) *s = 0; else tmp[0] = 0;
    if (lookup(v, tmp, parent) != 0 || !parent->is_dir) return -1;
    return 0;
}

/* ---- Creating directory entries ------------------------------------------- */
static uint8_t lfn_checksum(const uint8_t *sn)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + sn[i]);
    return sum;
}

static int sfn_char_ok(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c && strchr("!#$%&'()-@^_`{}~", c));
}

typedef struct { const uint8_t *sn; int hit; } sn_ctx_t;
static int sn_cb(const fat_entry_t *e, void *c)
{
    sn_ctx_t *s = (sn_ctx_t *)c;
    if (!memcmp(e->short_name, s->sn, 11)) { s->hit = 1; return 1; }
    return 0;
}

/* Short (8.3) name for a long name; *needs_lfn says whether the long name
 * must be stored as well. Unique within the folder. */
static void make_short_name(fat_vol_t *v, uint32_t dir, const char *name, uint8_t sn[11], int *needs_lfn, uint8_t *case_flags)
{
    memset(sn, ' ', 11);
    *case_flags = 0;
    const char *dot = strrchr(name, '.');
    if (dot == name) dot = NULL;
    size_t blen = dot ? (size_t)(dot - name) : strlen(name);
    size_t elen = dot ? strlen(dot + 1) : 0;

    /* fits 8.3 exactly (all one case per part)? */
    int fits = blen >= 1 && blen <= 8 && elen <= 3;
    int any_upper_b = 0, any_lower_b = 0, any_upper_e = 0, any_lower_e = 0;
    for (size_t i = 0; i < blen && fits; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') any_lower_b = 1; else if (c >= 'A' && c <= 'Z') any_upper_b = 1;
        if (!sfn_char_ok(upper_c(c))) fits = 0;
    }
    for (size_t i = 0; i < elen && fits; i++) {
        char c = dot[1 + i];
        if (c >= 'a' && c <= 'z') any_lower_e = 1; else if (c >= 'A' && c <= 'Z') any_upper_e = 1;
        if (!sfn_char_ok(upper_c(c))) fits = 0;
    }
    if (fits && !(any_upper_b && any_lower_b) && !(any_upper_e && any_lower_e)) {
        for (size_t i = 0; i < blen; i++) sn[i] = (uint8_t)upper_c(name[i]);
        for (size_t i = 0; i < elen; i++) sn[8 + i] = (uint8_t)upper_c(dot[1 + i]);
        if (any_lower_b) *case_flags |= 0x08;
        if (any_lower_e) *case_flags |= 0x10;
        *needs_lfn = 0;
        sn_ctx_t c = { sn, 0 };
        dir_walk(v, dir, sn_cb, &c);
        if (!c.hit) return;
    }
    *needs_lfn = 1;
    *case_flags = 0;
    /* basis name: valid upper-case characters only */
    char base[9] = "", ext[4] = "";
    int bl = 0, el = 0;
    for (size_t i = 0; i < blen && bl < 8; i++) { char c = upper_c(name[i]); if (c == ' ' || c == '.') continue; base[bl++] = sfn_char_ok(c) ? c : '_'; }
    for (size_t i = 0; i < elen && el < 3; i++) { char c = upper_c(dot[1 + i]); if (c == ' ') continue; ext[el++] = sfn_char_ok(c) ? c : '_'; }
    if (!bl) base[bl++] = '_';
    for (int n = 1; n < 1000000; n++) {
        char tail[8];
        int tl = snprintf(tail, sizeof(tail), "~%d", n);
        int keep = bl;
        if (keep + tl > 8) keep = 8 - tl;
        memset(sn, ' ', 11);
        memcpy(sn, base, (size_t)keep);
        memcpy(sn + keep, tail, (size_t)tl);
        memcpy(sn + 8, ext, (size_t)el);
        sn_ctx_t c = { sn, 0 };
        dir_walk(v, dir, sn_cb, &c);
        if (!c.hit) return;
    }
}

/* Finds `need` consecutive free slots in a directory, growing it if needed.
 * Returns 0 and the slot positions. */
static int dir_find_free(fat_vol_t *v, uint32_t dir, int need, uint64_t *lbas, uint16_t *offs)
{
    uint64_t lba;
    uint32_t sectors_left, cluster = dir;
    int run = 0;
    if (dir == 0 && !v->fat32) { lba = v->root_start; sectors_left = v->root_sectors; }
    else {
        if (cluster == 0) cluster = v->root_cluster;
        lba = clus_lba(v, cluster); sectors_left = v->sec_per_clus;
    }
    for (int guard = 0; guard < 65536; guard++) {
        static uint8_t s[512];
        if (block_read(v->dev, lba, 1, s) != 0) return -1;
        for (int i = 0; i < 512; i += 32) {
            if (s[i] == 0 || s[i] == 0xE5) {
                lbas[run] = lba; offs[run] = (uint16_t)i;
                if (++run == need) return 0;
            } else run = 0;
        }
        lba++;
        if (--sectors_left == 0) {
            if (dir == 0 && !v->fat32) return -2;          /* FAT16 root is full */
            uint32_t next = fat_get(v, cluster);
            if (!clus_valid(v, next)) {                     /* grow the directory */
                next = fat_alloc(v, cluster);
                if (!next || clus_zero(v, next) != 0) return -3;
            }
            cluster = next;
            lba = clus_lba(v, cluster);
            sectors_left = v->sec_per_clus;
        }
    }
    return -1;
}

static int slot_write(fat_vol_t *v, uint64_t lba, uint16_t off, const uint8_t ent[32])
{
    static uint8_t s[512];
    if (block_read(v->dev, lba, 1, s) != 0) return -1;
    memcpy(s + off, ent, 32);
    return block_write(v->dev, lba, 1, s);
}

/* Adds an entry for `name` in directory `dir`; fills out (with slot positions). */
static int dir_add(fat_vol_t *v, uint32_t dir, const char *name, int is_dir, uint32_t first, uint32_t size, fat_entry_t *out)
{
    uint8_t sn[11], cf;
    int lfn;
    make_short_name(v, dir, name, sn, &lfn, &cf);
    int nl = (int)strlen(name);
    if (nl > 255) return -1;
    int pieces = lfn ? (nl + 12) / 13 : 0;
    uint64_t lbas[21];
    uint16_t offs[21];
    if (dir_find_free(v, dir, pieces + 1, lbas, offs) != 0) return -1;

    uint8_t ent[32];
    uint8_t sum = lfn_checksum(sn);
    static const int pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    for (int p = 0; p < pieces; p++) {
        int seq = pieces - p;                               /* written last piece first */
        memset(ent, 0, 32);
        ent[0] = (uint8_t)(seq | (p == 0 ? 0x40 : 0));
        ent[11] = 0x0F;
        ent[13] = sum;
        for (int k = 0; k < 13; k++) {
            int at = (seq - 1) * 13 + k;
            uint16_t c = at < nl ? (uint8_t)name[at] : at == nl ? 0 : 0xFFFF;
            put16(ent + pos[k], c);
        }
        if (slot_write(v, lbas[p], offs[p], ent) != 0) return -1;
    }
    memset(ent, 0, 32);
    memcpy(ent, sn, 11);
    ent[11] = is_dir ? 0x10 : 0x20;
    ent[12] = cf;
    uint16_t d, t;
    fat_now(&d, &t);
    put16(ent + 14, t); put16(ent + 16, d); put16(ent + 18, d);
    put16(ent + 22, t); put16(ent + 24, d);
    put16(ent + 20, (uint16_t)(first >> 16));
    put16(ent + 26, (uint16_t)first);
    put32(ent + 28, is_dir ? 0 : size);
    if (slot_write(v, lbas[pieces], offs[pieces], ent) != 0) return -1;
    if (out) {
        memset(out, 0, sizeof(*out));
        snprintf(out->name, sizeof(out->name), "%s", name);
        out->is_dir = is_dir; out->cluster = first; out->size = size;
        out->nslots = pieces + 1;
        for (int k = 0; k <= pieces; k++) { out->slot_lba[k] = lbas[k]; out->slot_off[k] = offs[k]; }
        memcpy(out->short_name, sn, 11);
    }
    return 0;
}

/* Rewrites the short entry's first cluster and size (and modified time). */
static int entry_update(fat_vol_t *v, const fat_entry_t *e, uint32_t first, uint32_t size)
{
    static uint8_t s[512];
    uint64_t lba = e->slot_lba[e->nslots - 1];
    uint16_t off = e->slot_off[e->nslots - 1];
    if (block_read(v->dev, lba, 1, s) != 0) return -1;
    uint8_t *ent = s + off;
    uint16_t d, t;
    fat_now(&d, &t);
    put16(ent + 22, t); put16(ent + 24, d); put16(ent + 18, d);
    put16(ent + 20, (uint16_t)(first >> 16));
    put16(ent + 26, (uint16_t)first);
    put32(ent + 28, size);
    return block_write(v->dev, lba, 1, s);
}

static int entry_remove(fat_vol_t *v, const fat_entry_t *e)
{
    static uint8_t s[512];
    for (int k = 0; k < e->nslots; k++) {
        if (block_read(v->dev, e->slot_lba[k], 1, s) != 0) return -1;
        s[e->slot_off[k]] = 0xE5;
        if (block_write(v->dev, e->slot_lba[k], 1, s) != 0) return -1;
    }
    return 0;
}

/* Writes data into a fresh cluster chain. Returns the first cluster (0 for
 * an empty file) or -1. */
static int64_t write_chain(fat_vol_t *v, const uint8_t *data, size_t len)
{
    if (!len) return 0;
    uint32_t cbytes = v->sec_per_clus * 512;
    uint64_t need = (len + cbytes - 1) / cbytes;
    if (need > v->free_clusters) return -1;
    uint32_t first = 0, prev = 0;
    size_t done = 0;
    static uint8_t tail[512];
    while (done < len) {
        uint32_t c = fat_alloc(v, prev);
        if (!c) { if (first) fat_free_chain(v, first); return -1; }
        if (!first) first = c;
        for (uint32_t s = 0; s < v->sec_per_clus; s++) {
            size_t n = len - done;
            const void *src = data + done;
            if (n >= 512) n = 512;
            else { memset(tail, 0, 512); if (n) memcpy(tail, data + done, n); src = tail; }
            if (block_write(v->dev, clus_lba(v, c) + s, 1, src) != 0) { fat_free_chain(v, first); return -1; }
            done += n;
            if (done >= len && n < 512) {
                /* clear the rest of the cluster: no stale data */
                memset(tail, 0, 512);
                for (uint32_t r = s + 1; r < v->sec_per_clus; r++) block_write(v->dev, clus_lba(v, c) + r, 1, tail);
                break;
            }
            if (done >= len) break;
        }
        prev = c;
    }
    return first;
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
    static fat_entry_t e;
    if (!v) return -1;
    fat_lock();
    int rc = lookup(v, rel, &e);
    fat_unlock();
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
    static fat_entry_t e;
    if (!v || max == 0) return -1;

    char dir[256];
    snprintf(dir, sizeof(dir), "%s", path);
    size_t dl = strlen(dir);
    while (dl > 1 && dir[dl - 1] == '/') dir[--dl] = 0;

    fat_lock();
    list_ctx_t l = { buf, max, 0, dir };
    int rc = lookup(v, rel, &e) == 0 && e.is_dir ? dir_walk(v, e.cluster, list_cb, &l) : -1;
    fat_unlock();
    buf[l.out] = 0;
    return rc < 0 ? -1 : (int)l.out;
}

/* Read up to len bytes of a file starting at offset. Returns bytes read. */
int fatfs_read(const char *path, uint64_t offset, void *buf, size_t len)
{
    const char *rel;
    fat_vol_t *v = vol_for(path, &rel);
    static fat_entry_t e;
    if (!v) return -1;

    fat_lock();
    if (lookup(v, rel, &e) != 0 || e.is_dir) { fat_unlock(); return -1; }
    if (offset >= e.size) { fat_unlock(); return 0; }
    if (len > e.size - offset) len = (size_t)(e.size - offset);

    uint32_t clus_bytes = v->sec_per_clus * 512;
    uint32_t c = e.cluster;
    for (uint64_t skip = offset / clus_bytes; skip && clus_valid(v, c); skip--)
        c = fat_next(v, c);

    uint8_t *out = (uint8_t *)buf;
    size_t done = 0;
    uint32_t in_clus = (uint32_t)(offset % clus_bytes);
    while (done < len && clus_valid(v, c)) {
        /* whole sectors straight into the caller's buffer */
        while (done < len && in_clus < clus_bytes) {
            uint64_t lba = clus_lba(v, c) + in_clus / 512;
            uint32_t sec_off = in_clus % 512;
            if (sec_off == 0 && len - done >= 512) {
                uint32_t secs = (clus_bytes - in_clus) / 512;
                if ((len - done) / 512 < secs) secs = (uint32_t)((len - done) / 512);
                if (block_read(v->dev, lba, secs, out + done) != 0) { fat_unlock(); return -1; }
                done += (size_t)secs * 512;
                in_clus += secs * 512;
                continue;
            }
            if (block_read(v->dev, lba, 1, g_sec) != 0) { fat_unlock(); return -1; }
            size_t n = 512 - sec_off;
            if (n > len - done) n = len - done;
            memcpy(out + done, g_sec + sec_off, n);
            done += n;
            in_clus += (uint32_t)n;
        }
        c = fat_next(v, c);
        in_clus = 0;
    }
    fat_unlock();
    return (int)done;
}

/* Create or replace a file with the given contents. 0 on success. */
int fatfs_write_file(const char *path, const void *data, size_t len)
{
    const char *rel;
    fat_vol_t *v = vol_for(path, &rel);
    if (!v || len > 0xFFFFFFFFu) return -1;
    fat_lock();
    static fat_entry_t parent, e;
    char leaf[FAT_NAME_MAX];
    int rc = -1;
    if (lookup_parent(v, rel, &parent, leaf, sizeof(leaf)) == 0) {
        int exists = lookup(v, rel, &e) == 0;
        if (exists && e.is_dir) goto out;
        int64_t first = write_chain(v, (const uint8_t *)data, len);   /* new data first */
        if (first < 0) goto out;
        if (exists) {
            uint32_t old = e.cluster;
            if (entry_update(v, &e, (uint32_t)first, (uint32_t)len) != 0) { fat_free_chain(v, (uint32_t)first); goto out; }
            if (old) fat_free_chain(v, old);
            rc = 0;
        } else {
            rc = dir_add(v, parent.cluster, leaf, 0, (uint32_t)first, (uint32_t)len, NULL);
            if (rc) fat_free_chain(v, (uint32_t)first);
        }
    }
out:
    fsinfo_write(v);
    fat_unlock();
    return rc;
}

/* Append to a file (created if missing). */
int fatfs_append(const char *path, const void *data, size_t len)
{
    const char *rel;
    fat_vol_t *v = vol_for(path, &rel);
    if (!v) return -1;
    static fat_entry_t e;
    fat_lock();
    int exists = lookup(v, rel, &e) == 0;
    fat_unlock();
    if (!exists) return fatfs_write_file(path, data, len);
    if (e.is_dir) return -1;
    if (!len) return 0;

    fat_lock();
    int rc = -1;
    uint32_t cbytes = v->sec_per_clus * 512;
    uint32_t last = e.cluster, prev = 0;
    for (int guard = 0; clus_valid(v, last) && guard < 4 * 1024 * 1024; guard++) {
        uint32_t n = fat_get(v, last);
        if (!clus_valid(v, n)) break;
        prev = last;
        last = n;
    }
    (void)prev;
    const uint8_t *src = (const uint8_t *)data;
    size_t done = 0;
    uint32_t used_in_last = e.size % cbytes;
    uint32_t first = e.cluster;
    if (!e.cluster) { last = 0; used_in_last = 0; }
    else if (e.size && used_in_last == 0) used_in_last = cbytes;
    static uint8_t s[512];
    /* fill the end of the last cluster */
    while (last && used_in_last < cbytes && done < len) {
        uint64_t lba = clus_lba(v, last) + used_in_last / 512;
        uint32_t off = used_in_last % 512;
        if (block_read(v->dev, lba, 1, s) != 0) goto out;
        size_t n = 512 - off;
        if (n > len - done) n = len - done;
        memcpy(s + off, src + done, n);
        if (block_write(v->dev, lba, 1, s) != 0) goto out;
        done += n;
        used_in_last += (uint32_t)n;
    }
    /* new clusters for the rest */
    while (done < len) {
        uint32_t c = fat_alloc(v, last);
        if (!c) goto out;
        if (!first) first = c;
        for (uint32_t k = 0; k < v->sec_per_clus; k++) {
            memset(s, 0, 512);
            size_t n = len - done < 512 ? len - done : 512;
            memcpy(s, src + done, n);
            if (block_write(v->dev, clus_lba(v, c) + k, 1, s) != 0) goto out;
            done += n;
        }
        last = c;
    }
    rc = entry_update(v, &e, first, e.size + (uint32_t)len);
out:
    fsinfo_write(v);
    fat_unlock();
    return rc;
}

int fatfs_mkdir(const char *path)
{
    const char *rel;
    fat_vol_t *v = vol_for(path, &rel);
    if (!v) return -1;
    fat_lock();
    static fat_entry_t parent, e;
    char leaf[FAT_NAME_MAX];
    int rc = -1;
    if (lookup_parent(v, rel, &parent, leaf, sizeof(leaf)) != 0 || lookup(v, rel, &e) == 0) goto out;
    uint32_t c = fat_alloc(v, 0);
    if (!c) goto out;
    if (clus_zero(v, c) != 0) { fat_free_chain(v, c); goto out; }
    uint8_t ent[32];
    uint16_t d, t;
    fat_now(&d, &t);
    for (int k = 0; k < 2; k++) {
        memset(ent, ' ', 11);
        memset(ent + 11, 0, 21);
        ent[0] = '.';
        if (k) ent[1] = '.';
        ent[11] = 0x10;
        put16(ent + 14, t); put16(ent + 16, d); put16(ent + 18, d); put16(ent + 22, t); put16(ent + 24, d);
        uint32_t target = k ? (parent.cluster == v->root_cluster && v->fat32 ? 0 : parent.cluster) : c;
        put16(ent + 20, (uint16_t)(target >> 16));
        put16(ent + 26, (uint16_t)target);
        slot_write(v, clus_lba(v, c), (uint16_t)(k * 32), ent);
    }
    rc = dir_add(v, parent.cluster, leaf, 1, c, 0, NULL);
    if (rc) fat_free_chain(v, c);
out:
    fsinfo_write(v);
    fat_unlock();
    return rc;
}

/* Delete a file, or a folder with everything in it. */
typedef struct { char names[64][FAT_NAME_MAX]; int n; } kids_t;
static int kids_cb(const fat_entry_t *e, void *c)
{
    kids_t *k = (kids_t *)c;
    if (k->n < 64) snprintf(k->names[k->n++], FAT_NAME_MAX, "%s", e->name);
    return 0;
}

static int delete_rel(fat_vol_t *v, const char *rel, int depth)
{
    static fat_entry_t e;
    if (depth > 16 || lookup(v, rel, &e) != 0 || !e.name[0]) return -1;
    if (e.is_dir) {
        uint32_t dc = e.cluster;
        for (int round = 0; round < 1024; round++) {        /* in batches of 64 */
            kids_t *k = (kids_t *)kmalloc(sizeof(kids_t));
            if (!k) return -1;
            k->n = 0;
            dir_walk(v, dc, kids_cb, k);
            int n = k->n;
            for (int i = 0; i < n; i++) {
                char child[384];
                snprintf(child, sizeof(child), "%s/%s", rel, k->names[i]);
                if (delete_rel(v, child, depth + 1) != 0) { kfree(k); return -1; }
            }
            kfree(k);
            if (n < 64) break;
        }
        if (lookup(v, rel, &e) != 0) return -1;             /* slots may have moved: look again */
    }
    if (entry_remove(v, &e) != 0) return -1;
    if (e.cluster) fat_free_chain(v, e.cluster);
    return 0;
}

int fatfs_delete(const char *path)
{
    const char *rel;
    fat_vol_t *v = vol_for(path, &rel);
    if (!v) return -1;
    while (*rel == '/') rel++;
    if (!*rel) return -1;                                   /* never the volume root */
    fat_lock();
    char r[256];
    snprintf(r, sizeof(r), "/%s", rel);
    int rc = delete_rel(v, r, 0);
    fsinfo_write(v);
    fat_unlock();
    return rc;
}

/* Rename / move within one volume. */
int fatfs_rename(const char *from, const char *to)
{
    const char *rf, *rt;
    fat_vol_t *v = vol_for(from, &rf), *v2 = vol_for(to, &rt);
    if (!v || v != v2) return -1;
    fat_lock();
    static fat_entry_t e, parent, existing, added;
    char leaf[FAT_NAME_MAX];
    int rc = -1;
    if (lookup(v, rf, &e) != 0 || !e.name[0]) goto out;
    if (lookup_parent(v, rt, &parent, leaf, sizeof(leaf)) != 0) goto out;
    if (lookup(v, rt, &existing) == 0) {
        /* only a change of letter case in the same folder is allowed */
        if (memcmp(existing.short_name, e.short_name, 11) || existing.slot_lba[0] != e.slot_lba[0]) goto out;
    }
    if (dir_add(v, parent.cluster, leaf, e.is_dir, e.cluster, e.size, &added) != 0) goto out;
    if (lookup(v, rf, &e) != 0 || entry_remove(v, &e) != 0) goto out;   /* fresh slot positions */
    if (e.is_dir && e.cluster) {                              /* point ".." at the new parent */
        static uint8_t s[512];
        if (block_read(v->dev, clus_lba(v, e.cluster), 1, s) == 0 && s[32] == '.' && s[33] == '.') {
            uint32_t target = (v->fat32 && parent.cluster == v->root_cluster) ? 0 : parent.cluster;
            put16(s + 32 + 20, (uint16_t)(target >> 16));
            put16(s + 32 + 26, (uint16_t)target);
            block_write(v->dev, clus_lba(v, e.cluster), 1, s);
        }
    }
    rc = 0;
out:
    fat_unlock();
    return rc;
}

/* ---- Volume information (Disks app, This PC) ------------------------------- */
int fatfs_volume_info(const char *mount, char *label, size_t ln, int *fat32, uint64_t *total, uint64_t *free_b)
{
    for (int i = 0; i < FAT_MAX_VOLS; i++)
        if (g_vols[i].used && !strcmp(g_vols[i].mount, mount)) {
            if (label) snprintf(label, ln, "%s", g_vols[i].label);
            if (fat32) *fat32 = g_vols[i].fat32;
            if (total) *total = g_vols[i].total_bytes;
            if (free_b) *free_b = g_vols[i].free_bytes;
            return 0;
        }
    return -1;
}

/* Mount point of the FAT volume on (dev, partition); "" if none. */
const char *fatfs_mount_of(int dev, int part)
{
    for (int i = 0; i < FAT_MAX_VOLS; i++)
        if (g_vols[i].used && g_vols[i].dev == dev && g_vols[i].part == part) return g_vols[i].mount;
    return "";
}

/* Highest cluster in use (for shrinking), or 0 if none. */
uint32_t fatfs_highest_used(const char *mount, uint32_t *clusters, uint32_t *spc)
{
    fat_vol_t *v = NULL;
    for (int i = 0; i < FAT_MAX_VOLS; i++)
        if (g_vols[i].used && !strcmp(g_vols[i].mount, mount)) v = &g_vols[i];
    if (!v) return 0;
    fat_lock();
    uint32_t hi = 0;
    for (uint32_t c = 2; c < v->clusters + 2; c++) if (fat_get(v, c)) hi = c;
    fat_unlock();
    if (clusters) *clusters = v->clusters;
    if (spc) *spc = v->sec_per_clus;
    return hi;
}

/* Every place files can live, for "This PC": the JARVIS disk, then each
 * mounted FAT volume, then disks that hold nothing readable. */
int storage_list(drive_info_t *out, int max)
{
    int n = 0;
    diskfs_stats_t ds;

    if (n < max) {
        drive_info_t *d = &out[n++];
        memset(d, 0, sizeof(*d));
        snprintf(d->mount, sizeof(d->mount), "/jarvis");
        if (diskfs_get_stats(&ds) == 0) {
            snprintf(d->name, sizeof(d->name), "Local Disk");
            snprintf(d->fs, sizeof(d->fs), "JARVISFS");
            snprintf(d->device, sizeof(d->device), "%s", ds.device);
            d->total = ds.total_bytes;
            d->free = ds.total_bytes - ds.used_bytes;
        } else {
            snprintf(d->name, sizeof(d->name), "Memory (not saved)");
            snprintf(d->fs, sizeof(d->fs), "RAM");
        }
    }

    for (int dev = 0; dev < block_count() && n < max; dev++) {
        block_info_t info;
        if (block_get_info(dev, &info) != 0 || dev == diskfs_device()) continue;
        int any = 0;
        for (int i = 0; i < FAT_MAX_VOLS && n < max; i++) {
            fat_vol_t *v = &g_vols[i];
            if (!v->used || v->dev != dev) continue;
            drive_info_t *d = &out[n++];
            memset(d, 0, sizeof(*d));
            d->removable = !strncmp(info.name, "usb", 3);
            snprintf(d->device, sizeof(d->device), "%s", info.name);
            snprintf(d->name, sizeof(d->name), "%s", v->label[0] ? v->label : d->removable ? "USB Drive" : "Volume");
            snprintf(d->mount, sizeof(d->mount), "%s", v->mount);
            snprintf(d->fs, sizeof(d->fs), "%s", v->fat32 ? "FAT32" : "FAT16");
            d->total = v->total_bytes;
            d->free = v->free_bytes;
            any = 1;
        }
        if (!any && n < max) {
            drive_info_t *d = &out[n++];
            memset(d, 0, sizeof(*d));
            d->removable = !strncmp(info.name, "usb", 3);
            snprintf(d->device, sizeof(d->device), "%s", info.name);
            d->total = info.sectors * 512;
            snprintf(d->name, sizeof(d->name), "%s", d->removable ? "USB Drive" : "Disk");
            snprintf(d->fs, sizeof(d->fs), "Unformatted");
        }
    }
    return n;
}

/* ---- Mounting ------------------------------------------------------------ */
static int try_volume(int dev, uint64_t start, int part, const char *mount)
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
    if (total < reserved + nfats * fatsz + v.root_sectors) return -1;
    uint32_t data_secs = total - (reserved + nfats * fatsz + v.root_sectors);
    v.clusters = data_secs / spc;
    if (v.clusters < 4085) return -2;                       /* FAT12 */

    v.dev = dev;
    v.part = part;
    v.start = start;
    v.sec_per_clus = spc;
    v.nfats = nfats;
    v.fatsz = fatsz;
    v.fat32 = v.clusters >= 65525;
    v.fat_start = start + reserved;
    v.root_start = v.fat_start + (uint64_t)nfats * fatsz;
    v.data_start = v.root_start + v.root_sectors;
    v.root_cluster = v.fat32 ? le32(b + 44) : 0;
    v.fsinfo = v.fat32 ? le16(b + 48) : 0;
    v.total_bytes = (uint64_t)v.clusters * spc * 512;
    const uint8_t *lab = b + (v.fat32 ? 71 : 43);
    memcpy(v.label, lab, 11);
    for (int i = 10; i >= 0 && (v.label[i] == ' ' || !v.label[i]); i--) v.label[i] = 0;
    if (!strcmp(v.label, "NO NAME")) v.label[0] = 0;
    snprintf(v.mount, sizeof(v.mount), "%s", mount);
    v.used = 1;

    /* Free space: count the free entries in the allocation table (the
     * FSInfo hint is often stale after other systems wrote the drive). */
    static uint8_t chunk[64 * 512];
    uint32_t per = v.fat32 ? 128 : 256, counted = 0, free_clusters = 0;
    for (uint32_t sec = 0; sec < fatsz && counted < v.clusters + 2; sec += 64) {
        uint32_t n = fatsz - sec < 64 ? fatsz - sec : 64;
        if (block_read(dev, v.fat_start + sec, n, chunk) != 0) break;
        for (uint32_t k = 0; k < n * per && counted < v.clusters + 2; k++, counted++) {
            uint32_t val = v.fat32 ? le32(chunk + k * 4) & 0x0FFFFFFF : le16(chunk + k * 2);
            if (counted >= 2 && val == 0) { if (!free_clusters) v.next_free = counted; free_clusters++; }
        }
    }
    v.free_clusters = free_clusters;
    vol_update_free(&v);

    for (int i = 0; i < FAT_MAX_VOLS; i++)
        if (!g_vols[i].used) { g_vols[i] = v; return 0; }
    return -1;
}

/* Mount every FAT volume on one block device. 0 if at least one mounted. */
int fatfs_mount_dev(int dev)
{
    block_info_t info;
    static uint8_t mbr[512];
    if (block_get_info(dev, &info) != 0 || dev == diskfs_device() || block_read(dev, 0, 1, mbr) != 0) return -1;

    char mount[24];
    snprintf(mount, sizeof(mount), "/jarvis/%s", info.name);
    fat_lock();
    int mounted = 0, unsupported = 0;
    if (try_volume(dev, 0, 0, mount) == 0) mounted = 1;     /* whole drive is one volume */
    else if (mbr[510] == 0x55 && mbr[511] == 0xAA) {
        for (int p = 0; p < 4; p++) {
            const uint8_t *e = mbr + 446 + p * 16;
            uint8_t type = e[4];
            if (type == 0x07) unsupported = 1;
            else if (type == 0x04 || type == 0x06 || type == 0x0E || type == 0x0B || type == 0x0C) {
                if (mounted) snprintf(mount, sizeof(mount), "/jarvis/%sp%d", info.name, p + 1);
                if (try_volume(dev, le32(e + 8), p + 1, mount) == 0) mounted++;
            }
        }
    }
    fat_unlock();

    char msg[96];
    int usb = !strncmp(info.name, "usb", 3);
    if (mounted) {
        for (int i = 0; i < FAT_MAX_VOLS; i++) {
            fat_vol_t *v = &g_vols[i];
            if (!v->used || v->dev != dev) continue;
            vfs_mount_point(v->mount);
            terminal_printf("[FAT] %s: %s volume%s%s, %u MB (%u MB free), mounted at %s\n",
                info.name, v->fat32 ? "FAT32" : "FAT16", v->label[0] ? " " : "", v->label,
                (unsigned)(v->total_bytes >> 20), (unsigned)(v->free_bytes >> 20), v->mount);
        }
        fat_vol_t *first = NULL;
        for (int i = 0; i < FAT_MAX_VOLS && !first; i++) if (g_vols[i].used && g_vols[i].dev == dev) first = &g_vols[i];
        snprintf(msg, sizeof(msg), "%s connected: %s", usb ? "USB drive" : "Disk", first && first->label[0] ? first->label : info.model);
    } else {
        terminal_printf("[FAT] %s: no readable FAT file system%s\n", info.name, unsupported ? " (exFAT/NTFS)" : "");
        snprintf(msg, sizeof(msg), "%s connected, but it is not formatted as FAT (%s). Format it in Disks.", usb ? "USB drive" : "Disk", info.name);
    }
    if (g_kstate.uptime_ticks > 1000 && usb)       /* not for drives present at boot */
        scheduler_post_event(msg);
    g_storage_generation++;
    return mounted ? 0 : -1;
}

void fatfs_unmount_dev(int dev)
{
    fat_lock();
    for (int i = 0; i < FAT_MAX_VOLS; i++) {
        if (!g_vols[i].used || g_vols[i].dev != dev) continue;
        fsinfo_write(&g_vols[i]);
        vfs_unmount_point(g_vols[i].mount);
        terminal_printf("[FAT] %s unmounted\n", g_vols[i].mount);
        g_vols[i].used = 0;
    }
    g_fatsec_dev = -1;
    fat_unlock();
    block_flush(dev);
    g_storage_generation++;
}

void fatfs_mount_all(void)
{
    for (int dev = 0; dev < block_count(); dev++) {
        block_info_t info;
        if (block_get_info(dev, &info) == 0 && dev != diskfs_device())
            fatfs_mount_dev(dev);
    }
}
