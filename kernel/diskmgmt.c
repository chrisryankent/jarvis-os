/*
 * =============================================================================
 * JARVIS AI OS — Disk management (kernel/diskmgmt.c)
 * Language: C17
 *
 * The operations behind the Disks app, the Terminal's "disk" command and
 * JARVIS's disk_action tool:
 *
 *   info        partition layout (MBR, GPT read-only, or one volume)
 *   init        new, empty MBR partition table (erases the disk)
 *   create      new partition in free space (1 MB aligned), then format
 *   delete      remove a partition
 *   format      FAT32 or FAT16, sized by the volume; label; mounted again
 *   label       rename a FAT volume
 *   extend      grow a partition into the free space after it, and its
 *               file system with it (FAT tables are sized at format time
 *               to cover the whole rest of the disk, so this works on any
 *               volume formatted here)
 *   shrink      shrink a partition when no data lies past the new end
 *   JARVIS disk the system volume can be extended and shrunk (resize);
 *               it cannot be deleted or formatted while the system runs
 *
 * Every operation unmounts the volumes of the disk first and mounts them
 * again afterwards, so no stale view of the disk survives.
 * =============================================================================
 */

#include "kernel.h"
#include <string.h>
#include <stdio.h>

static uint8_t g_s[512];

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static const char *mbr_type_name(uint8_t t)
{
    switch (t) {
    case 0x01: return "FAT12";
    case 0x04: case 0x06: case 0x0E: return "FAT16";
    case 0x0B: case 0x0C: return "FAT32";
    case 0x07: return "NTFS/exFAT";
    case 0x05: case 0x0F: return "Extended";
    case 0x82: return "Linux swap";
    case 0x83: return "Linux";
    case 0xEE: return "GPT";
    case 0xEF: return "EFI system";
    default:   return "Other";
    }
}

/* What the boot sector at lba holds. */
static void probe_fs(int dev, uint64_t lba, char *fs, size_t n, char *label, size_t ln)
{
    snprintf(fs, n, "Unformatted");
    if (label) label[0] = 0;
    if (block_read(dev, lba, 1, g_s) != 0) { snprintf(fs, n, "Unreadable"); return; }
    if (!memcmp(g_s, "JARVISFS", 8)) { snprintf(fs, n, "JARVISFS"); return; }
    if (!memcmp(g_s + 3, "NTFS", 4)) { snprintf(fs, n, "NTFS"); return; }
    if (!memcmp(g_s + 3, "EXFAT", 5)) { snprintf(fs, n, "exFAT"); return; }
    if (g_s[510] == 0x55 && g_s[511] == 0xAA && le16(g_s + 11) == 512 && g_s[13] && g_s[16]) {
        int fat32 = le16(g_s + 22) == 0;
        snprintf(fs, n, "%s", fat32 ? "FAT32" : "FAT16");
        if (label) {
            const uint8_t *l = g_s + (fat32 ? 71 : 43);
            int k = 0;
            for (; k < 11 && k < (int)ln - 1; k++) label[k] = (char)l[k];
            label[k] = 0;
            while (k > 0 && (label[k - 1] == ' ' || !label[k - 1])) label[--k] = 0;
            if (!strcmp(label, "NO NAME")) label[0] = 0;
        }
        return;
    }
    int blank = 1;
    for (int i = 0; i < 512; i++) if (g_s[i]) { blank = 0; break; }
    if (!blank) snprintf(fs, n, "Unknown");
}

