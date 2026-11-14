/*
 * =============================================================================
 * JARVIS OS — Disks (apps/disks.c)
 *
 * Disk management, in the style of Windows Disk Management:
 *
 *   the disk list           every disk the storage drivers found
 *   the partition map       volumes and unallocated space, clickable
 *   volume details          file system, label, where it appears, space
 *
 * Operations (kernel/diskmgmt.c, kernel/diskfs.c):
 *   New volume              partition in unallocated space, formatted
 *   Format                  FAT32 / FAT16 with a label (erases the volume)
 *   Change label            rename a volume
 *   Extend / Shrink         grow into free space after it, or give space back
 *   Delete volume           remove a partition
 *   Initialize disk         new empty MBR partition table (erases the disk)
 *   Check disk / repair /   verify the JARVIS disk (separate process)
 *   surface scan
 *   Eject                   safe removal of a USB drive
 *
 * The system disk (where JARVIS OS keeps its files) can be checked,
 * extended and shrunk, but never formatted or deleted.
 * The same operations are in the Terminal ("disk", "format") and are
 * available to JARVIS (disk_action, check_disk, eject_drive ...).
 * =============================================================================
 */

#define DK_MAX_DISKS 8
#define DK_LIST_W    270
#define DK_ROW_H     64

typedef struct {
    int      kind;              /* 0 = volume/partition, 1 = unallocated */
    int      part;              /* index in dm_disk_t.parts (kind 0)     */
    uint64_t start, sectors;
} dk_seg_t;

enum { DKD_NONE, DKD_NEW, DKD_FORMAT, DKD_LABEL, DKD_EXTEND, DKD_SHRINK, DKD_DELETE, DKD_INIT };

static struct {
    wm_window_t *win;
    dm_disk_t    disks[DK_MAX_DISKS];
    block_info_t info[DK_MAX_DISKS];
    int          count, sel;
    dk_seg_t     segs[DM_MAX_PARTS * 2 + 2];
    int          nsegs, seg;
    uint32_t     gen;
    char         msg[160];
    uint32_t     msg_color;
    /* dialog */
    int          dlg, dlg_field;
    char         dlg_size[16], dlg_label[16];
    int          dlg_fs;        /* 0 auto, 1 fat32, 2 fat16 */
    uint64_t     dlg_min_mb, dlg_max_mb;
} dk;

static void dk_size(uint64_t bytes, char *out, size_t n)
{
    if (bytes >= (1ULL << 30))
        snprintf(out, n, "%u.%u GB", (unsigned)(bytes >> 30), (unsigned)(((bytes >> 20) % 1024) * 10 / 1024));
    else if (bytes >= (1ULL << 20))
        snprintf(out, n, "%u MB", (unsigned)(bytes >> 20));
    else
        snprintf(out, n, "%u KB", (unsigned)(bytes >> 10));
}

static uint32_t dk_part_color(const char *type)
{
    if (!strcmp(type, "JARVISFS")) return T(ACCENT);
    if (strstr(type, "FAT"))       return 0x002F6FD0;
    if (strstr(type, "NTFS") || strstr(type, "exFAT")) return 0x0000A4EF;
    if (strstr(type, "Linux"))     return 0x00E0A030;
    if (strstr(type, "EFI"))       return 0x007FBA00;
    if (!strcmp(type, "Unformatted")) return 0x00707880;
    return 0x00808890;
}

static const char *dk_scheme(int s)
{
    switch (s) { case DM_MBR: return "MBR"; case DM_GPT: return "GPT"; case DM_WHOLE: return "None (one volume)"; case DM_EMPTY: return "None (empty)"; default: return "?"; }
}

static void dk_build_segments(void)
{
    dk.nsegs = 0;
    if (!dk.count) return;
    dm_disk_t *d = &dk.disks[dk.sel];
    uint64_t pos = d->scheme == DM_MBR ? 2048 : 0;
    for (int i = 0; i <= d->nparts; i++) {
        uint64_t next = i < d->nparts ? d->parts[i].start : d->sectors;
        if (next > pos + 4096 && d->scheme != DM_GPT) {
            dk_seg_t *s = &dk.segs[dk.nsegs++];
            s->kind = 1; s->part = -1; s->start = pos; s->sectors = next - pos;
        }
        if (i < d->nparts) {
            dk_seg_t *s = &dk.segs[dk.nsegs++];
            s->kind = 0; s->part = i; s->start = d->parts[i].start; s->sectors = d->parts[i].sectors;
            pos = d->parts[i].start + d->parts[i].sectors;
        }
    }
    if (dk.seg >= dk.nsegs) dk.seg = 0;
}

static void dk_refresh(void)
{
    int prev_dev = dk.count ? dk.disks[dk.sel].dev : -1;
    dk.count = 0;
    dk.gen = g_storage_generation;
    for (int dev = 0; dev < block_count() && dk.count < DK_MAX_DISKS; dev++) {
        if (block_get_info(dev, &dk.info[dk.count]) != 0) continue;
        if (dm_disk_info(dev, &dk.disks[dk.count]) != 0) continue;
        dk.count++;
    }
    dk.sel = 0;
    for (int i = 0; i < dk.count; i++) if (dk.disks[i].dev == prev_dev) dk.sel = i;
    dk_build_segments();
}

static dm_part_t *dk_sel_part(void)
{
    if (!dk.count || dk.seg >= dk.nsegs || dk.segs[dk.seg].kind != 0) return NULL;
    return &dk.disks[dk.sel].parts[dk.segs[dk.seg].part];
}

