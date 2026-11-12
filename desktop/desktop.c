/*
 * =============================================================================
 * JARVIS OS — Desktop & Wallpaper (desktop/desktop.c)
 * Language: C17
 *
 * Desktop background rendering with gradient wallpaper.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "../drivers/gpu.h"
#include <string.h>

typedef struct {
    int icon_x, icon_y;
    char label[32];
    char path[128];
    uint32_t color;
} desktop_icon_t;

typedef struct {
    desktop_icon_t icons[16];
    int icon_count;
} desktop_context_t;

static desktop_context_t g_desktop = {};

void desktop_init(void)
{
    memset(&g_desktop, 0, sizeof(g_desktop));

    /* Add default desktop icons */
    strcpy(g_desktop.icons[0].label, "This PC");
    strcpy(g_desktop.icons[0].path, "/jarvis");
    g_desktop.icons[0].color = 0x0000D9FF;
    g_desktop.icons[0].icon_x = 40;
    g_desktop.icons[0].icon_y = 60;

    strcpy(g_desktop.icons[1].label, "Recycle Bin");
    strcpy(g_desktop.icons[1].path, "/jarvis/trash");
    g_desktop.icons[1].color = 0x00FF9800;
    g_desktop.icons[1].icon_x = 40;
    g_desktop.icons[1].icon_y = 160;

    g_desktop.icon_count = 2;
}

void desktop_draw_wallpaper(void)
{
    uint32_t W = fb_get_width();
    uint32_t H = fb_get_height();

    for (uint32_t y = 0; y < H; y++) {
        uint32_t t = (y * 255) / H;
        uint32_t r = (0x07 * (255 - t) + 0x16 * t) / 255;
        uint32_t g = (0x0D * (255 - t) + 0x2B * t) / 255;
        uint32_t b = (0x10 * (255 - t) + 0x2F * t) / 255;
        uint32_t color = (r << 16) | (g << 8) | b;
        fb_fill_rect(0, y, W, 1, color);
    }

    fb_blend_rect((int)W / 8, 0, 3, (int)H, 0x0046D3C6, 45);
    fb_blend_rect((int)W / 8 + 20, 0, 1, (int)H, 0x00F2C14E, 40);
    fb_blend_rect((int)W - (int)W / 5, 0, 2, (int)H, 0x006BE585, 35);

    for (uint32_t i = 0; i < W; i += 64)
        fb_blend_rect((int)i, (int)(H / 3), 1, (int)(H / 2), 0x00182328, 75);

    fb_blend_rect(0, (int)H - 190, (int)W, 126, 0x00000000, 70);
}

void desktop_draw_icon(int icon_idx)
{
    if (icon_idx >= g_desktop.icon_count) return;

    desktop_icon_t *icon = &g_desktop.icons[icon_idx];
    int x = icon->icon_x;
    int y = icon->icon_y;

    fb_fill_rounded_rect(x - 6, y - 6, 76, 92, 8, 0x0010191D);
    fb_fill_rounded_rect(x + 8, y + 4, 48, 48, 8, icon->color);
    fb_blend_rect(x + 14, y + 10, 36, 12, 0x00FFFFFF, 70);
    fb_fill_rect(x + 20, y + 20, 24, 20, 0x00FFFFFF);

    /* Icon label */
    fb_put_string(x - 2, y + 64, icon->label, 0x00FFFFFF, 0x0010191D);
}

void desktop_draw(void)
{
    /* Draw wallpaper background */
    desktop_draw_wallpaper();

    /* Draw desktop icons */
    for (int i = 0; i < g_desktop.icon_count; i++) {
        desktop_draw_icon(i);
    }
}

void desktop_add_icon(const char *label, const char *path, uint32_t color)
{
    if (g_desktop.icon_count < 16) {
        strncpy(g_desktop.icons[g_desktop.icon_count].label, label,
                sizeof(g_desktop.icons[0].label) - 1);
        strncpy(g_desktop.icons[g_desktop.icon_count].path, path,
                sizeof(g_desktop.icons[0].path) - 1);
        g_desktop.icons[g_desktop.icon_count].color = color;
        g_desktop.icons[g_desktop.icon_count].icon_x = 40 + (g_desktop.icon_count % 3) * 120;
        g_desktop.icons[g_desktop.icon_count].icon_y = 60 + (g_desktop.icon_count / 3) * 120;
        g_desktop.icon_count++;
    }
}

/* Double-click window: 400ms at the kernel's fixed 100 Hz tick rate. */
#define DESKTOP_DOUBLE_CLICK_TICKS 40

static int g_desktop_last_click_icon = -1;
static uint32_t g_desktop_last_click_time = 0;

/*
 * Hit-tests desktop icons. Returns the icon index on a confirmed
 * double-click (caller should launch/open whatever that icon represents —
 * see desktop_get_icon_path()/desktop_get_icon_label()), -2 if the click
 * landed on an icon but didn't complete a double-click (caller should
 * still treat this as "consumed", not as an empty-desktop click), or -1 if
 * the click didn't land on any icon at all.
 *
 * Kept as pure hit-testing with no calls into other modules deliberately:
 * desktop.c is included early in desktop_init.c's unity build, before
 * notifications.c/file_explorer.c, so it has no visibility into their real
 * types. Action dispatch happens in desktop_init.c instead, which (being
 * the outermost file) sees every other module's real definitions.
 */
int desktop_handle_click(int x, int y)
{
    for (int i = 0; i < g_desktop.icon_count; i++) {
        desktop_icon_t *icon = &g_desktop.icons[i];
        if (x < icon->icon_x || x >= icon->icon_x + 64) continue;
        if (y < icon->icon_y || y >= icon->icon_y + 64) continue;

        uint32_t now = (uint32_t)g_kstate.uptime_ticks;
        int is_double = (g_desktop_last_click_icon == i) &&
                         ((now - g_desktop_last_click_time) <= DESKTOP_DOUBLE_CLICK_TICKS);

        g_desktop_last_click_icon = i;
        g_desktop_last_click_time = now;

        return is_double ? i : -2;
    }
    return -1;
}

const char *desktop_get_icon_path(int icon_idx)
{
    if (icon_idx < 0 || icon_idx >= g_desktop.icon_count) return "";
    return g_desktop.icons[icon_idx].path;
}

const char *desktop_get_icon_label(int icon_idx)
{
    if (icon_idx < 0 || icon_idx >= g_desktop.icon_count) return "";
    return g_desktop.icons[icon_idx].label;
}
