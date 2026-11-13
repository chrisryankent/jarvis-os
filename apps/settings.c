/*
 * =============================================================================
 * JARVIS OS — Settings (apps/settings.c)
 * Language: C17
 *
 * Personalization (theme), System (device info + power) and About.
 * Only controls that actually do something are shown.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "../wm/wm.h"
#include "../desktop/ui.h"
#include <string.h>
#include <stdio.h>

#define SET_SIDEBAR_W 190
#define SET_ITEM_H    36

typedef enum {
    SETTINGS_PERSONALIZE = 0,
    SETTINGS_SYSTEM,
    SETTINGS_ABOUT,
    SETTINGS_PAGES
} settings_page_t;

static struct {
    wm_window_t *window;
    settings_page_t page;
} g_settings;

static const struct { const char *label; icon_id_t icon; } g_set_pages[SETTINGS_PAGES] = {
    { "Personalization", ICON_GEAR },
    { "System",          ICON_PC },
    { "About",           ICON_JARVIS },
};

/* Theme swatch geometry, relative to the content area origin */
#define SW_W 132
#define SW_H 88
static void swatch_rect(int i, int *x, int *y)
{
    *x = 24 + i * (SW_W + 12);
    *y = 84;
}

static void info_row(int x, int y, const char *label, const char *value)
{
    fb_draw_text(x, y, label, T(TEXT_DIM));
    fb_draw_text(x + 150, y, value, T(TEXT));
}

static void draw_personalize(int x, int y, int w)
{
    static const struct { const char *name; uint32_t bg, bar, win; } previews[3] = {
        { "Dark",          0x000F4C81, 0x001C1C1C, 0x00202020 },
        { "Light",         0x009CCBF2, 0x00EEF2F6, 0x00FFFFFF },
        { "High contrast", 0x00000000, 0x00FFFF00, 0x00FFFFFF },
    };
    (void)w;

    fb_draw_text_scaled(x + 24, y + 16, "Personalization", T(TEXT), 2);
    fb_draw_text(x + 24, y + 60, "Choose a theme", T(TEXT_DIM));

    for (int i = 0; i < 3; i++) {
        int sx, sy;
        swatch_rect(i, &sx, &sy);
        sx += x; sy += y;
        int selected = (int)theme_get_mode() == i;

        fb_fill_rounded_rect(sx - 3, sy - 3, SW_W + 6, SW_H + 6, 8,
                             selected ? T(ACCENT) : T(BORDER));
        fb_fill_rounded_rect(sx, sy, SW_W, SW_H, 6, previews[i].bg);
        fb_fill_rounded_rect(sx + 14, sy + 12, 70, 44, 3, previews[i].win);
        fb_fill_rect(sx, sy + SW_H - 14, SW_W, 14, previews[i].bar);
        ui_text_center(sx, sy + SW_H + 10, SW_W, previews[i].name,
                       selected ? T(ACCENT) : T(TEXT));
    }

    char res[48];
    snprintf(res, sizeof(res), "%ux%u, 32-bit colour", fb_get_width(), fb_get_height());
    info_row(x + 24, y + 222, "Display", res);
    info_row(x + 24, y + 246, "Current theme", theme_get_name());
}

static void draw_system(int x, int y, int w)
{
    char buf[64];
    (void)w;

    fb_draw_text_scaled(x + 24, y + 16, "System", T(TEXT), 2);

    info_row(x + 24, y + 64, "Device name", "JARVIS-PC");
    info_row(x + 24, y + 88, "User", g_kstate.username[0] ? g_kstate.username : "User");
    sys_format_uptime(buf, sizeof(buf));
    info_row(x + 24, y + 112, "Uptime", buf);
    snprintf(buf, sizeof(buf), "%u MB", (unsigned)(g_kstate.model_pool_size >> 20));
    info_row(x + 24, y + 136, "AI model pool", buf);
    diskfs_stats_t disk;
    if (diskfs_get_stats(&disk) == 0)
        snprintf(buf, sizeof(buf), "%s: %u MB, %u KB used", disk.model,
                 (unsigned)(disk.total_bytes >> 20), (unsigned)(disk.used_bytes >> 10));
    else
        snprintf(buf, sizeof(buf), "No disk (files are not saved)");
    info_row(x + 24, y + 160, "Storage", buf);
    {
        net_info_t ni; char ip[16];
        net_get_info(&ni);
        net_format_ip(ni.ip, ip, sizeof(ip));
        snprintf(buf, sizeof(buf), "%s%s", !ni.adapter ? "No adapter" : !ni.link ? "Cable unplugged"
                 : !ni.configured ? "Waiting for an address" : "Connected, ", ni.configured ? ip : "");
        info_row(x + 24, y + 184, "Network", buf);
    }

    ui_button(x + 24,  y + 224, 110, 32, "Lock",      T(HOVER), T(TEXT));
    ui_button(x + 144, y + 224, 110, 32, "Restart",   T(HOVER), T(TEXT));
    ui_button(x + 264, y + 224, 110, 32, "Shut down", T(DANGER), 0x00FFFFFF);
}