static void dk_say(uint32_t col, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dk.msg, sizeof(dk.msg), fmt, ap);
    va_end(ap);
    dk.msg_color = col;
}

/* ---- Drawing ------------------------------------------------------------- */
static void dk_draw_list(int x, int y, int h)
{
    char buf[64], sz[16];
    fb_fill_rect(x, y, DK_LIST_W, h, T(SURFACE));
    fb_fill_rect(x + DK_LIST_W - 1, y, 1, h, T(BORDER));
    fb_draw_text_bold(x + 16, y + 14, "Disks", T(TEXT));

    int ry = y + 44;
    for (int i = 0; i < dk.count; i++, ry += DK_ROW_H) {
        dm_disk_t *d = &dk.disks[i];
        uint32_t fg = T(TEXT), dim = T(TEXT_DIM);
        if (i == dk.sel) {
            fb_fill_rounded_rect(x + 8, ry, DK_LIST_W - 16, DK_ROW_H - 4, 6, T(ACCENT));
            fg = dim = T(ACCENT_TEXT);
        }
        icon_draw(d->removable ? ICON_USB : ICON_DRIVE, x + 18, ry + 14, 32);
        snprintf(buf, sizeof(buf), "Disk %d (%s)%s", d->dev, d->name, d->system ? "  System" : "");
        fb_draw_text_bold(x + 60, ry + 8, buf, fg);
        ui_text_fit(x + 60, ry + 26, DK_LIST_W - 76, d->model[0] ? d->model : "Unknown model", dim);
        dk_size(d->sectors * 512, sz, sizeof(sz));
        snprintf(buf, sizeof(buf), "%s  %s", sz, d->removable ? "Removable" : "Internal");
        fb_draw_text(x + 60, ry + 42, buf, dim);
    }
    if (!dk.count) fb_draw_text(x + 16, y + 48, "No disks found.", T(TEXT_DIM));
    ui_button(x + 16, y + h - 44, DK_LIST_W - 32, 30, "Refresh", T(HOVER), T(TEXT));
}

static void dk_row(int x, int y, const char *label, const char *value)
{
    fb_draw_text(x, y, label, T(TEXT_DIM));
    fb_draw_text(x + 160, y, value, T(TEXT));
}

/* Action buttons for the current selection: label, id, style (0 normal,
 * 1 primary, 2 danger). Shared by drawing and clicking. */
enum { DKA_OPEN = 1, DKA_NEW, DKA_FORMAT, DKA_LABEL, DKA_EXTEND, DKA_SHRINK, DKA_DELETE, DKA_INIT, DKA_EJECT,
       DKA_CHECK, DKA_REPAIR, DKA_SURFACE };
typedef struct { const char *label; int id, style, w; } dk_btn_t;

static int dk_buttons(dk_btn_t *b)
{
    int n = 0;
    if (!dk.count) return 0;
    dm_disk_t *d = &dk.disks[dk.sel];
    dm_part_t *p = dk_sel_part();
    diskfs_check_t ck;
    diskfs_check_status(&ck);
    #define ADD(L, I, S) do { b[n].label = L; b[n].id = I; b[n].style = S; b[n].w = fb_text_width(L) + 28; n++; } while (0)
    if (dk.seg < dk.nsegs && dk.segs[dk.seg].kind == 1) {
        if (!d->system) ADD("New volume...", DKA_NEW, 1);
    } else if (p) {
        if (p->mount[0]) ADD("Open", DKA_OPEN, 0);
        if (d->system) {
            if (!ck.running) { ADD("Check disk", DKA_CHECK, 1); ADD("Check & repair", DKA_REPAIR, 0); ADD("Surface scan", DKA_SURFACE, 0); }
            ADD("Extend...", DKA_EXTEND, 0);
            ADD("Shrink...", DKA_SHRINK, 0);
        } else {
            ADD("Format...", DKA_FORMAT, 0);
            if (!strncmp(p->fs, "FAT", 3)) ADD("Change label...", DKA_LABEL, 0);
            if (d->scheme != DM_GPT) { ADD("Extend...", DKA_EXTEND, 0); ADD("Shrink...", DKA_SHRINK, 0); }
            ADD("Delete volume", DKA_DELETE, 2);
        }
    }
    if (!d->system && d->scheme != DM_MBR) ADD("Initialize disk...", DKA_INIT, 0);
    if (d->removable) ADD("Eject", DKA_EJECT, 1);
    #undef ADD
    return n;
}

#define DK_MAP_Y   60
#define DK_MAP_H   58

