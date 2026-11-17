/*
 * Full-screen first-install wizard. The destructive work is delegated to
 * kernel/installer.c only after the user reviews the selected disk and
 * confirms the exact guided partition layout.
 */

enum {
    INST_WELCOME,
    INST_CHECKS,
    INST_DISKS,
    INST_LAYOUT,
    INST_REVIEW,
    INST_PROGRESS,
    INST_ERROR,
    INST_COMPLETE,
};

typedef struct {
    int dev;
    block_info_t info;
    dm_disk_t disk;
} installer_disk_row_t;

typedef struct {
    uint64_t start;
    uint64_t end;
} installer_gap_t;

static struct {
    int page;
    int disk_count;
    int selected;
    int efi_size_mb;
    int manual;
    int gap_count;
    int gap_selected;
    int existing_efi_part;         /* GPT partition number, 0 = create a new ESP */
    int data_size_gb;              /* 0 = use the largest size that fits */
    int busy;
    char error[128];
    installer_disk_row_t disks[8];
    installer_gap_t gaps[DM_MAX_PARTS + 1];
} ins;

static void installer_select_disk(int index);

static uint64_t installer_align_up(uint64_t sector)
{
    return (sector + 2047ull) & ~2047ull;
}

static int installer_efi_partition_supported(const dm_part_t *part)
{
    return part && part->efi_system &&
           (!strcmp(part->fs, "FAT32") || !strcmp(part->fs, "FAT16")) &&
           part->sectors >= 32ull * 2048ull;
}

static int installer_first_efi_partition(const installer_disk_row_t *row)
{
    if (!row || row->disk.scheme != DM_GPT) return 0;
    for (int i = 0; i < row->disk.nparts; i++)
        if (installer_efi_partition_supported(&row->disk.parts[i]))
            return row->disk.parts[i].num;
    return 0;
}

static int installer_find_efi_partition(const installer_disk_row_t *row, int part_num)
{
    if (!row || row->disk.scheme != DM_GPT) return -1;
    for (int i = 0; i < row->disk.nparts; i++)
        if (row->disk.parts[i].num == part_num &&
            installer_efi_partition_supported(&row->disk.parts[i])) return i;
    return -1;
}

static void installer_cycle_efi_partition(int direction)
{
    if (ins.selected < 0 || ins.selected >= ins.disk_count) return;
    installer_disk_row_t *row = &ins.disks[ins.selected];
    int indices[DM_MAX_PARTS], count = 0, current = -1;
    for (int i = 0; i < row->disk.nparts; i++) {
        if (!installer_efi_partition_supported(&row->disk.parts[i])) continue;
        indices[count] = row->disk.parts[i].num;
        if (indices[count] == ins.existing_efi_part) current = count;
        count++;
    }
    if (!count) { ins.existing_efi_part = 0; return; }
    if (current < 0) current = direction > 0 ? -1 : 0;
    current = (current + direction + count) % count;
    ins.existing_efi_part = indices[current];
}

static void installer_find_gaps(void)
{
    ins.gap_count = 0;
    ins.gap_selected = 0;
    if (ins.selected < 0 || ins.selected >= ins.disk_count) return;
    installer_disk_row_t *row = &ins.disks[ins.selected];
    if (row->disk.scheme != DM_GPT || row->disk.sectors < 4096) return;

    uint64_t cursor = 34;
    uint64_t last_usable = row->disk.sectors - 34;
    for (int i = 0; i < row->disk.nparts; i++) {
        dm_part_t *p = &row->disk.parts[i];
        uint64_t start = installer_align_up(cursor);
        if (p->start > start && p->start - 1 >= start &&
            ins.gap_count < DM_MAX_PARTS + 1) {
            ins.gaps[ins.gap_count++] = (installer_gap_t){ start, p->start - 1 };
        }
        uint64_t end = p->start + p->sectors;
        if (end > cursor) cursor = end;
    }
    uint64_t start = installer_align_up(cursor);
    if (last_usable >= start && ins.gap_count < DM_MAX_PARTS + 1)
        ins.gaps[ins.gap_count++] = (installer_gap_t){ start, last_usable };
}

static uint64_t installer_manual_data_mb(void)
{
    if (ins.gap_selected < 0 || ins.gap_selected >= ins.gap_count) return 0;
    installer_gap_t *gap = &ins.gaps[ins.gap_selected];
    uint64_t data_start = ins.existing_efi_part
        ? installer_align_up(gap->start)
        : installer_align_up(gap->start + (uint64_t)ins.efi_size_mb * 2048ull);
    if (data_start > gap->end) return 0;
    uint64_t max_mb = (gap->end - data_start + 1) / 2048ull;
    if (max_mb > 65536) max_mb = 65536;
    if (ins.data_size_gb > 0 && (uint64_t)ins.data_size_gb * 1024ull <= max_mb)
        return (uint64_t)ins.data_size_gb * 1024ull;
    return max_mb;
}

