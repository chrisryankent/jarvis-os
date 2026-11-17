/*
 * =============================================================================
 * Kryzen OS — Desktop surface (desktop/desktop.c)
 * Language: C17
 *
 * Wallpaper, desktop icons and the right-click context menu.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "../wm/wm.h"
#include "ui.h"
#include <string.h>

/* The wallpaper lives in desktop/wallpaper.c. */

void file_explorer_refresh(void);               /* apps/file_explorer.c */
int  file_explorer_trash_path(const char *path);

/* ---- Desktop icons -------------------------------------------------------
 * The fixed system icons come first, followed by one icon for every file and
 * folder in /kryzen/desktop. Creating something on the desktop creates it in
 * that folder; it shows up here and in Files > Desktop.
 */
#define DICON_W      92
#define DICON_H      80
#define DICON_GAP    4
#define DICON_MAX    40
#define DESKTOP_DIR  "/kryzen/desktop"

typedef struct {
    char label[64];
    char target[200];       /* app name, or a path */
    icon_id_t icon;
    int is_file_item;       /* lives in DESKTOP_DIR (can be renamed / deleted) */
} desktop_icon_t;

static desktop_icon_t g_dicons[DICON_MAX];
static int g_dicon_count = 0;
static int g_dicon_sys_count = 0;
static int g_dicon_selected = -1;
static int g_dicon_last_click = -1;
static uint32_t g_dicon_last_time = 0;
static int g_dfiles_dirty = 1;
static char g_dlisting[8192];

/* Inline rename of a desktop file icon */
static struct { int idx; char buf[64]; int len; int fresh; } g_drename = { -1, "", 0, 0 };

void desktop_files_changed(void)
{
    g_dfiles_dirty = 1;
    desktop_invalidate();
}

void desktop_add_icon(const char *label, const char *target, icon_id_t icon)
{
    if (g_dicon_count >= DICON_MAX) return;
    desktop_icon_t *d = &g_dicons[g_dicon_count++];
    memset(d, 0, sizeof(*d));
    strncpy(d->label, label, sizeof(d->label) - 1);
    strncpy(d->target, target, sizeof(d->target) - 1);
    d->icon = icon;
}

void desktop_init(void)
{
    memset(g_dicons, 0, sizeof(g_dicons));
    g_dicon_count = 0;
    g_dicon_selected = -1;
    g_dicon_last_click = -1;
    g_drename.idx = -1;

    /* Each icon opens something real: a folder, a Settings page or an app. */
    desktop_add_icon("This PC",        "::thispc",                ICON_PC);
    desktop_add_icon("Recycle Bin",    "/kryzen/trash",           ICON_TRASH);
    desktop_add_icon("Network",        "@network",                ICON_BROWSER);
    desktop_add_icon("Documents",      "/kryzen/data/Documents",  ICON_FOLDER);
    desktop_add_icon("Control Panel",  "Settings",                ICON_GEAR);
    desktop_add_icon("External Drives", "Disks",                  ICON_DISKS);
    desktop_add_icon("Task Manager",   "Tasks",                   ICON_CHART);
    desktop_add_icon("Media Library",  "Photos",                  ICON_PHOTOS);
    desktop_add_icon("AI Studio",      "JARVIS",                  ICON_NEURAL);
    desktop_add_icon("Code Workspace", "Notepad",                 ICON_CODE);
    desktop_add_icon("Games & Apps",   "Apps",                    ICON_STORE);
    g_dicon_sys_count = g_dicon_count;
    g_dfiles_dirty = 1;
}

/* Re-read DESKTOP_DIR and rebuild the file icons. */
static void desktop_reload_files(void)
{
    char selected[200] = "";
    if (g_dicon_selected >= g_dicon_sys_count && g_dicon_selected < g_dicon_count)
        strncpy(selected, g_dicons[g_dicon_selected].target, sizeof(selected) - 1);

    g_dfiles_dirty = 0;
    g_dicon_count = g_dicon_sys_count;
    if (g_dicon_selected >= g_dicon_sys_count) g_dicon_selected = -1;
    if (g_drename.idx >= 0) g_drename.idx = -1;

    vfs_mkdir(DESKTOP_DIR);
    if (vfs_list(DESKTOP_DIR, g_dlisting, sizeof(g_dlisting)) < 0)
        return;

    size_t dlen = strlen(DESKTOP_DIR);
    for (const char *p = g_dlisting; *p && g_dicon_count < DICON_MAX;) {
        const char *eol = p;
        while (*eol && *eol != '\n') eol++;

        int is_dir = strncmp(p, "[dir] ", 6) == 0;
        int is_file = strncmp(p, "[file] ", 7) == 0;
        const char *full = p + (is_dir ? 6 : 7);
        size_t flen = (size_t)(eol - full);

        if ((is_dir || is_file) && flen > dlen + 1 && flen < sizeof(g_dicons[0].target) &&
            strncmp(full, DESKTOP_DIR, dlen) == 0 && full[dlen] == '/') {
            const char *name = full + dlen + 1;
            size_t nlen = flen - dlen - 1;
            int direct = 1;
            for (size_t i = 0; i < nlen; i++)
                if (name[i] == '/') { direct = 0; break; }

            if (direct && nlen > 0 && nlen < sizeof(g_dicons[0].label)) {
                desktop_icon_t *d = &g_dicons[g_dicon_count];
                memset(d, 0, sizeof(*d));
                memcpy(d->label, name, nlen);
                memcpy(d->target, full, flen);
                d->icon = is_dir ? ICON_FOLDER : ICON_FILE;
                d->is_file_item = 1;
                if (selected[0] && strcmp(selected, d->target) == 0)
                    g_dicon_selected = g_dicon_count;
                g_dicon_count++;
            }
        }
        p = *eol ? eol + 1 : eol;
    }
}

