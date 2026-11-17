/*
 * =============================================================================
 * Kryzen OS — UI shell state + full-screen screens (desktop/ui_shell.c)
 *
 * Holds the assistant state the rest of the kernel reports into (status,
 * transcript lines, mic level, security label) and draws the screens that
 * replace the desktop: boot/loading, first-run setup and the lock screen.
 *
 * Once the desktop is up this file draws nothing itself: it just records the
 * new state and asks the desktop for a redraw. (It used to paint its panels
 * straight onto the screen from wherever it was called, on top of windows.)
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "ui.h"

int kernel_voice_bridge_connected(void); /* kernel/kernel.c */
#include <string.h>
#include <stdio.h>

#define UI_MAX_LINES 64
#define UI_LINE_LEN  128

typedef enum {
    UI_MODE_LOADING = 0,
    UI_MODE_SETUP,
    UI_MODE_LOCK,
    UI_MODE_INSTALLER,
    UI_MODE_DESKTOP,
} ui_shell_mode_t;

typedef struct {
    ui_shell_mode_t mode;
    int initialized;
    int cursor_x;
    int cursor_y;
    int mic_level;
    int security_alert;
    char status[32];
    char security_label[32];
    char setup_title[64];
    char setup_detail[128];
    char lines[UI_MAX_LINES][UI_LINE_LEN];
    int line_count;                 /* total ever added */
    char pin_entry[16];
    int pin_len;
    int pin_wrong;
} ui_shell_state_t;

static ui_shell_state_t g_ui = {
    .mode = UI_MODE_LOADING,
    .cursor_x = 512,
    .cursor_y = 384,
    .status = "BOOTING",
    .security_label = "SAFE",
};

