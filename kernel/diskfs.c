/*
 * =============================================================================
 * JARVIS AI OS — On-disk file system "JARVISFS" (kernel/diskfs.c)
 * Language: C17
 *
 * Makes files survive a reboot. The VFS keeps working copies of files in
 * memory; this module stores them on a block device and loads them back at
 * boot. Changed files are written out by diskfs_sync(), which the desktop
 * calls every couple of seconds and before shutting down.
 *
 * Disk layout (4 KB blocks):
 *   block 0            superblock
 *   bitmap blocks      one bit per block: 1 = in use     (space map)
 *   table blocks       512 file entries of 512 bytes     (path, size, extents)
 *   data blocks        file contents, in up to 14 contiguous extents per file
 *
 * Safety rules:
 *   - A disk is formatted only if it is completely blank. A disk that holds
 *     anything else (a partition table, another file system) is left alone.
 *   - New data is written to freshly allocated blocks before the file's
 *     entry is switched over, so a crash mid-save keeps the old version.
 *   - The space map is rebuilt from the file table at every mount, so a
 *     crash can never leave it inconsistent with the files.
 * There is no journal: a crash can lose changes made since the last sync.
 *
 * Not stored on disk: /jarvis/models (packaged with the system image) and
 * /jarvis/bin (reinstalled from the system image at every boot).
 * =============================================================================
 */

#include "kernel.h"
#include <string.h>
#include <stdio.h>

#define FS_BLOCK        4096
#define FS_SPB          8                   /* sectors per block */
#define FS_MAX_FILES    512
#define FS_MAX_EXTENTS  14
#define FS_MAX_BLOCKS   (1u << 20)          /* 4 GB */
#define FS_VERSION      1

typedef struct {
    char     magic[8];                      /* "JARVISFS" */
    uint32_t version;
    uint32_t block_size;
    uint64_t total_blocks;
    uint32_t bitmap_start, bitmap_blocks;
    uint32_t table_start, table_blocks;
    uint32_t data_start;
} fs_super_t;

typedef struct {
    uint32_t used;
    uint32_t is_dir;
    uint64_t size;
    uint32_t extent_count;
    uint32_t reserved0;
    struct { uint32_t start, count; } extents[FS_MAX_EXTENTS];
    char     path[256];
    uint8_t  reserved[120];
} fs_entry_t;

_Static_assert(sizeof(fs_entry_t) == 512, "file entry must be 512 bytes");

static fs_super_t g_super;
static fs_entry_t g_table[FS_MAX_FILES];
static uint8_t    g_bitmap[FS_MAX_BLOCKS / 8];
static uint8_t    g_io[65536];              /* transfer buffer (16 blocks) */
static int        g_dev = -1;
static int        g_mounted = 0;
static uint64_t   g_used_blocks = 0;
static int        g_full_reported = 0;
static int        g_fs_lock = -1;           /* sync vs. the disk check */

static void fs_lock(void)   { if (g_fs_lock >= 0) kmutex_lock(g_fs_lock); }
static void fs_unlock(void) { if (g_fs_lock >= 0) kmutex_unlock(g_fs_lock); }

/* ---- Block helpers ------------------------------------------------------- */
static int rd(uint64_t block, uint32_t count, void *buf)
{
    return block_read(g_dev, block * FS_SPB, count * FS_SPB, buf);
}

static int wr(uint64_t block, uint32_t count, const void *buf)
{
    return block_write(g_dev, block * FS_SPB, count * FS_SPB, buf);
}

static int  bit_get(uint32_t b) { return g_bitmap[b / 8] & (1u << (b % 8)); }
static void bit_set(uint32_t b) { if (!bit_get(b)) { g_bitmap[b / 8] |= (uint8_t)(1u << (b % 8)); g_used_blocks++; } }
static void bit_clr(uint32_t b) { if (bit_get(b))  { g_bitmap[b / 8] &= (uint8_t)~(1u << (b % 8)); g_used_blocks--; } }

static void free_extents(const fs_entry_t *e)
{
    for (uint32_t i = 0; i < e->extent_count && i < FS_MAX_EXTENTS; i++)
        for (uint32_t b = 0; b < e->extents[i].count; b++)
            bit_clr(e->extents[i].start + b);
}