static void draw_about(int x, int y, int w)
{
    (void)w;
    icon_draw(ICON_JARVIS, x + 24, y + 20, 48);
    fb_draw_text_scaled(x + 88, y + 20, "JARVIS OS", T(TEXT), 2);
    fb_draw_text(x + 88, y + 56, "Version 1.0  -  x86-64", T(TEXT_DIM));

    fb_draw_text(x + 24, y + 100, "A desktop operating system with a built-in assistant.", T(TEXT));
    info_row(x + 24, y + 136, "Kernel", "C17, higher-half, Multiboot2");
    info_row(x + 24, y + 160, "Graphics", "Double-buffered framebuffer");
    info_row(x + 24, y + 184, "Assistant", g_kstate.ai_ready ? "AI core online" : "AI core loading");
    info_row(x + 24, y + 208, "Voice", g_kstate.speech_ready ? "Speech pipeline online" : "Loading");
}

void settings_draw_callback(wm_window_t *win, uint32_t *framebuffer)
{
    (void)framebuffer;
    int x, y, w, h;
    wm_get_client(win, &x, &y, &w, &h);

    /* Sidebar */
    fb_fill_rect(x, y, SET_SIDEBAR_W, h, T(SURFACE));
    fb_fill_rect(x + SET_SIDEBAR_W - 1, y, 1, h, T(BORDER));
    for (int i = 0; i < SETTINGS_PAGES; i++) {
        int iy = y + 12 + i * (SET_ITEM_H + 4);
        if ((int)g_settings.page == i) {
            fb_fill_rounded_rect(x + 8, iy, SET_SIDEBAR_W - 17, SET_ITEM_H, 5, T(HOVER));
            fb_fill_rounded_rect(x + 8, iy + 10, 3, SET_ITEM_H - 20, 1, T(ACCENT));
        }
        icon_draw(g_set_pages[i].icon, x + 20, iy + 10, 16);
        fb_draw_text(x + 44, iy + 10, g_set_pages[i].label, T(TEXT));
    }

    int cx = x + SET_SIDEBAR_W, cw = w - SET_SIDEBAR_W;
    switch (g_settings.page) {
    case SETTINGS_PERSONALIZE: draw_personalize(cx, y, cw); break;
    case SETTINGS_SYSTEM:      draw_system(cx, y, cw);      break;
    default:                   draw_about(cx, y, cw);       break;
    }
}

void settings_event_callback(wm_window_t *win, wm_input_event_t *event)
{
    (void)win;
    if (event->type != IE_MOUSE_DOWN || event->data.mouse.button != 1)
        return;

    int mx = event->data.mouse.x, my = event->data.mouse.y;

    if (mx < SET_SIDEBAR_W) {
        int i = (my - 12) / (SET_ITEM_H + 4);
        if (my >= 12 && i >= 0 && i < SETTINGS_PAGES)
            g_settings.page = (settings_page_t)i;
        return;
    }

    mx -= SET_SIDEBAR_W;        /* content-relative */

    if (g_settings.page == SETTINGS_PERSONALIZE) {
        for (int i = 0; i < 3; i++) {
            int sx, sy;
            swatch_rect(i, &sx, &sy);
            if (ui_hit(mx, my, sx - 3, sy - 3, SW_W + 6, SW_H + 34))
                theme_set_mode((theme_mode_t)i);
        }
    } else if (g_settings.page == SETTINGS_SYSTEM) {
        if (ui_hit(mx, my, 24, 224, 110, 32))       desktop_lock();
        else if (ui_hit(mx, my, 144, 224, 110, 32)) desktop_system_restart();
        else if (ui_hit(mx, my, 264, 224, 110, 32)) desktop_system_shutdown();
    }
}

wm_window_t *settings_app_open(int x, int y, int w, int h)
{
    memset(&g_settings, 0, sizeof(g_settings));

    wm_window_t *win = wm_create_window("Settings", x, y, w, h, WS_DEFAULT,
                                        settings_draw_callback,
                                        settings_event_callback);
    if (!win) return NULL;

    win->icon = ICON_GEAR;
    win->min_w = 640;
    win->min_h = 320;
    g_settings.window = win;
    return win;
}