static uint64_t installer_guided_data_mb(const installer_disk_row_t *row)
{
    if (!row || row->info.sectors < 34) return 0;
    uint64_t data_start = installer_align_up(2048ull + (uint64_t)ins.efi_size_mb * 2048ull);
    uint64_t last_usable = row->info.sectors - 34;
    if (data_start > last_usable) return 0;
    uint64_t mb = (last_usable - data_start + 1) / 2048ull;
    return mb > 65536 ? 65536 : mb;
}

static void installer_scan_disks(void)
{
    ins.disk_count = 0;
    ins.selected = -1;
    for (int dev = 0; dev < block_count() && ins.disk_count < 8; dev++) {
        block_info_t info;
        if (block_get_info(dev, &info) != 0) continue;
        /* The live installer is loaded from removable media. Keep those
         * devices visible to the OS but out of the guided erase target list. */
        if (!strncmp(info.name, "usb", 3)) continue;
        installer_disk_row_t *row = &ins.disks[ins.disk_count];
        memset(row, 0, sizeof(*row));
        row->dev = dev;
        row->info = info;
        if (dm_disk_info(dev, &row->disk) != 0) continue;
        if (row->disk.scheme == DM_UNREADABLE) continue;
        for (int a = 1; a < row->disk.nparts; a++) {
            dm_part_t part = row->disk.parts[a];
            int b = a - 1;
            while (b >= 0 && row->disk.parts[b].start > part.start) {
                row->disk.parts[b + 1] = row->disk.parts[b];
                b--;
            }
            row->disk.parts[b + 1] = part;
        }
        ins.disk_count++;
    }
    if (ins.disk_count) installer_select_disk(0);
}

static void installer_select_disk(int index)
{
    if (index < 0 || index >= ins.disk_count) return;
    ins.selected = index;
    ins.manual = 0;
    ins.existing_efi_part = 0;
    ins.data_size_gb = 0;
    installer_find_gaps();
}

static void installer_geometry(int *x, int *y, int *w, int *h)
{
    wizard_rect(x, y, w, h);
}

static void installer_header(int x, int y, int w, const char *title, const char *detail)
{
    icon_kryzen_logo(x + 30, y + 26, 44);
    fb_text(x + 88, y + 22, title, FS_WHITE, FONT_TITLE);
    fs_text_fit(x + 88, y + 56, w - 120, detail, FS_DIM, FONT_UI);
    fb_blend_rect(x + 24, y + 90, w - 48, 1, 0x00FFFFFF, 36);
}

static void installer_footer(int x, int y, int w, int h)
{
    fb_blend_rect(x + 24, y + h - 74, w - 48, 1, 0x00FFFFFF, 36);
}

/* What can be installed from this media on this firmware. */
static int installer_ready(int *kernel_ok, int *efi_ok, int *bios_ok)
{
    int k, e, b;
    installer_media_check(&k, &e, &b);
    if (kernel_ok) *kernel_ok = k;
    if (efi_ok) *efi_ok = e;
    if (bios_ok) *bios_ok = b;
    return k && e && ins.disk_count > 0;
}

static void ins_check_row(int x, int y, int ok, const char *good, const char *bad)
{
    glyph_draw(ok ? GLYPH_CHECK : GLYPH_WARNING, x, y, 18, ok ? T(OK) : T(DANGER), 0);
    fb_text(x + 30, y + 1, ok ? good : bad, ok ? FS_WHITE : T(DANGER), FONT_UI);
}

static void ins_button(int x, int y, int w, int h, const char *label, int primary, int enabled)
{
    if (primary) fs_primary(x, y, w, h, label, enabled, 0);
    else fs_secondary(x, y, w, h, label, enabled);
}

