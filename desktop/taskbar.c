/*
 * =============================================================================
 * Kryzen OS — Taskbar and Start menu (desktop/taskbar.c)
 * Language: C17
 *
 * Taskbar: centred Start button, search and app icons (with running / focused
 * indicators); system tray on the right with the JARVIS status chip, network
 * and volume, clock and the notification bell.
 *
 * Start menu: search box (type to filter, Enter to launch or ask JARVIS),
 * app grid, JARVIS status card, and lock / restart / shut down.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "../wm/wm.h"
#include "ui.h"
#include <string.h>
#include <stdio.h>

typedef struct { int x, y, w, h; } rect_t;

static int net_connected(void)
{
    net_info_t ni;
    net_get_info(&ni);
    return ni.configured;
}

static int rect_hit(const rect_t *r, int px, int py)
{
    return ui_hit(px, py, r->x, r->y, r->w, r->h);
}

/* ==========================================================================
 * TASKBAR: the slim top bar and the dock
 *
 * Top bar   K (Start) on the left; on the right quick settings, lock,
 *           network, sound, the clock and show-desktop.
 * Dock      Kryzen logo (Start), the assistant orb, the search field, pinned
 *           and running apps, then Settings, quick settings, sound,
 *           notifications and show-desktop.
 * ========================================================================== */
#define TB_MAX_APPS 16
#define DOCK_H      64

enum {
    TBH_NONE = 0, TBH_START, TBH_SEARCH, TBH_CHIP, TBH_CLOCK, TBH_BELL, TBH_STATUS,
    TBH_SHOWDESK, TBH_SETTINGS, TBH_VOLUME,
    TBH_TOP_K, TBH_TOP_TRAY, TBH_TOP_LOCK, TBH_TOP_NET, TBH_TOP_VOL, TBH_TOP_DESK,
    TBH_APP0 = 100,
};

static struct {
    rect_t dock, start, search, apps[TB_MAX_APPS], chip, status, bell, showdesk, settings, volume;
    rect_t top_k, top_tray, top_lock, top_net, top_vol, clock, top_desk;
    int app_idx[TB_MAX_APPS];   /* taskbar slot -> app index */
    int app_count;
    int hover;
} g_tb;

static struct {
    int open;
    char query[40];
    int qlen;
    int hover;
} g_start;

/* ---- Pinned apps (setting "pins": names separated by commas) -------------- */
static const char *g_default_pins = "Files,Browser,Notepad,Photos,Terminal,JARVIS,Settings";

