/*
 * =============================================================================
 * JARVIS OS — Taskbar and Start menu (desktop/taskbar.c)
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
 * TASKBAR
 * ========================================================================== */
#define TB_MAX_APPS 16

enum {
    TBH_NONE = 0, TBH_START, TBH_SEARCH, TBH_CHIP, TBH_CLOCK, TBH_BELL, TBH_STATUS,
    TBH_SHOWDESK, TBH_APP0 = 100,
};

static struct {
    rect_t start, search, apps[TB_MAX_APPS], chip, status, clock, bell, showdesk;
    int app_idx[TB_MAX_APPS];   /* taskbar slot -> app index */
    int app_count;
    int wide_search;
    int hover;
} g_tb;

static struct {
    int open;
    char query[40];
    int qlen;
    int hover;
} g_start;

/* ---- Pinned apps (setting "pins": names separated by commas) -------------- */
static const char *g_default_pins = "Files,Terminal,JARVIS,Settings,Notepad,Photos";

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
    int W = (int)fb_get_width();
    int y = taskbar_y();
    /* pinned apps first, then any other open app */
    int n = 0;
    for (int i = 0; i < desktop_app_builtin_count() && n < TB_MAX_APPS; i++)
        if (taskbar_is_pinned(i)) g_tb.app_idx[n++] = i;
    for (int i = 0; i < desktop_app_builtin_count() && n < TB_MAX_APPS; i++)
        if (!taskbar_is_pinned(i) && desktop_app_get(i)->window) g_tb.app_idx[n++] = i;

    g_tb.app_count = n;
    g_tb.wide_search = W >= 1200;
    int search_w = g_tb.wide_search ? 200 : 44;
    int total = 44 + 4 + search_w + 4 + n * 44;
    int x = setting_get_int("taskbar_align", 0) == 1 ? 8 : (W - total) / 2;

    g_tb.start = (rect_t){ x, y + 4, 44, 40 };            x += 48;
    g_tb.search = (rect_t){ x, y + 4, search_w, 40 };     x += search_w + 4;
    for (int i = 0; i < n; i++, x += 44)
        g_tb.apps[i] = (rect_t){ x, y + 4, 44, 40 };

    int rx = W - 6;
    g_tb.showdesk = (rect_t){ rx, y, 6, TASKBAR_H };
    g_tb.bell   = (rect_t){ rx - 36, y + 4, 36, 40 };     rx -= 38;
    g_tb.clock  = (rect_t){ rx - 96, y + 4, 96, 40 };     rx -= 98;
    g_tb.status = (rect_t){ rx - 52, y + 4, 52, 40 };     rx -= 54;
    g_tb.chip   = (rect_t){ rx - 92, y + 4, 92, 40 };
}

static int taskbar_hit(int x, int y)
{
    taskbar_layout();
    if (rect_hit(&g_tb.start, x, y))    return TBH_START;
    if (rect_hit(&g_tb.search, x, y))   return TBH_SEARCH;
    for (int i = 0; i < g_tb.app_count; i++)
        if (rect_hit(&g_tb.apps[i], x, y)) return TBH_APP0 + i;
    if (rect_hit(&g_tb.chip, x, y))     return TBH_CHIP;
    if (rect_hit(&g_tb.status, x, y))   return TBH_STATUS;
    if (rect_hit(&g_tb.clock, x, y))    return TBH_CLOCK;
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
    int h = taskbar_hit(x, y);
    if (h == g_tb.hover) return 0;
    g_tb.hover = h;
    return 1;
}

static void tb_hover_bg(const rect_t *r, int code)
{
    if (g_tb.hover == code)
        fb_fill_rounded_rect(r->x, r->y, r->w, r->h, 5, T(HOVER));
}