static void installer_draw(void)
{
    int x, y, w, h;
    installer_geometry(&x, &y, &w, &h);
    wizard_frame("Kryzen Installer");

    const char *title = "Install Kryzen OS";
    const char *detail = "Install a bootable system on your computer";
    if (ins.page == INST_CHECKS) { title = "System check"; detail = "Checking installer media and available storage"; }
    if (ins.page == INST_DISKS) { title = "Choose a drive"; detail = "Select the internal drive for Kryzen OS"; }
    if (ins.page == INST_LAYOUT) { title = "Choose a partition layout"; detail = "Use a guided erase or install into GPT free space"; }
    if (ins.page == INST_REVIEW) { title = "Review before installing"; detail = ins.manual ? "Existing partitions will be preserved" : "The selected drive will be erased"; }
    if (ins.page == INST_PROGRESS) { title = "Installing Kryzen OS"; detail = "Writing and verifying every boot file"; }
    if (ins.page == INST_ERROR) { title = "Installation could not finish"; detail = "The installer stopped safely and reports the failed step below"; }
    if (ins.page == INST_COMPLETE) { title = "Kryzen OS is ready"; detail = "Installed, verified and set up"; }
    installer_header(x, y, w, title, detail);
    installer_footer(x, y, w, h);
    if (ins.page != INST_PROGRESS) fs_tray();

    int bx = x + w - 28 - 180;
    int by = y + h - 60;
    switch (ins.page) {
    case INST_WELCOME:
        fb_text(x + 52, y + 118, "A complete Kryzen installation", FS_WHITE, FONT_SUBHEAD);
        fs_paragraph(x + 54, y + 156, w - 108,
                     "This installer prepares the boot partitions, installs the Kryzen kernel and boot menu, creates "
                     "persistent storage, and then guides you through the first account setup.", FS_DIM, FONT_UI, 20);
        fs_paragraph(x + 54, y + 208, w - 108,
                     "The guided install erases the selected internal drive. To look around first without changing "
                     "any disk, choose Try Kryzen.", FS_DIM, FONT_UI, 20);
        fs_card(x + 50, y + 270, w - 100, 112, 0);
        fb_text(x + 72, y + 286, "What will be installed", FS_WHITE, FONT_UI_BOLD);
        fb_text(x + 72, y + 314, "Boot loaders for UEFI and legacy BIOS  \x80  Kryzen kernel  \x80  themed boot menu", FS_DIM, FONT_UI);
        fb_text(x + 72, y + 338, "Kryzen data volume (KryzenFS)  \x80  first account  \x80  device preferences", FS_DIM, FONT_UI);
        ins_button(x + w - 388, by, 150, 42, "Try Kryzen", 0, 1);
        ins_button(bx, by, 180, 42, "Install", 1, 1);
        break;

    case INST_CHECKS: {
        int kernel_ok, efi_ok, bios_ok;
        int ready = installer_ready(&kernel_ok, &efi_ok, &bios_ok);
        int disks_ok = ins.disk_count > 0;
        fs_card(x + 42, y + 112, w - 84, 268, 0);
        ins_check_row(x + 68, y + 132, kernel_ok, "Kryzen kernel image is on the installer media", "The Kryzen kernel image is missing from the installer media");
        ins_check_row(x + 68, y + 166, efi_ok, "UEFI boot loader and boot menu are on the installer media", "The UEFI boot loader is missing from the installer media");
        ins_check_row(x + 68, y + 200, bios_ok, "Legacy BIOS boot loader is on the installer media", "No legacy BIOS boot loader: the disk will start on UEFI only");
        ins_check_row(x + 68, y + 234, disks_ok, "An internal disk was detected", "No supported internal disk was found");
        char fw[96];
        snprintf(fw, sizeof(fw), "This computer started the installer with %s firmware.", kernel_booted_uefi() ? "UEFI" : "legacy BIOS");
        fb_text(x + 98, y + 276, fw, FS_DIM, FONT_UI);
        fb_text(x + 98, y + 300, "A guided install needs about 1.3 GB: boot partitions plus at least 1 GB of storage.", FS_DIM, FONT_UI);
        fb_text(x + 98, y + 324, "Network is optional. Large AI model files are not part of this compact installer.", FS_DIM, FONT_UI);
        if (!ready)
            fs_paragraph(x + 54, y + 396, w - 108, !disks_ok ? "Connect an internal disk, go back and try again." :
                         "This installer media is incomplete. Rebuild it with make iso and start it again.", T(WARN), FONT_UI, 20);
        ins_button(x + 28, by, 110, 42, "Back", 0, 1);
        ins_button(bx, by, 180, 42, "Choose drive", 1, ready);
        break;
    }

    case INST_DISKS:
        if (!ins.disk_count) {
            fb_text(x + 52, y + 132, "No supported internal drive found", T(WARN), FONT_SUBHEAD);
            fs_fit13(x + 54, y + 184, w - 108,
                        "Connect an internal disk of about 1.25 GB or larger and choose Rescan. Removable installer media is excluded from the erase list.", FS_DIM);
        } else {
            int row_h = 48;
            for (int i = 0; i < ins.disk_count; i++) {
                installer_disk_row_t *row = &ins.disks[i];
                int ry = y + 110 + i * row_h;
                uint64_t mb = row->info.sectors / 2048;
                char size[64];
                if (mb >= 1024) snprintf(size, sizeof(size), "%llu GB", (unsigned long long)(mb / 1024));
                else snprintf(size, sizeof(size), "%llu MB", (unsigned long long)mb);
                fs_card(x + 32, ry, w - 64, 40, i == ins.selected);
                glyph_draw(GLYPH_DISK, x + 44, ry + 10, 20, i == ins.selected ? UI_NEON_CYAN : FS_DIM, 0);
                fs_t13(x + 76, ry + 12, row->info.name, FS_WHITE);
                fs_t13(x + 160, ry + 12, row->info.model[0] ? row->info.model : "Internal disk", FS_DIM);
                int sw = fb_text_width(size);
                fs_t13(x + w - 52 - sw, ry + 12, size, FS_WHITE);
            }
        }
        ins_button(x + 28, by, 110, 42, "Back", 0, 1);
        ins_button(x + 150, by, 110, 42, "Rescan", 0, 1);
        ins_button(bx, by, 180, 42, "Partition layout", 1, ins.selected >= 0 );
        break;

    case INST_LAYOUT: {
        installer_disk_row_t *row = ins.selected >= 0 && ins.selected < ins.disk_count ? &ins.disks[ins.selected] : NULL;
        int manual_available = row && row->disk.scheme == DM_GPT && ins.gap_count > 0;
        (void)manual_available;
        fs_chip(x + 54, y + 106, 180, 30, "Guided erase", !ins.manual);
        fs_chip(x + 244, y + 106, 220, 30, "Manual free space", ins.manual);
        int options[] = { 256, 512, 1024 };
        const char *labels[] = { "256 MB", "512 MB", "1 GB" };
        if (!ins.manual) {
            fs_card(x + 42, y + 148, w - 84, 108, 0);
            fs_t13(x + 64, y + 166, "EFI System Partition", FS_WHITE);
            fs_t13(x + 64, y + 190, "FAT32  \x80  UEFI loader, kernel, and boot menu", FS_DIM);
            for (int i = 0; i < 3; i++)
                fs_chip(x + 64 + i * 128, y + 214, 112, 28, labels[i], ins.efi_size_mb == options[i]);
            fs_card(x + 42, y + 266, w - 84, 86, 0);
            fs_t13(x + 64, y + 284, "Kryzen Data Partition", FS_WHITE);
            uint64_t data_mb = installer_guided_data_mb(row);
            char data_line[112];
            snprintf(data_line, sizeof(data_line), "KryzenFS  \x80  %llu GB usable%s",
                     (unsigned long long)(data_mb / 1024),
                     row && row->info.sectors / 2048 > (uint64_t)ins.efi_size_mb + 65538 ?
                        "  \x80  extra space stays unallocated" : "");
            fs_fit13(x + 64, y + 310, w - 128, data_line, FS_DIM);
            fs_fit13(x + 52, y + 376, w - 104,
                        "Guided install erases the selected drive and creates a new GPT partition table.", T(WARN));
        } else {
            fs_card(x + 42, y + 148, w - 84, 126, 0);
            fs_t13(x + 64, y + 164, "Existing GPT partitions (preserved)", FS_WHITE);
            if (row && row->disk.scheme == DM_GPT && row->disk.nparts) {
                int shown = row->disk.nparts < 4 ? row->disk.nparts : 4;
                for (int i = 0; i < shown; i++) {
                    dm_part_t *part = &row->disk.parts[i];
                    char line[112];
                    const char *name = part->label[0] ? part->label : part->fs;
                    snprintf(line, sizeof(line), "Partition %d  \x80  %s  \x80  %llu GB",
                             part->num, name, (unsigned long long)(part->sectors / 2048 / 1024));
                    fs_t13(x + 64, y + 186 + i * 19, line, FS_DIM);
                }
                if (row->disk.nparts > shown) {
                    char more[64];
                    snprintf(more, sizeof(more), "+ %d more existing partition(s)", row->disk.nparts - shown);
                    fs_t13(x + 64, y + 186 + shown * 19, more, FS_DIM);
                }
            } else {
                fs_t13(x + 64, y + 194, "Manual mode needs a GPT disk with free partition space.", T(WARN));
            }
            fs_card(x + 42, y + 282, w - 84, 132, 0);
            fs_t13(x + 64, y + 298, "Unallocated region", FS_WHITE);
            char gapline[112];
            if (ins.gap_count && ins.gap_selected < ins.gap_count) {
                installer_gap_t *gap = &ins.gaps[ins.gap_selected];
                uint64_t region_mb = (gap->end - gap->start + 1) / 2048;
                snprintf(gapline, sizeof(gapline), "Region %d of %d  \x80  about %llu GB free",
                         ins.gap_selected + 1, ins.gap_count,
                         (unsigned long long)(region_mb / 1024));
            } else snprintf(gapline, sizeof(gapline), "No suitable free region found on this disk");
            fs_t13(x + 64, y + 324, gapline, ins.gap_count ? FS_DIM : T(WARN));
            fs_chip(x + 64, y + 350, 112, 28, "Previous", 0);
            fs_chip(x + 184, y + 350, 112, 28, "Next", 0);
            uint64_t max_data_mb = installer_manual_data_mb();
            char sizing[96];
            int display_gb = ins.data_size_gb ? ins.data_size_gb : (int)(max_data_mb / 1024);
            snprintf(sizing, sizeof(sizing), "%d GB", display_gb);
            fs_chip(x + 320, y + 350, 72, 28, "- 1 GB", 0);
            fs_chip(x + 398, y + 350, 72, 28, "Max", ins.data_size_gb == 0);
            fs_chip(x + 476, y + 350, 72, 28, "+ 1 GB", 0);
            fs_t13(x + 562, y + 356, sizing, FS_DIM);
            int efi_index = row ? installer_find_efi_partition(row, ins.existing_efi_part) : -1;
            int first_efi = row ? installer_first_efi_partition(row) : 0;
            (void)first_efi;
            fs_chip(x + 54, y + 422, 178, 26, "Use existing EFI", ins.existing_efi_part > 0);
            fs_chip(x + 242, y + 422, 144, 26, "Create new EFI", ins.existing_efi_part == 0);
            if (ins.existing_efi_part > 0 && efi_index >= 0) {
                fs_chip(x + 398, y + 422, 72, 26, "Prev EFI", 0);
                fs_chip(x + 476, y + 422, 72, 26, "Next EFI", 0);
                dm_part_t *efi = &row->disk.parts[efi_index];
                char efi_line[72];
                snprintf(efi_line, sizeof(efi_line), "Partition %d  \x80  %s  \x80  %llu MB",
                         efi->num, efi->fs, (unsigned long long)(efi->sectors / 2048));
                fs_fit13(x + 556, y + 427, w - 584, efi_line, FS_DIM);
                fs_fit13(x + 52, y + 458, w - 104,
                            "Kryzen uses EFI/Kryzen and preserves the existing EFI files.", T(OK));
            } else {
                for (int i = 0; i < 3; i++)
                    fs_chip(x + 64 + i * 128, y + 456, 112, 26, labels[i], ins.efi_size_mb == options[i]);
                fs_fit13(x + 52, y + 488, w - 104,
                            "Kryzen files go only in free space. Other GPT partitions remain untouched.", T(WARN));
            }
        }
        ins_button(x + 28, by, 110, 42, "Back", 0, 1);
        uint64_t guided_mb = installer_guided_data_mb(row);
        int layout_ok = ins.manual ? installer_manual_data_mb() >= 1024 : guided_mb >= 1024;
        ins_button(bx, by, 180, 42, "Review changes", 1, layout_ok );
        break;
    }

    case INST_REVIEW: {
        installer_disk_row_t *row = ins.selected >= 0 && ins.selected < ins.disk_count ? &ins.disks[ins.selected] : NULL;
        fs_card(x + 42, y + 116, w - 84, 310, 0);
        fs_t13(x + 68, y + 140, row ? row->info.name : "No drive selected", FS_WHITE);
        if (row) {
            fs_t13(x + 68, y + 164, row->info.model, FS_DIM);
            if (ins.manual) {
                fs_t13(x + 68, y + 192, "Existing partitions that will be preserved:", FS_WHITE);
                int shown = row->disk.nparts < 4 ? row->disk.nparts : 4;
                for (int i = 0; i < shown; i++) {
                    dm_part_t *part = &row->disk.parts[i];
                    char line[112];
                    snprintf(line, sizeof(line), "Partition %d  \x80  %s  \x80  %llu GB",
                             part->num, part->label[0] ? part->label : part->fs,
                             (unsigned long long)(part->sectors / 2048 / 1024));
                    fs_t13(x + 84, y + 214 + i * 19, line, FS_DIM);
                }
                if (row->disk.nparts > shown) {
                    char more[72];
                    snprintf(more, sizeof(more), "+ %d additional partition(s) will be preserved",
                             row->disk.nparts - shown);
                    fs_t13(x + 84, y + 214 + shown * 19, more, FS_DIM);
                }
                if (ins.gap_count && ins.gap_selected < ins.gap_count) {
                    char line[112];
                    int efi_index = installer_find_efi_partition(row, ins.existing_efi_part);
                    if (efi_index >= 0) {
                        dm_part_t *efi = &row->disk.parts[efi_index];
                        snprintf(line, sizeof(line), "Existing EFI System  \x80  Partition %d  \x80  %s  \x80  files preserved",
                                 efi->num, efi->fs);
                        fs_t13(x + 68, y + 310, line, FS_DIM);
                        fs_t13(x + 68, y + 334, "Kryzen boot files will be added under EFI/Kryzen.", FS_DIM);
                    } else {
                        fs_t13(x + 68, y + 310, "New EFI System Partition:", FS_WHITE);
                        snprintf(line, sizeof(line), "FAT32  \x80  %d MB  \x80  formatted", ins.efi_size_mb);
                        fs_t13(x + 84, y + 334, line, FS_DIM);
                    }
                    snprintf(line, sizeof(line), "Kryzen Data  \x80  KryzenFS  \x80  %llu GB",
                             (unsigned long long)(installer_manual_data_mb() / 1024));
                    fs_t13(x + 84, y + 358, line, FS_DIM);
                }
                fs_t13(x + 68, y + 386,
                             ins.existing_efi_part ? "Only the new Kryzen data partition will be formatted." :
                                                    "Only the new EFI and Kryzen data partitions will be formatted.", T(OK));
            } else {
                fs_t13(x + 68, y + 194, "All existing partitions and files on this drive will be erased.", T(DANGER));
                fs_t13(x + 68, y + 236, "New GPT partition table", FS_WHITE);
                char part[112];
                uint64_t total_mb = row->info.sectors / 2048;
                uint64_t data_mb = installer_guided_data_mb(row);
                snprintf(part, sizeof(part), "1. EFI System  \x80  FAT32  \x80  %d MB", ins.efi_size_mb);
                fs_t13(x + 84, y + 264, part, FS_DIM);
                snprintf(part, sizeof(part), "2. Kryzen Data  \x80  KryzenFS  \x80  %llu GB", (unsigned long long)(data_mb / 1024));
                fs_t13(x + 84, y + 292, part, FS_DIM);
                if (total_mb > (uint64_t)ins.efi_size_mb + 65538)
                    fs_t13(x + 68, y + 326, "Drive space beyond 64 GB remains unallocated.", FS_DIM);
            }
        }
        fs_t13(x + 52, y + 442, "The installer reads back and verifies every installed boot file.", FS_DIM);
        ins_button(x + 28, by, 110, 42, "Back", 0, 1);
        ins_button(bx, by, 180, 42, ins.manual ? "Install in free space" : "Erase and install", 1, 1);
        break;
    }

    case INST_PROGRESS: {
        installer_status_t status;
        installer_get_status(&status);
        fs_card(x + 42, y + 144, w - 84, 220, 0);
        fs_t13(x + 68, y + 174, status.detail[0] ? status.detail : "Preparing installation", FS_WHITE);
        fb_blend_rounded_rect(x + 68, y + 224, w - 136, 10, 5, 0x00B8C4E8, 50);
        if (status.percent > 0) fb_fill_rounded_rect_grad(x + 68, y + 224, (w - 136) * status.percent / 100 > 10 ? (w - 136) * status.percent / 100 : 10, 10, 5, UI_NEON_CYAN, UI_NEON_VIOLET, 255);
        char pct[32];
        snprintf(pct, sizeof(pct), "%d%%", status.percent);
        fs_t13(x + 68, y + 250, pct, FS_DIM);
        fs_fit13(x + 68, y + 296, w - 136, "Keep the computer powered on until installation is complete.", FS_DIM);
        break;
    }

    case INST_ERROR:
        fs_card(x + 42, y + 142, w - 84, 164, 0);
        fs_fit13(x + 66, y + 166, w - 132, ins.error, T(DANGER));
        fs_fit13(x + 66, y + 210, w - 132,
                    "Restart from the installer media to try again. Do not remove power while the disk activity light is on.", FS_DIM);
        ins_button(bx, by, 180, 42, "Restart installer", 1, 1);
        break;

    case INST_COMPLETE:
        fs_card(x + 42, y + 142, w - 84, 164, 0);
        fb_text(x + 66, y + 160, "Installation complete", T(OK), FONT_SUBHEAD);
        fs_t13(x + 66, y + 218, "Your account and first-run configuration are saved.", FS_DIM);
        fs_t13(x + 66, y + 250, "Remove the installer media, then restart: the computer starts Kryzen from its own disk.", FS_DIM);
        fs_t13(x + 66, y + 274, "On UEFI firmware, pick the disk (or EFI/Kryzen/BOOTX64.EFI) in the boot menu if it does not start by itself.", FS_DIM);
        ins_button(bx, by, 180, 42, "Restart now", 1, 1);
        break;
    }
}

