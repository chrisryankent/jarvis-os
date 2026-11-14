/*
 * =============================================================================
 * JARVIS OS — Taskbar flyouts (desktop/flyouts.c)
 * Language: C17
 *
 * Quick Settings (click the network / volume icons, or Win+A):
 *   tiles      Network, Airplane mode, Night light, Dark mode,
 *              Do not disturb, Mute, Screenshot, Lock
 *   sliders    Brightness and Volume (click, drag or mouse wheel)
 *   footer     PC name, link to Settings
 * Calendar (click the clock, or Win+N with the notification centre):
 *   date and time with seconds, the month with today marked, previous /
 *   next month.
 * Everything here changes settings through prefs_set(), the same path the
 * Settings app, the Terminal and JARVIS use.
 * =============================================================================
 */


static struct {
    int open;
    int dragging;               /* 1 = brightness slider, 2 = volume slider */
    int cal_month, cal_year;    /* month shown in the calendar */
    int hover;
} g_fly;

#define QS_W 380
#define QS_H 352
#define CAL_W 340
#define CAL_H 380

static void fly_rect(int *x, int *y, int *w, int *h)
{
    int W = (int)fb_get_width(), H = (int)fb_get_height();
    *w = g_fly.open == FLY_QUICK ? QS_W : CAL_W;
    *h = g_fly.open == FLY_QUICK ? QS_H : CAL_H;
    *x = W - *w - 12;
    *y = H - TASKBAR_H - *h - 12;
}

int shell_flyout_is_open(void) { return g_fly.open; }

void shell_flyout_close(void)
{
    if (g_fly.open) { g_fly.open = FLY_NONE; g_fly.dragging = 0; desktop_invalidate(); }
}

void shell_flyout_toggle(int which)
{
    if (g_fly.open == which) { shell_flyout_close(); return; }
    g_fly.open = which;
    g_fly.dragging = 0;
    if (which == FLY_CALENDAR) {
        sys_time_t t;
        sys_get_time(&t);
        g_fly.cal_month = t.month;
        g_fly.cal_year = t.year;
    }
    start_menu_close();
    notifications_close_center();
    desktop_context_close();
    desktop_invalidate();
}

/* ---- Quick settings ---------------------------------------------------------- */
typedef struct { const char *label; glyph_id_t glyph; int on; const char *sub; } qs_tile_t;

static int qs_tiles(qs_tile_t *t)
{
    static char netsub[32];
    net_info_t ni;
    net_get_info(&ni);
    snprintf(netsub, sizeof(netsub), "%s", net_is_offline() ? "Off" : ni.configured ? "Connected" : ni.adapter ? "Not connected" : "No adapter");
    int n = 0;
    t[n++] = (qs_tile_t){ "Ethernet", GLYPH_NET, ni.configured && !net_is_offline(), netsub };
    t[n++] = (qs_tile_t){ "Airplane mode", GLYPH_PLANE, setting_get_int("airplane", 0), NULL };
    t[n++] = (qs_tile_t){ "Night light", GLYPH_MOON, setting_get_int("night_light", 0), NULL };
    t[n++] = (qs_tile_t){ "Dark mode", GLYPH_CONTRAST, theme_get_mode() == THEME_DARK, NULL };
    t[n++] = (qs_tile_t){ "Do not disturb", GLYPH_BELL, setting_get_int("dnd", 0), NULL };
    t[n++] = (qs_tile_t){ "Mute", GLYPH_VOLUME, setting_get_int("mute", 0), NULL };
    return n;
}

static void qs_tile_rect(int i, int x, int y, int *tx, int *ty, int *tw, int *th)
{
    *tw = (QS_W - 48 - 16) / 3;
    *th = 64;
    *tx = x + 24 + (i % 3) * (*tw + 8);
    *ty = y + 20 + (i / 3) * (*th + 26);
}

static void qs_slider_rect(int which, int x, int y, int *sx, int *sy, int *sw)
{
    *sx = x + 64;
    *sy = y + 212 + which * 44;
    *sw = QS_W - 64 - 64;
}

