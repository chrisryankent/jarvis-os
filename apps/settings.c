/*
 * =============================================================================
 * JARVIS OS — Settings Application (apps/settings.c)
 * Language: C17
 *
 * System settings for display, sound, themes, and language.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "../wm/wm.h"
#include "../drivers/gpu.h"
#include <string.h>
#include <stdio.h>

typedef enum {
    SETTINGS_DISPLAY = 0,
    SETTINGS_SOUND = 1,
    SETTINGS_SYSTEM = 2,
    SETTINGS_ABOUT = 3,
} settings_category_t;

typedef struct {
    wm_window_t *window;
    settings_category_t current_category;
    int selected_item;
    theme_mode_t selected_theme;
} settings_context_t;

static settings_context_t g_settings = {};

void settings_draw_sidebar(int x, int y, int w)
{
    /* Left sidebar with categories */
    fb_fill_rect(x, y, w, 200, 0x00282828);

    const char *categories[] = {
        "Display",
        "Sound",
        "System",
        "About"
    };

    for (int i = 0; i < 4; i++) {
        int item_y = y + (i * 50);
        uint32_t bg = (g_settings.current_category == (settings_category_t)i) ? 0x005BA3D6 : 0x00282828;
        uint32_t fg = (g_settings.current_category == (settings_category_t)i) ? 0x00FFFFFF : 0x00A0A0A0;

        fb_fill_rect(x, item_y, w, 50, bg);
        fb_put_string(x + 12, item_y + 16, categories[i], fg, bg);
    }
}

void settings_draw_display_panel(int x, int y, int w, int h)
{
    fb_fill_rect(x, y, w, h, 0x001F1F1F);

    int content_x = x + 24;
    int content_y = y + 24;

    /* Brightness control */
    fb_put_string(content_x, content_y, "Brightness", 0x00E0E0E0, 0x001F1F1F);
    fb_fill_rect(content_x + 150, content_y + 2, 200, 16, 0x003A3A3A);
    fb_fill_rect(content_x + 150, content_y + 2, 160, 16, 0x0000D9FF);
    fb_put_string(content_x + 180, content_y + 24, "80%", 0x00808080, 0x001F1F1F);

    content_y += 60;

    /* Theme selector */
    fb_put_string(content_x, content_y, "Theme", 0x00E0E0E0, 0x001F1F1F);

    const char *themes[] = {"Dark", "Light", "High Contrast"};

    for (int i = 0; i < 3; i++) {
        int btn_x = content_x + (i * 140);
        uint32_t bg = (g_settings.selected_theme == (theme_mode_t)i) ? 0x005BA3D6 : 0x003A3A3A;
        fb_fill_rect(btn_x, content_y + 24, 130, 40, bg);
        fb_put_string(btn_x + 36, content_y + 32, themes[i],
                     (g_settings.selected_theme == (theme_mode_t)i) ? 0x00FFFFFF : 0x00A0A0A0, bg);
    }

    content_y += 80;

    /* Resolution */
    fb_put_string(content_x, content_y, "Resolution", 0x00E0E0E0, 0x001F1F1F);
    char res_str[64];
    snprintf(res_str, sizeof(res_str), "%ux%u @ 60 Hz",
            fb_get_width(), fb_get_height());
    fb_put_string(content_x + 150, content_y, res_str, 0x00A0A0A0, 0x001F1F1F);
}

void settings_draw_sound_panel(int x, int y, int w, int h)
{
    fb_fill_rect(x, y, w, h, 0x001F1F1F);

    int content_x = x + 24;
    int content_y = y + 24;

    /* Master volume */
    fb_put_string(content_x, content_y, "Master Volume", 0x00E0E0E0, 0x001F1F1F);
    fb_fill_rect(content_x + 150, content_y + 2, 200, 16, 0x003A3A3A);
    fb_fill_rect(content_x + 150, content_y + 2, 140, 16, 0x0000FF00);
    fb_put_string(content_x + 180, content_y + 24, "70%", 0x00808080, 0x001F1F1F);

    content_y += 60;

    /* Microphone volume */
    fb_put_string(content_x, content_y, "Microphone", 0x00E0E0E0, 0x001F1F1F);
    fb_fill_rect(content_x + 150, content_y + 2, 200, 16, 0x003A3A3A);
    fb_fill_rect(content_x + 150, content_y + 2, 120, 16, 0x0000FF00);
    fb_put_string(content_x + 180, content_y + 24, "60%", 0x00808080, 0x001F1F1F);

    content_y += 60;

    /* Audio output device */
    fb_put_string(content_x, content_y, "Output Device", 0x00E0E0E0, 0x001F1F1F);
    fb_fill_rect(content_x + 150, content_y, 200, 32, 0x003A3A3A);
    fb_put_string(content_x + 160, content_y + 8, "Built-in Speaker", 0x00A0A0A0, 0x003A3A3A);
}