/* ---- App panel (left) -------------------------------------------------------
 * A glass column with the assistant's state and the apps: pinned first, then
 * Settings and the Task Manager, then the app store. The chevron folds it
 * into a strip of icons (setting "panel_collapsed").
 */
#define PANEL_X        8
#define PANEL_W_OPEN   176
#define PANEL_W_SHUT   48
#define PANEL_ROW_H    34
#define PANEL_MAX_ROWS 14

static struct { int app[PANEL_MAX_ROWS]; int sep_before[PANEL_MAX_ROWS]; int count; int hover; } g_panel = { .hover = -1 };

static int panel_collapsed(void) { return setting_get_int("panel_collapsed", 0); }
static int panel_w(void) { return panel_collapsed() ? PANEL_W_SHUT : PANEL_W_OPEN; }

static void panel_rect(int *x, int *y, int *w, int *h)
{
    *x = PANEL_X;
    *y = TOPBAR_H + 6;
    *w = panel_w();
    *h = (int)fb_get_height() - TASKBAR_H - *y - 4;
}

/* Where the desktop icons start, to the right of the panel. */
int desktop_panel_right(void)
{
    return PANEL_X + panel_w();
}

static void panel_build(void)
{
    int n = 0;
    for (int i = 0; i < desktop_app_builtin_count() && n < PANEL_MAX_ROWS - 3; i++) {
        const char *nm = desktop_app_get(i)->name;
        if (!taskbar_is_pinned(i) || !strcmp(nm, "Settings") || !strcmp(nm, "Tasks") || !strcmp(nm, "Apps")) continue;
        g_panel.app[n] = i; g_panel.sep_before[n] = 0; n++;
    }
    static const char *const tail[3] = { "Settings", "Tasks", "Apps" };
    for (int k = 0; k < 3; k++) {
        int idx = desktop_app_find(tail[k]);
        if (idx < 0) continue;
        g_panel.app[n] = idx;
        g_panel.sep_before[n] = k == 0 || k == 2;
        n++;
    }
    g_panel.count = n;
}

static int panel_list_top(void)
{
    int x, y, w, h;
    panel_rect(&x, &y, &w, &h);
    return panel_collapsed() ? y + 46 : y + 138;
}

static int panel_row_y(int row)
{
    int y = panel_list_top();
    for (int i = 0; i <= row && i < g_panel.count; i++) {
        if (g_panel.sep_before[i]) y += 13;
        if (i < row) y += PANEL_ROW_H;
    }
    return y;
}

/* 0..count-1: an app row, -2: the fold button, -3: the assistant card, -1: none */
static int panel_hit(int px, int py)
{
    int x, y, w, h;
    panel_rect(&x, &y, &w, &h);
    if (!ui_hit(px, py, x, y, w, h)) return -1;
    if (py < y + 40) return (panel_collapsed() || px >= x + w - 44) ? -2 : -4;
    if (!panel_collapsed() && py >= y + 44 && py < y + 96) return -3;
    for (int i = 0; i < g_panel.count; i++) {
        int ry = panel_row_y(i);
        if (py >= ry && py < ry + PANEL_ROW_H && ry + PANEL_ROW_H <= y + h - 4) return i;
    }
    return -4;                                  /* on the panel, nothing to press */
}

int desktop_panel_hover(int px, int py)
{
    int h = panel_hit(px, py);
    if (h == g_panel.hover) return 0;
    g_panel.hover = h;
    return 1;
}