static int ui_clamp(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void ui_copy(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0)
        return;
    if (!src)
        src = "";
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

static void ui_shell_ensure_init(void)
{
    if (g_ui.initialized)
        return;

    uint32_t W = fb_get_width();
    uint32_t H = fb_get_height();
    if (W > 0 && H > 0) {
        g_ui.cursor_x = (int)W / 2;
        g_ui.cursor_y = (int)H / 2;
    }
    g_ui.initialized = 1;
}

/* ---- Full-screen screens ------------------------------------------------- */
static void draw_backdrop(void)
{
    fb_reset_clip();
    wallpaper_paint_backdrop(0);
}

/* ---- Shared look: sign-in, first-run setup, installer -----------------------
 * The nebula backdrop, a slim glass side bar, a status list top-left and
 * frosted panels with the cyan-to-violet outline. Everything shown is live
 * state; nothing here is decoration with made-up values.
 */
#define FS_SIDE_W   46
#define FS_WHITE    0x00F2F5FF
#define FS_DIM      UI_TEXT_SOFT
#define FS_FAINT    0x007C87A8

static struct { int status_hidden; int power_open; } g_fs;

static void fs_text_center(int x, int y, int w, const char *text, uint32_t col, int font)
{
    fb_text(x + (w - fb_text_width_font(text, font)) / 2, y, text, col, font);
}

/* Draws text cut to max_w pixels (with an ellipsis). */
static void fs_text_fit(int x, int y, int max_w, const char *text, uint32_t col, int font)
{
    if (fb_text_width_font(text, font) <= max_w) { fb_text(x, y, text, col, font); return; }
    int n = (int)strlen(text), ew = fb_text_width_font(SYM_ELLIPSIS, font);
    while (n > 0 && fb_text_width_n_font(text, n, font) + ew > max_w) n--;
    int end = fb_text_n(x, y, text, n, col, font);
    fb_text(end, y, SYM_ELLIPSIS, col, font);
}

/* Word-wrapped paragraph; returns the y after the last line. */
static int fs_paragraph(int x, int y, int max_w, const char *text, uint32_t col, int font, int line_h)
{
    const char *p = text;
    while (*p) {
        int n = 0, last_space = -1;
        while (p[n] && p[n] != '\n') {
            if (p[n] == ' ') last_space = n;
            if (fb_text_width_n_font(p, n + 1, font) > max_w) break;
            n++;
        }
        if (p[n] && p[n] != '\n' && last_space > 0) n = last_space;
        if (n == 0 && p[0] && p[0] != '\n') n = 1;
        fb_text_n(x, y, p, n, col, font);
        y += line_h;
        p += n;
        while (*p == ' ') p++;
        if (*p == '\n') p++;
    }
    return y;
}

static void fs_status_row(int x, int y, int w, const char *text, int ok)
{
    fb_blend_rounded_rect(x, y, w, 24, 6, 0x00080C1E, 130);
    fs_text_fit(x + 10, y + 4, w - 44, text, FS_WHITE, FONT_UI);
    glyph_draw(ok ? GLYPH_CHECK : GLYPH_WARNING, x + w - 26, y + 4, 16, ok ? 0x005CE6C8 : T(WARN), 0);
}

typedef struct { char text[72]; int ok; } fs_status_t;

/* Live system state for the status list. Returns the number of rows. */
static int fs_collect_status(fs_status_t *rows, int max)
{
    int n = 0;
    net_info_t ni;
    diskfs_stats_t ds;
    net_get_info(&ni);
    if (n < max) { snprintf(rows[n].text, sizeof(rows[n].text), "EN_US / US keyboard layout"); rows[n++].ok = 1; }
    if (n < max) {
        snprintf(rows[n].text, sizeof(rows[n].text), "Started by %s firmware%s",
                 kernel_booted_uefi() ? "UEFI" : "legacy BIOS", kernel_safe_mode() ? " (Safe Mode)" : "");
        rows[n++].ok = 1;
    }
    if (n < max) {
        int ok = diskfs_get_stats(&ds) == 0;
        snprintf(rows[n].text, sizeof(rows[n].text), "%s",
                 ok ? "Kryzen data volume mounted" :
                 kernel_installer_boot_mode() ? "Installer session: no data volume yet" :
                                                "No data volume: changes stay in memory");
        rows[n++].ok = ok;
    }
    if (n < max) {
        int ok = vfs_is_dir("/kryzen/linux") && reg_open_key("HKLM", 0) >= 0;
        snprintf(rows[n].text, sizeof(rows[n].text), "%s",
                 ok ? "Linux layer and registry ready" : "Linux layer or registry not ready");
        rows[n++].ok = ok;
    }
    if (n < max) {
        snprintf(rows[n].text, sizeof(rows[n].text), "%s",
                 g_kstate.security_enabled ? (g_ui.security_alert ? "Security monitor: alert raised" : "Security monitor armed")
                                           : "Security monitor is off");
        rows[n++].ok = g_kstate.security_enabled && !g_ui.security_alert;
    }
    if (n < max) {
        snprintf(rows[n].text, sizeof(rows[n].text), "%s",
                 ni.configured ? "Network connected" : ni.adapter ? "Network adapter found, not connected" : "No network adapter");
        rows[n++].ok = ni.configured;
    }
    return n;
}

/* first: index of the first row shown (the wizard skips the keyboard row). */
static void fs_status_list(const char *title, int first, int max_rows)
{
    if (g_fs.status_hidden) return;
    static fs_status_t all[8];
    int total = fs_collect_status(all, 8);
    fs_status_t *rows = all + (first < total ? first : total);
    int n = total - first;
    if (n > max_rows) n = max_rows;
    if (n < 0) n = 0;
    int x = FS_SIDE_W + 26, y = 18;
    if (title) { fb_text(x, y, title, FS_WHITE, FONT_SUBHEAD); y += 30; }
    int w = 0;
    for (int i = 0; i < n; i++) {
        int tw = fb_text_width_font(rows[i].text, FONT_UI) + 54;
        if (tw > w) w = tw;
    }
    if (w > 330) w = 330;
    for (int i = 0; i < n; i++, y += 27) fs_status_row(x, y, w, rows[i].text, rows[i].ok);
}

/* Slim glass bar on the left: status list, sign-in options (lock screen). */
static void fs_sidebar(int with_options)
{
    int H = (int)fb_get_height();
    ui_glass_tinted(8, 8, FS_SIDE_W, H - 16, 14, 0x00181C34, 150);
    if (!g_fs.status_hidden) fb_blend_rounded_rect(13, 16, FS_SIDE_W - 10, 36, 8, 0x00FFFFFF, 26);
    glyph_draw(GLYPH_MENU, 8 + (FS_SIDE_W - 22) / 2, 23, 22, FS_WHITE, 0);
    if (with_options)
        glyph_draw(GLYPH_COG, 8 + (FS_SIDE_W - 22) / 2, 69, 22, FS_DIM, 0);
}

/* 1: status toggle, 2: options, 0: not on the bar */
static int fs_sidebar_hit(int mx, int my)
{
    if (!ui_hit(mx, my, 8, 8, FS_SIDE_W, (int)fb_get_height() - 16)) return 0;
    if (my < 56) return 1;
    if (my < 102) return 2;
    return 3;
}

/* Bottom-right tray: network state, high contrast, power. */
static void fs_tray_rect(int *x, int *y, int *w, int *h)
{
    *w = 150; *h = 50;
    *x = (int)fb_get_width() - *w - 10;
    *y = (int)fb_get_height() - *h - 10;
}

static void fs_tray(void)
{
    int x, y, w, h;
    net_info_t ni;
    net_get_info(&ni);
    fs_tray_rect(&x, &y, &w, &h);
    ui_glass_tinted(x, y, w, h, 14, 0x00181C34, 150);
    glyph_draw(GLYPH_NET, x + 18, y + 14, 22, ni.configured ? FS_WHITE : FS_FAINT, 0);
    glyph_draw(GLYPH_ACCESS, x + 64, y + 13, 24, theme_get_mode() == THEME_HIGH_CONTRAST ? T(ACCENT) : FS_WHITE, 0);
    glyph_draw(GLYPH_POWER, x + 110, y + 14, 22, g_fs.power_open ? T(ACCENT) : FS_WHITE, 0);
    if (g_fs.power_open) {
        int px = x + w - 170, py = y - 92;
        ui_glass_tinted(px, py, 170, 84, 12, 0x00181C34, 190);
        glyph_draw(GLYPH_RESTART, px + 14, py + 12, 18, FS_WHITE, 0);
        fb_text(px + 44, py + 12, "Restart", FS_WHITE, FONT_UI);
        fb_blend_rect(px + 12, py + 42, 146, 1, 0x00FFFFFF, 30);
        glyph_draw(GLYPH_POWER, px + 14, py + 54, 18, FS_WHITE, 0);
        fb_text(px + 44, py + 54, "Shut down", FS_WHITE, FONT_UI);
    }
}

/* Returns 1 if the click was on the tray or its power menu. */
static int fs_tray_click(int mx, int my)
{
    int x, y, w, h;
    fs_tray_rect(&x, &y, &w, &h);
    if (g_fs.power_open) {
        int px = x + w - 170, py = y - 92;
        g_fs.power_open = 0;
        if (ui_hit(mx, my, px, py, 170, 42)) { desktop_system_restart(); return 1; }
        if (ui_hit(mx, my, px, py + 42, 170, 42)) { desktop_system_shutdown(); return 1; }
    }
    if (!ui_hit(mx, my, x, y, w, h)) return 0;
    if (mx >= x + 100) g_fs.power_open = !g_fs.power_open;
    else if (mx >= x + 54) {
        theme_mode_t next = theme_get_mode() == THEME_HIGH_CONTRAST ? THEME_DARK : THEME_HIGH_CONTRAST;
        theme_set_mode(next);
        if (account_exists()) setting_set_int("theme", next);
    }
    return 1;
}

/* Frosted panel inside the neon frame. */
static void fs_panel(int x, int y, int w, int h, int r)
{
    ui_neon_frame(x - 9, y - 9, w + 18, h + 18, r + 8, 2, 9);
    ui_glass_tinted(x, y, w, h, r, 0x002A2E4C, 142);
}

/* Small tab on the top edge of a panel carrying its title. */
static void fs_panel_tab(int x, int y, int w, const char *title)
{
    int tw = fb_text_width_font(title, FONT_MEDIUM) + 64;
    int tx = x + (w - tw) / 2;
    ui_glass_tinted(tx, y - 4, tw, 36, 10, 0x001C2038, 170);
    fb_neon_rounded_rect(tx + 14, y + 30, tw - 28, 2, 1, 1, 4, UI_NEON_CYAN, UI_NEON_VIOLET, 230);
    fs_text_center(tx, y + 4, tw, title, FS_WHITE, FONT_MEDIUM);
}

/* Inner card: darker glass with a hairline; `selected` adds the neon outline. */
static void fs_card(int x, int y, int w, int h, int selected)
{
    fb_blend_rounded_rect(x, y, w, h, 10, 0x000A0E22, 118);
    if (selected) ui_neon_frame(x, y, w, h, 10, 2, 5);
    else fb_neon_rounded_rect(x, y, w, h, 10, 1, 0, 0x00C8D8FF, 0x00C8D8FF, 40);
}

/* Pill-shaped text field. Returns nothing; the caret follows the text. */
static void fs_field(int x, int y, int w, int h, const char *text, int len, int masked,
                     const char *placeholder, int focused, int lead_glyph)
{
    fb_blend_rounded_rect(x, y, w, h, h / 2, 0x00B8C4E8, 52);
    if (focused) ui_neon_frame(x, y, w, h, h / 2, 2, 4);
    else fb_neon_rounded_rect(x, y, w, h, h / 2, 1, 0, 0x00C8D8FF, 0x00C8D8FF, 70);
    int tx = x + h / 2 + 2, ty = y + (h - 20) / 2;
    if (lead_glyph >= 0) { glyph_draw((glyph_id_t)lead_glyph, x + 14, y + (h - 20) / 2, 20, FS_DIM, 0); tx = x + 46; }
    int max_w = x + w - h / 2 - tx;
    if (len > 0) {
        if (masked) {
            int n = len > 28 ? 28 : len;
            for (int i = 0; i < n; i++) fb_fill_circle(tx + 5 + i * 13, y + h / 2, 4, FS_WHITE);
            if (focused) fb_fill_rect(tx + 4 + n * 13, y + h / 2 - 9, 1, 18, FS_WHITE);
        } else {
            const char *p = text;
            int n = len;
            while (n > 1 && fb_text_width_n_font(p, n, FONT_MEDIUM) > max_w - 4) { p++; n--; }    /* keep the end visible */
            int end = fb_text_n(tx, ty, p, n, FS_WHITE, FONT_MEDIUM);
            if (focused) fb_fill_rect(end + 1, y + h / 2 - 9, 1, 18, FS_WHITE);
        }
    } else {
        fs_text_fit(tx, ty, max_w, placeholder ? placeholder : "", FS_FAINT, FONT_MEDIUM);
        if (focused) fb_fill_rect(tx - 2, y + h / 2 - 9, 1, 18, FS_WHITE);
    }
}

/* Primary action: blue gradient pill, optional arrow. */
static void fs_primary(int x, int y, int w, int h, const char *label, int enabled, int arrow)
{
    if (!enabled) {
        fb_blend_rounded_rect(x, y, w, h, h / 2, 0x00606A8C, 120);
        fs_text_center(x, y + (h - 20) / 2, w, label, FS_FAINT, FONT_MEDIUM_BOLD);
        return;
    }
    fb_neon_rounded_rect(x, y, w, h, h / 2, 1, 7, 0x003C8CFF, 0x0036B4FF, 150);
    fb_fill_rounded_rect_grad(x, y, w, h, h / 2, 0x002E7BFF, 0x0038B6FF, 255);
    int tw = fb_text_width_font(label, FONT_MEDIUM_BOLD) + (arrow ? 30 : 0);
    int tx = x + (w - tw) / 2;
    fb_text(tx, y + (h - 20) / 2, label, 0x00FFFFFF, FONT_MEDIUM_BOLD);
    if (arrow) glyph_draw(GLYPH_ARROW_RIGHT, tx + tw - 22, y + (h - 22) / 2, 22, 0x00FFFFFF, 0);
}

/* Secondary action: glass pill. */
static void fs_secondary(int x, int y, int w, int h, const char *label, int enabled)
{
    fb_blend_rounded_rect(x, y, w, h, h / 2, 0x00B8C4E8, enabled ? 46 : 20);
    fb_neon_rounded_rect(x, y, w, h, h / 2, 1, 0, 0x00C8D8FF, 0x00C8D8FF, enabled ? 80 : 30);
    fs_text_center(x, y + (h - 20) / 2, w, label, enabled ? FS_WHITE : FS_FAINT, FONT_MEDIUM);
}

/* Choice chip (theme, clock format...). */
static void fs_chip(int x, int y, int w, int h, const char *label, int active)
{
    fb_blend_rounded_rect(x, y, w, h, h / 2, active ? 0x005CE6FF : 0x00B8C4E8, active ? 60 : 36);
    if (active) ui_neon_frame(x, y, w, h, h / 2, 2, 3);
    else fb_neon_rounded_rect(x, y, w, h, h / 2, 1, 0, 0x00C8D8FF, 0x00C8D8FF, 60);
    fs_text_center(x, y + (h - 16) / 2, w, label, active ? 0x00FFFFFF : FS_DIM, FONT_UI);
}

static void fs_toggle(int x, int y, int on)
{
    fb_fill_rounded_rect(x, y, 40, 20, 10, on ? 0x007C8CE8 : 0x00465070);
    fb_fill_circle(x + (on ? 30 : 10), y + 10, 7, on ? 0x00FFFFFF : 0x00B0B8D0);
}

/* The big assistant orb of the setup and sign-in screens: rings, tick marks,
 * the microphone and a waveform that follows the real microphone level. */
static void fs_voice_wave(int cx, int cy, int half_w, int max_h, int level, uint32_t left, uint32_t right)
{
    static const uint8_t shape[16] = { 20, 45, 70, 100, 60, 85, 40, 95, 55, 30, 75, 50, 25, 60, 35, 15 };
    int bars = half_w / 5;
    if (bars > 16) bars = 16;
    for (int side = -1; side <= 1; side += 2)
        for (int i = 0; i < bars; i++) {
            int amp = 2 + (max_h * shape[i] / 100) * (12 + level) / 112;
            if (amp > max_h) amp = max_h;
            int bx = cx + side * (i * 5 + 6);
            fb_draw_line_aa(bx, cy - amp, bx, cy + amp, 2, side < 0 ? left : right, (uint8_t)(230 - i * 10));
        }
}

static void fs_big_orb(int cx, int cy, int r, int listening)
{
    int level = ui_shell_get_mic_level();
    fb_glow_circle(cx, cy, r - 6, 16, UI_NEON_CYAN, 46);
    fb_fill_circle(cx, cy, r - 4, 0x000A1228);
    fb_draw_arc(cx, cy, r, 4, 150, 200, UI_NEON_CYAN, 255);
    fb_draw_arc(cx, cy, r, 3, 350, 150, UI_NEON_VIOLET, 235);
    fb_draw_arc(cx, cy, r - 8, 1, 0, 360, 0x0070D8F8, 150);
    for (int a = 0; a < 360; a += 6) {                 /* tick marks */
        int r0 = r - 20, r1 = r - 13;
        uint32_t col = (a > 130 && a < 320) ? UI_NEON_CYAN : UI_NEON_VIOLET;
        fb_draw_line_aa(cx + fb_cos1024(a) * r0 / 1024, cy + fb_sin1024(a) * r0 / 1024,
                        cx + fb_cos1024(a) * r1 / 1024, cy + fb_sin1024(a) * r1 / 1024, 1, col, 170);
    }
    fb_draw_arc(cx, cy, r - 26, 2, 0, 360, 0x0048C0F0, 200);
    /* microphone in its own ring, waveform to both sides */
    int my = cy - r / 6;
    fs_voice_wave(cx, my, r - 44 > 30 ? 60 : 30, r / 4, listening ? level : 0, UI_NEON_CYAN, UI_NEON_VIOLET);
    fb_fill_circle(cx, my, r / 4 + 2, 0x000A1228);
    fb_draw_arc(cx, my, r / 4 + 2, 2, 0, 360, listening ? UI_NEON_CYAN : 0x006C7CA8, 255);
    glyph_draw(GLYPH_MIC, cx - r / 7, my - r / 7, r * 2 / 7, listening ? 0x00C8F4FF : 0x008C98B8, 0x000A1228);
}

/* 13 px text helpers for dense pages (installer). */
static void fs_t13(int x, int y, const char *text, uint32_t col) { fb_text(x, y, text, col, FONT_UI); }
static void fs_fit13(int x, int y, int max_w, const char *text, uint32_t col) { fs_text_fit(x, y, max_w, text, col, FONT_UI); }

static void draw_loading(void)
{
    int W = (int)fb_get_width(), H = (int)fb_get_height();
    int cx = W / 2, cy = H / 2;

    draw_backdrop();
    icon_kryzen_logo(cx - 56, cy - 150, 112);
    fs_text_center(0, cy - 22, W, "Kryzen OS", FS_WHITE, FONT_HEADLINE);
    fs_text_center(0, cy + 30, W, "Neural Hybrid Desktop  \x80  by Chris Ryan Kent", UI_NEON_CYAN, FONT_UI);

    int progress = 15 + (g_ui.line_count * 12) % 80;
    fb_blend_rounded_rect(cx - 130, cy + 64, 260, 8, 4, 0x00B8C4E8, 50);
    fb_fill_rounded_rect_grad(cx - 130, cy + 64, 260 * progress / 100, 8, 4, UI_NEON_CYAN, 0x0096D8FF, 255);
    fs_text_center(0, cy + 84, W,
                   g_ui.mode == UI_MODE_SETUP ? g_ui.setup_title : g_ui.status, FS_DIM, FONT_UI);
}

#include "login.c"
#include "installer.c"

void ui_shell_render_fullscreen(void)
{
    if (!fb_get_base())
        return;
    ui_shell_ensure_init();

    switch (g_ui.mode) {
    case UI_MODE_LOADING: draw_loading(); break;
    case UI_MODE_SETUP:   login_draw_setup(); break;
    case UI_MODE_LOCK:    login_draw_lock();  break;
    case UI_MODE_INSTALLER: installer_draw(); break;
    default: return;
    }
    fb_present();
}

/* Called whenever shell state changes. */
void ui_shell_draw_frame(void)
{
    if (g_ui.mode == UI_MODE_DESKTOP)
        desktop_invalidate();
    else
        ui_shell_render_fullscreen();
}

/* ---- State reported by the rest of the kernel ---------------------------- */
void ui_shell_init(void)
{
    memset(&g_ui, 0, sizeof(g_ui));
    g_ui.mode = UI_MODE_LOADING;
    ui_copy(g_ui.status, sizeof(g_ui.status), "BOOTING");
    ui_copy(g_ui.security_label, sizeof(g_ui.security_label), "SAFE");
    ui_shell_ensure_init();
}

void ui_shell_display(const char *text)
{
    ui_copy(g_ui.lines[g_ui.line_count % UI_MAX_LINES], UI_LINE_LEN, text);
    g_ui.line_count++;
    ui_shell_draw_frame();
}

void ui_shell_show_loading_desktop(void)
{
    ui_shell_ensure_init();
    g_ui.mode = UI_MODE_LOADING;
    ui_copy(g_ui.status, sizeof(g_ui.status), "Starting services");
    ui_shell_draw_frame();
}

void ui_shell_show_lock_screen(void)
{
    ui_shell_ensure_init();
    g_ui.mode = UI_MODE_LOCK;
    ui_copy(g_ui.status, sizeof(g_ui.status), "LOCKED");
    login_on_show_lock();
    desktop_invalidate();
}

void ui_shell_show_setup_wizard(void)
{
    ui_shell_ensure_init();
    g_ui.mode = UI_MODE_SETUP;
    ui_copy(g_ui.status, sizeof(g_ui.status), "SETUP");
    login_on_show_setup();
    desktop_invalidate();
}

void ui_shell_show_installer_wizard(void)
{
    ui_shell_ensure_init();
    g_ui.mode = UI_MODE_INSTALLER;
    ui_copy(g_ui.status, sizeof(g_ui.status), "INSTALLER");
    installer_on_show();
    ui_shell_draw_frame();
}

void ui_shell_show_installer_complete(void)
{
    ui_shell_ensure_init();
    g_ui.mode = UI_MODE_INSTALLER;
    ui_copy(g_ui.status, sizeof(g_ui.status), "INSTALL COMPLETE");
    installer_on_complete();
    ui_shell_draw_frame();
}

/* The selected user is in (or the live session starts): show the desktop.
 * A new session loads that user's settings and wakes the assistant for them;
 * unlocking your own locked session changes nothing. */
void ui_shell_unlock_desktop(void)
{
    static int greeted_live;
    ui_shell_ensure_init();
    int before = user_current();
    if (user_session_begin()) {                 /* someone else was signed in */
        desktop_system_close_all();
        g_ui.line_count = 0;
        ai_host_pref("forget");
    }
    g_ui.mode = UI_MODE_DESKTOP;
    ui_copy(g_ui.status, sizeof(g_ui.status), "READY");
    if (user_current() != before) {
        desktop_apply_settings();
        desktop_files_changed();
        kernel_session_started();
    } else if (user_current() < 0 && !greeted_live) {      /* live session without an account */
        greeted_live = 1;
        kernel_session_started();
    }
    desktop_invalidate();
}

void ui_shell_set_setup_step(const char *title, const char *detail)
{
    ui_copy(g_ui.setup_title, sizeof(g_ui.setup_title), title);
    ui_copy(g_ui.setup_detail, sizeof(g_ui.setup_detail), detail);
    ui_shell_draw_frame();
}

void ui_shell_set_status(const char *status)
{
    if (strcmp(g_ui.status, status ? status : "") == 0)
        return;
    ui_copy(g_ui.status, sizeof(g_ui.status), status);
    ui_shell_draw_frame();
}

void ui_shell_set_security(const char *label, int alert)
{
    ui_copy(g_ui.security_label, sizeof(g_ui.security_label), label);
    g_ui.security_alert = alert;
    ui_shell_draw_frame();
}

void ui_shell_set_mic_level(int level)
{
    level = ui_clamp(level, 0, 100);
    if (level == g_ui.mic_level)
        return;
    g_ui.mic_level = level;
    if (g_ui.mode == UI_MODE_DESKTOP)
        desktop_invalidate();
}

void ui_shell_add_transcript(const char *text)
{
    char line[UI_LINE_LEN];
    snprintf(line, sizeof(line), "You: %s", text ? text : "");
    ui_shell_display(line);
}

/* ---- Read-only accessors ------------------------------------------------- */
int ui_shell_is_desktop(void)               { return g_ui.mode == UI_MODE_DESKTOP; }
int ui_shell_is_locked(void)                { return g_ui.mode == UI_MODE_LOCK; }
const char *ui_shell_get_status(void)       { return g_ui.status; }
const char *ui_shell_get_security_label(void) { return g_ui.security_label; }
int ui_shell_get_security_alert(void)       { return g_ui.security_alert; }
int ui_shell_get_mic_level(void)            { return g_ui.mic_level; }

int ui_shell_line_count(void)
{
    return g_ui.line_count < UI_MAX_LINES ? g_ui.line_count : UI_MAX_LINES;
}

const char *ui_shell_get_line(int index_from_oldest)
{
    int count = ui_shell_line_count();
    if (index_from_oldest < 0 || index_from_oldest >= count)
        return "";
    int first = g_ui.line_count - count;
    return g_ui.lines[(first + index_from_oldest) % UI_MAX_LINES];
}

/* ---- Sign-in / setup input ---------------------------------------------- */
/* key: WM key code (8 Backspace, 10 Enter, 9 Tab, ...) or 0; ch: typed char */
void ui_shell_lock_input(uint32_t key, char ch)
{
    if (g_ui.mode == UI_MODE_LOCK)       login_lock_input(key, ch);
    else if (g_ui.mode == UI_MODE_SETUP) login_setup_input(key, ch);
    else if (g_ui.mode == UI_MODE_INSTALLER) installer_input(key, ch);
    desktop_invalidate();
}

void ui_shell_login_click(int x, int y)
{
    if (g_ui.mode == UI_MODE_LOCK)       login_lock_click(x, y);
    else if (g_ui.mode == UI_MODE_SETUP) login_setup_click(x, y);
    else if (g_ui.mode == UI_MODE_INSTALLER) installer_click(x, y);
    desktop_invalidate();
}

/* ---- Mouse --------------------------------------------------------------- */
static int g_mouse_speed = 2;           /* 1 slow, 2 normal, 3 fast, 4 fastest */
static int g_mouse_swap = 0;            /* left-handed: swap the buttons */

void ui_shell_set_mouse(int speed, int swap)
{
    g_mouse_speed = speed < 1 ? 1 : speed > 4 ? 4 : speed;
    g_mouse_swap = swap ? 1 : 0;
}

void ui_shell_move_cursor(int dx, int dy)
{
    dx = dx * g_mouse_speed / 2;
    dy = dy * g_mouse_speed / 2;
    uint32_t W = fb_get_width();
    uint32_t H = fb_get_height();

    ui_shell_ensure_init();
    if (W == 0 || H == 0)
        return;

    g_ui.cursor_x = ui_clamp(g_ui.cursor_x + dx, 0, (int)W - 1);
    g_ui.cursor_y = ui_clamp(g_ui.cursor_y + dy, 0, (int)H - 1);

    if (desktop_system_is_initialized())
        desktop_system_handle_mouse_move(g_ui.cursor_x, g_ui.cursor_y);
}

void ui_shell_mouse_button(int button, int pressed)
{
    if (g_mouse_swap && (button == 1 || button == 2)) button = 3 - button;
    if (!desktop_system_is_initialized())
        return;

    if (pressed)
        desktop_system_handle_mouse_click(g_ui.cursor_x, g_ui.cursor_y, button);
    else
        desktop_system_handle_mouse_release(g_ui.cursor_x, g_ui.cursor_y, button);
}

void desktop_system_handle_mouse_wheel(int x, int y, int delta);   /* desktop_init.c */
void ui_shell_mouse_wheel(int delta)
{
    if (desktop_system_is_initialized())
        desktop_system_handle_mouse_wheel(g_ui.cursor_x, g_ui.cursor_y, delta);
}

/* Used by the "click" voice command. */
void ui_shell_click_cursor(void)
{
    ui_shell_mouse_button(1, 1);
    ui_shell_mouse_button(1, 0);
}

/* Settings > Privacy: forget the conversation shown in the JARVIS window. */
void ui_shell_clear_lines(void)
{
    g_ui.line_count = 0;
    desktop_invalidate();
}