static void dk_draw_details(int x, int y, int w, int h)
{
    char buf[160], a[16], b[16];
    if (!dk.count) return;
    dm_disk_t *d = &dk.disks[dk.sel];
    block_info_t *bi = &dk.info[dk.sel];
    block_get_info(d->dev, bi);                     /* live counters */

    int px = x + 20, pw = w - 40;
    fb_draw_text_bold(px, y + 14, d->model[0] ? d->model : d->name, T(TEXT));
    dk_size(d->sectors * 512, a, sizeof(a));
    snprintf(buf, sizeof(buf), "%s  " SYM_BULLET "  %s  " SYM_BULLET "  %s  " SYM_BULLET "  partition table: %s", d->name, a,
             d->removable ? "USB, removable" : d->system ? "System disk" : "Internal", dk_scheme(d->scheme));
    fb_draw_text(px, y + 34, buf, T(TEXT_DIM));

    /* partition map: proportional, but every segment at least 70 px */
    int my = y + DK_MAP_Y, mh = DK_MAP_H;
    fb_fill_rounded_rect(px, my, pw, mh, 6, T(HOVER));
    uint64_t total = d->sectors ? d->sectors : 1;
    int minw = 70, fixed = 0;
    for (int i = 0; i < dk.nsegs; i++) if ((int)(dk.segs[i].sectors * (uint64_t)pw / total) < minw) fixed++;
    int avail = pw - fixed * minw;
    if (avail < 0) avail = 0;
    uint64_t big = 0;
    for (int i = 0; i < dk.nsegs; i++) if ((int)(dk.segs[i].sectors * (uint64_t)pw / total) >= minw) big += dk.segs[i].sectors;
    int sx = px;
    for (int i = 0; i < dk.nsegs; i++) {
        dk_seg_t *s = &dk.segs[i];
        int sw = (int)(s->sectors * (uint64_t)pw / total) < minw ? minw : (big ? (int)(s->sectors * (uint64_t)avail / big) : minw);
        if (i == dk.nsegs - 1) sw = px + pw - sx;
        uint32_t col = s->kind ? 0x00404850 : dk_part_color(d->parts[s->part].fs);
        if (s->kind) {
            fb_fill_rounded_rect(sx + 1, my + 1, sw - 2, mh - 2, 5, T(SURFACE));
            for (int k = 0; k < sw - 4; k += 8) fb_fill_rect(sx + 2 + k, my + 4, 4, 3, T(BORDER));
        } else fb_fill_rounded_rect(sx + 1, my + 1, sw - 2, mh - 2, 5, col);
        if (i == dk.seg) { fb_draw_rounded_rect(sx, my - 2, sw, mh + 4, 6, T(TEXT)); fb_draw_rounded_rect(sx + 1, my - 1, sw - 2, mh + 2, 5, T(TEXT)); }
        uint32_t tc = s->kind ? T(TEXT) : 0x00FFFFFF;
        dk_size(s->sectors * 512, a, sizeof(a));
        if (s->kind) snprintf(buf, sizeof(buf), "Unallocated");
        else {
            dm_part_t *p = &d->parts[s->part];
            snprintf(buf, sizeof(buf), "%s", p->label[0] ? p->label : p->num ? "Volume" : d->system ? "Local Disk" : "Volume");
        }
        ui_text_fit(sx + 8, my + 6, sw - 14, buf, tc);
        if (!s->kind) snprintf(buf, sizeof(buf), "%s  %s", a, d->parts[s->part].fs);
        else snprintf(buf, sizeof(buf), "%s free", a);
        ui_text_fit(sx + 8, my + 22, sw - 14, buf, tc);
        if (!s->kind && d->parts[s->part].mount[0]) ui_text_fit(sx + 8, my + 38, sw - 14, d->parts[s->part].mount, tc);
        sx += sw;
    }

    /* selection details */
    int vy = my + mh + 18;
    dm_part_t *p = dk_sel_part();
    if (dk.seg < dk.nsegs && dk.segs[dk.seg].kind == 1) {
        fb_draw_text_bold(px, vy, "Unallocated space", T(TEXT));
        vy += 24;
        dk_size(dk.segs[dk.seg].sectors * 512, a, sizeof(a));
        dk_row(px, vy, "Size", a); vy += 20;
        fb_draw_text(px, vy, d->system ? "The system disk has no partition table; this space can be used by Extend." :
                                         "Create a new volume here with New volume.", T(TEXT_DIM));
        vy += 20;
    } else if (p) {
        snprintf(buf, sizeof(buf), "%s%s", p->num ? "Volume " : "Volume", p->num ? "" : "");
        if (p->num) snprintf(buf, sizeof(buf), "Partition %d", p->num);
        else snprintf(buf, sizeof(buf), "Volume");
        fb_draw_text_bold(px, vy, buf, T(TEXT));
        vy += 24;
        dk_row(px, vy, "Name", p->label[0] ? p->label : "(no label)"); vy += 20;
        dk_row(px, vy, "File system", p->fs); vy += 20;
        dk_row(px, vy, "Shown in Files as", p->mount[0] ? p->mount : "- (not mounted)"); vy += 20;
        uint64_t tot = 0, fr = 0;
        if (d->system) { diskfs_stats_t st; if (diskfs_get_stats(&st) == 0) { tot = st.total_bytes; fr = st.total_bytes - st.used_bytes; } }
        else if (p->mount[0]) fatfs_volume_info(p->mount, NULL, 0, NULL, &tot, &fr);
        if (tot) {
            dk_size(tot - fr, a, sizeof(a));
            dk_size(tot, b, sizeof(b));
            snprintf(buf, sizeof(buf), "%s of %s used", a, b);
            dk_row(px, vy, "Space", buf);
            int pct = (int)((tot - fr) * 100 / tot);
            ui_progress(px + 330, vy + 3, pw - 330, 10, pct, T(HOVER), pct > 90 ? T(DANGER) : T(ACCENT));
            vy += 20;
        }
        dk_size(p->sectors * 512, a, sizeof(a));
        snprintf(buf, sizeof(buf), "%s, from sector %u", a, (unsigned)p->start);
        dk_row(px, vy, "Partition", buf); vy += 20;
    }

    vy += 6;
    dk_size(bi->sectors_read * 512, a, sizeof(a));
    dk_size(bi->sectors_written * 512, b, sizeof(b));
    snprintf(buf, sizeof(buf), "%s read, %s written since start", a, b);
    dk_row(px, vy, "Disk activity", buf); vy += 20;
    snprintf(buf, sizeof(buf), "%u", bi->errors);
    fb_draw_text(px, vy, "I/O errors", T(TEXT_DIM));
    fb_draw_text(px + 160, vy, buf, bi->errors ? T(DANGER) : T(TEXT));

    /* actions */
    int by = y + h - 150;
    fb_fill_rect(x, by - 12, w, 1, T(BORDER));
    dk_btn_t btn[12];
    int nb = dk_buttons(btn), bx = px;
    for (int i = 0; i < nb; i++) {
        uint32_t bg = btn[i].style == 1 ? T(ACCENT) : btn[i].style == 2 ? T(DANGER) : T(HOVER);
        uint32_t fg = btn[i].style == 1 ? T(ACCENT_TEXT) : btn[i].style == 2 ? 0x00FFFFFF : T(TEXT);
        if (bx + btn[i].w > px + pw) { bx = px; by += 38; }
        ui_button(bx, by, btn[i].w, 30, btn[i].label, bg, fg);
        bx += btn[i].w + 8;
    }

    /* check results / last message */
    int cy = by + 44;
    diskfs_check_t ck;
    diskfs_check_status(&ck);
    if (d->system && (ck.running || ck.done) && !dk.msg[0]) {
        snprintf(buf, sizeof(buf), "%s%s%s", ck.running ? ck.phase : "Last check: ",
                 ck.running ? "" : (ck.repair ? "repair - " : ck.surface ? "surface scan - " : ""),
                 ck.running ? "" : ck.summary);
        ui_text_fit(px, cy, pw - 160, buf, ck.done && ck.problems ? T(WARN) : T(TEXT));
        ui_progress(px + pw - 150, cy + 3, 150, 10, ck.progress, T(HOVER), T(ACCENT));
        cy += 20;
        for (int i = 0; i < ck.issue_count && cy < y + h - 14; i++, cy += 18)
            ui_text_fit(px + 12, cy, pw - 12, ck.issue[i], T(TEXT));
    } else if (dk.msg[0]) {
        ui_text_fit(px, cy, pw, dk.msg, dk.msg_color);
    }
}