static void panel_draw(void)
{
    int x, y, w, h;
    int shut = panel_collapsed();
    panel_rect(&x, &y, &w, &h);
    panel_build();
    ui_glass(x, y, w, h, 12);

    /* header: menu mark and the fold chevron */
    if (g_panel.hover == -2) fb_blend_rounded_rect(shut ? x + 6 : x + w - 40, y + 6, shut ? w - 12 : 34, 30, 8, 0x00FFFFFF, 30);
    if (!shut) glyph_draw(GLYPH_MENU, x + 14, y + 10, 22, T(TEXT_DIM), 0);
    glyph_draw(shut ? GLYPH_MENU : GLYPH_CHEVRON_LEFT, shut ? x + (w - 22) / 2 : x + w - 34, y + 10, 22, T(TEXT_DIM), 0);

    if (!shut) {
        /* the assistant: its real state */
        const char *st = ui_shell_get_status();
        fb_blend_rounded_rect(x + 10, y + 46, w - 20, 48, 8, g_panel.hover == -3 ? 0x00FFFFFF : 0x00080C1E, g_panel.hover == -3 ? 34 : 110);
        ui_orb(x + 30, y + 70, 12, ui_shell_get_mic_level());
        fb_text(x + 50, y + 52, "JARVIS", T(TEXT), FONT_UI_BOLD);
        char line[40];
        snprintf(line, sizeof(line), "%s", ai_host_online() ? "Brain online" : kernel_safe_mode() ? "Off (Safe Mode)" : "Built-in commands");
        fb_text(x + 50, y + 68, line, T(TEXT_DIM), FONT_SMALL);
        int sw = fb_text_width_font(st, FONT_SMALL);
        if (sw < w - 110) fb_text(x + w - 18 - sw, y + 54, st, UI_NEON_CYAN, FONT_SMALL);
        fb_blend_rect(x + 14, y + 104, w - 28, 1, 0x00FFFFFF, 30);
        fb_text(x + 16, y + 112, "Apps", T(TEXT), FONT_MEDIUM);
    }

    for (int i = 0; i < g_panel.count; i++) {
        int ry = panel_row_y(i);
        if (ry + PANEL_ROW_H > y + h - 4) break;
        desktop_app_t *app = desktop_app_get(g_panel.app[i]);
        int running = app->window != NULL;
        int focused = running && app->window == g_wm_ctx.focused;
        if (g_panel.sep_before[i]) fb_blend_rect(x + 14, ry - 7, w - 28, 1, 0x00FFFFFF, 30);
        if (focused || g_panel.hover == i)
            fb_blend_rounded_rect(x + 6, ry + 1, w - 12, PANEL_ROW_H - 2, 8, 0x00FFFFFF, focused ? 40 : 26);
        if (focused) fb_fill_rounded_rect(x + 2, ry + 9, 3, PANEL_ROW_H - 18, 1, UI_NEON_CYAN);
        icon_draw(app->icon, shut ? x + (w - 24) / 2 : x + 14, ry + 5, 24);
        if (!shut) {
            ui_text_fit(x + 50, ry + 9, w - 70, app->name, T(TEXT));
            if (running) fb_fill_circle(x + w - 14, ry + PANEL_ROW_H / 2, 2, focused ? UI_NEON_CYAN : T(TEXT_DIM));
        } else if (running) {
            fb_fill_circle(x + w - 7, ry + PANEL_ROW_H / 2, 2, focused ? UI_NEON_CYAN : T(TEXT_DIM));
        }
    }
}

/* Returns 1 if the click was on the panel. */
static int panel_click(int px, int py)
{
    int hit = panel_hit(px, py);
    if (hit == -1) return 0;
    if (hit == -2) { setting_set_int("panel_collapsed", !panel_collapsed()); g_panel.hover = -1; }
    else if (hit == -3) desktop_app_launch(desktop_app_find("JARVIS"));
    else if (hit >= 0 && hit < g_panel.count) desktop_app_activate(g_panel.app[hit]);
    desktop_invalidate();
    return 1;
}

/* ---- Widgets (right) --------------------------------------------------------
 * Status strip (network, notifications, sound, processor load), this
 * month's calendar, and the assistant's activity: all read from the running
 * system each frame.
 */
#define WIDGET_W 196
#define CPU_SAMPLES 28
static struct {
    uint8_t cpu[CPU_SAMPLES]; int cpu_n, cpu_now;
    uint8_t mic[CPU_SAMPLES]; int mic_n;
    uint64_t last_tick, last_idle;
} g_wd;

static void widgets_sample(void)
{
    uint64_t now = g_kstate.uptime_ticks, idle;
    if (now - g_wd.last_tick < 45 && g_wd.cpu_n) return;
    scheduler_stats(&idle, NULL);
    uint64_t dt = now - g_wd.last_tick, di = idle - g_wd.last_idle;
    int busy = (g_wd.last_tick && dt) ? (int)(100 - (di > dt ? dt : di) * 100 / dt) : 0;
    g_wd.last_tick = now; g_wd.last_idle = idle;
    g_wd.cpu_now = busy;
    if (g_wd.cpu_n == CPU_SAMPLES) { memmove(g_wd.cpu, g_wd.cpu + 1, CPU_SAMPLES - 1); g_wd.cpu_n--; }
    g_wd.cpu[g_wd.cpu_n++] = (uint8_t)busy;
    if (g_wd.mic_n == CPU_SAMPLES) { memmove(g_wd.mic, g_wd.mic + 1, CPU_SAMPLES - 1); g_wd.mic_n--; }
    g_wd.mic[g_wd.mic_n++] = (uint8_t)ui_shell_get_mic_level();
}