static void qs_draw(int x, int y)
{
    qs_tile_t t[8];
    int n = qs_tiles(t);
    for (int i = 0; i < n; i++) {
        int tx, ty, tw, th;
        qs_tile_rect(i, x, y, &tx, &ty, &tw, &th);
        uint32_t bg = t[i].on ? T(ACCENT) : T(HOVER), fg = t[i].on ? T(ACCENT_TEXT) : T(TEXT);
        if (g_fly.hover == 10 + i) bg = t[i].on ? fb_alpha_blend(T(ACCENT), 0x00FFFFFF, 210) : fb_alpha_blend(T(HOVER), T(TEXT), 225);
        fb_fill_rounded_rect(tx, ty, tw, th - 16, 6, bg);
        glyph_draw(t[i].glyph, tx + tw / 2 - 10, ty + 14, 20, fg, bg);
        ui_text_center(tx - 4, ty + th - 12, tw + 8, t[i].label, T(TEXT));
        if (t[i].sub) ui_text_center(tx - 4, ty + th + 4, tw + 8, t[i].sub, T(TEXT_DIM));
    }
    /* sliders */
    static const glyph_id_t sg[2] = { GLYPH_SUN, GLYPH_VOLUME };
    int vals[2] = { setting_get_int("brightness", 100), setting_get_int("mute", 0) ? 0 : setting_get_int("volume", 80) };
    for (int k = 0; k < 2; k++) {
        int sx, sy, sw;
        qs_slider_rect(k, x, y, &sx, &sy, &sw);
        glyph_draw(sg[k], x + 26, sy - 2, 22, T(TEXT), T(MENU));
        int min = k == 0 ? 10 : 0;
        int fill = (vals[k] - min) * sw / (100 - min);
        fb_fill_rounded_rect(sx, sy + 8, sw, 4, 2, T(HOVER));
        fb_fill_rounded_rect(sx, sy + 8, fill, 4, 2, T(ACCENT));
        fb_fill_circle(sx + fill, sy + 10, 9, T(ACCENT));
        fb_fill_circle(sx + fill, sy + 10, 4, T(MENU));
        char v[8];
        snprintf(v, sizeof(v), "%d", vals[k]);
        fb_draw_text(x + QS_W - 22 - fb_text_width(v), sy + 2, v, T(TEXT_DIM));
    }
    /* footer */
    int fy = y + QS_H - 52;
    fb_fill_rect(x + 1, fy, QS_W - 2, 51, T(SURFACE));
    fb_fill_rect(x + 1, fy, QS_W - 2, 1, T(BORDER));
    char host[48];
    prefs_get("device_name", host, sizeof(host));
    fb_draw_text(x + 20, fy + 18, host, T(TEXT_DIM));
    uint32_t gbg = g_fly.hover == 30 ? T(HOVER) : T(SURFACE);
    fb_fill_rounded_rect(x + QS_W - 92, fy + 10, 32, 32, 5, gbg);
    glyph_draw(GLYPH_CAMERA, x + QS_W - 86, fy + 16, 20, T(TEXT), gbg);
    gbg = g_fly.hover == 31 ? T(HOVER) : T(SURFACE);
    fb_fill_rounded_rect(x + QS_W - 52, fy + 10, 32, 32, 5, gbg);
    icon_draw(ICON_GEAR, x + QS_W - 46, fy + 16, 20);
}

static void qs_set_slider(int which, int px)
{
    int x, y, w, h, sx, sy, sw;
    fly_rect(&x, &y, &w, &h);
    qs_slider_rect(which, x, y, &sx, &sy, &sw);
    int min = which == 0 ? 10 : 0;
    int v = min + (px - sx) * (100 - min) / sw;
    if (v < min) v = min;
    if (v > 100) v = 100;
    char val[8], msg[96];
    snprintf(val, sizeof(val), "%d", v);
    if (which == 1 && setting_get_int("mute", 0)) prefs_set("mute", "off", msg, sizeof(msg));
    prefs_set(which == 0 ? "brightness" : "volume", val, msg, sizeof(msg));
}

static int qs_hit(int px, int py, int x, int y)
{
    qs_tile_t t[8];
    int n = qs_tiles(t);
    for (int i = 0; i < n; i++) {
        int tx, ty, tw, th;
        qs_tile_rect(i, x, y, &tx, &ty, &tw, &th);
        if (ui_hit(px, py, tx, ty, tw, th - 16)) return 10 + i;
    }
    for (int k = 0; k < 2; k++) {
        int sx, sy, sw;
        qs_slider_rect(k, x, y, &sx, &sy, &sw);
        if (ui_hit(px, py, sx - 10, sy - 4, sw + 20, 28)) return 20 + k;
    }
    int fy = y + QS_H - 52;
    if (ui_hit(px, py, x + QS_W - 92, fy + 10, 32, 32)) return 30;
    if (ui_hit(px, py, x + QS_W - 52, fy + 10, 32, 32)) return 31;
    return 0;
}