int dm_disk_info(int dev, dm_disk_t *d)
{
    memset(d, 0, sizeof(*d));
    block_info_t info;
    if (block_get_info(dev, &info) != 0) return -1;
    d->dev = dev;
    snprintf(d->name, sizeof(d->name), "%s", info.name);
    snprintf(d->model, sizeof(d->model), "%s", info.model);
    d->sectors = info.sectors;
    d->system = dev == diskfs_device();
    d->removable = !strncmp(info.name, "usb", 3);

    static uint8_t mbr[512];
    if (block_read(dev, 0, 1, mbr) != 0) { d->scheme = DM_UNREADABLE; return 0; }
    if (!memcmp(mbr, "JARVISFS", 8)) {
        d->scheme = DM_WHOLE;
        dm_part_t *p = &d->parts[d->nparts++];
        p->num = 0;
        p->start = 0;
        p->sectors = diskfs_max_blocks() ? 0 : 0;
        diskfs_stats_t st;
        if (diskfs_get_stats(&st) == 0) p->sectors = st.total_bytes / 512;
        else p->sectors = info.sectors;
        snprintf(p->fs, sizeof(p->fs), "JARVISFS");
        snprintf(p->label, sizeof(p->label), "Local Disk");
        snprintf(p->mount, sizeof(p->mount), "/jarvis");
        return 0;
    }
    char fs[16], label[16];
    probe_fs(dev, 0, fs, sizeof(fs), label, sizeof(label));
    if (!strncmp(fs, "FAT", 3)) {                           /* one volume, no partition table */
        d->scheme = DM_WHOLE;
        dm_part_t *p = &d->parts[d->nparts++];
        p->num = 0;
        p->start = 0;
        p->sectors = le16(g_s + 19) ? le16(g_s + 19) : le32(g_s + 32);
        snprintf(p->fs, sizeof(p->fs), "%s", fs);
        snprintf(p->label, sizeof(p->label), "%s", label);
        snprintf(p->mount, sizeof(p->mount), "%s", fatfs_mount_of(dev, 0));
        return 0;
    }
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) { d->scheme = DM_EMPTY; return 0; }
    if (mbr[446 + 4] == 0xEE) {                             /* protective MBR: GPT */
        d->scheme = DM_GPT;
        static uint8_t hdr[512], ent[512];
        if (block_read(dev, 1, 1, hdr) != 0 || memcmp(hdr, "EFI PART", 8)) return 0;
        uint64_t lba = (uint64_t)le32(hdr + 72) | ((uint64_t)le32(hdr + 76) << 32);
        uint32_t count = le32(hdr + 80), esz = le32(hdr + 84);
        if (esz < 128 || esz > 512) return 0;
        for (uint32_t i = 0; i < count && i < 128 && d->nparts < DM_MAX_PARTS; i++) {
            if (block_read(dev, lba + (i * esz) / 512, 1, ent) != 0) break;
            const uint8_t *e = ent + (i * esz) % 512;
            int empty = 1;
            for (int k = 0; k < 16; k++) if (e[k]) empty = 0;
            if (empty) continue;
            dm_part_t *p = &d->parts[d->nparts++];
            p->num = (int)i + 1;
            p->start = (uint64_t)le32(e + 32) | ((uint64_t)le32(e + 36) << 32);
            uint64_t end = (uint64_t)le32(e + 40) | ((uint64_t)le32(e + 44) << 32);
            p->sectors = end >= p->start ? end - p->start + 1 : 0;
            probe_fs(dev, p->start, p->fs, sizeof(p->fs), p->label, sizeof(p->label));
            snprintf(p->mount, sizeof(p->mount), "%s", fatfs_mount_of(dev, p->num));
        }
        return 0;
    }
    d->scheme = DM_MBR;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = mbr + 446 + i * 16;
        if (!e[4] || !le32(e + 12)) continue;
        dm_part_t *p = &d->parts[d->nparts++];
        p->num = i + 1;
        p->type = e[4];
        p->bootable = e[0] == 0x80;
        p->start = le32(e + 8);
        p->sectors = le32(e + 12);
        probe_fs(dev, p->start, p->fs, sizeof(p->fs), p->label, sizeof(p->label));
        if (!strcmp(p->fs, "Unformatted") && e[4] != 0x0C && e[4] != 0x0E) snprintf(p->fs, sizeof(p->fs), "%s", mbr_type_name(e[4]));
        snprintf(p->mount, sizeof(p->mount), "%s", fatfs_mount_of(dev, p->num));
    }
    /* sort by start */
    for (int a = 1; a < d->nparts; a++) {
        dm_part_t t = d->parts[a];
        int b = a - 1;
        while (b >= 0 && d->parts[b].start > t.start) { d->parts[b + 1] = d->parts[b]; b--; }
        d->parts[b + 1] = t;
    }
    return 0;
}