int desktop_cpu_percent(void) { return g_wd.cpu_now; }

static void widget_rects(int *x, int *ys, int *yc, int *ya)
{
    *x = (int)fb_get_width() - WIDGET_W - 10;
    *ys = TOPBAR_H + 8;                 /* status strip, 40 high */
    *yc = *ys + 48;                     /* calendar, 176 high */
    *ya = *yc + 184;                    /* assistant activity, 70 high */
}

static int wd_days_in(int year, int month)
{
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
    return d[(month + 11) % 12];
}

static void widgets_draw(void)
{
    int x, ys, yc, ya;
    int W = (int)fb_get_width(), H = (int)fb_get_height();
    if (W < 900) return;
    widgets_sample();
    widget_rects(&x, &ys, &yc, &ya);

    /* status strip */
    net_info_t ni;
    net_get_info(&ni);
    ui_glass(x, ys, WIDGET_W, 40, 10);
    glyph_draw(GLYPH_DISPLAY, x + 10, ys + 11, 18, T(TEXT), 0);
    glyph_draw(GLYPH_NET, x + 38, ys + 11, 18, ni.configured ? T(TEXT) : T(TEXT_DIM), 0);
    glyph_draw(GLYPH_BELL, x + 66, ys + 11, 18, T(TEXT), 0);
    if (notifications_get_unread_count() > 0) fb_fill_circle(x + 81, ys + 12, 3, UI_NEON_CYAN);
    glyph_draw(GLYPH_VOLUME, x + 94, ys + 11, 18, g_kstate.audio_ready && !setting_get_int("mute", 0) ? T(TEXT) : T(TEXT_DIM), 0);
    {   /* processor load, last half minute */
        int gx = x + 122, gw = WIDGET_W - 132, gy = ys + 8, gh = 24;
        fb_blend_rounded_rect(gx, gy, gw, gh, 5, 0x00080C1E, 120);
        int px = -1, py = 0;
        for (int i = 0; i < g_wd.cpu_n; i++) {
            int sx = gx + 3 + i * (gw - 6) / (CPU_SAMPLES - 1);
            int sy = gy + gh - 4 - g_wd.cpu[i] * (gh - 8) / 100;
            if (px >= 0) fb_draw_line_aa(px, py, sx, sy, 1, UI_NEON_VIOLET, 255);
            px = sx; py = sy;
        }
    }

    /* calendar */
    if (yc + 176 < H - TASKBAR_H) {
        static const char *const dn[7] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };
        sys_time_t t;
        sys_get_time(&t);
        ui_glass(x, yc, WIDGET_W, 176, 10);
        int cw = (WIDGET_W - 16) / 7;
        for (int d = 0; d < 7; d++)
            fb_text(x + 8 + d * cw + (cw - fb_text_width_font(dn[d], FONT_SMALL)) / 2, yc + 8, dn[d], T(TEXT_DIM), FONT_SMALL);
        int first = ((t.weekday - (t.day - 1)) % 7 + 7) % 7;       /* weekday of the 1st */
        int dim = wd_days_in(t.year, t.month);
        int prev = wd_days_in(t.month == 1 ? t.year - 1 : t.year, t.month == 1 ? 12 : t.month - 1);
        for (int cell = 0; cell < 42; cell++) {
            int day = cell - first + 1, other = 0;
            if (day < 1) { day += prev; other = 1; }
            else if (day > dim) { day -= dim; other = 1; }
            char num[4];
            snprintf(num, sizeof(num), "%d", day);
            int cx = x + 8 + (cell % 7) * cw, cy = yc + 30 + (cell / 7) * 23;
            int today = !other && day == t.day;
            if (today) fb_blend_rounded_rect(cx + 2, cy - 2, cw - 4, 20, 6, UI_NEON_CYAN, 70);
            fb_text(cx + (cw - fb_text_width_font(num, FONT_SMALL)) / 2, cy + 1, num,
                    other ? T(TEXT_DIM) : today ? 0x00FFFFFF : T(TEXT), FONT_SMALL);
        }
    }

    /* the assistant's activity: microphone level over the last half minute */
    if (ya + 70 < H - TASKBAR_H) {
        char title[64];
        ui_glass(x, ya, WIDGET_W, 70, 10);
        snprintf(title, sizeof(title), "AI Activity  \x80  %s", ui_shell_get_status());
        ui_text_fit(x + 12, ya + 8, WIDGET_W - 24, title, T(TEXT));
        int bw = (WIDGET_W - 24) / CPU_SAMPLES;
        for (int i = 0; i < g_wd.mic_n; i++) {
            int bh = 2 + g_wd.mic[i] * 26 / 100;
            fb_fill_rect(x + 12 + i * bw, ya + 60 - bh, bw > 2 ? bw - 2 : 1, bh,
                         i > CPU_SAMPLES * 2 / 3 ? UI_NEON_VIOLET : 0x00E8C060);
        }
    }
}

