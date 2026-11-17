/*
 * Kryzen OS native installer backend.
 *
 * The live ISO hands the kernel two Multiboot2 modules:
 *
 *   kryzen-kernel   the kernel image to install
 *   kryzen-boot     the boot bundle, a ustar archive built by
 *                   scripts/make_boot_bundle.py:
 *                     efi/BOOTX64.EFI   UEFI boot loader (GRUB)
 *                     bios/boot.img     legacy-BIOS boot sector code
 *                     bios/core.img     legacy-BIOS second stage (GRUB)
 *                     esp/...           files copied as they are to the EFI
 *                                       System Partition: the boot menu, its
 *                                       theme and fonts, and the marker file
 *                                       the loaders search for
 *
 * A guided install writes a GPT with a BIOS boot partition, an EFI System
 * Partition and a KryzenFS data partition, so the disk starts on both UEFI
 * and legacy BIOS firmware. A manual install adds partitions in free space
 * and is UEFI only: it never touches the boot sector of a disk that other
 * systems may be using. Every file and boot sector written is read back and
 * compared. The UI confirms the layout before this code runs.
 */

#include "kernel.h"
#include <string.h>
#include <stdio.h>

enum { INSTALLER_PHASE_IDLE, INSTALLER_PHASE_PARTITIONS,
       INSTALLER_PHASE_DATA, INSTALLER_PHASE_LOADER,
       INSTALLER_PHASE_KERNEL, INSTALLER_PHASE_VERIFY,
       INSTALLER_PHASE_FINISH, INSTALLER_PHASE_ERROR };

static installer_status_t g_installer_status;
static uint8_t g_verify_buf[32768];

static void installer_progress(int phase, int percent, const char *detail)
{
    g_installer_status.phase = phase;
    g_installer_status.percent = percent;
    snprintf(g_installer_status.detail, sizeof(g_installer_status.detail), "%s", detail ? detail : "");
    ui_shell_draw_frame();
}

/* ---- Boot bundle (ustar) --------------------------------------------------- */
static const uint8_t *g_bundle;
static size_t g_bundle_size;

static uint64_t tar_octal(const uint8_t *p, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n && p[i]; i++)
        if (p[i] >= '0' && p[i] <= '7') v = v * 8 + (uint64_t)(p[i] - '0');
    return v;
}

/* Steps to the next regular file. *pos starts at 0. Returns 1 with the entry,
 * 0 at the end of the archive, -1 if the archive is damaged. */
static int bundle_next(size_t *pos, char *name, size_t name_len,
                       const uint8_t **data, size_t *size)
{
    while (*pos + 512 <= g_bundle_size) {
        const uint8_t *h = g_bundle + *pos;
        int empty = 1;
        for (int i = 0; i < 512; i++) if (h[i]) { empty = 0; break; }
        if (empty) return 0;
        if (memcmp(h + 257, "ustar", 5)) return -1;
        uint64_t sz = tar_octal(h + 124, 12);
        size_t body = *pos + 512;
        if (sz > g_bundle_size || body + sz > g_bundle_size) return -1;
        *pos = body + (size_t)((sz + 511) & ~511ull);
        if (h[156] != '0' && h[156] != 0) continue;            /* folders etc. */
        size_t k = 0;
        if (h[345]) {
            for (int i = 0; i < 155 && h[345 + i] && k + 2 < name_len; i++) name[k++] = (char)h[345 + i];
            name[k++] = '/';
        }
        for (int i = 0; i < 100 && h[i] && k + 1 < name_len; i++) name[k++] = (char)h[i];
        name[k] = 0;
        *data = g_bundle + body;
        *size = (size_t)sz;
        return 1;
    }
    return 0;
}

static int bundle_find(const char *want, const uint8_t **data, size_t *size)
{
    size_t pos = 0;
    char name[200];
    while (bundle_next(&pos, name, sizeof(name), data, size) == 1)
        if (!strcmp(name, want)) return 0;
    return -1;
}

/* Total size of the files that go to the EFI System Partition. */
static uint64_t bundle_esp_bytes(int *count)
{
    size_t pos = 0, size;
    const uint8_t *data;
    char name[200];
    uint64_t total = 0;
    int n = 0;
    while (bundle_next(&pos, name, sizeof(name), &data, &size) == 1)
        if (!strncmp(name, "esp/", 4)) { total += size; n++; }
    if (count) *count = n;
    return total;
}