/* First-fit allocation of `blocks` blocks in at most FS_MAX_EXTENTS runs.
 * Returns 0 and fills e->extents, or -1 (nothing stays allocated). */
static int alloc_extents(fs_entry_t *e, uint32_t blocks)
{
    uint32_t total = (uint32_t)g_super.total_blocks;
    uint32_t b = g_super.data_start;

    e->extent_count = 0;
    while (blocks && b < total && e->extent_count < FS_MAX_EXTENTS) {
        while (b < total && bit_get(b)) b++;
        if (b >= total) break;
        uint32_t start = b, run = 0;
        while (b < total && !bit_get(b) && run < blocks) { b++; run++; }
        e->extents[e->extent_count].start = start;
        e->extents[e->extent_count].count = run;
        e->extent_count++;
        blocks -= run;
    }
    if (blocks) {
        e->extent_count = 0;
        return -1;
    }
    for (uint32_t i = 0; i < e->extent_count; i++)
        for (uint32_t k = 0; k < e->extents[i].count; k++)
            bit_set(e->extents[i].start + k);
    return 0;
}

static int write_entry(int idx)
{
    int first = idx & ~7;                           /* 8 entries per block */
    return wr(g_super.table_start + (uint32_t)idx / 8, 1, &g_table[first]);
}

static int write_bitmap(void)
{
    return wr(g_super.bitmap_start, g_super.bitmap_blocks, g_bitmap);
}

static int persistable(const char *path)
{
    return strncmp(path, "/jarvis/models", 14) != 0 &&
           strncmp(path, "/jarvis/bin", 11) != 0 &&
           strlen(path) < sizeof(g_table[0].path);
}

int diskfs_device(void) { return g_mounted ? g_dev : -1; }

/* ---- Format / mount ------------------------------------------------------ */
static int format(uint64_t total_blocks)
{
    memset(&g_super, 0, sizeof(g_super));
    memcpy(g_super.magic, "JARVISFS", 8);
    g_super.version = FS_VERSION;
    g_super.block_size = FS_BLOCK;
    g_super.total_blocks = total_blocks;
    g_super.bitmap_start = 1;
    g_super.bitmap_blocks = (uint32_t)((total_blocks + FS_BLOCK * 8 - 1) / (FS_BLOCK * 8));
    g_super.table_start = g_super.bitmap_start + g_super.bitmap_blocks;
    g_super.table_blocks = FS_MAX_FILES * sizeof(fs_entry_t) / FS_BLOCK;
    g_super.data_start = g_super.table_start + g_super.table_blocks;

    memset(g_table, 0, sizeof(g_table));
    memset(g_io, 0, FS_BLOCK);
    memcpy(g_io, &g_super, sizeof(g_super));

    if (wr(g_super.table_start, g_super.table_blocks, g_table) != 0 || wr(0, 1, g_io) != 0)
        return -1;
    terminal_printf("[FS] Formatted blank disk: %llu MB\n",
        (unsigned long long)(total_blocks * FS_BLOCK >> 20));
    return 0;
}

static void rebuild_bitmap(void)
{
    memset(g_bitmap, 0, sizeof(g_bitmap));
    g_used_blocks = 0;
    for (uint32_t b = 0; b < g_super.data_start; b++)
        bit_set(b);                                 /* metadata */
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (g_table[i].used)
            for (uint32_t x = 0; x < g_table[i].extent_count && x < FS_MAX_EXTENTS; x++)
                for (uint32_t k = 0; k < g_table[i].extents[x].count; k++)
                    if (g_table[i].extents[x].start + k < g_super.total_blocks)
                        bit_set(g_table[i].extents[x].start + k);
}

static int load_file(int idx)
{
    fs_entry_t *e = &g_table[idx];
    uint8_t *data = vfs_restore_node(e->path, (int)e->is_dir, (size_t)e->size, idx);
    if (!data) return -1;

    uint64_t left = e->size;
    for (uint32_t x = 0; x < e->extent_count && left; x++) {
        for (uint32_t k = 0; k < e->extents[x].count && left; k += 16) {
            uint32_t n = e->extents[x].count - k;
            if (n > 16) n = 16;
            if (rd(e->extents[x].start + k, n, g_io) != 0) return -1;
            uint64_t bytes = (uint64_t)n * FS_BLOCK;
            if (bytes > left) bytes = left;
            memcpy(data, g_io, bytes);
            data += bytes;
            left -= bytes;
        }
    }
    return left ? -1 : 0;
}