/* 1 status strip, 2 calendar, 3 assistant activity, 0 none */
static int widgets_hit(int px, int py)
{
    int x, ys, yc, ya;
    if ((int)fb_get_width() < 900) return 0;
    widget_rects(&x, &ys, &yc, &ya);
    if (ui_hit(px, py, x, ys, WIDGET_W, 40)) return 1;
    if (ui_hit(px, py, x, yc, WIDGET_W, 176)) return 2;
    if (ui_hit(px, py, x, ya, WIDGET_W, 70)) return 3;
    return 0;
}

/* "Listening" pill at the top while speech is coming in through the bridge. */
static void listening_draw(void)
{
    const char *st = ui_shell_get_status();
    if (strcmp(st, "LISTENING") && strcmp(st, "THINKING") && strcmp(st, "SPEAKING")) return;
    int W = (int)fb_get_width();
    int w = 300, h = 58, x = (W - w) / 2, y = TOPBAR_H + 10;
    int level = ui_shell_get_mic_level();
    ui_glass_tinted(x, y, w, h, h / 2, 0x00101428, 170);
    ui_neon_frame(x, y, w, h, h / 2, 2, 6);
    static const uint8_t shape[20] = { 15, 30, 55, 90, 60, 35, 20, 45, 80, 100, 70, 40, 25, 50, 85, 60, 30, 45, 20, 12 };
    for (int side = -1; side <= 1; side += 2)
        for (int i = 0; i < 20; i++) {
            int amp = 1 + (13 * shape[i] / 100) * (15 + level) / 115;
            int bx = W / 2 + side * (4 + i * 6);
            fb_draw_line_aa(bx, y + 22 - amp, bx, y + 22 + amp, 2, side < 0 ? UI_NEON_CYAN : UI_NEON_VIOLET, 255);
        }
    const char *label = !strcmp(st, "LISTENING") ? "Listening..." : !strcmp(st, "THINKING") ? "Thinking..." : "Speaking...";
    fb_text(x + (w - fb_text_width_font(label, FONT_UI)) / 2, y + 38, label, 0x00DDE6FF, FONT_UI);
}

static void dicon_rect(int i, int *x, int *y)
{
    int top = TOPBAR_H + 10;
    int rows = ((int)fb_get_height() - TASKBAR_H - top - 4) / (DICON_H + DICON_GAP);
    if (rows < 1) rows = 1;
    *x = desktop_panel_right() + 10 + (i / rows) * (DICON_W + DICON_GAP);
    *y = top + (i % rows) * (DICON_H + DICON_GAP);
}

void desktop_draw(void)
{
    if (g_dfiles_dirty)
        desktop_reload_files();

    wallpaper_draw();

    if (kernel_safe_mode()) {
        int W = (int)fb_get_width(), H = (int)fb_get_height();
        fb_text(W / 2 - fb_text_width_font("Safe Mode", FONT_UI_BOLD) / 2, TOPBAR_H + 6, "Safe Mode", 0x00FFFFFF, FONT_UI_BOLD);
        fb_text(W - 90, H - TASKBAR_H - 20, "Safe Mode", 0x00FFFFFF, FONT_UI_BOLD);
    }

    if (setting_get_int("desktop_icons", 1)) {
        for (int i = 0; i < g_dicon_count; i++) {
            int x, y;
            dicon_rect(i, &x, &y);

            if (i == g_dicon_selected) {
                fb_blend_rounded_rect(x, y, DICON_W, DICON_H, 10, T(ACCENT), 44);
                fb_neon_rounded_rect(x, y, DICON_W, DICON_H, 10, 1, 0, T(ACCENT), T(ACCENT), 200);
            }
            icon_draw(g_dicons[i].icon, x + (DICON_W - 40) / 2, y + 6, 40);

            if (i == g_drename.idx) {
                /* edit box, wide enough for the name being typed */
                int bw = fb_text_width_n(g_drename.buf, g_drename.len) + 14;
                if (bw < DICON_W + 20) bw = DICON_W + 20;
                int bx = x + (DICON_W - bw) / 2;
                if (bx < 2) bx = 2;
                fb_fill_rect(bx, y + 53, bw, 22, T(INPUT));
                fb_draw_rect(bx, y + 53, bw, 22, T(ACCENT));
                if (g_drename.fresh)
                    fb_fill_rect(bx + 5, y + 56, fb_text_width_n(g_drename.buf, g_drename.len) + 2, 16, T(ACCENT));
                fb_draw_text(bx + 6, y + 56, g_drename.buf,
                             g_drename.fresh ? T(ACCENT_TEXT) : T(TEXT));
                if (!g_drename.fresh)
                    fb_fill_rect(bx + 7 + fb_text_width_n(g_drename.buf, g_drename.len), y + 56, 1, 16, T(TEXT));
                continue;
            }

            /* Label with drop shadow so it stays crisp on any background */
            char label[48];
            snprintf(label, sizeof(label), "%s", g_dicons[i].label);
            int n = (int)strlen(label);
            if (fb_text_width(label) > DICON_W + 2) {
                while (n > 1 && fb_text_width_n(label, n) + fb_text_width(SYM_ELLIPSIS) > DICON_W + 2) n--;
                memcpy(label + n, SYM_ELLIPSIS, 2);
            }
            int tx = x + (DICON_W - fb_text_width(label)) / 2;
            fb_draw_text(tx + 1, y + 55, label, 0x00000000);
            fb_draw_text(tx, y + 54, label, 0x00F2F5FF);
        }
    }

    panel_draw();
    widgets_draw();
    listening_draw();
}