/* ---- Dialog ----------------------------------------------------------------- */
static const char *const dk_fs_names[3] = { "Automatic", "FAT32", "FAT16" };

static void dk_dlg_rect(int x, int y, int w, int h, int *dx, int *dy, int *dw, int *dh)
{
    *dw = 460; *dh = 270;
    *dx = x + (w - *dw) / 2; *dy = y + (h - *dh) / 2;
}

static void dk_draw_field(int x, int y, int w, const char *label, const char *val, int focused)
{
    fb_draw_text(x, y + 6, label, T(TEXT_DIM));
    fb_fill_rounded_rect(x + 140, y, w - 140, 28, 4, T(INPUT));
    fb_draw_rounded_rect(x + 140, y, w - 140, 28, 4, focused ? T(ACCENT) : T(BORDER));
    fb_draw_text(x + 148, y + 6, val, T(TEXT));
    if (focused) fb_fill_rect(x + 149 + fb_text_width(val), y + 6, 1, 16, T(TEXT));
}

static void dk_draw_dialog(int x, int y, int w, int h)
{
    if (!dk.dlg) return;
    fb_blend_rect(x, y, w, h, 0x00000000, 110);
    int dx, dy, dw, dh;
    dk_dlg_rect(x, y, w, h, &dx, &dy, &dw, &dh);
    fb_fill_rounded_rect(dx, dy, dw, dh, 8, T(SURFACE));
    fb_draw_rounded_rect(dx, dy, dw, dh, 8, T(BORDER));
    static const char *const titles[] = { "", "New volume", "Format", "Change label", "Extend volume", "Shrink volume", "Delete volume", "Initialize disk" };
    fb_draw_text_scaled(dx + 20, dy + 14, titles[dk.dlg], T(TEXT), 2);
    char buf[160];
    dm_disk_t *d = &dk.disks[dk.sel];
    dm_part_t *p = dk_sel_part();
    int fy = dy + 62;
    const char *warn = NULL;
    switch (dk.dlg) {
    case DKD_NEW:
        snprintf(buf, sizeof(buf), "Size in MB (%u to %u):", (unsigned)dk.dlg_min_mb, (unsigned)dk.dlg_max_mb);
        dk_draw_field(dx + 20, fy, dw - 40, buf, dk.dlg_size, dk.dlg_field == 0); fy += 38;
        dk_draw_field(dx + 20, fy, dw - 40, "Label:", dk.dlg_label, dk.dlg_field == 1); fy += 38;
        snprintf(buf, sizeof(buf), "File system:  %s   (click to change)", dk_fs_names[dk.dlg_fs]);
        fb_draw_text(dx + 20, fy + 6, buf, T(TEXT)); fy += 34;
        break;
    case DKD_FORMAT:
        dk_draw_field(dx + 20, fy, dw - 40, "Label:", dk.dlg_label, 1); fy += 38;
        snprintf(buf, sizeof(buf), "File system:  %s   (click to change)", dk_fs_names[dk.dlg_fs]);
        fb_draw_text(dx + 20, fy + 6, buf, T(TEXT)); fy += 34;
        warn = "Formatting erases everything on this volume.";
        break;
    case DKD_LABEL:
        dk_draw_field(dx + 20, fy, dw - 40, "New label:", dk.dlg_label, 1); fy += 38;
        fb_draw_text(dx + 20, fy, "Up to 11 letters; shown as the drive's name.", T(TEXT_DIM));
        break;
    case DKD_EXTEND: case DKD_SHRINK:
        snprintf(buf, sizeof(buf), "New size in MB (%u to %u):", (unsigned)dk.dlg_min_mb, (unsigned)dk.dlg_max_mb);
        dk_draw_field(dx + 20, fy, dw - 40, buf, dk.dlg_size, 1); fy += 38;
        fb_draw_text(dx + 20, fy, dk.dlg == DKD_EXTEND ? "Files stay where they are; the volume gets the free space after it."
                                                       : "Only space with no files in it can be given back.", T(TEXT_DIM));
        break;
    case DKD_DELETE:
        snprintf(buf, sizeof(buf), "Delete %s (%s) on %s?", p && p->label[0] ? p->label : "this volume", p ? p->fs : "", d->name);
        ui_text_fit(dx + 20, fy, dw - 40, buf, T(TEXT));
        warn = "Everything stored on it will be lost.";
        break;
    case DKD_INIT:
        snprintf(buf, sizeof(buf), "Create a new, empty MBR partition table on %s (%s)?", d->name, d->model);
        ui_text_fit(dx + 20, fy, dw - 40, buf, T(TEXT));
        warn = "Every volume and file on this disk will be erased.";
        break;
    }
    if (warn) fb_draw_text(dx + 20, dy + dh - 84, warn, T(DANGER));
    int danger = dk.dlg == DKD_FORMAT || dk.dlg == DKD_DELETE || dk.dlg == DKD_INIT;
    static const char *const ok[] = { "", "Create", "Format", "Save", "Extend", "Shrink", "Delete", "Initialize" };
    ui_button(dx + dw - 240, dy + dh - 50, 110, 32, ok[dk.dlg], danger ? T(DANGER) : T(ACCENT), danger ? 0x00FFFFFF : T(ACCENT_TEXT));
    ui_button(dx + dw - 120, dy + dh - 50, 100, 32, "Cancel", T(HOVER), T(TEXT));
}