void diskfs_mount(void)
{
    block_info_t info;

    if (block_count() == 0 || block_get_info(0, &info) != 0) {
        terminal_puts("[FS] No disk: files are kept in memory only and will not survive a restart\n");
        return;
    }
    g_dev = 0;

    uint64_t total_blocks = info.sectors / FS_SPB;
    if (total_blocks > FS_MAX_BLOCKS) total_blocks = FS_MAX_BLOCKS;
    if (total_blocks < 1024) {
        terminal_puts("[FS] Disk is too small (need at least 4 MB)\n");
        return;
    }

    if (rd(0, 16, g_io) != 0) {
        terminal_puts("[FS] Could not read the disk\n");
        return;
    }
    memcpy(&g_super, g_io, sizeof(g_super));

    if (memcmp(g_super.magic, "JARVISFS", 8) != 0) {
        int blank = 1;
        for (size_t i = 0; i < sizeof(g_io); i++)
            if (g_io[i]) { blank = 0; break; }
        if (!blank) {
            terminal_puts("[FS] Disk contains data that is not a JARVIS file system; leaving it untouched\n");
            return;
        }
        if (format(total_blocks) != 0) {
            terminal_puts("[FS] Format failed\n");
            return;
        }
    } else if (g_super.version != FS_VERSION || g_super.block_size != FS_BLOCK ||
               g_super.total_blocks > total_blocks ||
               g_super.table_blocks != FS_MAX_FILES * sizeof(fs_entry_t) / FS_BLOCK) {
        terminal_puts("[FS] Unsupported file system version; leaving the disk untouched\n");
        return;
    } else if (rd(g_super.table_start, g_super.table_blocks, g_table) != 0) {
        terminal_puts("[FS] Could not read the file table\n");
        return;
    }

    rebuild_bitmap();
    g_mounted = 1;
    if (g_fs_lock < 0) g_fs_lock = kmutex_create("jarvisfs");

    int files = 0, bad = 0;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!g_table[i].used) continue;
        g_table[i].path[sizeof(g_table[i].path) - 1] = 0;
        if (load_file(i) == 0) files++; else bad++;
    }
    terminal_printf("[FS] Mounted %s (%s): %d item(s) restored, %llu of %llu MB used\n",
        info.name, info.model, files,
        (unsigned long long)(g_used_blocks * FS_BLOCK >> 20),
        (unsigned long long)(g_super.total_blocks * FS_BLOCK >> 20));
    if (bad)
        terminal_printf("[FS] %d item(s) could not be read\n", bad);
}

/* ---- Sync ---------------------------------------------------------------- */
static int save_slot(int slot, const vfs_slot_info_t *s)
{
    int idx = s->disk_idx;
    if (idx < 0) {
        for (int i = 0; i < FS_MAX_FILES; i++)
            if (!g_table[i].used) { idx = i; break; }
        if (idx < 0) return -1;
    }

    fs_entry_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    fresh.used = 1;
    fresh.is_dir = (uint32_t)s->is_dir;
    fresh.size = s->is_dir ? 0 : s->size;
    strncpy(fresh.path, s->path, sizeof(fresh.path) - 1);

    uint32_t blocks = (uint32_t)((fresh.size + FS_BLOCK - 1) / FS_BLOCK);
    if (blocks && alloc_extents(&fresh, blocks) != 0)
        return -1;

    /* 1. data into the new blocks */
    uint64_t off = 0;
    for (uint32_t x = 0; x < fresh.extent_count; x++) {
        for (uint32_t k = 0; k < fresh.extents[x].count; k += 16) {
            uint32_t n = fresh.extents[x].count - k;
            if (n > 16) n = 16;
            memset(g_io, 0, (size_t)n * FS_BLOCK);
            if (vfs_slot_read(slot, off, g_io, (size_t)n * FS_BLOCK, s->gen) < 0 ||
                wr(fresh.extents[x].start + k, n, g_io) != 0) {
                free_extents(&fresh);       /* changed under us, or I/O error */
                return -2;
            }
            off += (uint64_t)n * FS_BLOCK;
        }
    }

    /* 2. switch the entry over, 3. release the old blocks */
    fs_entry_t old = g_table[idx];
    g_table[idx] = fresh;
    if (write_entry(idx) != 0) {
        g_table[idx] = old;
        free_extents(&fresh);
        return -2;
    }
    if (old.used) free_extents(&old);
    vfs_slot_synced(slot, idx, s->gen);
    return 0;
}