/* ---- Inline rename ------------------------------------------------------- */
int desktop_rename_active(void)
{
    return g_drename.idx >= 0;
}

static void desktop_rename_start(int idx)
{
    if (idx < g_dicon_sys_count || idx >= g_dicon_count) return;
    g_drename.idx = idx;
    strncpy(g_drename.buf, g_dicons[idx].label, sizeof(g_drename.buf) - 1);
    g_drename.buf[sizeof(g_drename.buf) - 1] = 0;
    g_drename.len = (int)strlen(g_drename.buf);
    g_drename.fresh = 1;
    g_dicon_selected = idx;
    desktop_invalidate();
}

void desktop_rename_finish(int commit)
{
    int idx = g_drename.idx;
    if (idx < 0) return;
    g_drename.idx = -1;
    desktop_invalidate();

    if (!commit || idx >= g_dicon_count || g_drename.len == 0 ||
        strcmp(g_drename.buf, g_dicons[idx].label) == 0)
        return;

    char target[200];
    snprintf(target, sizeof(target), "%s/%s", DESKTOP_DIR, g_drename.buf);
    if (vfs_exists(target) || vfs_rename(g_dicons[idx].target, target) != 0) {
        notification_show("Rename", "That name is already in use, or is not valid.",
                          NOTIF_WARNING, 4000);
        return;
    }
    strncpy(g_dicons[idx].target, target, sizeof(g_dicons[idx].target) - 1);
    desktop_files_changed();
    file_explorer_refresh();
}

/* Keyboard while renaming. key is a WM_KEY_* code (0 for a text character). */
void desktop_rename_input(uint32_t key, char ch)
{
    if (g_drename.idx < 0) return;

    if (ch >= 32 && ch < 127 && ch != '/') {
        if (g_drename.fresh) { g_drename.len = 0; g_drename.fresh = 0; }
        if (g_drename.len < (int)sizeof(g_drename.buf) - 1) {
            g_drename.buf[g_drename.len++] = ch;
            g_drename.buf[g_drename.len] = 0;
        }
    } else if (key == WM_KEY_ENTER) {
        desktop_rename_finish(1);
    } else if (key == WM_KEY_ESC) {
        desktop_rename_finish(0);
    } else if (key == WM_KEY_BACKSPACE) {
        if (g_drename.fresh) g_drename.len = 0;
        else if (g_drename.len > 0) g_drename.len--;
        g_drename.buf[g_drename.len] = 0;
        g_drename.fresh = 0;
    } else if (key == WM_KEY_RIGHT || key == WM_KEY_END) {
        g_drename.fresh = 0;
    }
    desktop_invalidate();
}

/*
 * Returns the icon index on a completed double-click, -2 if an icon was
 * clicked once (selected), -1 if the click missed every icon.
 */
int desktop_handle_click(int x, int y)
{
    if (panel_click(x, y)) return -2;
    switch (widgets_hit(x, y)) {
    case 1: shell_flyout_toggle(FLY_QUICK); return -2;
    case 2: shell_flyout_toggle(FLY_CALENDAR); return -2;
    case 3: desktop_app_launch(desktop_app_find("JARVIS")); return -2;
    default: break;
    }

    if (!setting_get_int("desktop_icons", 1)) return -1;
    for (int i = 0; i < g_dicon_count; i++) {
        int ix, iy;
        dicon_rect(i, &ix, &iy);
        if (!ui_hit(x, y, ix, iy, DICON_W, DICON_H)) continue;

        uint32_t now = (uint32_t)g_kstate.uptime_ticks;
        int is_double = (g_dicon_last_click == i) && (now - g_dicon_last_time <= 50);

        g_dicon_selected = i;
        g_dicon_last_click = is_double ? -1 : i;
        g_dicon_last_time = now;
        return is_double ? i : -2;
    }
    g_dicon_selected = -1;
    g_dicon_last_click = -1;
    return -1;
}

const char *desktop_get_icon_target(int i)
{
    if (i < 0 || i >= g_dicon_count) return "";
    return g_dicons[i].target;
}