int taskbar_is_pinned(int idx)
{
    char pins[256];
    setting_get("pins", pins, sizeof(pins));
    if (!pins[0]) snprintf(pins, sizeof(pins), "%s", g_default_pins);
    desktop_app_t *a = desktop_app_get(idx);
    if (!a) return 0;
    size_t l = strlen(a->name);
    for (const char *p = pins; *p;) {
        const char *e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n == l && !strncmp(p, a->name, l)) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

void taskbar_set_pinned(int idx, int on)
{
    char pins[256], out[256] = "";
    setting_get("pins", pins, sizeof(pins));
    if (!pins[0]) snprintf(pins, sizeof(pins), "%s", g_default_pins);
    desktop_app_t *a = desktop_app_get(idx);
    if (!a) return;
    for (char *p = pins; *p;) {
        char *e = strchr(p, ',');
        if (e) *e = 0;
        if (strcmp(p, a->name) && *p) snprintf(out + strlen(out), sizeof(out) - strlen(out), "%s%s", out[0] ? "," : "", p);
        if (!e) break;
        p = e + 1;
    }
    if (on) snprintf(out + strlen(out), sizeof(out) - strlen(out), "%s%s", out[0] ? "," : "", a->name);
    setting_set("pins", out[0] ? out : ",");
    desktop_invalidate();
}

static int taskbar_y(void)
{
    return (int)fb_get_height() - TASKBAR_H;
}

/* One layout function shared by drawing and hit-testing, so what you see is
 * always exactly what you can click. */
static void taskbar_layout(void)
{
    int W = (int)fb_get_width(), H = (int)fb_get_height();

    /* ---- top bar ---- */
    g_tb.top_k    = (rect_t){ 0, 0, 32, TOPBAR_H };
    int rx = W - 4;
    g_tb.top_desk = (rect_t){ rx - 26, 0, 26, TOPBAR_H };  rx -= 30;
    g_tb.clock    = (rect_t){ rx - 74, 0, 74, TOPBAR_H };  rx -= 78;
    g_tb.top_vol  = (rect_t){ rx - 24, 0, 24, TOPBAR_H };  rx -= 26;
    g_tb.top_net  = (rect_t){ rx - 24, 0, 24, TOPBAR_H };  rx -= 26;
    g_tb.top_lock = (rect_t){ rx - 24, 0, 24, TOPBAR_H };  rx -= 26;
    g_tb.top_tray = (rect_t){ rx - 24, 0, 24, TOPBAR_H };

    /* ---- dock ---- */
    int dx = 8, dy = H - DOCK_H - 6, dw = W - 16;
    g_tb.dock = (rect_t){ dx, dy, dw, DOCK_H };
    g_tb.start = (rect_t){ dx + 10, dy + 8, W >= 1000 ? 152 : 50, 48 };

    /* right cluster */
    rx = dx + dw - 10;
    g_tb.showdesk = (rect_t){ rx - 40, dy + 12, 40, 40 };  rx -= 44;
    g_tb.bell     = (rect_t){ rx - 40, dy + 12, 40, 40 };  rx -= 44;
    g_tb.volume   = (rect_t){ rx - 40, dy + 12, 40, 40 };  rx -= 44;
    g_tb.status   = (rect_t){ rx - 40, dy + 12, 40, 40 };  rx -= 44;
    g_tb.settings = (rect_t){ rx - 40, dy + 12, 40, 40 };  rx -= 44;
    int right_edge = rx - 8;

    /* pinned apps first, then any other open app */
    int n = 0;
    for (int i = 0; i < desktop_app_builtin_count() && n < TB_MAX_APPS; i++)
        if (taskbar_is_pinned(i)) g_tb.app_idx[n++] = i;
    for (int i = 0; i < desktop_app_builtin_count() && n < TB_MAX_APPS; i++)
        if (!taskbar_is_pinned(i) && desktop_app_get(i)->window) g_tb.app_idx[n++] = i;

    /* orb, search and apps share what is left between the logo and the tray */
    int left_edge = g_tb.start.x + g_tb.start.w + 12;
    int room = right_edge - left_edge;
    int max_apps = (room - 60 - 60) / 46;                   /* orb + a minimal search button */
    if (max_apps < 0) max_apps = 0;
    if (n > max_apps) n = max_apps;
    g_tb.app_count = n;
    int search_w = room - 60 - n * 46 - 24;
    if (search_w > 360) search_w = 360;
    if (search_w < 120) search_w = 44;                      /* just the magnifier */
    int total = 56 + 12 + search_w + 14 + n * 46;
    int cx = left_edge + (room - total) / 2;
    int ideal = W * 30 / 100 - 28;                          /* the orb sits about a third along */
    if (cx > ideal && ideal >= left_edge) cx = ideal;
    g_tb.chip = (rect_t){ cx, dy + 4, 56, 56 };             cx += 56 + 12;
    g_tb.search = (rect_t){ cx, dy + 12, search_w, 40 };    cx += search_w + 14;
    for (int i = 0; i < n; i++, cx += 46)
        g_tb.apps[i] = (rect_t){ cx, dy + 9, 46, 46 };
}

static int taskbar_hit(int x, int y)
{
    taskbar_layout();
    if (y < TOPBAR_H) {
        if (rect_hit(&g_tb.top_k, x, y))    return TBH_TOP_K;
        if (rect_hit(&g_tb.top_tray, x, y)) return TBH_TOP_TRAY;
        if (rect_hit(&g_tb.top_lock, x, y)) return TBH_TOP_LOCK;
        if (rect_hit(&g_tb.top_net, x, y))  return TBH_TOP_NET;
        if (rect_hit(&g_tb.top_vol, x, y))  return TBH_TOP_VOL;
        if (rect_hit(&g_tb.clock, x, y))    return TBH_CLOCK;
        if (rect_hit(&g_tb.top_desk, x, y)) return TBH_TOP_DESK;
        return TBH_NONE;
    }
    if (rect_hit(&g_tb.start, x, y))    return TBH_START;
    if (rect_hit(&g_tb.chip, x, y))     return TBH_CHIP;
    if (rect_hit(&g_tb.search, x, y))   return TBH_SEARCH;
    for (int i = 0; i < g_tb.app_count; i++)
        if (rect_hit(&g_tb.apps[i], x, y)) return TBH_APP0 + i;
    if (rect_hit(&g_tb.settings, x, y)) return TBH_SETTINGS;
    if (rect_hit(&g_tb.status, x, y))   return TBH_STATUS;
    if (rect_hit(&g_tb.volume, x, y))   return TBH_VOLUME;
    if (rect_hit(&g_tb.bell, x, y))     return TBH_BELL;
    if (rect_hit(&g_tb.showdesk, x, y)) return TBH_SHOWDESK;
    return TBH_NONE;
}

void taskbar_init(void)
{
    memset(&g_tb, 0, sizeof(g_tb));
    memset(&g_start, 0, sizeof(g_start));
    g_start.hover = -1;
}

int taskbar_hover(int x, int y)
{
    int h = (y < TOPBAR_H || y >= taskbar_y()) ? taskbar_hit(x, y) : TBH_NONE;
    if (h == g_tb.hover) return 0;
    g_tb.hover = h;
    return 1;
}

static void tb_hover_bg(const rect_t *r, int code, int active)
{
    if (g_tb.hover == code || active)
        fb_blend_rounded_rect(r->x, r->y, r->w, r->h, 9, 0x00FFFFFF, active ? 44 : 28);
}

static void tb_top_item(const rect_t *r, int code, glyph_id_t g, uint32_t col)
{
    if (g_tb.hover == code) fb_blend_rect(r->x, r->y, r->w, r->h, 0x00FFFFFF, 30);
    glyph_draw(g, r->x + (r->w - 16) / 2, r->y + (TOPBAR_H - 16) / 2, 16, col, 0);
}

void taskbar_draw(void)
{
    int W = (int)fb_get_width();
    uint32_t fg = T(TEXT), dim = T(TEXT_DIM);
    int muted = !g_kstate.audio_ready || setting_get_int("mute", 0);

    taskbar_layout();

    /* ---- top bar ---- */
    ui_glass_tinted(-12, -12, W + 24, TOPBAR_H + 12, 0, 0x00141830, 170);
    if (g_start.open || g_tb.hover == TBH_TOP_K) fb_blend_rect(0, 0, 32, TOPBAR_H, 0x005CE6FF, 60);
    icon_kryzen_logo(8, 3, 18);
    tb_top_item(&g_tb.top_tray, TBH_TOP_TRAY, GLYPH_CHEVRON_UP, fg);
    tb_top_item(&g_tb.top_lock, TBH_TOP_LOCK, GLYPH_PADLOCK, fg);
    tb_top_item(&g_tb.top_net, TBH_TOP_NET, GLYPH_NET, net_connected() ? fg : dim);
    tb_top_item(&g_tb.top_vol, TBH_TOP_VOL, GLYPH_VOLUME, muted ? dim : fg);
    {
        char clock[16];
        sys_time_t t;
        sys_get_time(&t);
        sys_format_clock(clock, sizeof(clock));
        if (setting_get_int("clock_seconds", 0)) {
            if (sys_clock_24h()) snprintf(clock, sizeof(clock), "%02d:%02d:%02d", t.hour, t.minute, t.second);
            else snprintf(clock, sizeof(clock), "%d:%02d:%02d %s", t.hour % 12 ? t.hour % 12 : 12, t.minute, t.second, t.hour < 12 ? "AM" : "PM");
        }
        if (g_tb.hover == TBH_CLOCK || shell_flyout_is_open() == FLY_CALENDAR)
            fb_blend_rect(g_tb.clock.x, 0, g_tb.clock.w, TOPBAR_H, 0x00FFFFFF, 30);
        fb_draw_text(g_tb.clock.x + (g_tb.clock.w - fb_text_width(clock)) / 2, (TOPBAR_H - 16) / 2, clock, fg);
    }
    tb_top_item(&g_tb.top_desk, TBH_TOP_DESK, GLYPH_SQUARE, fg);

    /* ---- dock ---- */
    rect_t *d = &g_tb.dock;
    fb_blend_rounded_rect(d->x, d->y + 3, d->w, d->h, 16, 0x00000000, 60);
    ui_glass(d->x, d->y, d->w, d->h, 14);

    /* logo: opens Start */
    tb_hover_bg(&g_tb.start, TBH_START, g_start.open);
    icon_kryzen_logo(g_tb.start.x + 6, g_tb.start.y + 5, 38);
    if (g_tb.start.w > 100)
        fb_text(g_tb.start.x + 52, g_tb.start.y + 8, "Kryzen", 0x008FEAFF, FONT_TITLE);

    /* the assistant */
    {
        rect_t *r = &g_tb.chip;
        int level = ui_shell_get_mic_level();
        ui_orb(r->x + r->w / 2, r->y + r->h / 2, g_tb.hover == TBH_CHIP ? 25 : 23, level);
    }

    /* search */
    {
        rect_t *r = &g_tb.search;
        if (r->w > 60) {
            fb_blend_rounded_rect(r->x, r->y, r->w, r->h, r->h / 2, 0x00050818, g_tb.hover == TBH_SEARCH ? 150 : 110);
            fb_neon_rounded_rect(r->x, r->y, r->w, r->h, r->h / 2, 1, 0, 0x00C8D8FF, 0x00C8D8FF, 70);
            glyph_draw(GLYPH_SEARCH, r->x + 14, r->y + 11, 18, dim, 0);
            ui_text_fit(r->x + 42, r->y + 12, r->w - 56, "Search applications, files, and AI...", dim);
        } else {
            tb_hover_bg(r, TBH_SEARCH, 0);
            glyph_draw(GLYPH_SEARCH, r->x + (r->w - 20) / 2, r->y + 10, 20, fg, 0);
        }
    }

    /* apps */
    for (int i = 0; i < g_tb.app_count; i++) {
        desktop_app_t *app = desktop_app_get(g_tb.app_idx[i]);
        rect_t *r = &g_tb.apps[i];
        int running = app->window != NULL;
        int focused = running && app->window == g_wm_ctx.focused;
        tb_hover_bg(r, TBH_APP0 + i, focused);
        icon_draw(app->icon, r->x + 8, r->y + 6, 30);
        if (focused) fb_fill_rounded_rect(r->x + 15, r->y + 41, 16, 3, 1, UI_NEON_CYAN);
        else if (running) fb_fill_circle(r->x + 23, r->y + 42, 2, 0x00FFFFFF);
    }

    /* right cluster */
    tb_hover_bg(&g_tb.settings, TBH_SETTINGS, 0);
    glyph_draw(GLYPH_COG, g_tb.settings.x + 8, g_tb.settings.y + 8, 24, fg, 0);
    tb_hover_bg(&g_tb.status, TBH_STATUS, shell_flyout_is_open() == FLY_QUICK);
    glyph_draw(GLYPH_DISPLAY, g_tb.status.x + 8, g_tb.status.y + 8, 24, fg, 0);
    tb_hover_bg(&g_tb.volume, TBH_VOLUME, 0);
    glyph_draw(GLYPH_VOLUME, g_tb.volume.x + 9, g_tb.volume.y + 9, 22, muted ? dim : fg, 0);
    if (muted) fb_draw_line_aa(g_tb.volume.x + 10, g_tb.volume.y + 30, g_tb.volume.x + 30, g_tb.volume.y + 10, 2, T(DANGER), 255);
    tb_hover_bg(&g_tb.bell, TBH_BELL, notifications_center_is_visible());
    glyph_draw(GLYPH_BELL, g_tb.bell.x + 9, g_tb.bell.y + 9, 22, fg, 0);
    if (notifications_get_unread_count() > 0) fb_fill_circle(g_tb.bell.x + 29, g_tb.bell.y + 11, 4, UI_NEON_CYAN);
    tb_hover_bg(&g_tb.showdesk, TBH_SHOWDESK, 0);
    glyph_draw(GLYPH_SQUARE, g_tb.showdesk.x + 8, g_tb.showdesk.y + 8, 24, fg, 0);
}

static int g_tb_menu_app = -1;
static void taskbar_menu_action(int id, void *ctx)
{
    (void)ctx;
    int idx = g_tb_menu_app;
    desktop_app_t *a = idx >= 0 ? desktop_app_get(idx) : NULL;
    switch (id) {
    case 1: if (a) desktop_app_launch(idx); break;
    case 2: if (a && a->window) wm_request_close(a->window); break;
    case 3: taskbar_set_pinned(idx, !taskbar_is_pinned(idx)); break;
    case 4: desktop_app_launch(desktop_app_find("Tasks")); break;
    case 5: desktop_app_launch(desktop_app_find("Settings")); settings_show_page("personalization"); break;
    }
}

/* Is (x,y) on a sound icon? (the mouse wheel changes the volume there) */
int taskbar_tray_hit(int x, int y)
{
    taskbar_layout();
    return rect_hit(&g_tb.volume, x, y) || rect_hit(&g_tb.top_vol, x, y);
}

/* Right-click on the taskbar: per-app menu, or taskbar menu. */
int taskbar_handle_right_click(int x, int y)
{
    if (y < taskbar_y() && y >= TOPBAR_H) return 0;
    int hit = taskbar_hit(x, y);
    if (hit >= TBH_APP0) {
        g_tb_menu_app = g_tb.app_idx[hit - TBH_APP0];
        desktop_app_t *a = desktop_app_get(g_tb_menu_app);
        ui_menu_item_t items[3] = {
            { a->window ? "Bring to front" : "Open", 1, 0, 0 },
            { taskbar_is_pinned(g_tb_menu_app) ? "Unpin from taskbar" : "Pin to taskbar", 3, 0, 1 },
            { "Close window", 2, !a->window, 0 },
        };
        ui_menu_open(x, taskbar_y() - 3 * 30 - 16, items, 3, taskbar_menu_action, NULL);
    } else {
        ui_menu_item_t items[2] = { { "Task Manager", 4, 0, 0 }, { "Taskbar settings", 5, 0, 0 } };
        ui_menu_open(x, taskbar_y() - 2 * 30 - 16, items, 2, taskbar_menu_action, NULL);
    }
    return 1;
}

/* Returns 1 if the click landed on the top bar or the dock. */
int taskbar_handle_click(int x, int y)
{
    if (y < taskbar_y() && y >= TOPBAR_H)
        return 0;

    int hit = taskbar_hit(x, y);

    if (hit != TBH_START && hit != TBH_SEARCH && hit != TBH_TOP_K)
        start_menu_close();
    if (hit != TBH_BELL)
        notifications_close_center();
    if (hit != TBH_STATUS && hit != TBH_CLOCK && hit != TBH_TOP_TRAY && hit != TBH_TOP_NET)
        shell_flyout_close();

    if (hit == TBH_START || hit == TBH_TOP_K) {
        taskbar_toggle_start_menu();
    } else if (hit == TBH_SEARCH) {
        if (!g_start.open) taskbar_toggle_start_menu();
    } else if (hit >= TBH_APP0) {
        desktop_app_activate(g_tb.app_idx[hit - TBH_APP0]);
    } else if (hit == TBH_STATUS || hit == TBH_TOP_TRAY || hit == TBH_TOP_NET) {
        shell_flyout_toggle(FLY_QUICK);
    } else if (hit == TBH_VOLUME || hit == TBH_TOP_VOL) {
        char msg[96];
        prefs_set("mute", setting_get_int("mute", 0) ? "off" : "on", msg, sizeof(msg));
    } else if (hit == TBH_SETTINGS) {
        desktop_app_launch(desktop_app_find("Settings"));
    } else if (hit == TBH_CHIP) {
        desktop_app_launch(desktop_app_find("JARVIS"));
    } else if (hit == TBH_CLOCK) {
        shell_flyout_toggle(FLY_CALENDAR);
    } else if (hit == TBH_BELL) {
        notifications_toggle_center();
    } else if (hit == TBH_TOP_LOCK) {
        desktop_lock();
    } else if (hit == TBH_SHOWDESK || hit == TBH_TOP_DESK) {
        wm_minimize_all();
    }

    desktop_invalidate();
    return 1;
}

/* ==========================================================================
 * START MENU
 * ========================================================================== */
#define SM_W        600
#define SM_COLS     6
#define SM_CELL_W   92
#define SM_CELL_H   84
#define SM_FOOTER_H 60

enum { SMH_NONE = -1, SMH_SIGNOUT = 200, SMH_LOCK, SMH_RESTART, SMH_POWER };

static void start_rect(int *x, int *y, int *w, int *h)
{
    int H = (int)fb_get_height() - TASKBAR_H - 24;
    *w = SM_W;
    if (*w > (int)fb_get_width() - 24) *w = (int)fb_get_width() - 24;
    H -= TOPBAR_H;
    *h = H < 580 ? H : 580;
    *x = 8;
    *y = taskbar_y() - *h - 2;
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int icontains(const char *hay, const char *needle)
{
    if (!*needle) return 1;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && lower(*h) == lower(*n)) { h++; n++; }
        if (!*n) return 1;
    }
    return 0;
}