/* Write every change to disk. Returns the number of items written. */
int diskfs_sync(void)
{
    if (!g_mounted) return 0;

    fs_lock();
    int changed = 0, idx;
    while (vfs_take_deleted(&idx)) {
        if (idx >= 0 && idx < FS_MAX_FILES && g_table[idx].used) {
            free_extents(&g_table[idx]);
            memset(&g_table[idx], 0, sizeof(g_table[idx]));
            write_entry(idx);
            changed++;
        }
    }

    int slots = vfs_slot_max();
    for (int i = 0; i < slots; i++) {
        vfs_slot_info_t s;
        if (vfs_slot_info(i, &s) != 0 || !s.dirty || !persistable(s.path))
            continue;
        int rc = save_slot(i, &s);
        if (rc == 0) {
            changed++;
        } else if (rc == -1 && !g_full_reported) {
            g_full_reported = 1;
            scheduler_post_event("Disk is full: recent changes could not be saved");
        }
    }

    if (changed) {
        write_bitmap();
        block_flush(g_dev);
    }
    fs_unlock();
    return changed;
}

int diskfs_get_stats(diskfs_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    out->mounted = g_mounted;
    if (!g_mounted) return -1;

    block_info_t info;
    block_get_info(g_dev, &info);
    strncpy(out->device, info.name, sizeof(out->device) - 1);
    strncpy(out->model, info.model, sizeof(out->model) - 1);
    out->total_bytes = g_super.total_blocks * FS_BLOCK;
    out->used_bytes = g_used_blocks * FS_BLOCK;
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (g_table[i].used) out->items++;
    return 0;
}

/* ---- Resize (Disks: Extend / Shrink volume) --------------------------------
 * The JARVIS volume starts at sector 0 and may end before the disk does.
 * Growing is limited by the disk and by the space map's capacity (one bit
 * per block in the bitmap blocks reserved at format time); shrinking needs
 * the blocks past the new end to be free. Returns 0, or -1 with the reason. */
uint64_t diskfs_max_blocks(void)
{
    if (!g_mounted) return 0;
    block_info_t info;
    if (block_get_info(g_dev, &info) != 0) return 0;
    uint64_t m = info.sectors / FS_SPB;
    uint64_t cap = (uint64_t)g_super.bitmap_blocks * FS_BLOCK * 8;
    if (m > cap) m = cap;
    if (m > FS_MAX_BLOCKS) m = FS_MAX_BLOCKS;
    return m;
}

uint64_t diskfs_min_blocks(void)
{
    if (!g_mounted) return 0;
    uint64_t hi = g_super.data_start;
    for (uint64_t b = g_super.data_start; b < g_super.total_blocks; b++)
        if (bit_get((uint32_t)b)) hi = b + 1;
    /* keep a margin so the next saves have room (files are rewritten into
     * fresh blocks before the old ones are released) */
    uint64_t margin = g_used_blocks / 4 + 256;
    return hi + margin;
}

int diskfs_resize(uint64_t new_blocks, char *err, size_t n)
{
    if (!g_mounted) { snprintf(err, n, "no JARVIS disk is mounted"); return -1; }
    fs_lock();
    uint64_t maxb = diskfs_max_blocks(), minb = diskfs_min_blocks();
    int rc = -1;
    if (new_blocks > maxb) snprintf(err, n, "the largest possible size is %u MB", (unsigned)(maxb * FS_BLOCK >> 20));
    else if (new_blocks < minb) snprintf(err, n, "files use the disk up to %u MB; the smallest possible size is %u MB",
                                          (unsigned)((minb - g_used_blocks / 4 - 256) * FS_BLOCK >> 20), (unsigned)(minb * FS_BLOCK >> 20));
    else {
        uint64_t old = g_super.total_blocks;
        g_super.total_blocks = new_blocks;
        memset(g_io, 0, FS_BLOCK);
        memcpy(g_io, &g_super, sizeof(g_super));
        if (wr(0, 1, g_io) != 0) { g_super.total_blocks = old; snprintf(err, n, "could not write the superblock"); }
        else {
            rc = 0;
            terminal_printf("[FS] Volume resized from %u MB to %u MB\n", (unsigned)(old * FS_BLOCK >> 20), (unsigned)(new_blocks * FS_BLOCK >> 20));
        }
    }
    fs_unlock();
    return rc;
}

