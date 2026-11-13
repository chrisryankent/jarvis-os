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
