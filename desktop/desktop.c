/*
 * =============================================================================
 * JARVIS OS — Desktop surface (desktop/desktop.c)
 * Language: C17
 *
 * Wallpaper, desktop icons and the right-click context menu.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "../wm/wm.h"
#include "ui.h"
#include <string.h>

/* ---- Wallpaper -----------------------------------------------------------
 * Rendered once into a cache and copied into the back buffer every frame;
 * regenerated only when the theme changes.
 */
static uint32_t *g_wallpaper = NULL;
static int g_wallpaper_valid = 0;

void desktop_wallpaper_invalidate(void)
{
    g_wallpaper_valid = 0;
}

static void wallpaper_render(void)
{
    int W = (int)fb_get_width();
    int H = (int)fb_get_height();

    fb_reset_clip();
    fb_gradient_v(0, 0, W, H, T(WALL_TOP), T(WALL_BOTTOM));

    if (theme_get_mode() != THEME_HIGH_CONTRAST) {
        /* Soft "bloom" rising from the bottom centre, plus a faint glow in
         * the top-right corner. */
        uint32_t glow = (theme_get_mode() == THEME_LIGHT) ? 0x00FFFFFF : T(ACCENT);
        int cx = W / 2, cy = H + H / 6;
        static const int radii_pct[4] = { 95, 72, 52, 34 };
        for (int i = 0; i < 4; i++) {
            int r = (H * radii_pct[i]) / 100;
            fb_blend_rounded_rect(cx - r, cy - r, 2 * r, 2 * r, r, glow, 16);
        }
        int r2 = H / 2;
        fb_blend_rounded_rect(W - r2 / 2, -r2, 2 * r2, 2 * r2, r2, glow, 10);
    }
}

static void wallpaper_draw(void)
{
    if (!g_wallpaper)
        g_wallpaper = (uint32_t *)kmalloc_large((size_t)fb_get_width() *
                                                fb_get_height() * 4);

    if (!g_wallpaper) {             /* no memory for a cache: flat gradient */
        fb_gradient_v(0, 0, (int)fb_get_width(), (int)fb_get_height(),
                      T(WALL_TOP), T(WALL_BOTTOM));
        return;
    }

    if (!g_wallpaper_valid) {
        wallpaper_render();
        fb_save_frame(g_wallpaper);
        g_wallpaper_valid = 1;
        return;
    }
    fb_load_frame(g_wallpaper);
}

/* ---- Desktop icons ------------------------------------------------------- */
#define DICON_W      84
#define DICON_H      84
#define DICON_GAP    6
#define DICON_MAX    12

typedef struct {
    char label[24];
    char target[64];        /* app name, or a path to open in Files */
    icon_id_t icon;
} desktop_icon_t;

static desktop_icon_t g_dicons[DICON_MAX];
static int g_dicon_count = 0;
static int g_dicon_selected = -1;
static int g_dicon_last_click = -1;
static uint32_t g_dicon_last_time = 0;

void desktop_add_icon(const char *label, const char *target, icon_id_t icon)
{
    if (g_dicon_count >= DICON_MAX) return;
    desktop_icon_t *d = &g_dicons[g_dicon_count++];
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

    desktop_add_icon("This PC",     "System",     ICON_PC);
    desktop_add_icon("Files",       "/jarvis",    ICON_FOLDER);
    desktop_add_icon("JARVIS",      "JARVIS",     ICON_JARVIS);
    desktop_add_icon("Terminal",    "Terminal",   ICON_TERMINAL);
    desktop_add_icon("Recycle Bin", "trash:",     ICON_TRASH);
}

static void dicon_rect(int i, int *x, int *y)
{
    int rows = ((int)fb_get_height() - 48 - 16) / (DICON_H + DICON_GAP);
    if (rows < 1) rows = 1;
    *x = 12 + (i / rows) * (DICON_W + DICON_GAP);
    *y = 12 + (i % rows) * (DICON_H + DICON_GAP);
}

void desktop_draw(void)
{
    wallpaper_draw();

    for (int i = 0; i < g_dicon_count; i++) {
        int x, y;
        dicon_rect(i, &x, &y);

        if (i == g_dicon_selected) {
            fb_blend_rounded_rect(x, y, DICON_W, DICON_H, 6, T(ACCENT), 60);
            fb_draw_rounded_rect(x, y, DICON_W, DICON_H, 6, T(ACCENT));
        }
        icon_draw(g_dicons[i].icon, x + (DICON_W - 40) / 2, y + 8, 40);

        /* Label with a 1px shadow so it stays readable on any wallpaper */
        int tw = fb_text_width(g_dicons[i].label);
        int tx = x + (DICON_W - tw) / 2;
        fb_draw_text(tx + 1, y + 57, g_dicons[i].label, 0x00000000);
        fb_draw_text(tx, y + 56, g_dicons[i].label, 0x00FFFFFF);
    }
}