static void installer_on_show(void)
{
    memset(&ins, 0, sizeof(ins));
    ins.page = INST_WELCOME;
    ins.selected = -1;
    ins.efi_size_mb = 512;
    installer_scan_disks();
}

static void installer_on_complete(void)
{
    ins.page = INST_COMPLETE;
    desktop_invalidate();
}

static void installer_run_selected(void)
{
    if (ins.selected < 0 || ins.selected >= ins.disk_count) return;
    uint64_t gap_start = 0, gap_end = 0, data_size_mb = 0;
    if (ins.manual) {
        if (ins.gap_selected < 0 || ins.gap_selected >= ins.gap_count) {
            snprintf(ins.error, sizeof(ins.error), "Choose a free GPT region before continuing.");
            ins.page = INST_ERROR;
            return;
        }
        gap_start = ins.gaps[ins.gap_selected].start;
        gap_end = ins.gaps[ins.gap_selected].end;
        data_size_mb = installer_manual_data_mb();
        if (data_size_mb < 1024) {
            snprintf(ins.error, sizeof(ins.error), "The selected free region must fit at least 1 GB of Kryzen storage.");
            ins.page = INST_ERROR;
            return;
        }
    }
    ins.page = INST_PROGRESS;
    ins.error[0] = 0;
    ui_shell_draw_frame();
    char err[128];
    if (installer_install_disk(ins.disks[ins.selected].dev, ins.manual,
                               gap_start, gap_end, (uint64_t)ins.efi_size_mb,
                               data_size_mb, ins.manual ? ins.existing_efi_part : 0,
                               err, sizeof(err)) == 0) {
        ui_shell_show_setup_wizard();
        return;
    }
    snprintf(ins.error, sizeof(ins.error), "%s", err[0] ? err : "The installation failed.");
    ins.page = INST_ERROR;
    ui_shell_draw_frame();
}