/* Fills out[] with the indices of apps matching the search query. */
static int start_matches(int *out, int max)
{
    int n = 0;
    for (int i = 0; i < desktop_app_count() && n < max; i++)
        if (icontains(desktop_app_get(i)->name, g_start.query))
            out[n++] = i;
    return n;
}

static void start_cell_rect(int k, int sx, int sy, rect_t *r)
{
    r->x = sx + 24 + (k % SM_COLS) * SM_CELL_W;
    r->y = sy + 106 + (k / SM_COLS) * SM_CELL_H;
    r->w = SM_CELL_W;
    r->h = SM_CELL_H - 4;
}

static void start_power_rect(int which, int sx, int sy, int sw, int sh, rect_t *r)
{
    /* which: 0 = sign out, 1 = lock, 2 = restart, 3 = power (right-most) */
    r->w = 36; r->h = 36;
    r->x = sx + sw - 20 - 36 - (3 - which) * 42;
    r->y = sy + sh - SM_FOOTER_H + 12;
}

static int start_hit(int px, int py)
{
    int x, y, w, h, m[TB_MAX_APPS];
    rect_t r;

    start_rect(&x, &y, &w, &h);
    int n = start_matches(m, TB_MAX_APPS);
    for (int k = 0; k < n; k++) {
        start_cell_rect(k, x, y, &r);
        if (rect_hit(&r, px, py)) return 100 + k;
    }
    for (int b = 0; b < 4; b++) {
        start_power_rect(b, x, y, w, h, &r);
        if (rect_hit(&r, px, py)) return SMH_SIGNOUT + b;
    }
    return SMH_NONE;
}