/* What the installer media carries; shown by the wizard's system check. */
int installer_media_check(int *kernel_ok, int *uefi_ok, int *bios_ok)
{
    const void *p;
    const uint8_t *d;
    size_t n;
    int k = kernel_installer_payload(1, &p, &n) == 0 && n >= 4 && !memcmp(p, "\x7f" "ELF", 4);
    int u = 0, b = 0;
    if (kernel_installer_payload(2, &p, &n) == 0) {
        g_bundle = (const uint8_t *)p;
        g_bundle_size = n;
        u = bundle_find("efi/BOOTX64.EFI", &d, &n) == 0 && n > 64 && !memcmp(d, "MZ", 2) &&
            bundle_find("esp/EFI/Kryzen/grub/grub.cfg", &d, &n) == 0 &&
            bundle_find("esp/EFI/Kryzen/kryzen.marker", &d, &n) == 0;
        size_t cn;
        const uint8_t *c;
        b = bundle_find("bios/boot.img", &d, &n) == 0 && n == 512 && d[510] == 0x55 && d[511] == 0xAA &&
            bundle_find("bios/core.img", &c, &cn) == 0 && cn > 1024;
    }
    if (kernel_ok) *kernel_ok = k;
    if (uefi_ok) *uefi_ok = u;
    if (bios_ok) *bios_ok = b;
    return k && u;
}

/* ---- Files on the EFI System Partition ------------------------------------- */
static int installer_verify_file(const char *path, const void *source,
                                 size_t size, char *err, size_t n)
{
    const uint8_t *src = (const uint8_t *)source;
    size_t offset = 0;
    uint32_t on_disk = 0;
    int is_dir = 0;
    if (fatfs_stat(path, &is_dir, &on_disk) != 0 || is_dir || on_disk != size) {
        snprintf(err, n, "verification failed for %s (wrong size)", path);
        return -1;
    }
    while (offset < size) {
        size_t want = size - offset;
        if (want > sizeof(g_verify_buf)) want = sizeof(g_verify_buf);
        int got = fatfs_read(path, offset, g_verify_buf, want);
        if (got < 0 || (size_t)got != want || memcmp(src + offset, g_verify_buf, want)) {
            snprintf(err, n, "verification failed for %s", path);
            return -1;
        }
        offset += want;
    }
    return 0;
}

/* Creates every folder on the way to a file. */
static int installer_make_parents(const char *mount, const char *path)
{
    char dir[192];
    size_t ml = strlen(mount);
    for (size_t i = ml + 1; path[i]; i++) {
        if (path[i] != '/') continue;
        if (i >= sizeof(dir)) return -1;
        memcpy(dir, path, i);
        dir[i] = 0;
        int is_dir = 0;
        if (fatfs_stat(dir, &is_dir, NULL) == 0) {
            if (!is_dir) return -1;
            continue;
        }
        if (fatfs_mkdir(dir) != 0) return -1;
    }
    return 0;
}

static int installer_put_file(const char *mount, const char *path, const void *data,
                              size_t size, const char *what, char *err, size_t n)
{
    if (installer_make_parents(mount, path) != 0) {
        snprintf(err, n, "could not create the folder for %s", what);
        return -1;
    }
    if (fatfs_write_file(path, data, size) != 0) {
        snprintf(err, n, "could not write %s", what);
        return -1;
    }
    return installer_verify_file(path, data, size, err, n);
}

/* Reusing somebody else's EFI System Partition: it must be FAT, have room,
 * and not already hold Kryzen boot files (nothing is ever overwritten). */
