/*
 * =============================================================================
 * JARVIS OS — Theme Manager (desktop/theme.c)
 * Language: C17
 *
 * One palette per mode. Everything on screen reads its colours through
 * theme_color(), so switching mode restyles the whole desktop: windows,
 * taskbar, Start menu, apps and wallpaper.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "ui.h"

static const uint32_t g_palettes[3][TC_COUNT] = {
    [THEME_DARK] = {
        [TC_ACCENT]         = 0x0060CDFF,
        [TC_ACCENT_TEXT]    = 0x00000000,
        [TC_WINDOW]         = 0x00202020,
        [TC_SURFACE]        = 0x002B2B2B,
        [TC_HOVER]          = 0x003A3A3A,
        [TC_TEXT]           = 0x00FFFFFF,
        [TC_TEXT_DIM]       = 0x00A6A6A6,
        [TC_BORDER]         = 0x00404040,
        [TC_TITLE_ACTIVE]   = 0x00202020,
        [TC_TITLE_INACTIVE] = 0x002B2B2B,
        [TC_TITLE_TEXT]     = 0x00FFFFFF,
        [TC_TASKBAR]        = 0x001C1C1C,
        [TC_MENU]           = 0x00242424,
        [TC_WALL_TOP]       = 0x00071426,
        [TC_WALL_BOTTOM]    = 0x000F4C81,
        [TC_INPUT]          = 0x00161616,
        [TC_OK]             = 0x006CCB5F,
        [TC_WARN]           = 0x00F2C14E,
        [TC_DANGER]         = 0x00FF6B6B,
    },
    [THEME_LIGHT] = {
        [TC_ACCENT]         = 0x00005FB8,
        [TC_ACCENT_TEXT]    = 0x00FFFFFF,
        [TC_WINDOW]         = 0x00F3F3F3,
        [TC_SURFACE]        = 0x00FFFFFF,
        [TC_HOVER]          = 0x00E2E2E2,
        [TC_TEXT]           = 0x001A1A1A,
        [TC_TEXT_DIM]       = 0x00606060,
        [TC_BORDER]         = 0x00C8C8C8,
        [TC_TITLE_ACTIVE]   = 0x00F3F3F3,
        [TC_TITLE_INACTIVE] = 0x00E9E9E9,
        [TC_TITLE_TEXT]     = 0x001A1A1A,
        [TC_TASKBAR]        = 0x00EEF2F6,
        [TC_MENU]           = 0x00F9F9F9,
        [TC_WALL_TOP]       = 0x004A9BE0,
        [TC_WALL_BOTTOM]    = 0x00CFE8FA,
        [TC_INPUT]          = 0x00FFFFFF,
        [TC_OK]             = 0x000F7B0F,
        [TC_WARN]           = 0x009D5D00,
        [TC_DANGER]         = 0x00C42B1C,
    },
    [THEME_HIGH_CONTRAST] = {
        [TC_ACCENT]         = 0x00FFFF00,
        [TC_ACCENT_TEXT]    = 0x00000000,
        [TC_WINDOW]         = 0x00000000,
        [TC_SURFACE]        = 0x00000000,
        [TC_HOVER]          = 0x00303030,
        [TC_TEXT]           = 0x00FFFFFF,
        [TC_TEXT_DIM]       = 0x00FFFF00,
        [TC_BORDER]         = 0x00FFFFFF,
        [TC_TITLE_ACTIVE]   = 0x00000000,
        [TC_TITLE_INACTIVE] = 0x00000000,
        [TC_TITLE_TEXT]     = 0x00FFFFFF,
        [TC_TASKBAR]        = 0x00000000,
        [TC_MENU]           = 0x00000000,
        [TC_WALL_TOP]       = 0x00000000,
        [TC_WALL_BOTTOM]    = 0x00000000,
        [TC_INPUT]          = 0x00000000,
        [TC_OK]             = 0x0000FF00,
        [TC_WARN]           = 0x00FFFF00,
        [TC_DANGER]         = 0x00FF4040,
    },
};

static theme_mode_t g_theme_mode = THEME_DARK;

/* desktop/desktop.c: the wallpaper is cached, so it must be rebuilt. */
void desktop_wallpaper_invalidate(void);

void theme_init(void)
{
    g_theme_mode = THEME_DARK;
}

void theme_set_mode(theme_mode_t mode)
{
    if (mode > THEME_HIGH_CONTRAST || mode == g_theme_mode)
        return;
    g_theme_mode = mode;
    desktop_wallpaper_invalidate();
    desktop_invalidate();
}

theme_mode_t theme_get_mode(void)
{
    return g_theme_mode;
}

const char *theme_get_name(void)
{
    static const char *const names[3] = { "Dark", "Light", "High Contrast" };
    return names[g_theme_mode];
}

/* Accent colour and wallpaper chosen in Settings > Personalization
 * (index 0 = the theme's own). High contrast keeps its fixed colours. */
static const uint32_t g_accents[] = { 0, 0x0060CDFF, 0x00E74856, 0x0016C60C, 0x00FFB900, 0x00B146C2, 0x00FF8C00 };
static const uint32_t g_walls[][2] = {
    { 0, 0 }, { 0x00200A30, 0x006B2D8F }, { 0x00052A1A, 0x001E7A4C }, { 0x00301008, 0x00C4501E },
    { 0x00101010, 0x00383838 }, { 0x00042030, 0x00087A8C },
};
static int g_accent_idx = 0, g_wall_idx = 0;

void theme_set_accent(int idx)
{
    if (idx < 0 || idx >= (int)(sizeof(g_accents) / sizeof(g_accents[0]))) idx = 0;
    g_accent_idx = idx;
    desktop_invalidate();
}

void theme_set_wallpaper(int idx)
{
    if (idx < 0 || idx >= (int)(sizeof(g_walls) / sizeof(g_walls[0]))) idx = 0;
    g_wall_idx = idx;
    desktop_wallpaper_invalidate();
    desktop_invalidate();
}

uint32_t theme_color(theme_color_t id)
{
    if ((unsigned)id >= TC_COUNT)
        id = TC_WINDOW;
    if (g_theme_mode != THEME_HIGH_CONTRAST) {
        if (g_accent_idx && id == TC_ACCENT) return g_accents[g_accent_idx];
        if (g_accent_idx && id == TC_ACCENT_TEXT) {
            uint32_t c = g_accents[g_accent_idx];
            int lum = (int)(((c >> 16) & 255) * 3 + ((c >> 8) & 255) * 6 + (c & 255)) / 10;
            return lum > 140 ? 0x00000000 : 0x00FFFFFF;
        }
        if (g_wall_idx && id == TC_WALL_TOP)    return g_walls[g_wall_idx][0];
        if (g_wall_idx && id == TC_WALL_BOTTOM) return g_walls[g_wall_idx][1];
    }
    return g_palettes[g_theme_mode][id];
}