/* Deletes the selected desktop file icon (to the Recycle Bin). */
void desktop_delete_selected(void)
{
    if (g_dicon_selected < g_dicon_sys_count || g_dicon_selected >= g_dicon_count)
        return;
    if (file_explorer_trash_path(g_dicons[g_dicon_selected].target) != 0)
        notification_show("Delete", "The item could not be moved to the Recycle Bin.",
                          NOTIF_WARNING, 4000);
    g_dicon_selected = -1;
}

/* ==========================================================================
 * POPUP MENU
 *
 * One menu can be open at a time, anywhere on screen. Used for the desktop's
 * right-click menu and by apps (Files). The owner supplies the items and a
 * callback that receives the chosen item's id.
 * ========================================================================== */
#define MENU_ITEM_H   30
#define MENU_SEP_H    9
#define MENU_MAX      12

static struct {
    int open;
    int x, y, w, h;
    ui_menu_item_t items[MENU_MAX];
    int count;
    int hover;
    ui_menu_cb callback;
    void *ctx;
} g_menu;

static int menu_item_y(int index)
{
    int y = g_menu.y + 4;
    for (int i = 0; i <= index && i < g_menu.count; i++) {
        if (g_menu.items[i].separator_before) y += MENU_SEP_H;
        if (i < index) y += MENU_ITEM_H;
    }
    return y;
}

void ui_menu_open(int x, int y, const ui_menu_item_t *items, int count,
                  ui_menu_cb callback, void *ctx)
{
    if (count > MENU_MAX) count = MENU_MAX;
    if (count <= 0) return;

    int widest = 0;
    g_menu.count = count;
    for (int i = 0; i < count; i++) {
        g_menu.items[i] = items[i];
        int tw = fb_text_width(items[i].label);
        if (tw > widest) widest = tw;
    }
    g_menu.w = widest + 44;
    if (g_menu.w < 180) g_menu.w = 180;

    g_menu.x = 0; g_menu.y = 0;
    g_menu.h = menu_item_y(count - 1) + MENU_ITEM_H + 4;

    int W = (int)fb_get_width(), H = (int)fb_get_height() - TASKBAR_H;
    if (x + g_menu.w > W) x = W - g_menu.w;
    if (y + g_menu.h > H) y = H - g_menu.h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    g_menu.x = x; g_menu.y = y;

    g_menu.callback = callback;
    g_menu.ctx = ctx;
    g_menu.hover = -1;
    g_menu.open = 1;
    desktop_invalidate();
}

int desktop_context_is_open(void) { return g_menu.open; }

void desktop_context_close(void)
{
    if (g_menu.open) {
        g_menu.open = 0;
        desktop_invalidate();
    }
}

static int menu_item_at(int x, int y)
{
    if (!g_menu.open || x < g_menu.x || x >= g_menu.x + g_menu.w) return -1;
    for (int i = 0; i < g_menu.count; i++) {
        int iy = menu_item_y(i);
        if (y >= iy && y < iy + MENU_ITEM_H)
            return g_menu.items[i].disabled ? -1 : i;
    }
    return -1;
}

/* Returns 1 if the hover highlight changed. */
int desktop_context_hover(int x, int y)
{
    if (!g_menu.open) return 0;
    int item = menu_item_at(x, y);
    if (item == g_menu.hover) return 0;
    g_menu.hover = item;
    return 1;
}

void desktop_context_draw(void)
{
    if (!g_menu.open) return;

    int x = g_menu.x, y = g_menu.y, w = g_menu.w, h = g_menu.h;
    fb_blend_rounded_rect(x + 2, y + 4, w, h, 8, 0x00000000, 70);
    fb_fill_rounded_rect(x, y, w, h, 8, T(BORDER));
    fb_fill_rounded_rect(x + 1, y + 1, w - 2, h - 2, 7, T(MENU));

    for (int i = 0; i < g_menu.count; i++) {
        int iy = menu_item_y(i);
        if (g_menu.items[i].separator_before)
            fb_fill_rect(x + 10, iy - MENU_SEP_H / 2 - 1, w - 20, 1, T(BORDER));
        if (i == g_menu.hover)
            fb_fill_rounded_rect(x + 4, iy + 1, w - 8, MENU_ITEM_H - 2, 4, T(HOVER));
        fb_draw_text(x + 16, iy + 7, g_menu.items[i].label,
                     g_menu.items[i].disabled ? T(TEXT_DIM) : T(TEXT));
    }
}

/* Any click while a menu is open closes it; a click on an item runs it.
 * Returns 1 if a menu was open (the click is consumed). */
int desktop_context_click(int x, int y)
{
    if (!g_menu.open) return 0;

    int item = menu_item_at(x, y);
    g_menu.open = 0;
    desktop_invalidate();

    if (item >= 0 && g_menu.callback)
        g_menu.callback(g_menu.items[item].id, g_menu.ctx);
    return 1;
}