void taskbar_toggle_start_menu(void)
{
    g_start.open = !g_start.open;
    g_start.query[0] = 0;
    g_start.qlen = 0;
    g_start.hover = SMH_NONE;
    if (g_start.open) {
        notifications_close_center();
        desktop_context_close();
    }
    desktop_invalidate();
}

int taskbar_is_start_menu_open(void)
{
    return g_start.open;
}

void start_menu_close(void)
{
    if (g_start.open)
        taskbar_toggle_start_menu();
}

int start_menu_hover(int px, int py)
{
    if (!g_start.open) return 0;
    int h = start_hit(px, py);
    if (h == g_start.hover) return 0;
    g_start.hover = h;
    return 1;
}

static void status_row(int x, int y, const char *label, const char *value, uint32_t dot)
{
    fb_fill_circle(x + 5, y + 8, 4, dot);
    fb_draw_text(x + 18, y, label, T(TEXT_DIM));
    fb_draw_text(x + 18 + fb_text_width(label) + 8, y, value, T(TEXT));
}

void start_menu_draw(void)
{
    if (!g_start.open) return;

    int x, y, w, h, m[TB_MAX_APPS];
    rect_t r;
    start_rect(&x, &y, &w, &h);

    fb_blend_rounded_rect(x + 2, y + 6, w, h, 14, 0x00000000, 70);
    ui_glass_tinted(x, y, w, h, 14, 0x001A1E36, 200);
    ui_neon_frame(x, y, w, h, 14, 1, 5);

    /* ---- Search box ---- */
    fb_blend_rounded_rect(x + 24, y + 24, w - 48, 36, 18, 0x00050818, 130);
    fb_neon_rounded_rect(x + 24, y + 24, w - 48, 36, 18, 1, 0, UI_NEON_CYAN, UI_NEON_VIOLET, 150);
    glyph_draw(GLYPH_SEARCH, x + 38, y + 34, 16, T(TEXT_DIM), 0);
    if (g_start.qlen) {
        fb_draw_text(x + 64, y + 34, g_start.query, T(TEXT));
        fb_fill_rect(x + 64 + fb_text_width_n(g_start.query, g_start.qlen) + 1, y + 34, 1, 16, T(TEXT));
    } else {
        fb_draw_text(x + 64, y + 34, "Type to search apps, or ask JARVIS", T(TEXT_DIM));
    }

    /* ---- App grid ---- */
    int n = start_matches(m, TB_MAX_APPS);
    fb_draw_text_bold(x + 32, y + 78, g_start.qlen ? "Best match" : "Pinned", T(TEXT));

    for (int k = 0; k < n; k++) {
        desktop_app_t *app = desktop_app_get(m[k]);
        start_cell_rect(k, x, y, &r);
        if (g_start.hover == 100 + k || (g_start.qlen && k == 0))
            fb_blend_rounded_rect(r.x + 2, r.y, r.w - 4, r.h, 8, 0x00FFFFFF, 30);
        icon_draw(app->icon, r.x + (r.w - 36) / 2, r.y + 8, 36);
        ui_text_center(r.x, r.y + 54, r.w, app->name, T(TEXT));
        if (app->window)
            fb_fill_rounded_rect(r.x + r.w / 2 - 4, r.y + r.h - 6, 8, 3, 1, T(ACCENT));
    }

    if (n == 0) {
        fb_draw_text(x + 32, y + 116, "No app matches. Press Enter to ask JARVIS:", T(TEXT_DIM));
        fb_draw_text(x + 32, y + 138, g_start.query, T(ACCENT));
    }

    /* ---- JARVIS status card ---- */
    int card_h = 96;
    int card_y = y + h - SM_FOOTER_H - card_h - 16;
    int rows_end = y + 106 + ((n + SM_COLS - 1) / SM_COLS) * SM_CELL_H;
    if (card_y >= rows_end + 8) {
        int half = (w - 48) / 2;
        fb_blend_rounded_rect(x + 24, card_y, w - 48, card_h, 10, 0x00050818, 120);
        icon_draw(ICON_JARVIS, x + 36, card_y + 10, 20);
        fb_draw_text_bold(x + 64, card_y + 12, "JARVIS", T(TEXT));
        fb_draw_text(x + 64 + 56, card_y + 12, ui_shell_get_status(), T(ACCENT));

        status_row(x + 36, card_y + 42, "AI brain", g_kstate.ai_ready ? "Ready" : "Loading",
                   g_kstate.ai_ready ? T(OK) : T(WARN));
        status_row(x + 36 + half, card_y + 42, "Voice", g_kstate.speech_ready ? "Ready" : "Loading",
                   g_kstate.speech_ready ? T(OK) : T(WARN));
        status_row(x + 36, card_y + 66, "Security", ui_shell_get_security_label(),
                   ui_shell_get_security_alert() ? T(DANGER) : T(OK));
        net_info_t ni;
        net_get_info(&ni);
        status_row(x + 36 + half, card_y + 66, "Network", ni.configured ? "Connected" : ni.adapter ? "Not connected" : "No adapter",
                   ni.configured ? T(OK) : T(TEXT_DIM));
    }

    /* ---- Footer: user + power ---- */
    int fy = y + h - SM_FOOTER_H;
    fb_blend_rect(x + 14, fy, w - 28, 1, 0x00FFFFFF, 30);
    fb_fill_circle(x + 40, fy + 30, 16, T(ACCENT));
    glyph_draw(GLYPH_USER, x + 31, fy + 20, 18, T(ACCENT_TEXT), T(ACCENT));
    {
        user_info_t u;
        int have = user_current() >= 0 && user_info(user_current(), &u) == 0;
        fb_draw_text(x + 66, fy + 14, have ? u.name : "Live session", T(TEXT));
        fb_text(x + 66, fy + 32, have ? (u.admin ? "Administrator" : "Standard user") : "No account: nothing is kept",
                T(TEXT_DIM), FONT_SMALL);
    }

    /* sign out, lock, restart, shut down */
    static const glyph_id_t power_glyphs[4] = { GLYPH_PROFILE, GLYPH_PADLOCK, GLYPH_RESTART, GLYPH_POWER };
    for (int b = 0; b < 4; b++) {
        start_power_rect(b, x, y, w, h, &r);
        if (g_start.hover == SMH_SIGNOUT + b)
            fb_blend_rounded_rect(r.x, r.y, r.w, r.h, 8, 0x00FFFFFF, 34);
        glyph_draw(power_glyphs[b], r.x + 8, r.y + 8, 20, b == 0 && user_current() < 0 ? T(TEXT_DIM) : T(TEXT), 0);
    }
    static const char *const tips[4] = { "Sign out / switch user", "Lock", "Restart", "Shut down" };
    if (g_start.hover >= SMH_SIGNOUT && g_start.hover <= SMH_POWER) {
        start_power_rect(g_start.hover - SMH_SIGNOUT, x, y, w, h, &r);
        int tw = fb_text_width(tips[g_start.hover - SMH_SIGNOUT]) + 16;
        fb_fill_rounded_rect(r.x + r.w - tw, r.y - 28, tw, 22, 6, 0x000A0E20);
        fb_draw_text(r.x + r.w - tw + 8, r.y - 25, tips[g_start.hover - SMH_SIGNOUT], 0x00F2F5FF);
    }
}