static int installer_existing_efi_preflight(int dev, int part_num, uint64_t need_bytes,
                                            char *mount, size_t mount_len,
                                            char *err, size_t n)
{
    static dm_disk_t disk;
    if (dm_disk_info(dev, &disk) != 0 || disk.scheme != DM_GPT) {
        snprintf(err, n, "reusing an EFI partition requires a readable GPT disk");
        return -1;
    }
    dm_part_t *esp = NULL;
    for (int i = 0; i < disk.nparts; i++)
        if (disk.parts[i].num == part_num) esp = &disk.parts[i];
    if (!esp || !esp->efi_system ||
        (strcmp(esp->fs, "FAT32") && strcmp(esp->fs, "FAT16"))) {
        snprintf(err, n, "the selected EFI partition is not a supported FAT volume");
        return -1;
    }
    if (fatfs_mount_dev(dev) != 0) {
        snprintf(err, n, "the existing EFI partition could not be mounted");
        return -1;
    }
    const char *mounted = fatfs_mount_of(dev, part_num);
    if (!mounted[0]) {
        snprintf(err, n, "the selected EFI partition could not be mounted");
        return -1;
    }
    snprintf(mount, mount_len, "%s", mounted);
    uint64_t free_bytes = 0;
    if (fatfs_volume_info(mount, NULL, 0, NULL, NULL, &free_bytes) != 0) {
        snprintf(err, n, "the existing EFI partition could not be inspected");
        return -1;
    }
    if (free_bytes < need_bytes + 8ull * 1024ull * 1024ull) {
        snprintf(err, n, "the EFI partition needs more room for Kryzen boot files and an 8 MB reserve");
        return -1;
    }
    char target[192];
    int is_dir = 0;
    snprintf(target, sizeof(target), "%s/EFI/Kryzen", mount);
    if (fatfs_stat(target, &is_dir, NULL) == 0) {
        snprintf(err, n, "an EFI/Kryzen folder already exists on that partition; it will not be overwritten");
        return -1;
    }
    return 0;
}

/* ---- Legacy BIOS boot -------------------------------------------------------
 * boot.img goes into the first 440 bytes of the disk's first sector (the
 * disk signature and partition table after it are kept) and points at
 * core.img, which is stored at the start of the BIOS boot partition. The
 * two fields grub-bios-setup would fill in are filled in here.
 */
static uint64_t rd64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
static void wr64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

#define BIOS_BOOT_KERNEL_SECTOR 0x5C    /* boot.img: LBA of core.img */
#define BIOS_BOOT_DRIVE_CHECK   0x66    /* boot.img: jmp over the drive check */
#define BIOS_CORE_BLOCKLIST     0x1F4   /* core.img sector 0: start(8) len(2) seg(2) */

static int installer_write_bios_boot(int dev, uint64_t part_start, uint64_t part_sectors,
                                     const uint8_t *boot_img, const uint8_t *core, size_t core_size,
                                     char *err, size_t n)
{
    static uint8_t mbr[512], back[512];
    uint64_t core_sectors = (core_size + 511) / 512;
    if (core_sectors < 2 || core_sectors > part_sectors || core_sectors > 0xFFFF) {
        snprintf(err, n, "the BIOS boot loader does not fit in its partition");
        return -1;
    }
    if (rd64(boot_img + BIOS_BOOT_KERNEL_SECTOR) != 1 || boot_img[BIOS_BOOT_DRIVE_CHECK] != 0xEB ||
        rd64(core + BIOS_CORE_BLOCKLIST) != 2 ||
        core[BIOS_CORE_BLOCKLIST + 10] != 0x20 || core[BIOS_CORE_BLOCKLIST + 11] != 0x08) {
        snprintf(err, n, "the BIOS boot loader on the installer media has an unexpected layout");
        return -1;
    }

    /* core.img, with its own location filled in */
    for (uint64_t s = 0; s < core_sectors;) {
        uint32_t k = core_sectors - s > 64 ? 64 : (uint32_t)(core_sectors - s);
        size_t off = (size_t)s * 512, bytes = (size_t)k * 512;
        size_t have = core_size - off < bytes ? core_size - off : bytes;
        memset(g_verify_buf, 0, bytes);
        memcpy(g_verify_buf, core + off, have);
        if (s == 0) {
            wr64(g_verify_buf + BIOS_CORE_BLOCKLIST, part_start + 1);
            g_verify_buf[BIOS_CORE_BLOCKLIST + 8] = (uint8_t)(core_sectors - 1);
            g_verify_buf[BIOS_CORE_BLOCKLIST + 9] = (uint8_t)((core_sectors - 1) >> 8);
        }
        if (block_write(dev, part_start + s, k, g_verify_buf) != 0) {
            snprintf(err, n, "could not write the BIOS boot loader");
            return -1;
        }
        s += k;
    }
    /* read it back */
    for (uint64_t s = 0; s < core_sectors; s++) {
        size_t off = (size_t)s * 512;
        size_t have = core_size - off < 512 ? core_size - off : 512;
        if (block_read(dev, part_start + s, 1, back) != 0) goto verify_fail;
        if (s == 0) {
            if (rd64(back + BIOS_CORE_BLOCKLIST) != part_start + 1 ||
                memcmp(back, core, BIOS_CORE_BLOCKLIST)) goto verify_fail;
        } else if (memcmp(back, core + off, have)) goto verify_fail;
    }

    /* boot sector code; the partition table is kept */
    if (block_read(dev, 0, 1, mbr) != 0 || mbr[510] != 0x55 || mbr[511] != 0xAA) {
        snprintf(err, n, "could not read the disk's first sector");
        return -1;
    }
    memcpy(mbr, boot_img, 440);
    wr64(mbr + BIOS_BOOT_KERNEL_SECTOR, part_start);
    mbr[BIOS_BOOT_DRIVE_CHECK] = 0x90;          /* hard disk: keep the drive check */
    mbr[BIOS_BOOT_DRIVE_CHECK + 1] = 0x90;
    if (block_write(dev, 0, 1, mbr) != 0 || block_read(dev, 0, 1, back) != 0 ||
        memcmp(mbr, back, 512)) {
        snprintf(err, n, "could not write the BIOS boot sector");
        return -1;
    }
    return 0;

verify_fail:
    snprintf(err, n, "verification failed for the BIOS boot loader");
    return -1;
}