/* ---- Consistency check ("Check disk") -------------------------------------
 * Runs as its own process so the desktop keeps working. Read-only unless
 * `repair` is set; repairs use the intact copies the VFS holds in memory:
 *   - a file whose entry, blocks or contents are wrong is saved again into
 *     fresh blocks (its memory copy is the one the user sees and edits)
 *   - entries no file refers to are released
 *   - the space map is rewritten from the file table
 * -------------------------------------------------------------------------- */
static diskfs_check_t g_chk;
static int g_chk_surface, g_chk_repair;
static fs_entry_t g_chk_disk_table[FS_MAX_FILES];
static uint8_t g_chk_owned[FS_MAX_BLOCKS / 8];
static uint8_t g_chk_buf[65536], g_chk_mem[65536];

static void chk_issue(const char *fmt, const char *a, long n)
{
    g_chk.problems++;
    if (g_chk.issue_count >= DISKFS_CHECK_ISSUES) return;
    char *o = g_chk.issue[g_chk.issue_count++];
    /* fmt holds at most one %s and one %ld (kernel snprintf has no widths) */
    snprintf(o, sizeof(g_chk.issue[0]), fmt, a ? a : "", n);
}

static int chk_entry_blocks(const fs_entry_t *e)
{
    uint64_t sum = 0;
    for (uint32_t x = 0; x < e->extent_count && x < FS_MAX_EXTENTS; x++) sum += e->extents[x].count;
    return (int)sum;
}