void taskbar_draw(void)
{
    int W = (int)fb_get_width();
    int y = taskbar_y();

    taskbar_layout();

    if (setting_get_int("transparency", 1) && theme_get_mode() != THEME_HIGH_CONTRAST)
        fb_blend_rect(0, y, W, TASKBAR_H, T(TASKBAR), 225);         /* see-through, like Windows */
    else
        fb_fill_rect(0, y, W, TASKBAR_H, T(TASKBAR));
    fb_fill_rect(0, y, W, 1, T(BORDER));

    /* ---- Start ---- */
    if (g_start.open)
        fb_fill_rounded_rect(g_tb.start.x, g_tb.start.y, 44, 40, 5, T(HOVER));
    tb_hover_bg(&g_tb.start, TBH_START);
    icon_draw(ICON_START, g_tb.start.x + 10, g_tb.start.y + 8, 24);

    /* ---- Search ---- */
    if (g_tb.wide_search) {
        rect_t *s = &g_tb.search;
        fb_fill_rounded_rect(s->x, s->y + 4, s->w, 32, 16,
                             g_tb.hover == TBH_SEARCH ? T(HOVER) : T(INPUT));
        glyph_draw(GLYPH_SEARCH, s->x + 12, s->y + 12, 16, T(TEXT_DIM), T(INPUT));
        fb_draw_text(s->x + 36, s->y + 12, "Ask JARVIS", T(TEXT_DIM));
    } else {
        tb_hover_bg(&g_tb.search, TBH_SEARCH);
        glyph_draw(GLYPH_SEARCH, g_tb.search.x + 12, g_tb.search.y + 10, 20,
                   T(TEXT), g_tb.hover == TBH_SEARCH ? T(HOVER) : T(TASKBAR));
    }

    /* ---- Apps ---- */
    for (int i = 0; i < g_tb.app_count; i++) {
        desktop_app_t *app = desktop_app_get(g_tb.app_idx[i]);
        rect_t *r = &g_tb.apps[i];
        int running = app->window != NULL;
        int focused = running && app->window == g_wm_ctx.focused;

        if (focused || g_tb.hover == TBH_APP0 + i)
            fb_fill_rounded_rect(r->x + 2, r->y, r->w - 4, r->h, 5, T(HOVER));
        icon_draw(app->icon, r->x + 10, r->y + 6, 24);

        if (focused)
            fb_fill_rounded_rect(r->x + 14, r->y + 35, 16, 3, 1, T(ACCENT));
        else if (running)
            fb_fill_rounded_rect(r->x + 19, r->y + 35, 6, 3, 1, T(TEXT_DIM));
    }

    /* ---- JARVIS status chip ---- */
    {
        rect_t *r = &g_tb.chip;
        uint32_t col = ui_shell_get_security_alert() ? T(DANGER) : T(ACCENT);
        char status[9];
        tb_hover_bg(r, TBH_CHIP);
        glyph_draw(GLYPH_MIC, r->x + 6, r->y + 12, 16, col,
                   g_tb.hover == TBH_CHIP ? T(HOVER) : T(TASKBAR));
        strncpy(status, ui_shell_get_status(), 8);
        status[8] = 0;
        fb_draw_text(r->x + 26, r->y + 12, status, col);
    }

    /* ---- Network + volume ---- */
    if (shell_flyout_is_open() == FLY_QUICK) fb_fill_rounded_rect(g_tb.status.x, g_tb.status.y, g_tb.status.w, g_tb.status.h, 5, T(HOVER));
    tb_hover_bg(&g_tb.status, TBH_STATUS);
    {
        uint32_t sbg = (shell_flyout_is_open() == FLY_QUICK || g_tb.hover == TBH_STATUS) ? T(HOVER) : T(TASKBAR);
        glyph_draw(setting_get_int("airplane", 0) ? GLYPH_PLANE : GLYPH_NET, g_tb.status.x + 6, g_tb.status.y + 12, 16,
                   net_connected() ? T(TEXT) : T(TEXT_DIM), sbg);
        glyph_draw(GLYPH_VOLUME, g_tb.status.x + 30, g_tb.status.y + 12, 16,
                   g_kstate.audio_ready && !setting_get_int("mute", 0) ? T(TEXT) : T(TEXT_DIM), sbg);
        if (setting_get_int("mute", 0)) fb_draw_line(g_tb.status.x + 30, g_tb.status.y + 28, g_tb.status.x + 46, g_tb.status.y + 12, T(DANGER));
    }

    /* ---- Clock ---- */
    {
        static const char *const months[12] =
            { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
        rect_t *r = &g_tb.clock;
        char clock[12], date[16];
        sys_time_t t;

        if (shell_flyout_is_open() == FLY_CALENDAR) fb_fill_rounded_rect(r->x, r->y, r->w, r->h, 5, T(HOVER));
        tb_hover_bg(r, TBH_CLOCK);
        sys_get_time(&t);
        sys_format_clock(clock, sizeof(clock));
        if (setting_get_int("clock_seconds", 0)) {
            if (sys_clock_24h()) snprintf(clock, sizeof(clock), "%02d:%02d:%02d", t.hour, t.minute, t.second);
            else snprintf(clock, sizeof(clock), "%d:%02d:%02d %s", t.hour % 12 ? t.hour % 12 : 12, t.minute, t.second, t.hour < 12 ? "AM" : "PM");
        }
        snprintf(date, sizeof(date), "%d %s %d", t.day, months[t.month - 1], t.year);
        fb_draw_text(r->x + r->w - 6 - fb_text_width(clock), r->y + 3, clock, T(TEXT));
        fb_draw_text(r->x + r->w - 6 - fb_text_width(date), r->y + 21, date, T(TEXT));
    }

    /* ---- Notification bell ---- */
    {
        rect_t *r = &g_tb.bell;
        int unread = notifications_get_unread_count();
        if (notifications_center_is_visible())
            fb_fill_rounded_rect(r->x, r->y, r->w, r->h, 5, T(HOVER));
        tb_hover_bg(r, TBH_BELL);
        glyph_draw(GLYPH_BELL, r->x + 9, r->y + 11, 18, T(TEXT), T(TASKBAR));
        if (unread > 0) {
            char num[4];
            snprintf(num, sizeof(num), "%d", unread > 9 ? 9 : unread);
            fb_fill_circle(r->x + 27, r->y + 11, 8, T(ACCENT));
            fb_draw_text(r->x + 23, r->y + 3, num, T(ACCENT_TEXT));
        }
    }

    /* ---- Show desktop sliver ---- */
    fb_fill_rect(g_tb.showdesk.x, y + 8, 1, TASKBAR_H - 16, T(BORDER));
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

/* Is (x,y) on the network / volume icons? (mouse wheel changes the volume) */
int taskbar_tray_hit(int x, int y)
{
    taskbar_layout();
    return rect_hit(&g_tb.status, x, y);
}

/* Right-click on the taskbar: per-app menu, or taskbar menu. */
int taskbar_handle_right_click(int x, int y)
{
    if (y < taskbar_y()) return 0;
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

/* Returns 1 if the click landed on the taskbar. */
int taskbar_handle_click(int x, int y)
{
    if (y < taskbar_y())
        return 0;

    int hit = taskbar_hit(x, y);

    if (hit != TBH_START && hit != TBH_SEARCH)
        start_menu_close();
    if (hit != TBH_BELL)
        notifications_close_center();
    if (hit != TBH_STATUS && hit != TBH_CLOCK)
        shell_flyout_close();

    if (hit == TBH_START) {
        taskbar_toggle_start_menu();
    } else if (hit == TBH_SEARCH) {
        if (!g_start.open) taskbar_toggle_start_menu();
    } else if (hit >= TBH_APP0) {
        desktop_app_activate(g_tb.app_idx[hit - TBH_APP0]);
    } else if (hit == TBH_STATUS) {
        shell_flyout_toggle(FLY_QUICK);
    } else if (hit == TBH_CHIP) {
        desktop_app_launch(desktop_app_find("JARVIS"));
    } else if (hit == TBH_CLOCK) {
        shell_flyout_toggle(FLY_CALENDAR);
    } else if (hit == TBH_BELL) {
        notifications_toggle_center();
    } else if (hit == TBH_SHOWDESK) {
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

enum { SMH_NONE = -1, SMH_LOCK = 200, SMH_RESTART, SMH_POWER };

static void start_rect(int *x, int *y, int *w, int *h)
{
    int H = (int)fb_get_height() - TASKBAR_H - 24;
    *w = SM_W;
    if (*w > (int)fb_get_width() - 24) *w = (int)fb_get_width() - 24;
    *h = H < 600 ? H : 600;
    *x = ((int)fb_get_width() - *w) / 2;
    *y = taskbar_y() - *h - 12;
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
    /* which: 0 = lock, 1 = restart, 2 = power (right-most) */
    r->w = 36; r->h = 36;
    r->x = sx + sw - 20 - 36 - (2 - which) * 42;
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
    for (int b = 0; b < 3; b++) {
        start_power_rect(b, x, y, w, h, &r);
        if (rect_hit(&r, px, py)) return SMH_LOCK + b;
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

    fb_blend_rounded_rect(x + 2, y + 6, w, h, 10, 0x00000000, 80);
    fb_fill_rounded_rect(x, y, w, h, 10, T(BORDER));
    fb_fill_rounded_rect(x + 1, y + 1, w - 2, h - 2, 9, T(MENU));

    /* ---- Search box ---- */
    fb_fill_rounded_rect(x + 24, y + 24, w - 48, 36, 18, T(INPUT));
    fb_fill_rect(x + 40, y + 59, w - 80, 1, T(ACCENT));
    glyph_draw(GLYPH_SEARCH, x + 38, y + 34, 16, T(TEXT_DIM), T(INPUT));
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
            fb_fill_rounded_rect(r.x + 2, r.y, r.w - 4, r.h, 6, T(HOVER));
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
        ui_card(x + 24, card_y, w - 48, card_h);
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
    fb_fill_rect(x + 1, fy, w - 2, 1, T(BORDER));
    fb_fill_circle(x + 40, fy + 30, 16, T(ACCENT));
    glyph_draw(GLYPH_USER, x + 31, fy + 20, 18, T(ACCENT_TEXT), T(ACCENT));
    fb_draw_text(x + 66, fy + 22, g_kstate.username[0] ? g_kstate.username : "User", T(TEXT));

    static const glyph_id_t power_glyphs[3] = { GLYPH_LOCK, GLYPH_RESTART, GLYPH_POWER };
    for (int b = 0; b < 3; b++) {
        start_power_rect(b, x, y, w, h, &r);
        uint32_t bg = (g_start.hover == SMH_LOCK + b) ? T(HOVER) : T(MENU);
        if (g_start.hover == SMH_LOCK + b)
            fb_fill_rounded_rect(r.x, r.y, r.w, r.h, 5, bg);
        glyph_draw(power_glyphs[b], r.x + 9, r.y + 9, 18, T(TEXT), bg);
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