static void installer_input(uint32_t key, char ch)
{
    (void)ch;
    if (ins.page == INST_DISKS) {
        if (key == 0x102 && ins.selected > 0) installer_select_disk(ins.selected - 1);
        else if (key == 0x103 && ins.selected + 1 < ins.disk_count) installer_select_disk(ins.selected + 1);
        else if (key == 10 && ins.selected >= 0) ins.page = INST_LAYOUT;
    } else if (ins.page == INST_LAYOUT && key == 0x102 && ins.manual && ins.gap_count > 1) {
        ins.gap_selected = (ins.gap_selected + ins.gap_count - 1) % ins.gap_count;
        ins.data_size_gb = 0;
    } else if (ins.page == INST_LAYOUT && key == 0x103 && ins.manual && ins.gap_count > 1) {
        ins.gap_selected = (ins.gap_selected + 1) % ins.gap_count;
        ins.data_size_gb = 0;
    } else if (key == 10) {
        if (ins.page == INST_WELCOME) ins.page = INST_CHECKS;
        else if (ins.page == INST_CHECKS) { if (installer_ready(NULL, NULL, NULL)) { installer_scan_disks(); ins.page = INST_DISKS; } }
        else if (ins.page == INST_LAYOUT &&
                 (ins.manual ? installer_manual_data_mb() >= 1024 :
                               installer_guided_data_mb(ins.selected >= 0 ? &ins.disks[ins.selected] : NULL) >= 1024))
            ins.page = INST_REVIEW;
        else if (ins.page == INST_REVIEW) installer_run_selected();
    } else if (key == 0x101) {
        if (ins.page == INST_CHECKS) ins.page = INST_WELCOME;
        else if (ins.page == INST_DISKS) ins.page = INST_CHECKS;
        else if (ins.page == INST_LAYOUT) ins.page = INST_DISKS;
        else if (ins.page == INST_REVIEW) ins.page = INST_LAYOUT;
    }
    ui_shell_draw_frame();
}