/* Click while the Start menu is open. Returns 1 if it landed inside. */
int start_menu_click(int px, int py)
{
    if (!g_start.open) return 0;

    int x, y, w, h, m[TB_MAX_APPS];
    start_rect(&x, &y, &w, &h);
    if (!ui_hit(px, py, x, y, w, h))
        return 0;

    int hit = start_hit(px, py);
    if (hit >= 100 && hit < 200) {
        int n = start_matches(m, TB_MAX_APPS);
        int k = hit - 100;
        start_menu_close();
        if (k < n) desktop_app_launch(m[k]);
    } else if (hit == SMH_SIGNOUT) {
        start_menu_close();
        if (user_current() >= 0) desktop_sign_out();
    } else if (hit == SMH_LOCK) {
        start_menu_close();
        desktop_lock();
    } else if (hit == SMH_RESTART) {
        desktop_system_restart();
    } else if (hit == SMH_POWER) {
        desktop_system_shutdown();
    }
    return 1;
}

int start_menu_key(uint32_t key, int mods)
{
    (void)mods;
    if (!g_start.open) return 0;

    if (key == WM_KEY_ESC) {
        start_menu_close();
    } else if (key == WM_KEY_BACKSPACE) {
        if (g_start.qlen > 0)
            g_start.query[--g_start.qlen] = 0;
    } else if (key == WM_KEY_ENTER) {
        int m[TB_MAX_APPS];
        int n = start_matches(m, TB_MAX_APPS);
        char query[sizeof(g_start.query)];
        memcpy(query, g_start.query, sizeof(query));
        start_menu_close();
        if (n > 0)
            desktop_app_launch(m[0]);
        else if (query[0])
            desktop_ask_jarvis(query);
    }
    desktop_invalidate();
    return 1;                       /* the menu swallows all keys while open */
}

int start_menu_text(char ch)
{
    if (!g_start.open) return 0;
    if (ch >= 32 && ch < 127 && g_start.qlen < (int)sizeof(g_start.query) - 1) {
        g_start.query[g_start.qlen++] = ch;
        g_start.query[g_start.qlen] = 0;
        desktop_invalidate();
    }
    return 1;
}