void settings_draw_system_panel(int x, int y, int w, int h)
{
    fb_fill_rect(x, y, w, h, 0x001F1F1F);

    int content_x = x + 24;
    int content_y = y + 24;

    /* System info */
    fb_put_string(content_x, content_y, "Device", 0x00E0E0E0, 0x001F1F1F);
    fb_put_string(content_x, content_y + 24, "JARVIS AI OS", 0x00A0A0A0, 0x001F1F1F);

    content_y += 60;

    fb_put_string(content_x, content_y, "Uptime", 0x00E0E0E0, 0x001F1F1F);
    char uptime_str[64];
    uint64_t seconds = g_kstate.uptime_ticks / 100;
    uint64_t hours = seconds / 3600;
    uint64_t mins = (seconds % 3600) / 60;
    snprintf(uptime_str, sizeof(uptime_str), "%lu h %lu m", hours, mins);
    fb_put_string(content_x, content_y + 24, uptime_str, 0x00A0A0A0, 0x001F1F1F);

    content_y += 60;

    /* Security status */
    fb_put_string(content_x, content_y, "Security", 0x00E0E0E0, 0x001F1F1F);
    fb_put_string(content_x + 150, content_y, "SAFE", 0x0000E676, 0x001F1F1F);

    content_y += 40;

    /* Restart button */
    fb_fill_rect(content_x, content_y, 140, 36, 0x005BA3D6);
    fb_put_string(content_x + 32, content_y + 10, "Restart", 0x00FFFFFF, 0x005BA3D6);
}

void settings_draw_about_panel(int x, int y, int w, int h)
{
    fb_fill_rect(x, y, w, h, 0x001F1F1F);

    int content_x = x + 24;
    int content_y = y + 48;

    /* Logo / Title */
    fb_put_string(content_x, content_y, "JARVIS AI OS", 0x0000D9FF, 0x001F1F1F);
    fb_put_string(content_x, content_y + 28, "Windows 11-style Desktop Environment", 0x00A0A0A0, 0x001F1F1F);

    content_y += 80;

    /* Version info */
    fb_put_string(content_x, content_y, "Version 1.0", 0x00E0E0E0, 0x001F1F1F);
    fb_put_string(content_x, content_y + 24, "Built on C17 kernel with AI subsystems", 0x00808080, 0x001F1F1F);

    content_y += 60;

    /* Credits */
    fb_put_string(content_x, content_y, "Architecture:", 0x00E0E0E0, 0x001F1F1F);
    fb_put_string(content_x, content_y + 20, "• GPU: UEFI GOP framebuffer", 0x00A0A0A0, 0x001F1F1F);
    fb_put_string(content_x, content_y + 36, "• WM: Window manager with tiling support", 0x00A0A0A0, 0x001F1F1F);
    fb_put_string(content_x, content_y + 52, "• AI: Integrated LLM + speech pipeline", 0x00A0A0A0, 0x001F1F1F);
}

void settings_draw_callback(wm_window_t *win, uint32_t *framebuffer)
{
    if (!win) return;

    /* Window background */
    fb_fill_rect(win->x, win->y + 32, win->w, win->h - 32, 0x001F1F1F);

    int left_x = win->x + 8;
    int left_y = win->y + 48;
    int left_w = 140;

    int content_x = left_x + left_w + 16;
    int content_y = left_y;
    int content_w = win->w - left_w - 32;
    int content_h = win->h - 80;

    /* Draw sidebar */
    settings_draw_sidebar(left_x, left_y, left_w);

    /* Draw content based on category */
    switch (g_settings.current_category) {
    case SETTINGS_DISPLAY:
        settings_draw_display_panel(content_x, content_y, content_w, content_h);
        break;
    case SETTINGS_SOUND:
        settings_draw_sound_panel(content_x, content_y, content_w, content_h);
        break;
    case SETTINGS_SYSTEM:
        settings_draw_system_panel(content_x, content_y, content_w, content_h);
        break;
    case SETTINGS_ABOUT:
        settings_draw_about_panel(content_x, content_y, content_w, content_h);
        break;
    }
}

void settings_event_callback(wm_window_t *win, wm_input_event_t *event)
{
    if (!event || !win) return;

    switch (event->type) {
    case IE_MOUSE_DOWN:
        {
            int mx = event->data.mouse.x;
            int my = event->data.mouse.y;

            /* Sidebar: x in [win->x+8, win->x+148], y in [win->y+48, win->y+248] */
            int sidebar_x0 = win->x + 8;
            int sidebar_x1 = win->x + 148;
            int sidebar_y0 = win->y + 48;
            if (mx >= sidebar_x0 && mx < sidebar_x1 && my >= sidebar_y0) {
                int item = (my - sidebar_y0) / 50;
                if (item >= 0 && item < 4)
                    g_settings.current_category = (settings_category_t)item;
                break;
            }

            /* Theme buttons in Display panel.
             * content_x passed to draw panel = win->x + 164 (left_x+left_w+16)
             * inner content_x inside draw fn = win->x + 164 + 24 = win->x + 188
             * theme button row y:
             *   content_y = (win->y+48) + 24 = win->y+72
             *   after += 60 → win->y+132
             *   buttons drawn at content_y+24 = win->y+156, height 40 */
            if (g_settings.current_category == SETTINGS_DISPLAY) {
                int btn_x0 = win->x + 188;
                int btn_y0 = win->y + 156;
                int btn_h  = 40;
                if (my >= btn_y0 && my < btn_y0 + btn_h && mx >= btn_x0) {
                    int idx = (mx - btn_x0) / 140;
                    if (idx >= 0 && idx < 3) {
                        g_settings.selected_theme = (theme_mode_t)idx;
                        theme_set_mode((theme_mode_t)idx);
                    }
                }
            }
        }
        break;

    default:
        break;
    }
}

wm_window_t *settings_app_open(int x, int y, int w, int h)
{
    memset(&g_settings, 0, sizeof(g_settings));

    wm_window_t *win = wm_create_window(
        "Settings",
        x, y, w, h,
        WS_DEFAULT,
        settings_draw_callback,
        settings_event_callback
    );

    g_settings.window = win;
    g_settings.current_category = SETTINGS_DISPLAY;
    g_settings.selected_theme = THEME_DARK;

    return win;
}