/* ---- Desktop right-click menus ------------------------------------------- */
enum {
    CTX_REFRESH = 1, CTX_OPEN_TERMINAL, CTX_OPEN_FILES, CTX_ASK_JARVIS,
    CTX_TOGGLE_THEME, CTX_SETTINGS, CTX_SHOW_DESKTOP, CTX_NEW_FOLDER,
    CTX_NEW_TEXT, CTX_ICON_OPEN, CTX_EMPTY_TRASH, CTX_ICON_RENAME, CTX_ICON_DELETE,
};

void desktop_open_icon(int icon);                       /* desktop_init.c */
int  file_explorer_empty_trash(void);                   /* apps/file_explorer.c */
int  file_explorer_create_in(const char *dir, int folder, char *out, size_t n);
void file_explorer_refresh(void);

/* Create a new item on the desktop itself and start naming it. */
static void desktop_new_item(int folder)
{
    char path[200];
    if (file_explorer_create_in(DESKTOP_DIR, folder, path, sizeof(path)) != 0) {
        notification_show("New", "The item could not be created.", NOTIF_WARNING, 4000);
        return;
    }
    desktop_reload_files();
    for (int i = g_dicon_sys_count; i < g_dicon_count; i++)
        if (strcmp(g_dicons[i].target, path) == 0)
            desktop_rename_start(i);
}

static int g_ctx_icon = -1;

static void desktop_menu_action(int id, void *ctx)
{
    (void)ctx;
    switch (id) {
    case CTX_REFRESH:       desktop_wallpaper_invalidate(); break;
    case CTX_SHOW_DESKTOP:  wm_minimize_all(); break;
    case CTX_OPEN_TERMINAL: desktop_app_launch(desktop_app_find("Terminal")); break;
    case CTX_OPEN_FILES:    desktop_app_launch(desktop_app_find("Files")); break;
    case CTX_ASK_JARVIS:    desktop_app_launch(desktop_app_find("JARVIS")); break;
    case CTX_SETTINGS:      desktop_app_launch(desktop_app_find("Settings")); break;
    case CTX_TOGGLE_THEME:
        theme_set_mode(theme_get_mode() == THEME_DARK ? THEME_LIGHT : THEME_DARK);
        break;
    case CTX_NEW_FOLDER:    desktop_new_item(1); break;
    case CTX_NEW_TEXT:      desktop_new_item(0); break;
    case CTX_ICON_RENAME:   desktop_rename_start(g_ctx_icon); break;
    case CTX_ICON_DELETE:   g_dicon_selected = g_ctx_icon; desktop_delete_selected(); break;
    case CTX_ICON_OPEN:     desktop_open_icon(g_ctx_icon); break;
    case CTX_EMPTY_TRASH:   file_explorer_empty_trash(); break;
    }
}

/* Index of the desktop icon under (x,y), or -1. */
int desktop_icon_at(int x, int y)
{
    if (!setting_get_int("desktop_icons", 1)) return -1;
    for (int i = 0; i < g_dicon_count; i++) {
        int ix, iy;
        dicon_rect(i, &ix, &iy);
        if (ui_hit(x, y, ix, iy, DICON_W, DICON_H)) return i;
    }
    return -1;
}

void desktop_context_open(int x, int y)
{
    int icon = desktop_icon_at(x, y);

    if (icon >= 0) {
        ui_menu_item_t items[4] = { { "Open", CTX_ICON_OPEN, 0, 0 } };
        int n = 1;
        g_ctx_icon = icon;
        g_dicon_selected = icon;
        if (g_dicons[icon].is_file_item) {
            items[n++] = (ui_menu_item_t){ "Rename", CTX_ICON_RENAME, 0, 1 };
            items[n++] = (ui_menu_item_t){ "Delete", CTX_ICON_DELETE, 0, 0 };
        }
        if (strcmp(g_dicons[icon].target, "/kryzen/trash") == 0)
            items[n++] = (ui_menu_item_t){ "Empty Recycle Bin", CTX_EMPTY_TRASH, 0, 1 };
        ui_menu_open(x, y, items, n, desktop_menu_action, NULL);
        return;
    }

    static const ui_menu_item_t items[] = {
        { "Refresh",              CTX_REFRESH,       0, 0 },
        { "Show desktop",         CTX_SHOW_DESKTOP,  0, 0 },
        { "New folder",           CTX_NEW_FOLDER,    0, 1 },
        { "New text document",    CTX_NEW_TEXT,      0, 0 },
        { "Open Terminal",        CTX_OPEN_TERMINAL, 0, 1 },
        { "Open Files",           CTX_OPEN_FILES,    0, 0 },
        { "Ask JARVIS",           CTX_ASK_JARVIS,    0, 0 },
        { "Switch light / dark",  CTX_TOGGLE_THEME,  0, 1 },
        { "Personalize",          CTX_SETTINGS,      0, 0 },
    };
    g_dicon_selected = -1;
    ui_menu_open(x, y, items, (int)(sizeof(items) / sizeof(items[0])),
                 desktop_menu_action, NULL);
}