void disks_draw_callback(wm_window_t *win, uint32_t *framebuffer)
{
    (void)framebuffer;
    int x, y, w, h;
    wm_get_client(win, &x, &y, &w, &h);
    if (dk.gen != g_storage_generation) dk_refresh();
    fb_fill_rect(x, y, w, h, T(WINDOW));
    dk_draw_list(x, y, h);
    dk_draw_details(x + DK_LIST_W, y, w - DK_LIST_W, h);
    dk_draw_dialog(x, y, w, h);
}

/* ---- Actions ------------------------------------------------------------------ */
static void dk_open_dialog(int which)
{
    dm_disk_t *d = &dk.disks[dk.sel];
    dm_part_t *p = dk_sel_part();
    dk.dlg = which;
    dk.dlg_field = 0;
    dk.dlg_fs = 0;
    dk.dlg_label[0] = 0;
    dk.dlg_size[0] = 0;
    if (which == DKD_NEW) {
        dk.dlg_min_mb = 2;
        dk.dlg_max_mb = dk.segs[dk.seg].sectors / 2048;
        snprintf(dk.dlg_size, sizeof(dk.dlg_size), "%u", (unsigned)dk.dlg_max_mb);
        snprintf(dk.dlg_label, sizeof(dk.dlg_label), "NEW VOLUME");
    } else if (which == DKD_FORMAT || which == DKD_LABEL) {
        if (p) snprintf(dk.dlg_label, sizeof(dk.dlg_label), "%s", p->label);
    } else if ((which == DKD_EXTEND || which == DKD_SHRINK) && p) {
        uint64_t cur = p->sectors / 2048;
        if (d->system) {
            uint64_t mx = diskfs_max_blocks() / 256, mn = diskfs_min_blocks() / 256 + 1;
            dk.dlg_min_mb = which == DKD_EXTEND ? cur + 1 : mn;
            dk.dlg_max_mb = which == DKD_EXTEND ? mx : cur - 1;
        } else {
            uint64_t room = cur;
            for (int i = 0; i < dk.nsegs; i++)
                if (dk.segs[i].kind == 1 && dk.segs[i].start == p->start + p->sectors) room += dk.segs[i].sectors / 2048;
            if (d->scheme == DM_WHOLE) room = d->sectors / 2048;
            dk.dlg_min_mb = which == DKD_EXTEND ? cur + 1 : 2;
            dk.dlg_max_mb = which == DKD_EXTEND ? room : cur - 1;
        }
        snprintf(dk.dlg_size, sizeof(dk.dlg_size), "%u", (unsigned)(which == DKD_EXTEND ? dk.dlg_max_mb : (cur * 3 / 4 > dk.dlg_min_mb ? cur * 3 / 4 : dk.dlg_min_mb)));
        if (dk.dlg_max_mb < dk.dlg_min_mb) {
            dk.dlg = DKD_NONE;
            dk_say(T(WARN), which == DKD_EXTEND ? "There is no free space after this volume to extend into." : "This volume cannot be made smaller.");
        }
    }
}