/* Free space after a partition (or the largest gap if num < 0). */
static uint64_t free_after(const dm_disk_t *d, uint64_t from)
{
    uint64_t next = d->sectors;
    for (int i = 0; i < d->nparts; i++)
        if (d->parts[i].start >= from && d->parts[i].start < next) next = d->parts[i].start;
    return next > from ? next - from : 0;
}

static dm_part_t *find_part(dm_disk_t *d, int num)
{
    for (int i = 0; i < d->nparts; i++) if (d->parts[i].num == num) return &d->parts[i];
    return NULL;
}

static int check_dev(int dev, char *err, size_t n, int allow_system)
{
    block_info_t info;
    if (block_get_info(dev, &info) != 0) { snprintf(err, n, "there is no disk %d", dev); return -1; }
    if (!allow_system && dev == diskfs_device()) {
        snprintf(err, n, "%s is the system disk; JARVIS OS is running from it", info.name);
        return -1;
    }
    return 0;
}

static void remount(int dev)
{
    block_flush(dev);
    fatfs_mount_dev(dev);
    g_storage_generation++;
}

static int write_mbr_entry(int dev, int slot, uint8_t type, uint32_t start, uint32_t count)
{
    static uint8_t mbr[512];
    if (block_read(dev, 0, 1, mbr) != 0) return -1;
    uint8_t *e = mbr + 446 + slot * 16;
    memset(e, 0, 16);
    if (count) {
        e[0] = 0;
        e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;              /* CHS: use LBA */
        e[4] = type;
        e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
        put32(e + 8, start);
        put32(e + 12, count);
    }
    mbr[510] = 0x55; mbr[511] = 0xAA;
    return block_write(dev, 0, 1, mbr);
}

int dm_init_mbr(int dev, char *err, size_t n)
{
    if (check_dev(dev, err, n, 0)) return -1;
    fatfs_unmount_dev(dev);
    static uint8_t mbr[512];
    memset(mbr, 0, sizeof(mbr));
    uint32_t sig = (uint32_t)(sys_unix_time() ^ 0x4A415256u);
    put32(mbr + 440, sig);
    mbr[510] = 0x55; mbr[511] = 0xAA;
    int rc = block_write(dev, 0, 1, mbr);
    /* clear the next sectors too: an old GPT header or boot sector must not linger */
    memset(mbr, 0, sizeof(mbr));
    for (int s = 1; s < 64 && !rc; s++) rc = block_write(dev, (uint64_t)s, 1, mbr);
    remount(dev);
    if (rc) snprintf(err, n, "the disk did not accept the write");
    else terminal_printf("[DISK] disk %d: new MBR partition table\n", dev);
    return rc ? -1 : 0;
}

/* ---- FAT formatting -------------------------------------------------------- */
static int zero_sectors(int dev, uint64_t lba, uint64_t count)
{
    static uint8_t z[32768];
    memset(z, 0, sizeof(z));
    while (count) {
        uint32_t k = count > 64 ? 64 : (uint32_t)count;
        if (block_write(dev, lba, k, z) != 0) return -1;
        lba += k;
        count -= k;
    }
    return 0;
}

/* Formats [start, start+sectors) as FAT. max_sectors sizes the FAT tables
 * so the volume can later be extended up to that size. */