/*
 * Returns the icon index on a completed double-click, -2 if an icon was
 * clicked once (selected), -1 if the click missed every icon.
 */
int desktop_handle_click(int x, int y)
{
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

/* ---- Context menu -------------------------------------------------------- */
#define CTX_ITEM_H   30
#define CTX_W        210

typedef struct {
    const char *label;
    int action;
} ctx_item_t;

enum {
    CTX_REFRESH = 1, CTX_OPEN_TERMINAL, CTX_OPEN_FILES, CTX_ASK_JARVIS,
    CTX_TOGGLE_THEME, CTX_SETTINGS, CTX_SHOW_DESKTOP,
};

static const ctx_item_t g_ctx_items[] = {
    { "Refresh",                CTX_REFRESH },
    { "Show desktop",           CTX_SHOW_DESKTOP },
    { "Open Terminal here",     CTX_OPEN_TERMINAL },
    { "Open Files",             CTX_OPEN_FILES },
    { "Ask JARVIS",             CTX_ASK_JARVIS },
    { "Switch light / dark",    CTX_TOGGLE_THEME },
    { "Personalize",            CTX_SETTINGS },
};
#define CTX_COUNT ((int)(sizeof(g_ctx_items) / sizeof(g_ctx_items[0])))

static int g_ctx_open = 0, g_ctx_x = 0, g_ctx_y = 0, g_ctx_hover = -1;

int desktop_context_is_open(void) { return g_ctx_open; }

void desktop_context_close(void)
{
    if (g_ctx_open) {
        g_ctx_open = 0;
        desktop_invalidate();
    }
}

void desktop_context_open(int x, int y)
{
    int h = CTX_COUNT * CTX_ITEM_H + 8;
    int W = (int)fb_get_width(), H = (int)fb_get_height() - 48;
    if (x + CTX_W > W) x = W - CTX_W;
    if (y + h > H) y = H - h;
    g_ctx_x = x; g_ctx_y = y;
    g_ctx_open = 1;
    g_ctx_hover = -1;
    desktop_invalidate();
}

static int ctx_item_at(int x, int y)
{
    if (!g_ctx_open) return -1;
    if (!ui_hit(x, y, g_ctx_x, g_ctx_y + 4, CTX_W, CTX_COUNT * CTX_ITEM_H)) return -1;
    return (y - g_ctx_y - 4) / CTX_ITEM_H;
}

/* Returns 1 if the hover highlight changed. */
int desktop_context_hover(int x, int y)
{
    int item = ctx_item_at(x, y);
    if (item == g_ctx_hover) return 0;
    g_ctx_hover = item;
    return 1;
}

void desktop_context_draw(void)
{
    if (!g_ctx_open) return;

    int h = CTX_COUNT * CTX_ITEM_H + 8;
    fb_blend_rounded_rect(g_ctx_x + 2, g_ctx_y + 4, CTX_W, h, 8, 0x00000000, 70);
    fb_fill_rounded_rect(g_ctx_x, g_ctx_y, CTX_W, h, 8, T(BORDER));
    fb_fill_rounded_rect(g_ctx_x + 1, g_ctx_y + 1, CTX_W - 2, h - 2, 7, T(MENU));

    for (int i = 0; i < CTX_COUNT; i++) {
        int iy = g_ctx_y + 4 + i * CTX_ITEM_H;
        if (i == g_ctx_hover)
            fb_fill_rounded_rect(g_ctx_x + 4, iy + 1, CTX_W - 8, CTX_ITEM_H - 2, 4, T(HOVER));
        fb_draw_text(g_ctx_x + 14, iy + 7, g_ctx_items[i].label, T(TEXT));
    }
}

/* Returns 1 if the click was consumed (menu was open). */
int desktop_context_click(int x, int y)
{
    if (!g_ctx_open) return 0;

    int item = ctx_item_at(x, y);
    g_ctx_open = 0;
    desktop_invalidate();
    if (item < 0) return 1;

    switch (g_ctx_items[item].action) {
    case CTX_REFRESH:
        desktop_wallpaper_invalidate();
        break;
    case CTX_SHOW_DESKTOP:
        wm_minimize_all();
        break;
    case CTX_OPEN_TERMINAL:
        desktop_app_launch(desktop_app_find("Terminal"));
        break;
    case CTX_OPEN_FILES:
        desktop_app_launch(desktop_app_find("Files"));
        break;
    case CTX_ASK_JARVIS:
        desktop_app_launch(desktop_app_find("JARVIS"));
        break;
    case CTX_TOGGLE_THEME:
        theme_set_mode(theme_get_mode() == THEME_DARK ? THEME_LIGHT : THEME_DARK);
        break;
    case CTX_SETTINGS:
        desktop_app_launch(desktop_app_find("Settings"));
        break;
    }
    return 1;
}