static void dk_run_dialog(void)
{
    char err[200] = "";
    dm_disk_t *d = &dk.disks[dk.sel];
    dm_part_t *p = dk_sel_part();
    int dev = d->dev, num = p ? p->num : 0, rc = -1;
    long mb = sh_num(dk.dlg_size, -1);
    const char *fs = dk.dlg_fs == 1 ? "fat32" : dk.dlg_fs == 2 ? "fat16" : "auto";
    switch (dk.dlg) {
    case DKD_NEW: {
        if (mb < 2 || (uint64_t)mb > dk.dlg_max_mb) { dk_say(T(DANGER), "Give a size from 2 to %u MB.", (unsigned)dk.dlg_max_mb); return; }
        int pn = 0;
        rc = dm_create_partition(dev, (uint64_t)mb == dk.dlg_max_mb ? 0 : (uint64_t)mb, fs, dk.dlg_label, &pn, err, sizeof(err));
        if (!rc) dk_say(T(OK), "New %u MB volume created and formatted.", (unsigned)mb);
        break;
    }
    case DKD_FORMAT:
        rc = dm_format(dev, num, fs, dk.dlg_label, err, sizeof(err));
        if (!rc) dk_say(T(OK), "Formatted. The volume is empty and ready.");
        break;
    case DKD_LABEL:
        rc = dm_set_label(dev, num, dk.dlg_label, err, sizeof(err));
        if (!rc) dk_say(T(OK), "Label changed.");
        break;
    case DKD_EXTEND: case DKD_SHRINK:
        if (mb < (long)dk.dlg_min_mb || (uint64_t)mb > dk.dlg_max_mb) { dk_say(T(DANGER), "Give a size from %u to %u MB.", (unsigned)dk.dlg_min_mb, (unsigned)dk.dlg_max_mb); return; }
        rc = dm_resize(dev, num, (uint64_t)mb, err, sizeof(err));
        if (!rc) dk_say(T(OK), "The volume is now %u MB.", (unsigned)mb);
        break;
    case DKD_DELETE:
        rc = dm_delete_partition(dev, num, err, sizeof(err));
        if (!rc) dk_say(T(OK), "Volume deleted; its space is unallocated.");
        break;
    case DKD_INIT:
        rc = dm_init_mbr(dev, err, sizeof(err));
        if (!rc) dk_say(T(OK), "The disk is initialized with an empty MBR partition table.");
        break;
    }
    if (rc) dk_say(T(DANGER), "Could not do it: %s", err);
    dk.dlg = DKD_NONE;
    dk_refresh();
    desktop_files_changed();
}

static void dk_eject(dm_disk_t *d)
{
    int port = usb_port_of_block(d->dev);
    if (port < 0 || usb_eject_port(port) != 0) dk_say(T(DANGER), "Could not eject %s.", d->name);
    else {
        dk_say(T(OK), "%s can now be removed safely.", d->name);
        notification_show("Safe to remove", dk.msg, NOTIF_INFO, 5000);
    }
    dk_refresh();
}

static void dk_action(int id)
{
    dm_disk_t *d = &dk.disks[dk.sel];
    dm_part_t *p = dk_sel_part();
    dk.msg[0] = 0;
    switch (id) {
    case DKA_OPEN: if (p && p->mount[0]) { desktop_app_launch(desktop_app_find("Files")); file_explorer_navigate_to(p->mount); } break;
    case DKA_NEW: dk_open_dialog(DKD_NEW); break;
    case DKA_FORMAT: dk_open_dialog(DKD_FORMAT); break;
    case DKA_LABEL: dk_open_dialog(DKD_LABEL); break;
    case DKA_EXTEND: dk_open_dialog(DKD_EXTEND); break;
    case DKA_SHRINK: dk_open_dialog(DKD_SHRINK); break;
    case DKA_DELETE: dk_open_dialog(DKD_DELETE); break;
    case DKA_INIT: dk_open_dialog(DKD_INIT); break;
    case DKA_EJECT: dk_eject(d); break;
    case DKA_CHECK: diskfs_check_start(0, 0); break;
    case DKA_REPAIR: diskfs_check_start(0, 1); break;
    case DKA_SURFACE: diskfs_check_start(1, 0); break;
    }
}

static void dk_dialog_event(wm_input_event_t *event, int w, int h)
{
    char *fld = (dk.dlg == DKD_NEW && dk.dlg_field == 0) || dk.dlg == DKD_EXTEND || dk.dlg == DKD_SHRINK ? dk.dlg_size
              : (dk.dlg == DKD_NEW || dk.dlg == DKD_FORMAT || dk.dlg == DKD_LABEL) ? dk.dlg_label : NULL;
    size_t cap = fld == dk.dlg_size ? sizeof(dk.dlg_size) : sizeof(dk.dlg_label);
    if (event->type == IE_TEXT_INPUT && fld) {
        char c = event->data.text.ch;
        size_t l = strlen(fld);
        if (fld == dk.dlg_size && !(c >= '0' && c <= '9')) return;
        if (fld == dk.dlg_label && l >= 11) return;
        if (c >= 32 && c < 127 && l < cap - 1) { fld[l] = c; fld[l + 1] = 0; }
        return;
    }
    if (event->type == IE_KEY_DOWN) {
        uint32_t k = event->data.keyboard.key;
        if (k == WM_KEY_ESC) dk.dlg = DKD_NONE;
        else if (k == WM_KEY_ENTER) dk_run_dialog();
        else if (k == WM_KEY_TAB && dk.dlg == DKD_NEW) dk.dlg_field ^= 1;
        else if (k == WM_KEY_BACKSPACE && fld && fld[0]) fld[strlen(fld) - 1] = 0;
        return;
    }
    if (event->type != IE_MOUSE_DOWN || event->data.mouse.button != 1) return;
    int dx, dy, dw, dh;
    dk_dlg_rect(0, 0, w, h, &dx, &dy, &dw, &dh);
    int mx = event->data.mouse.x, my = event->data.mouse.y;
    if (ui_hit(mx, my, dx + dw - 240, dy + dh - 50, 110, 32)) { dk_run_dialog(); return; }
    if (ui_hit(mx, my, dx + dw - 120, dy + dh - 50, 100, 32)) { dk.dlg = DKD_NONE; return; }
    if (dk.dlg == DKD_NEW) {
        if (ui_hit(mx, my, dx + 160, dy + 62, dw - 180, 28)) dk.dlg_field = 0;
        else if (ui_hit(mx, my, dx + 160, dy + 100, dw - 180, 28)) dk.dlg_field = 1;
        else if (ui_hit(mx, my, dx + 20, dy + 138, dw - 40, 28)) dk.dlg_fs = (dk.dlg_fs + 1) % 3;
    } else if (dk.dlg == DKD_FORMAT && ui_hit(mx, my, dx + 20, dy + 100, dw - 40, 28)) {
        dk.dlg_fs = (dk.dlg_fs + 1) % 3;
    }
}