static void qs_click(int hit, int px)
{
    char msg[96];
    switch (hit) {
    case 10: desktop_app_launch(desktop_app_find("Settings")); settings_show_page("network"); shell_flyout_close(); break;
    case 11: prefs_set("airplane", "toggle", msg, sizeof(msg)); break;
    case 12: prefs_set("night_light", "toggle", msg, sizeof(msg)); break;
    case 13: prefs_set("theme", theme_get_mode() == THEME_DARK ? "light" : "dark", msg, sizeof(msg)); break;
    case 14: prefs_set("dnd", "toggle", msg, sizeof(msg)); break;
    case 15: prefs_set("mute", "toggle", msg, sizeof(msg)); break;
    case 20: case 21: g_fly.dragging = hit - 19; qs_set_slider(hit - 20, px); break;
    case 30: { shell_flyout_close(); desktop_invalidate(); char s[160]; desktop_take_screenshot(s, sizeof(s)); break; }
    case 31: shell_flyout_close(); desktop_app_launch(desktop_app_find("Settings")); break;
    }
    desktop_invalidate();
}

/* ---- Calendar ---------------------------------------------------------------- */
static int days_in(int m, int y)
{
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return d[m - 1] + (m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)));
}

static int weekday_of(int y, int m, int d)         /* 0 = Sunday (Sakamoto) */
{
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

static const char *const g_months[12] = { "January", "February", "March", "April", "May", "June", "July",
                                          "August", "September", "October", "November", "December" };
static const char *const g_wdays[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };

static void cal_draw(int x, int y)
{
    sys_time_t t;
    sys_get_time(&t);
    char b[64];
    /* time with seconds */
    if (sys_clock_24h()) snprintf(b, sizeof(b), "%02d:%02d:%02d", t.hour, t.minute, t.second);
    else snprintf(b, sizeof(b), "%d:%02d:%02d %s", t.hour % 12 ? t.hour % 12 : 12, t.minute, t.second, t.hour < 12 ? "AM" : "PM");
    fb_draw_text_scaled(x + 20, y + 14, b, T(TEXT), 3);
    snprintf(b, sizeof(b), "%s, %d %s %d", g_wdays[t.weekday % 7], t.day, g_months[(t.month + 11) % 12], t.year);
    fb_draw_text(x + 22, y + 66, b, T(ACCENT));
    fb_fill_rect(x + 1, y + 94, CAL_W - 2, 1, T(BORDER));

    /* month header with arrows */
    snprintf(b, sizeof(b), "%s %d", g_months[(g_fly.cal_month + 11) % 12], g_fly.cal_year);
    fb_draw_text_bold(x + 22, y + 108, b, T(TEXT));
    uint32_t hb = g_fly.hover == 40 ? T(HOVER) : T(MENU);
    fb_fill_rounded_rect(x + CAL_W - 84, y + 102, 30, 28, 5, hb);
    fb_draw_text(x + CAL_W - 74, y + 108, SYM_UP, T(TEXT));  /* previous month */
    hb = g_fly.hover == 41 ? T(HOVER) : T(MENU);
    fb_fill_rounded_rect(x + CAL_W - 48, y + 102, 30, 28, 5, hb);
    fb_draw_text(x + CAL_W - 38, y + 108, SYM_DOWN, T(TEXT));

    static const char *const wd[7] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };
    int cw = (CAL_W - 40) / 7;
    for (int i = 0; i < 7; i++) ui_text_center(x + 20 + i * cw, y + 142, cw, wd[i], T(TEXT_DIM));
    int first = weekday_of(g_fly.cal_year, g_fly.cal_month, 1);
    int nd = days_in(g_fly.cal_month, g_fly.cal_year);
    for (int d = 1; d <= nd; d++) {
        int cell = first + d - 1;
        int cx = x + 20 + (cell % 7) * cw, cy = y + 168 + (cell / 7) * 32;
        int today = d == t.day && g_fly.cal_month == t.month && g_fly.cal_year == t.year;
        if (today) fb_fill_circle(cx + cw / 2, cy + 8, 14, T(ACCENT));
        snprintf(b, sizeof(b), "%d", d);
        ui_text_center(cx, cy, cw, b, today ? T(ACCENT_TEXT) : T(TEXT));
    }
}