static void installer_click(int mx, int my)
{
    int x, y, w, h;
    installer_geometry(&x, &y, &w, &h);
    int bx = x + w - 28 - 180, by = y + h - 60;
    if (ins.page != INST_PROGRESS) {
        if (fs_tray_click(mx, my)) { ui_shell_draw_frame(); return; }
        if (fs_sidebar_hit(mx, my) == 1) { g_fs.status_hidden = !g_fs.status_hidden; ui_shell_draw_frame(); return; }
    }
    if (ins.page == INST_WELCOME) {
        if (ui_hit(mx, my, x + w - 388, by, 150, 42)) { ui_shell_unlock_desktop(); return; }
        if (ui_hit(mx, my, bx, by, 180, 42)) ins.page = INST_CHECKS;
    } else if (ins.page == INST_CHECKS) {
        if (ui_hit(mx, my, x + 28, by, 110, 42)) ins.page = INST_WELCOME;
        else if (ui_hit(mx, my, bx, by, 180, 42) && installer_ready(NULL, NULL, NULL)) { installer_scan_disks(); ins.page = INST_DISKS; }
    } else if (ins.page == INST_DISKS) {
        if (ui_hit(mx, my, x + 28, by, 110, 42)) ins.page = INST_CHECKS;
        else if (ui_hit(mx, my, x + 150, by, 110, 42)) installer_scan_disks();
        else if (ui_hit(mx, my, bx, by, 180, 42) && ins.selected >= 0) ins.page = INST_LAYOUT;
        else {
            for (int i = 0; i < ins.disk_count; i++)
                if (ui_hit(mx, my, x + 32, y + 110 + i * 48, w - 64, 40)) installer_select_disk(i);
        }
    } else if (ins.page == INST_LAYOUT) {
        installer_disk_row_t *row = ins.selected >= 0 && ins.selected < ins.disk_count ? &ins.disks[ins.selected] : NULL;
        int manual_available = row && row->disk.scheme == DM_GPT && ins.gap_count > 0;
        if (ui_hit(mx, my, x + 54, y + 106, 180, 30)) { ins.manual = 0; ins.data_size_gb = 0; }
        else if (ui_hit(mx, my, x + 244, y + 106, 220, 30) && manual_available) {
            ins.manual = 1;
            ins.gap_selected = 0;
            ins.existing_efi_part = installer_first_efi_partition(row);
            ins.data_size_gb = 0;
        }
        else if (ui_hit(mx, my, x + 28, by, 110, 42)) ins.page = INST_DISKS;
        else if (ins.manual && ui_hit(mx, my, x + 64, y + 350, 112, 28)) {
            if (ins.gap_count) ins.gap_selected = (ins.gap_selected + ins.gap_count - 1) % ins.gap_count;
            ins.data_size_gb = 0;
        }
        else if (ins.manual && ui_hit(mx, my, x + 184, y + 350, 112, 28)) {
            if (ins.gap_count) ins.gap_selected = (ins.gap_selected + 1) % ins.gap_count;
            ins.data_size_gb = 0;
        }
        else if (ins.manual && ui_hit(mx, my, x + 320, y + 350, 72, 28)) {
            uint64_t max_gb = installer_manual_data_mb() / 1024;
            int current = ins.data_size_gb ? ins.data_size_gb : (int)max_gb;
            ins.data_size_gb = current > 1 ? current - 1 : 1;
        }
        else if (ins.manual && ui_hit(mx, my, x + 398, y + 350, 72, 28)) ins.data_size_gb = 0;
        else if (ins.manual && ui_hit(mx, my, x + 476, y + 350, 72, 28)) {
            uint64_t max_gb = installer_manual_data_mb() / 1024;
            int current = ins.data_size_gb ? ins.data_size_gb : (int)max_gb;
            if ((uint64_t)current < max_gb) ins.data_size_gb = current + 1;
        }
        else if (!ins.manual && ui_hit(mx, my, x + 64, y + 214, 112, 28)) ins.efi_size_mb = 256;
        else if (!ins.manual && ui_hit(mx, my, x + 192, y + 214, 112, 28)) ins.efi_size_mb = 512;
        else if (!ins.manual && ui_hit(mx, my, x + 320, y + 214, 112, 28)) ins.efi_size_mb = 1024;
        else if (ins.manual && ui_hit(mx, my, x + 54, y + 422, 178, 26) &&
                 installer_first_efi_partition(row)) {
            if (!ins.existing_efi_part) ins.existing_efi_part = installer_first_efi_partition(row);
            ins.data_size_gb = 0;
        }
        else if (ins.manual && ui_hit(mx, my, x + 242, y + 422, 144, 26)) {
            ins.existing_efi_part = 0;
            ins.data_size_gb = 0;
        }
        else if (ins.manual && ins.existing_efi_part && ui_hit(mx, my, x + 398, y + 422, 72, 26)) {
            installer_cycle_efi_partition(-1);
            ins.data_size_gb = 0;
        }
        else if (ins.manual && ins.existing_efi_part && ui_hit(mx, my, x + 476, y + 422, 72, 26)) {
            installer_cycle_efi_partition(1);
            ins.data_size_gb = 0;
        }
        else if (ins.manual && !ins.existing_efi_part && ui_hit(mx, my, x + 64, y + 456, 112, 26)) { ins.efi_size_mb = 256; ins.data_size_gb = 0; }
        else if (ins.manual && !ins.existing_efi_part && ui_hit(mx, my, x + 192, y + 456, 112, 26)) { ins.efi_size_mb = 512; ins.data_size_gb = 0; }
        else if (ins.manual && !ins.existing_efi_part && ui_hit(mx, my, x + 320, y + 456, 112, 26)) { ins.efi_size_mb = 1024; ins.data_size_gb = 0; }
        else if (ui_hit(mx, my, bx, by, 180, 42)) {
            uint64_t guided_mb = installer_guided_data_mb(row);
            int valid = ins.manual ? installer_manual_data_mb() >= 1024 : guided_mb >= 1024;
            if (valid) ins.page = INST_REVIEW;
        }
    } else if (ins.page == INST_REVIEW) {
        if (ui_hit(mx, my, x + 28, by, 110, 42)) ins.page = INST_LAYOUT;
        else if (ui_hit(mx, my, bx, by, 180, 42)) installer_run_selected();
    } else if (ins.page == INST_ERROR && ui_hit(mx, my, bx, by, 180, 42)) {
        desktop_system_restart();
    } else if (ins.page == INST_COMPLETE && ui_hit(mx, my, bx, by, 180, 42)) {
        desktop_system_restart();
    }
    ui_shell_draw_frame();
}