void disks_event_callback(wm_window_t *win, wm_input_event_t *event)
{
    int x, y, w, h;
    wm_get_client(win, &x, &y, &w, &h);
    if (dk.dlg) { dk_dialog_event(event, w, h); return; }
    if (event->type == IE_KEY_DOWN && event->data.keyboard.key == WM_KEY_DELETE) {
        dm_part_t *p = dk_sel_part();
        if (p && !dk.disks[dk.sel].system) dk_action(DKA_DELETE);
        return;
    }
    if (event->type != IE_MOUSE_DOWN || event->data.mouse.button != 1) return;
    int mx = event->data.mouse.x, my = event->data.mouse.y;

    if (mx < DK_LIST_W) {
        if (ui_hit(mx, my, 16, h - 44, DK_LIST_W - 32, 30)) { dk_refresh(); dk.msg[0] = 0; return; }
        int row = (my - 44) / DK_ROW_H;
        if (my >= 44 && row < dk.count) { dk.sel = row; dk.seg = 0; dk.msg[0] = 0; dk_build_segments(); }
        return;
    }
    if (!dk.count) return;
    int px = DK_LIST_W + 20, pw = w - DK_LIST_W - 40;
    /* partition map: same geometry as drawing */
    if (my >= DK_MAP_Y && my < DK_MAP_Y + DK_MAP_H) {
        dm_disk_t *d = &dk.disks[dk.sel];
        uint64_t total = d->sectors ? d->sectors : 1;
        int minw = 70, fixed = 0;
        for (int i = 0; i < dk.nsegs; i++) if ((int)(dk.segs[i].sectors * (uint64_t)pw / total) < minw) fixed++;
        int avail = pw - fixed * minw;
        if (avail < 0) avail = 0;
        uint64_t big = 0;
        for (int i = 0; i < dk.nsegs; i++) if ((int)(dk.segs[i].sectors * (uint64_t)pw / total) >= minw) big += dk.segs[i].sectors;
        int sx = px;
        for (int i = 0; i < dk.nsegs; i++) {
            int sw = (int)(dk.segs[i].sectors * (uint64_t)pw / total) < minw ? minw : (big ? (int)(dk.segs[i].sectors * (uint64_t)avail / big) : minw);
            if (i == dk.nsegs - 1) sw = px + pw - sx;
            if (mx >= sx && mx < sx + sw) { dk.seg = i; dk.msg[0] = 0; return; }
            sx += sw;
        }
        return;
    }
    dk_btn_t btn[12];
    int nb = dk_buttons(btn), bx = px, by = h - 150;
    for (int i = 0; i < nb; i++) {
        if (bx + btn[i].w > px + pw) { bx = px; by += 38; }
        if (ui_hit(mx, my, bx, by, btn[i].w, 30)) { dk_action(btn[i].id); return; }
        bx += btn[i].w + 8;
    }
}

wm_window_t *disks_app_open(int x, int y, int w, int h)
{
    memset(&dk, 0, sizeof(dk));
    dk_refresh();
    wm_window_t *win = wm_create_window("Disks", x, y, w, h, WS_DEFAULT,
                                        disks_draw_callback, disks_event_callback);
    if (!win) return NULL;
    win->icon = ICON_DISKS;
    win->min_w = 820;
    win->min_h = 520;
    dk.win = win;
    return win;
}

/* ---- Terminal: disk / diskpart / format ---------------------------------------- */
static int dk_parse_dev(const char *s)
{
    if (!s) return -1;
    if (*s >= '0' && *s <= '9') return (int)sh_num(s, -1);
    for (int dev = 0; dev < block_count(); dev++) {
        block_info_t b;
        if (block_get_info(dev, &b) == 0 && !strcmp(b.name, s)) return dev;
    }
    return -1;
}

static void dk_print_disk(int dev)
{
    static dm_disk_t d;
    if (dm_disk_info(dev, &d) != 0) { sh_err("disk: there is no disk %d\n", dev); return; }
    char a[16];
    dk_size(d.sectors * 512, a, sizeof(a));
    sh_printf("Disk %d (%s) %s  %s  table: %s%s\n", d.dev, d.name, d.model, a, dk_scheme(d.scheme), d.system ? "  [system disk]" : "");
    uint64_t pos = d.scheme == DM_MBR ? 2048 : 0;
    for (int i = 0; i <= d.nparts; i++) {
        uint64_t next = i < d.nparts ? d.parts[i].start : d.sectors;
        if (next > pos + 4096 && d.scheme != DM_GPT) { dk_size((next - pos) * 512, a, sizeof(a)); sh_printf("    -   unallocated     %10s\n", a); }
        if (i < d.nparts) {
            dm_part_t *p = &d.parts[i];
            dk_size(p->sectors * 512, a, sizeof(a));
            sh_printf("    %d   %-11s %-13s %10s  %s\n", p->num, p->fs, p->label[0] ? p->label : "-", a, p->mount[0] ? p->mount : "(not mounted)");
            pos = p->start + p->sectors;
        }
    }
}