static void diskfs_check_process(void)
{
    int need_save[FS_MAX_FILES];
    int referenced[FS_MAX_FILES];
    memset(need_save, 0, sizeof(need_save));
    memset(referenced, 0, sizeof(referenced));
    uint64_t total = g_super.total_blocks;

    /* 1. superblock and file table as stored on the disk */
    g_chk.phase = "Reading the file table";
    fs_lock();
    if (block_read(g_dev, 0, FS_SPB, g_chk_buf) != 0) { g_chk.read_errors++; chk_issue("The superblock cannot be read", 0, 0); }
    else if (memcmp(g_chk_buf, &g_super, sizeof(g_super)) != 0) chk_issue("The superblock on disk differs from the one in use", 0, 0);
    if (block_read(g_dev, (uint64_t)g_super.table_start * FS_SPB, g_super.table_blocks * FS_SPB, g_chk_disk_table) != 0) {
        g_chk.read_errors++;
        chk_issue("The file table cannot be read", 0, 0);
        memcpy(g_chk_disk_table, g_table, sizeof(g_table));
    }
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (memcmp(&g_chk_disk_table[i], &g_table[i], sizeof(fs_entry_t)) != 0) {
            chk_issue("Entry %s%ld was not written correctly", "#", i);
            if (g_table[i].used) need_save[i] = 1;
        }

    /* 2. every entry: path, size, extents, overlaps */
    memset(g_chk_owned, 0, sizeof(g_chk_owned));
    for (int i = 0; i < FS_MAX_FILES; i++) {
        fs_entry_t *e = &g_table[i];
        if (!e->used) continue;
        if (e->is_dir) g_chk.dirs++; else g_chk.files++;
        if (e->path[0] != '/' || !memchr(e->path, 0, sizeof(e->path))) { chk_issue("Entry #%s%ld has a damaged name", "", i); continue; }
        for (int j = 0; j < i; j++)
            if (g_table[j].used && !strcmp(g_table[j].path, e->path)) { chk_issue("%s is stored twice (%ld)", e->path, j); need_save[i] = 1; }
        if (e->is_dir && (e->size || e->extent_count)) { chk_issue("Folder %s claims data blocks", e->path, 0); need_save[i] = 1; }
        if (e->extent_count > FS_MAX_EXTENTS) { chk_issue("%s has %ld block runs (damaged)", e->path, (long)e->extent_count); need_save[i] = 1; continue; }
        if ((uint64_t)chk_entry_blocks(e) != (e->size + FS_BLOCK - 1) / FS_BLOCK) {
            chk_issue("%s: size and blocks disagree (%ld blocks)", e->path, chk_entry_blocks(e));
            need_save[i] = 1;
        }
        int shared = 0;
        for (uint32_t x = 0; x < e->extent_count; x++) {
            uint64_t st = e->extents[x].start, n = e->extents[x].count;
            if (st < g_super.data_start || st + n > total) { chk_issue("%s points outside the data area (block %ld)", e->path, (long)st); need_save[i] = 1; continue; }
            for (uint64_t b = st; b < st + n; b++) {
                if (g_chk_owned[b / 8] & (1u << (b % 8))) { g_chk.overlaps++; need_save[i] = 1; shared = 1; }
                g_chk_owned[b / 8] |= (uint8_t)(1u << (b % 8));
            }
        }
        if (shared) chk_issue("%s shares blocks with another file", e->path, 0);
    }
    fs_unlock();
    g_chk.progress = 10;

    /* 3. which entries the files in memory refer to */
    int slots = vfs_slot_max();
    for (int s = 0; s < slots; s++) {
        vfs_slot_info_t si;
        if (vfs_slot_info(s, &si) == 0 && si.disk_idx >= 0 && si.disk_idx < FS_MAX_FILES) referenced[si.disk_idx] = 1;
    }
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (g_table[i].used && !referenced[i]) chk_issue("%s is on disk but no longer in use (entry %ld)", g_table[i].path, i);

    /* 4. the space map on disk against the file table */
    g_chk.phase = "Checking the space map";
    uint64_t bm_bytes = (uint64_t)g_super.bitmap_blocks * FS_BLOCK;
    for (uint64_t off = 0; off < bm_bytes; off += sizeof(g_chk_buf)) {
        uint32_t n = (uint32_t)((bm_bytes - off) < sizeof(g_chk_buf) ? (bm_bytes - off) : sizeof(g_chk_buf));
        if (block_read(g_dev, (uint64_t)g_super.bitmap_start * FS_SPB + off / 512, n / 512, g_chk_buf) != 0) { g_chk.read_errors++; break; }
        fs_lock();
        for (uint32_t k = 0; k < n && off + k < sizeof(g_bitmap); k++) {
            uint8_t d = (uint8_t)(g_chk_buf[k] ^ g_bitmap[off + k]);
            while (d) { g_chk.bitmap_diff += d & 1; d >>= 1; }
        }
        fs_unlock();
    }
    /* blocks a file owns must be marked used, or a later save could overwrite them */
    {
        uint64_t unprotected = 0;
        fs_lock();
        for (uint64_t b = g_super.data_start; b < total; b++)
            if ((g_chk_owned[b / 8] & (1u << (b % 8))) && !bit_get((uint32_t)b)) unprotected++;
        fs_unlock();
        if (unprotected) chk_issue("%s%ld blocks in use are marked free", "", (long)unprotected);
    }
    g_chk.progress = 15;

    /* 5. contents: every saved file read back and compared with memory */
    g_chk.phase = "Comparing file contents";
    for (int s = 0; s < slots; s++) {
        vfs_slot_info_t si;
        if (vfs_slot_info(s, &si) != 0 || si.is_dir || si.dirty || si.disk_idx < 0 || si.disk_idx >= FS_MAX_FILES) continue;
        fs_lock();
        fs_entry_t e = g_table[si.disk_idx];
        int bad = 0, changed = 0;
        if (e.used && !need_save[si.disk_idx] && e.size == si.size) {
            uint64_t off = 0;
            for (uint32_t x = 0; x < e.extent_count && !bad && !changed && off < e.size; x++)
                for (uint32_t k = 0; k < e.extents[x].count && off < e.size; k += 16) {
                    uint32_t n = e.extents[x].count - k;
                    if (n > 16) n = 16;
                    if (block_read(g_dev, (uint64_t)(e.extents[x].start + k) * FS_SPB, n * FS_SPB, g_chk_buf) != 0) {
                        g_chk.read_errors++; bad = 1; break;
                    }
                    g_chk.blocks_read += n;
                    size_t want = (size_t)n * FS_BLOCK;
                    if (want > e.size - off) want = (size_t)(e.size - off);
                    int got = vfs_slot_read(s, off, g_chk_mem, want, si.gen);
                    if (got < 0) { changed = 1; break; }        /* edited meanwhile: skip */
                    if ((size_t)got != want || memcmp(g_chk_buf, g_chk_mem, want)) { bad = 1; break; }
                    off += want;
                }
        } else if (e.used && e.size != si.size && !need_save[si.disk_idx]) bad = 1;
        fs_unlock();
        if (bad && !changed) {
            g_chk.content_mismatch++;
            need_save[si.disk_idx] = 1;
            chk_issue("%s on disk differs from the file in use", si.path, 0);
        }
        g_chk.progress = 15 + (int)((long)s * (g_chk_surface ? 35 : 80) / (slots ? slots : 1));
    }

    /* 6. optional surface scan: read every block of the disk */
    if (g_chk_surface) {
        g_chk.phase = "Reading every block";
        for (uint64_t b = 0; b < total; b += 16) {
            uint32_t n = (uint32_t)(total - b < 16 ? total - b : 16);
            if (block_read(g_dev, b * FS_SPB, n * FS_SPB, g_chk_buf) != 0) {
                g_chk.read_errors++;
                if (g_chk.read_errors <= 3) chk_issue("Unreadable blocks near %s%ld", "block ", (long)b);
            }
            g_chk.blocks_read += n;
            g_chk.progress = 50 + (int)(b * 45 / total);
            if ((b & 1023) == 0) kthread_yield();
        }
    }

    /* 7. repair */
    if (g_chk_repair) {
        g_chk.phase = "Repairing";
        fs_lock();
        for (int i = 0; i < FS_MAX_FILES; i++) {
            if (g_table[i].used && !referenced[i]) {        /* release unused entries */
                free_extents(&g_table[i]);
                memset(&g_table[i], 0, sizeof(g_table[i]));
                write_entry(i);
                g_chk.repaired++;
            }
        }
        rebuild_bitmap();                                   /* drops blocks of damaged entries too */
        for (int s = 0; s < slots; s++) {
            vfs_slot_info_t si;
            if (vfs_slot_info(s, &si) == 0 && si.disk_idx >= 0 && si.disk_idx < FS_MAX_FILES && need_save[si.disk_idx]) {
                memset(&g_table[si.disk_idx], 0, sizeof(fs_entry_t));      /* forget the bad blocks */
                rebuild_bitmap();
                if (save_slot(s, &si) == 0) g_chk.repaired++;
            }
        }
        if (write_bitmap() == 0) g_chk.bitmap_diff = 0;
        block_flush(g_dev);
        fs_unlock();
    }

    /* the space map is rebuilt at every start, so a stale one is not a fault */
    if (g_chk.problems == 0 && g_chk.read_errors == 0)
        snprintf(g_chk.summary, sizeof(g_chk.summary), "No problems found.");
    else if (g_chk_repair)
        snprintf(g_chk.summary, sizeof(g_chk.summary), "%d problem(s) found, %d repaired.", g_chk.problems, g_chk.repaired);
    else
        snprintf(g_chk.summary, sizeof(g_chk.summary), "%d problem(s) found. Run the check with Repair to fix them.", g_chk.problems);
    g_chk.progress = 100;
    g_chk.phase = "Finished";
    g_chk.running = 0;
    g_chk.done = 1;
    g_storage_generation++;
    scheduler_post_event("Disk check finished");
    kthread_exit(0);
}

/* Start a check (surface: also read every block; repair: fix what is found).
 * Returns the checker's PID, or 0 if no JARVIS disk or a check is running. */
uint32_t diskfs_check_start(int surface, int repair)
{
    if (!g_mounted || g_chk.running) return 0;
    if (repair) diskfs_sync();                  /* save pending changes first */
    memset(&g_chk, 0, sizeof(g_chk));
    g_chk.running = 1;
    g_chk.surface = surface;
    g_chk.repair = repair;
    g_chk.phase = "Starting";
    g_chk_surface = surface;
    g_chk_repair = repair;
    uint32_t pid = proc_spawn("disk-check", diskfs_check_process, PRIO_USER, 0);
    if (!pid) g_chk.running = 0;
    g_chk.pid = pid;
    return pid;
}

void diskfs_check_status(diskfs_check_t *out) { *out = g_chk; }