/* ---- Install ---------------------------------------------------------------- */
int installer_install_disk(int dev, int manual, uint64_t gap_start,
                           uint64_t gap_end, uint64_t efi_size_mb,
                           uint64_t data_size_mb, int existing_efi_partition_num,
                           char *err, size_t n)
{
    const void *kernel_image = NULL;
    const uint8_t *efi_image = NULL, *boot_img = NULL, *core_img = NULL, *data = NULL;
    size_t kernel_size = 0, efi_size = 0, boot_size = 0, core_size = 0, size = 0;
    uint64_t bios_start = 0, bios_sectors = 0, efi_start, efi_sectors, data_start, data_sectors;
    int efi_partition_num = 0;
    block_info_t info;
    char mount[32], path[192], name[200];
    int reuse_efi = manual && existing_efi_partition_num > 0;
    int kernel_ok, uefi_ok, bios_ok;

    if (!err || !n) return -1;
    err[0] = 0;
    memset(&g_installer_status, 0, sizeof(g_installer_status));
    if (!kernel_installer_boot_mode()) {
        snprintf(err, n, "boot the Kryzen installer media before installing");
        goto fail;
    }
    if (diskfs_get_stats(&(diskfs_stats_t){0}) == 0) {
        snprintf(err, n, "the live session already mounted a data volume; restart in installer mode");
        goto fail;
    }
    installer_media_check(&kernel_ok, &uefi_ok, &bios_ok);
    if (block_get_info(dev, &info) != 0 || !kernel_ok || !uefi_ok ||
        kernel_installer_payload(1, &kernel_image, &kernel_size) != 0 ||
        bundle_find("efi/BOOTX64.EFI", &efi_image, &efi_size) != 0) {
        snprintf(err, n, "the installer media is missing a valid kernel or boot loader");
        goto fail;
    }
    if (bios_ok) {
        bundle_find("bios/boot.img", &boot_img, &boot_size);
        bundle_find("bios/core.img", &core_img, &core_size);
    }
    int esp_files = 0;
    uint64_t esp_bytes = bundle_esp_bytes(&esp_files) + kernel_size + efi_size;

    if (reuse_efi && installer_existing_efi_preflight(dev, existing_efi_partition_num, esp_bytes,
                                                       mount, sizeof(mount), err, n) != 0)
        goto fail;

    installer_progress(INSTALLER_PHASE_PARTITIONS, 3,
                       reuse_efi ? "Adding Kryzen storage while preserving the existing EFI partition" :
                       manual ? "Adding partitions in the selected free region" :
                                "Creating the boot and Kryzen data partitions");
    int layout_rc = reuse_efi
        ? dm_create_gpt_data_in_gap(dev, existing_efi_partition_num,
                                    gap_start, gap_end, data_size_mb,
                                    &efi_start, &efi_sectors, &data_start, &data_sectors,
                                    err, n)
        : manual
        ? dm_create_gpt_layout_in_gap(dev, gap_start, gap_end, efi_size_mb,
                                      data_size_mb, &efi_start, &efi_sectors,
                                      &data_start, &data_sectors,
                                      &efi_partition_num, err, n)
        : dm_create_gpt_layout(dev, efi_size_mb, &bios_start, &bios_sectors,
                               &efi_start, &efi_sectors,
                               &data_start, &data_sectors,
                               &efi_partition_num, err, n);
    if (layout_rc != 0) goto fail;

    installer_progress(INSTALLER_PHASE_DATA, 18, "Formatting the Kryzen data partition");
    if (diskfs_format_partition(dev, data_start, data_sectors) != 0) {
        snprintf(err, n, "could not format the Kryzen data partition");
        goto fail;
    }

    if (!reuse_efi) {
        snprintf(mount, sizeof(mount), "%s", fatfs_mount_of(dev, efi_partition_num));
        if (!mount[0]) {
            snprintf(err, n, "the EFI partition is not mounted for file installation");
            goto fail;
        }
    }

    /* UEFI loader: the firmware's default path on a disk of our own, a
     * Kryzen-only path next to another system's files. */
    installer_progress(INSTALLER_PHASE_LOADER, 30, "Installing the UEFI boot loader");
    snprintf(path, sizeof(path), reuse_efi ? "%s/EFI/Kryzen/BOOTX64.EFI" : "%s/EFI/BOOT/BOOTX64.EFI", mount);
    if (installer_put_file(mount, path, efi_image, efi_size, "the UEFI boot loader", err, n) != 0)
        goto fail;

    installer_progress(INSTALLER_PHASE_KERNEL, 40, "Installing the Kryzen kernel");
    snprintf(path, sizeof(path), "%s/EFI/Kryzen/boot/kernel.elf", mount);
    if (installer_put_file(mount, path, kernel_image, kernel_size, "the Kryzen kernel", err, n) != 0)
        goto fail;

    /* boot menu, theme, fonts, marker */
    size_t pos = 0;
    int done = 0, rc;
    while ((rc = bundle_next(&pos, name, sizeof(name), &data, &size)) == 1) {
        if (strncmp(name, "esp/", 4)) continue;
        snprintf(path, sizeof(path), "%s/%s", mount, name + 4);
        installer_progress(INSTALLER_PHASE_LOADER, 60 + (esp_files ? done * 25 / esp_files : 0),
                           "Installing the boot menu and its theme");
        if (installer_put_file(mount, path, data, size, name + 4, err, n) != 0)
            goto fail;
        done++;
    }
    if (rc < 0) {
        snprintf(err, n, "the boot bundle on the installer media is damaged");
        goto fail;
    }

    if (bios_start && boot_img && core_img) {
        installer_progress(INSTALLER_PHASE_LOADER, 88, "Installing the legacy BIOS boot loader");
        if (installer_write_bios_boot(dev, bios_start, bios_sectors, boot_img, core_img, core_size, err, n) != 0)
            goto fail;
    }

    installer_progress(INSTALLER_PHASE_VERIFY, 95, "Saving everything to the disk");
    if (block_flush(dev) != 0) {
        snprintf(err, n, "could not flush the installed system to disk");
        goto fail;
    }

    installer_progress(INSTALLER_PHASE_FINISH, 100, "Kryzen OS is installed");
    g_installer_status.complete = 1;
    terminal_printf("[INSTALL] Installed to %s: EFI %llu MB, data %llu MB, %d boot files%s%s\n", info.name,
                    (unsigned long long)(efi_sectors / 2048),
                    (unsigned long long)(data_sectors / 2048), done + 2,
                    bios_start && boot_img && core_img ? ", BIOS + UEFI boot" : ", UEFI boot",
                    reuse_efi ? " (existing EFI files preserved)" :
                    manual ? " (other GPT partitions preserved)" : "");
    return 0;

fail:
    g_installer_status.phase = INSTALLER_PHASE_ERROR;
    g_installer_status.error = 1;
    snprintf(g_installer_status.detail, sizeof(g_installer_status.detail), "%s", err);
    terminal_printf("[INSTALL] Failed: %s\n", err);
    ui_shell_draw_frame();
    return -1;
}

void installer_get_status(installer_status_t *out)
{
    if (out) *out = g_installer_status;
}