void shell_disk_command(int argc, char **argv)
{
    char err[200] = "";
    /* "format <disk> [part] [fs] [label]" is "disk format ..." */
    if (!strcmp(argv[0], "format")) {
        static char *a[12];
        a[0] = (char *)"disk"; a[1] = (char *)"format";
        int k = 2;
        for (int i = 1; i < argc && k < 11; i++) a[k++] = argv[i];
        a[k] = NULL;
        shell_disk_command(k, a);
        return;
    }
    const char *act = argc > 1 ? argv[1] : "list";
    if (!strcmp(act, "help") || !strcmp(act, "/?")) {
        sh_out("disk list                                      every disk and its volumes\n"
               "disk info <disk>                               one disk in detail\n"
               "disk init <disk>                               new empty MBR table (ERASES the disk)\n"
               "disk create <disk> <MB|max> [fat32|fat16|none] [label]   new volume in free space\n"
               "disk format <disk> [vol] [fat32|fat16] [label] format a volume (ERASES it)\n"
               "disk label <disk> <vol> <name>                 rename a volume\n"
               "disk extend <disk> <vol> [MB|max]              grow a volume into free space\n"
               "disk shrink <disk> <vol> <MB>                  make a volume smaller\n"
               "disk delete <disk> <vol>                       delete a volume\n"
               "<disk> is the number or name from 'disk list' (0, sd0, usb0); <vol> the volume number\n"
               "(0 for a disk that is one volume). The system disk (JARVIS) can be extended and shrunk.\n");
        return;
    }
    if (!strcmp(act, "list")) {
        for (int dev = 0; dev < block_count(); dev++) { block_info_t b; if (block_get_info(dev, &b) == 0) dk_print_disk(dev); }
        return;
    }
    int dev = dk_parse_dev(argc > 2 ? argv[2] : NULL);
    if (dev < 0) { sh_err("disk %s: which disk? (see: disk list)\n", act); return; }
    if (!strcmp(act, "info")) { dk_print_disk(dev); return; }
    static dm_disk_t d;
    dm_disk_info(dev, &d);
    int rc = -1;
    int vol = argc > 3 && argv[3][0] >= '0' && argv[3][0] <= '9' && strlen(argv[3]) < 3 ? (int)sh_num(argv[3], 0) : (d.scheme == DM_WHOLE || d.scheme == DM_EMPTY ? 0 : 1);
    int argi = argc > 3 && argv[3][0] >= '0' && argv[3][0] <= '9' && strlen(argv[3]) < 3 ? 4 : 3;
    if (!strcmp(act, "init") || !strcmp(act, "clean")) {
        rc = dm_init_mbr(dev, err, sizeof(err));
        if (!rc) sh_printf("Disk %d now has an empty MBR partition table.\n", dev);
    } else if (!strcmp(act, "create") || !strcmp(act, "new")) {
        if (argc < 4) { sh_err("usage: disk create <disk> <MB|max> [fat32|fat16|none] [label]\n"); return; }
        uint64_t mb = !strcmp(argv[3], "max") ? 0 : (uint64_t)sh_num(argv[3], 0);
        int pn = 0;
        rc = dm_create_partition(dev, mb, argc > 4 ? argv[4] : "auto", argc > 5 ? argv[5] : "", &pn, err, sizeof(err));
        if (!rc) sh_printf("Created volume %d on disk %d.\n", pn, dev);
    } else if (!strcmp(act, "delete")) {
        rc = dm_delete_partition(dev, vol, err, sizeof(err));
        if (!rc) sh_printf("Deleted volume %d on disk %d.\n", vol, dev);
    } else if (!strcmp(act, "format")) {
        const char *fs = argi < argc ? argv[argi] : "auto";
        const char *label = argi + 1 < argc ? argv[argi + 1] : "";
        if (strcmp(fs, "fat32") && strcmp(fs, "fat16") && strcmp(fs, "auto") && strcmp(fs, "fat")) { label = fs; fs = "auto"; }
        rc = dm_format(dev, vol, fs, label, err, sizeof(err));
        if (!rc) sh_printf("Formatted volume %d on disk %d.\n", vol, dev);
    } else if (!strcmp(act, "label")) {
        if (argi >= argc) { sh_err("usage: disk label <disk> <vol> <name>\n"); return; }
        rc = dm_set_label(dev, vol, argv[argi], err, sizeof(err));
        if (!rc) sh_printf("Volume %d is now called %s.\n", vol, argv[argi]);
    } else if (!strcmp(act, "extend") || !strcmp(act, "shrink") || !strcmp(act, "resize")) {
        const char *sz = argi < argc ? argv[argi] : "max";
        if (!strcmp(act, "shrink") && !strcmp(sz, "max")) { sh_err("usage: disk shrink <disk> <vol> <MB>\n"); return; }
        uint64_t mb = !strcmp(sz, "max") ? 0 : (uint64_t)sh_num(sz, 0);
        if (d.system) vol = 0;
        rc = dm_resize(dev, vol, mb, err, sizeof(err));
        if (!rc) { sh_printf("Volume resized.\n"); dk_print_disk(dev); }
    } else {
        sh_err("disk: unknown action %s (see: disk help)\n", act);
        return;
    }
    if (rc) sh_err("disk %s: %s\n", act, err);
    desktop_files_changed();
    g_storage_generation++;
}