static int cal_hit(int px, int py, int x, int y)
{
    if (ui_hit(px, py, x + CAL_W - 84, y + 102, 30, 28)) return 40;
    if (ui_hit(px, py, x + CAL_W - 48, y + 102, 30, 28)) return 41;
    return 0;
}

static void cal_step(int d)
{
    g_fly.cal_month += d;
    if (g_fly.cal_month < 1) { g_fly.cal_month = 12; g_fly.cal_year--; }
    if (g_fly.cal_month > 12) { g_fly.cal_month = 1; g_fly.cal_year++; }
    desktop_invalidate();
}

/* ---- Shared ------------------------------------------------------------------- */
void shell_flyout_draw(void)
{
    if (!g_fly.open) return;
    int x, y, w, h;
    fly_rect(&x, &y, &w, &h);
    fb_blend_rounded_rect(x + 2, y + 6, w, h, 10, 0x00000000, 70);
    fb_fill_rounded_rect(x, y, w, h, 10, T(BORDER));
    if (setting_get_int("transparency", 1) && theme_get_mode() != THEME_HIGH_CONTRAST)
        fb_blend_rounded_rect(x + 1, y + 1, w - 2, h - 2, 9, T(MENU), 242);
    else
        fb_fill_rounded_rect(x + 1, y + 1, w - 2, h - 2, 9, T(MENU));
    fb_clip_t saved = fb_get_clip();
    fb_set_clip(x, y, w, h);
    if (g_fly.open == FLY_QUICK) qs_draw(x, y); else cal_draw(x, y);
    fb_restore_clip(saved);
}

/* Returns 1 if the click was inside the open flyout. */
int shell_flyout_click(int px, int py)
{
    if (!g_fly.open) return 0;
    int x, y, w, h;
    fly_rect(&x, &y, &w, &h);
    if (!ui_hit(px, py, x, y, w, h)) return 0;
    if (g_fly.open == FLY_QUICK) qs_click(qs_hit(px, py, x, y), px);
    else { int hit = cal_hit(px, py, x, y); if (hit == 40) cal_step(-1); else if (hit == 41) cal_step(1); }
    return 1;
}

int shell_flyout_hover(int px, int py)
{
    if (!g_fly.open) return 0;
    if (g_fly.dragging) { qs_set_slider(g_fly.dragging - 1, px); return 1; }
    int x, y, w, h;
    fly_rect(&x, &y, &w, &h);
    int hit = !ui_hit(px, py, x, y, w, h) ? 0 : g_fly.open == FLY_QUICK ? qs_hit(px, py, x, y) : cal_hit(px, py, x, y);
    if (hit == g_fly.hover) return 0;
    g_fly.hover = hit;
    return 1;
}

void shell_flyout_release(void) { g_fly.dragging = 0; }
int  shell_flyout_dragging(void) { return g_fly.dragging; }

/* Mouse wheel: over the flyout's sliders, the calendar, or the tray icons. */
int shell_flyout_wheel(int px, int py, int delta)
{
    char msg[96];
    int H = (int)fb_get_height();
    if (py >= H - TASKBAR_H && taskbar_tray_hit(px, py)) {             /* over the volume icon */
        prefs_set("volume", delta > 0 ? "up" : "down", msg, sizeof(msg));
        if (setting_get_int("mute", 0)) prefs_set("mute", "off", msg, sizeof(msg));
        return 1;
    }
    if (!g_fly.open) return 0;
    int x, y, w, h;
    fly_rect(&x, &y, &w, &h);
    if (!ui_hit(px, py, x, y, w, h)) return 0;
    if (g_fly.open == FLY_CALENDAR) { cal_step(delta > 0 ? -1 : 1); return 1; }
    int hit = qs_hit(px, py, x, y);
    if (hit == 20 || hit == 21) prefs_set(hit == 20 ? "brightness" : "volume", delta > 0 ? "up" : "down", msg, sizeof(msg));
    return 1;
}