static int format_fat(int dev, uint64_t start, uint64_t sectors, uint64_t max_sectors, const char *want, const char *label, char *err, size_t n)
{
    if (max_sectors < sectors) max_sectors = sectors;
    if (sectors > 0xFFFFFFFFull) { snprintf(err, n, "volumes over 2 TB are not supported"); return -1; }
    int fat32;
    if (!strcmp(want, "fat16")) fat32 = 0;
    else if (!strcmp(want, "fat32")) fat32 = 1;
    else fat32 = sectors >= 128 * 2048;                     /* auto: FAT32 from 128 MB */
    uint32_t spc;
    uint64_t mb = sectors / 2048;
    if (fat32) spc = mb < 260 ? 1 : mb <= 8192 ? 8 : mb <= 16384 ? 16 : mb <= 32768 ? 32 : 64;
    else {
        spc = 1;
        while (spc < 128 && sectors / spc > 65000) spc <<= 1;
    }
    uint32_t reserved = fat32 ? 32 : 4, nfats = 2, root_secs = fat32 ? 0 : 32;
    /* FAT size for the largest size this volume may grow to */
    uint64_t fatsz = 1;
    for (int it = 0; it < 32; it++) {
        uint64_t data = max_sectors - reserved - nfats * fatsz - root_secs;
        uint64_t clusters = data / spc;
        uint64_t need = ((clusters + 2) * (fat32 ? 4 : 2) + 511) / 512;
        if (need == fatsz) break;
        fatsz = need;
    }
    if (reserved + nfats * fatsz + root_secs >= sectors) { snprintf(err, n, "the volume is too small"); return -1; }
    uint64_t clusters = (sectors - reserved - nfats * fatsz - root_secs) / spc;
    if (fat32 && clusters < 65525) {
        if (!strcmp(want, "fat32")) { snprintf(err, n, "FAT32 needs a volume of at least 33 MB here; use fat16"); return -1; }
        return format_fat(dev, start, sectors, max_sectors, "fat16", label, err, n);
    }
    if (!fat32 && (clusters < 4085 || clusters > 65524)) {
        if (clusters > 65524 && strcmp(want, "fat16")) return format_fat(dev, start, sectors, max_sectors, "fat32", label, err, n);
        snprintf(err, n, clusters < 4085 ? "the volume is too small for FAT16 (at least 2 MB)" : "the volume is too big for FAT16; use fat32");
        return -1;
    }
    if (!fat32 && fatsz > 0xFFFF) fatsz = 0xFFFF;

    char lab[12];
    memset(lab, ' ', 11);
    lab[11] = 0;
    const char *src = label && *label ? label : "NO NAME";
    for (int i = 0; i < 11 && src[i]; i++) {
        char c = src[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        lab[i] = (c < 32 || strchr("\"*+,./:;<=>?[\\]|", c)) ? '_' : c;
    }

    /* boot sector */
    static uint8_t b[512];
    memset(b, 0, sizeof(b));
    b[0] = 0xEB; b[1] = fat32 ? 0x58 : 0x3C; b[2] = 0x90;
    memcpy(b + 3, "JARVISOS", 8);
    put16(b + 11, 512);
    b[13] = (uint8_t)spc;
    put16(b + 14, (uint16_t)reserved);
    b[16] = (uint8_t)nfats;
    put16(b + 17, fat32 ? 0 : 512);
    if (!fat32 && sectors < 65536) put16(b + 19, (uint16_t)sectors); else put32(b + 32, (uint32_t)sectors);
    b[21] = 0xF8;
    put16(b + 24, 63);
    put16(b + 26, 255);
    put32(b + 28, (uint32_t)start);
    uint32_t serial = (uint32_t)(sys_unix_time() * 2654435761u);
    if (fat32) {
        put32(b + 36, (uint32_t)fatsz);
        put32(b + 44, 2);                                   /* root directory cluster */
        put16(b + 48, 1);                                   /* FSInfo */
        put16(b + 50, 6);                                   /* backup boot sector */
        b[64] = 0x80; b[66] = 0x29;
        put32(b + 67, serial);
        memcpy(b + 71, lab, 11);
        memcpy(b + 82, "FAT32   ", 8);
    } else {
        put16(b + 22, (uint16_t)fatsz);
        b[36] = 0x80; b[38] = 0x29;
        put32(b + 39, serial);
        memcpy(b + 43, lab, 11);
        memcpy(b + 54, "FAT16   ", 8);
    }
    b[510] = 0x55; b[511] = 0xAA;

    /* metadata area: reserved sectors, both FATs, the root directory */
    uint64_t meta = reserved + nfats * fatsz + root_secs + (fat32 ? spc : 0);
    if (zero_sectors(dev, start, meta) != 0) { snprintf(err, n, "the disk did not accept the write"); return -1; }
    if (block_write(dev, start, 1, b) != 0) { snprintf(err, n, "could not write the boot sector"); return -1; }
    if (fat32) {
        block_write(dev, start + 6, 1, b);                  /* backup */
        static uint8_t fsi[512];
        memset(fsi, 0, sizeof(fsi));
        put32(fsi, 0x41615252);
        put32(fsi + 484, 0x61417272);
        put32(fsi + 488, (uint32_t)clusters - 1);           /* the root uses one */
        put32(fsi + 492, 3);
        put32(fsi + 508, 0xAA550000);
        block_write(dev, start + 1, 1, fsi);
        block_write(dev, start + 7, 1, fsi);
    }
    /* first FAT entries, in each copy */
    static uint8_t f[512];
    memset(f, 0, sizeof(f));
    if (fat32) { put32(f, 0x0FFFFFF8); put32(f + 4, 0x0FFFFFFF); put32(f + 8, 0x0FFFFFFF); }
    else { put16(f, 0xFFF8); put16(f + 2, 0xFFFF); }
    for (uint32_t k = 0; k < nfats; k++) block_write(dev, start + reserved + k * fatsz, 1, f);
    /* volume label entry in the root directory */
    if (label && *label) {
        static uint8_t r[512];
        memset(r, 0, sizeof(r));
        memcpy(r, lab, 11);
        r[11] = 0x08;
        uint64_t root = start + reserved + nfats * fatsz;
        block_write(dev, root, 1, r);
    }
    block_flush(dev);
    terminal_printf("[DISK] disk %d: formatted %u MB at sector %u as %s (%u clusters of %u KB)\n", dev,
                    (unsigned)(sectors / 2048), (unsigned)start, fat32 ? "FAT32" : "FAT16", (unsigned)clusters, (unsigned)(spc / 2));
    return 0;
}

int dm_create_partition(int dev, uint64_t size_mb, const char *fs, const char *label, int *out_num, char *err, size_t n)
{
    if (check_dev(dev, err, n, 0)) return -1;
    static dm_disk_t d;
    dm_disk_info(dev, &d);
    if (d.scheme == DM_EMPTY) {                             /* a blank disk gets a table first */
        if (dm_init_mbr(dev, err, n)) return -1;
        dm_disk_info(dev, &d);
    }
    if (d.scheme != DM_MBR) {
        snprintf(err, n, d.scheme == DM_GPT ? "this disk uses GPT, which JARVIS OS can read but not change; initialize it as MBR first"
                                            : "this disk holds one volume without a partition table; initialize it first (erases it)");
        return -1;
    }
    int slot = -1;
    static uint8_t mbr[512];
    block_read(dev, 0, 1, mbr);
    for (int i = 0; i < 4; i++) if (!mbr[446 + i * 16 + 4]) { slot = i; break; }
    if (slot < 0) { snprintf(err, n, "the MBR already has 4 partitions"); return -1; }
    /* largest free gap, aligned to 1 MB */
    uint64_t best_start = 0, best_len = 0;
    uint64_t pos = 2048;
    for (int k = 0; k <= d.nparts; k++) {
        uint64_t next = k < d.nparts ? d.parts[k].start : d.sectors;
        uint64_t s0 = (pos + 2047) & ~2047ull;
        if (next > s0 && next - s0 > best_len) { best_start = s0; best_len = next - s0; }
        if (k < d.nparts) pos = d.parts[k].start + d.parts[k].sectors;
    }
    if (best_len < 4096) { snprintf(err, n, "there is no free space on this disk"); return -1; }
    uint64_t want = size_mb ? size_mb * 2048 : best_len;
    if (want > best_len) { snprintf(err, n, "only %u MB of free space in one piece", (unsigned)(best_len / 2048)); return -1; }
    fatfs_unmount_dev(dev);
    int fat32 = strcmp(fs ? fs : "", "fat16") && want >= 128 * 2048;
    if (write_mbr_entry(dev, slot, fat32 ? 0x0C : 0x0E, (uint32_t)best_start, (uint32_t)want) != 0) {
        snprintf(err, n, "could not write the partition table");
        remount(dev);
        return -1;
    }
    int rc = 0;
    if (!fs || strcmp(fs, "none")) {
        uint64_t room = want + free_after(&d, best_start + want);
        if (room > best_len) room = best_len;
        rc = format_fat(dev, best_start, want, room, fs ? fs : "auto", label, err, n);
        /* the partition type follows the file system chosen */
        char got[16];
        probe_fs(dev, best_start, got, sizeof(got), NULL, 0);
        if (!rc) write_mbr_entry(dev, slot, !strcmp(got, "FAT32") ? 0x0C : 0x0E, (uint32_t)best_start, (uint32_t)want);
    }
    if (out_num) *out_num = slot + 1;
    remount(dev);
    return rc;
}

int dm_delete_partition(int dev, int num, char *err, size_t n)
{
    if (check_dev(dev, err, n, 0)) return -1;
    static dm_disk_t d;
    dm_disk_info(dev, &d);
    if (d.scheme == DM_WHOLE && num == 0) {                 /* a whole-disk volume: erase its boot sector */
        fatfs_unmount_dev(dev);
        int rc = zero_sectors(dev, 0, 64);
        remount(dev);
        if (rc) snprintf(err, n, "the disk did not accept the write");
        return rc ? -1 : 0;
    }
    if (d.scheme != DM_MBR) { snprintf(err, n, "only MBR partitions can be deleted"); return -1; }
    dm_part_t *p = find_part(&d, num);
    if (!p) { snprintf(err, n, "there is no partition %d", num); return -1; }
    fatfs_unmount_dev(dev);
    uint64_t start = p->start;
    int rc = write_mbr_entry(dev, num - 1, 0, 0, 0);
    if (!rc) rc = zero_sectors(dev, start, 8);              /* old boot sector gone */
    remount(dev);
    if (rc) snprintf(err, n, "could not write the partition table");
    return rc ? -1 : 0;
}

int dm_format(int dev, int num, const char *fs, const char *label, char *err, size_t n)
{
    if (check_dev(dev, err, n, 0)) return -1;
    if (fs && *fs && strcmp(fs, "fat32") && strcmp(fs, "fat16") && strcmp(fs, "auto") && strcmp(fs, "fat")) {
        snprintf(err, n, "JARVIS OS can format fat32 or fat16 (%s is not supported)", fs);
        return -1;
    }
    if (fs && !strcmp(fs, "fat")) fs = "auto";
    static dm_disk_t d;
    dm_disk_info(dev, &d);
    uint64_t start, sectors, room;
    if (d.scheme == DM_EMPTY || d.scheme == DM_WHOLE || num == 0) {
        if (num != 0 && d.scheme == DM_MBR) { snprintf(err, n, "give a partition number"); return -1; }
        if (d.scheme == DM_MBR || d.scheme == DM_GPT) {
            snprintf(err, n, "this disk has a partition table; format a partition (1-4), or initialize the disk");
            return -1;
        }
        start = 0; sectors = d.sectors; room = d.sectors;
    } else if (d.scheme == DM_MBR) {
        dm_part_t *p = find_part(&d, num);
        if (!p) { snprintf(err, n, "there is no partition %d", num); return -1; }
        start = p->start; sectors = p->sectors;
        room = sectors + free_after(&d, start + sectors);
    } else { snprintf(err, n, "partitions on GPT disks cannot be formatted here yet"); return -1; }
    fatfs_unmount_dev(dev);
    int rc = format_fat(dev, start, sectors, room, fs && *fs ? fs : "auto", label, err, n);
    if (!rc && d.scheme == DM_MBR) {
        char got[16];
        probe_fs(dev, start, got, sizeof(got), NULL, 0);
        write_mbr_entry(dev, num - 1, !strcmp(got, "FAT32") ? 0x0C : 0x0E, (uint32_t)start, (uint32_t)sectors);
    }
    remount(dev);
    return rc;
}

static int vbr_of(int dev, int num, uint64_t *start, char *err, size_t n)
{
    static dm_disk_t d;
    dm_disk_info(dev, &d);
    dm_part_t *p = find_part(&d, num);
    if (!p) { snprintf(err, n, "there is no volume %d on this disk", num); return -1; }
    if (strncmp(p->fs, "FAT", 3)) { snprintf(err, n, "volume %d is %s, not FAT", num, p->fs); return -1; }
    *start = p->start;
    return 0;
}

int dm_set_label(int dev, int num, const char *label, char *err, size_t n)
{
    if (check_dev(dev, err, n, 0)) return -1;
    uint64_t start;
    if (vbr_of(dev, num, &start, err, n)) return -1;
    char lab[12];
    memset(lab, ' ', 11);
    lab[11] = 0;
    const char *src = label && *label ? label : "NO NAME";
    for (int i = 0; i < 11 && src[i]; i++) {
        char c = src[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        lab[i] = (c < 32 || strchr("\"*+,./:;<=>?[\\]|", c)) ? '_' : c;
    }
    fatfs_unmount_dev(dev);
    static uint8_t b[512];
    int rc = block_read(dev, start, 1, b);
    int fat32 = le16(b + 22) == 0;
    if (!rc) {
        memcpy(b + (fat32 ? 71 : 43), lab, 11);
        rc = block_write(dev, start, 1, b);
        if (fat32 && !rc) block_write(dev, start + 6, 1, b);
    }
    /* the label entry in the root directory (what Windows shows) */
    if (!rc) {
        uint32_t reserved = le16(b + 14), nfats = b[16], spc = b[13];
        uint32_t fatsz = le16(b + 22) ? le16(b + 22) : le32(b + 36);
        uint64_t root = start + reserved + (uint64_t)nfats * fatsz;
        uint32_t secs = fat32 ? spc : (uint32_t)((le16(b + 17) * 32 + 511) / 512);
        if (fat32) root += (uint64_t)(le32(b + 44) - 2) * spc;
        int done = 0;
        static uint8_t r[512];
        for (uint32_t s = 0; s < secs && !done; s++) {
            if (block_read(dev, root + s, 1, r) != 0) break;
            for (int i = 0; i < 512; i += 32) {
                if (r[i] == 0 || (r[i] != 0xE5 && r[i + 11] == 0x08)) {
                    int was_end = r[i] == 0;
                    memset(r + i, 0, 32);
                    memcpy(r + i, lab, 11);
                    r[i + 11] = 0x08;
                    if (was_end && i + 32 < 512) { /* still the end marker after it */ }
                    block_write(dev, root + s, 1, r);
                    done = 1;
                    break;
                }
            }
        }
    }
    remount(dev);
    if (rc) snprintf(err, n, "the disk did not accept the write");
    return rc ? -1 : 0;
}

/* Grow or shrink a volume to new_mb (0 = as large as possible). */
int dm_resize(int dev, int num, uint64_t new_mb, char *err, size_t n)
{
    if (check_dev(dev, err, n, 1)) return -1;
    if (dev == diskfs_device()) {                           /* the JARVIS system volume */
        uint64_t blocks = new_mb ? new_mb * 256 : diskfs_max_blocks();
        int rc = diskfs_resize(blocks, err, n);
        g_storage_generation++;
        return rc;
    }
    static dm_disk_t d;
    dm_disk_info(dev, &d);
    dm_part_t *p = find_part(&d, num);
    if (!p) { snprintf(err, n, "there is no volume %d on this disk", num); return -1; }
    uint64_t room = p->sectors + free_after(&d, p->start + p->sectors);
    if (d.scheme == DM_WHOLE) room = d.sectors;
    if (d.scheme == DM_GPT) { snprintf(err, n, "GPT partitions cannot be resized here yet"); return -1; }
    uint64_t target = new_mb ? new_mb * 2048 : room;
    if (target > room) { snprintf(err, n, "there is only room for %u MB", (unsigned)(room / 2048)); return -1; }
    if (target == p->sectors) { snprintf(err, n, "the volume is already %u MB", (unsigned)(target / 2048)); return -1; }

    if (!strncmp(p->fs, "FAT", 3)) {
        static uint8_t b[512];
        if (block_read(dev, p->start, 1, b) != 0) { snprintf(err, n, "cannot read the volume"); return -1; }
        int fat32 = le16(b + 22) == 0;
        uint32_t spc = b[13], reserved = le16(b + 14), nfats = b[16], root_secs = (le16(b + 17) * 32 + 511) / 512;
        uint32_t fatsz = le16(b + 22) ? le16(b + 22) : le32(b + 36);
        uint64_t overhead = reserved + (uint64_t)nfats * fatsz + root_secs;
        uint64_t new_clusters = (target - overhead) / spc;
        uint64_t cap = (uint64_t)fatsz * 512 / (fat32 ? 4 : 2) - 2;
        if (target <= overhead) { snprintf(err, n, "too small"); return -1; }
        if (new_clusters > cap) {
            uint64_t max_mb = (overhead + cap * spc) / 2048;
            snprintf(err, n, "this file system can grow to %u MB at most (its allocation tables are full); back it up and format again for more", (unsigned)max_mb);
            return -1;
        }
        if (fat32 ? new_clusters < 65525 : (new_clusters < 4085 || new_clusters > 65524)) {
            snprintf(err, n, "%s needs between %s; choose another size", fat32 ? "FAT32" : "FAT16", fat32 ? "33 MB and 2 TB" : "2 MB and 2 GB");
            return -1;
        }
        const char *mnt = fatfs_mount_of(dev, p->num);
        if (target < p->sectors && mnt[0]) {
            uint32_t hi = fatfs_highest_used(mnt, NULL, NULL);
            if (hi >= new_clusters + 2) {
                uint64_t need = (overhead + (uint64_t)(hi - 1) * spc) / 2048 + 1;
                snprintf(err, n, "files are stored up to %u MB into the volume; it cannot be shrunk below that", (unsigned)need);
                return -1;
            }
        }
        fatfs_unmount_dev(dev);
        if (!fat32 && target < 65536) { put16(b + 19, (uint16_t)target); put32(b + 32, 0); }
        else { put16(b + 19, 0); put32(b + 32, (uint32_t)target); }
        int rc = block_write(dev, p->start, 1, b);
        if (fat32 && !rc) block_write(dev, p->start + 6, 1, b);
        if (!rc && fat32) {                                 /* FSInfo free count: recounted at mount */
            static uint8_t fsi[512];
            if (block_read(dev, p->start + le16(b + 48), 1, fsi) == 0 && le32(fsi) == 0x41615252) {
                put32(fsi + 488, 0xFFFFFFFF);
                block_write(dev, p->start + le16(b + 48), 1, fsi);
            }
        }
        if (!rc && d.scheme == DM_MBR) rc = write_mbr_entry(dev, num - 1, p->type, (uint32_t)p->start, (uint32_t)target);
        remount(dev);
        if (rc) { snprintf(err, n, "the disk did not accept the write"); return -1; }
        terminal_printf("[DISK] disk %d volume %d resized to %u MB\n", dev, num, (unsigned)(target / 2048));
        return 0;
    }
    if (d.scheme == DM_MBR && !strcmp(p->fs, "Unformatted")) {
        int rc = write_mbr_entry(dev, num - 1, p->type, (uint32_t)p->start, (uint32_t)target);
        remount(dev);
        if (rc) snprintf(err, n, "could not write the partition table");
        return rc ? -1 : 0;
    }
    snprintf(err, n, "%s volumes cannot be resized", p->fs);
    return -1;
}
