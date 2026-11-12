/*
 * =============================================================================
 * JARVIS OS - Framebuffer UI Shell
 *
 * Boot/setup/lock overlays plus the live desktop HUD used by the kernel, speech,
 * AI, and input layers. This stays deliberately small and framebuffer-native so
 * the OS still runs before there is a userspace graphics stack.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "../drivers/gpu.h"
#include <string.h>
#include <stdio.h>

#define UI_MAX_LINES 8
#define UI_LINE_LEN  128

typedef enum {
    UI_MODE_LOADING = 0,
    UI_MODE_SETUP,
    UI_MODE_LOCK,
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
    int line_count;
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

static void ui_draw_panel(int x, int y, int w, int h, uint32_t fill, uint32_t edge)
{
    fb_fill_rounded_rect(x + 4, y + 6, w, h, 8, 0x00000000);
    fb_fill_rounded_rect(x, y, w, h, 8, fill);
    fb_fill_rect(x + 8, y, w - 16, 1, edge);
    fb_fill_rect(x + 8, y + h - 1, w - 16, 1, 0x0018242B);
}

static void ui_draw_bar(int x, int y, int w, int h, int percent,
                        uint32_t track, uint32_t fill)
{
    int filled = (w * ui_clamp(percent, 0, 100)) / 100;
    fb_fill_rounded_rect(x, y, w, h, h / 2, track);
    if (filled > 0)
        fb_fill_rounded_rect(x, y, filled, h, h / 2, fill);
}

static void ui_draw_cursor(void)
{
    int x = g_ui.cursor_x;
    int y = g_ui.cursor_y;

    fb_fill_rect(x, y, 2, 20, 0x00FFFFFF);
    fb_fill_rect(x + 2, y + 2, 2, 18, 0x00FFFFFF);
    fb_fill_rect(x + 4, y + 4, 2, 16, 0x00FFFFFF);
    fb_fill_rect(x + 6, y + 6, 2, 14, 0x00FFFFFF);
    fb_fill_rect(x + 8, y + 10, 8, 2, 0x00FFFFFF);
    fb_fill_rect(x + 10, y + 12, 2, 8, 0x00FFFFFF);
    fb_fill_rect(x + 12, y + 18, 6, 2, 0x00FFFFFF);
    fb_fill_rect(x + 2, y + 20, 8, 2, 0x000E1116);
}

static void ui_draw_full_background(uint32_t top, uint32_t bottom)
{
    uint32_t W = fb_get_width();
    uint32_t H = fb_get_height();

    if (W == 0 || H == 0)
        return;

    for (uint32_t y = 0; y < H; y++) {
        uint32_t t = (y * 255) / H;
        uint32_t r = ((((top >> 16) & 0xFF) * (255 - t)) +
                      (((bottom >> 16) & 0xFF) * t)) / 255;
        uint32_t g = ((((top >> 8) & 0xFF) * (255 - t)) +
                      (((bottom >> 8) & 0xFF) * t)) / 255;
        uint32_t b = (((top & 0xFF) * (255 - t)) +
                      ((bottom & 0xFF) * t)) / 255;
        fb_fill_rect(0, (int)y, (int)W, 1, (r << 16) | (g << 8) | b);
    }

    for (uint32_t i = 0; i < W; i += 48) {
        uint32_t shade = 0x00162226 + ((i / 48) % 4) * 0x00030503;
        fb_blend_rect((int)i, 0, 2, (int)H, shade, 70);
    }
}

static void ui_draw_loading(void)
{
    uint32_t W = fb_get_width();
    uint32_t H = fb_get_height();
    int panel_w = 540;
    int panel_h = 210;
    int x = ((int)W - panel_w) / 2;
    int y = ((int)H - panel_h) / 2;
    int progress = 18 + (int)((g_kstate.uptime_ticks / 4) % 76);

    ui_draw_full_background(0x00070D10, 0x001A2930);
    ui_draw_panel(x, y, panel_w, panel_h, 0x0010171B, 0x0046D3C6);

    fb_put_string(x + 28, y + 28, "JARVIS OS", 0x00FFFFFF, 0x0010171B);
    fb_put_string(x + 28, y + 54, "Native framebuffer desktop is starting",
                  0x009CB6B4, 0x0010171B);
    fb_put_string(x + 28, y + 92, "Kernel  Memory  Devices  Desktop",
                  0x006CE5D2, 0x0010171B);
    ui_draw_bar(x + 28, y + 128, panel_w - 56, 12, progress,
                0x00203238, 0x0046D3C6);
    fb_put_string(x + 28, y + 158, g_ui.status, 0x00FFFFFF, 0x0010171B);
}

static void ui_draw_setup(void)
{
    uint32_t W = fb_get_width();
    uint32_t H = fb_get_height();
    int panel_w = 620;
    int panel_h = 260;
    int x = ((int)W - panel_w) / 2;
    int y = ((int)H - panel_h) / 2;

    ui_draw_full_background(0x000A1114, 0x001D3034);
    ui_draw_panel(x, y, panel_w, panel_h, 0x0011191E, 0x0046D3C6);
    fb_put_string(x + 28, y + 26, "Welcome to JARVIS OS",
                  0x00FFFFFF, 0x0011191E);
    fb_put_string(x + 28, y + 62, g_ui.setup_title,
                  0x0046D3C6, 0x0011191E);
    fb_put_string(x + 28, y + 92, g_ui.setup_detail,
                  0x00B7C8C4, 0x0011191E);
    ui_draw_bar(x + 28, y + 150, panel_w - 56, 10, 74,
                0x0026343A, 0x00F2C14E);
    fb_put_string(x + 28, y + 184, "Profile ready  Security optional  Voice hooks armed",
                  0x009CB6B4, 0x0011191E);
}

static void ui_draw_lock(void)
{
    uint32_t W = fb_get_width();
    uint32_t H = fb_get_height();
    int panel_w = 460;
    int panel_h = 230;
    int x = ((int)W - panel_w) / 2;
    int y = ((int)H - panel_h) / 2;

    ui_draw_full_background(0x00080C10, 0x00202A3A);
    ui_draw_panel(x, y, panel_w, panel_h, 0x0010151C, 0x00F25F5C);
    fb_put_string(x + 28, y + 30, "Session Locked", 0x00FFFFFF, 0x0010151C);
    fb_put_string(x + 28, y + 66, "Voice verification is waiting.",
                  0x00B7C8C4, 0x0010151C);
    ui_draw_bar(x + 28, y + 116, panel_w - 56, 10, g_ui.mic_level,
                0x0026343A, 0x00F25F5C);
    fb_put_string(x + 28, y + 154, "Microphone level", 0x009CB6B4, 0x0010151C);
}

static void ui_draw_desktop_overlay(void)
{
    uint32_t W = fb_get_width();
    uint32_t H = fb_get_height();
    int right_w = 318;
    int x = (int)W - right_w - 18;
    int y = 18;

    if (W < 640 || H < 480)
        return;

    ui_draw_panel(x, y, right_w, 118, 0x0010191D, 0x0046D3C6);
    fb_put_string(x + 18, y + 14, "JARVIS", 0x00FFFFFF, 0x0010191D);
    fb_put_string(x + 18, y + 36, g_ui.status, 0x0046D3C6, 0x0010191D);

    fb_put_string(x + 18, y + 66, "SEC", 0x009CB6B4, 0x0010191D);
    fb_put_string(x + 52, y + 66, g_ui.security_label,
                  g_ui.security_alert ? 0x00F25F5C : 0x006BE585, 0x0010191D);
    fb_put_string(x + 142, y + 66, "MIC", 0x009CB6B4, 0x0010191D);
    ui_draw_bar(x + 176, y + 70, 108, 8, g_ui.mic_level,
                0x0026343A, 0x0046D3C6);

    int deck_h = 132;
    int deck_y = (int)H - 64 - deck_h - 14;
    ui_draw_panel(18, deck_y, right_w + 150, deck_h, 0x0010191D, 0x002F454A);
    fb_put_string(36, deck_y + 14, "AI Console", 0x00FFFFFF, 0x0010191D);

    int first = g_ui.line_count > UI_MAX_LINES ? g_ui.line_count - UI_MAX_LINES : 0;
    int draw_y = deck_y + 40;
    for (int i = first; i < g_ui.line_count; i++) {
        const char *line = g_ui.lines[i % UI_MAX_LINES];
        fb_put_string(36, draw_y, line, 0x00B7C8C4, 0x0010191D);
        draw_y += 16;
        if (draw_y > deck_y + deck_h - 18)
            break;
    }

    ui_draw_cursor();
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

void ui_shell_init(void)
{
    memset(&g_ui, 0, sizeof(g_ui));
    g_ui.mode = UI_MODE_LOADING;
    ui_copy(g_ui.status, sizeof(g_ui.status), "BOOTING");
    ui_copy(g_ui.security_label, sizeof(g_ui.security_label), "SAFE");
    ui_shell_ensure_init();
}

void ui_shell_draw_frame(void)
{
    if (!fb_get_base())
        return;

    ui_shell_ensure_init();

    switch (g_ui.mode) {
    case UI_MODE_LOADING:
        ui_draw_loading();
        break;
    case UI_MODE_SETUP:
        ui_draw_setup();
        ui_draw_cursor();
        break;
    case UI_MODE_LOCK:
        ui_draw_lock();
        ui_draw_cursor();
        break;
    case UI_MODE_DESKTOP:
    default:
        ui_draw_desktop_overlay();
        break;
    }
}

void ui_shell_display(const char *text)
{
    if (!text)
        text = "";

    ui_copy(g_ui.lines[g_ui.line_count % UI_MAX_LINES], UI_LINE_LEN, text);
    g_ui.line_count++;
    ui_shell_draw_frame();
}

void ui_shell_show_loading_desktop(void)
{
    ui_shell_ensure_init();
    g_ui.mode = UI_MODE_LOADING;
    ui_copy(g_ui.status, sizeof(g_ui.status), "LOADING DESKTOP");
    ui_shell_draw_frame();
}

void ui_shell_show_lock_screen(void)
{
    ui_shell_ensure_init();
    g_ui.mode = UI_MODE_LOCK;
    ui_copy(g_ui.status, sizeof(g_ui.status), "LOCKED");
    ui_shell_draw_frame();
}

void ui_shell_show_setup_wizard(void)
{
    ui_shell_ensure_init();
    g_ui.mode = UI_MODE_SETUP;
    ui_copy(g_ui.status, sizeof(g_ui.status), "SETUP");
    ui_copy(g_ui.setup_title, sizeof(g_ui.setup_title), "Preparing your profile");
    ui_copy(g_ui.setup_detail, sizeof(g_ui.setup_detail),
            "The desktop will open automatically after first-boot setup.");
    ui_shell_draw_frame();
}

void ui_shell_unlock_desktop(void)
{
    ui_shell_ensure_init();
    g_ui.mode = UI_MODE_DESKTOP;
    ui_copy(g_ui.status, sizeof(g_ui.status), "READY");
    ui_shell_display("Desktop ready. Say or type help for commands.");
}

void ui_shell_set_setup_step(const char *title, const char *detail)
{
    ui_copy(g_ui.setup_title, sizeof(g_ui.setup_title), title);
    ui_copy(g_ui.setup_detail, sizeof(g_ui.setup_detail), detail);
    ui_shell_draw_frame();
}

void ui_shell_set_status(const char *status)
{
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
    g_ui.mic_level = ui_clamp(level, 0, 100);
    ui_shell_draw_frame();
}

void ui_shell_add_transcript(const char *text)
{
    char line[UI_LINE_LEN];
    snprintf(line, sizeof(line), "Voice: %s", text ? text : "");
    ui_shell_display(line);
}

void ui_shell_move_cursor(int dx, int dy)
{
    uint32_t W = fb_get_width();
    uint32_t H = fb_get_height();

    ui_shell_ensure_init();
    if (W == 0 || H == 0)
        return;

    g_ui.cursor_x = ui_clamp(g_ui.cursor_x + dx, 0, (int)W - 20);
    g_ui.cursor_y = ui_clamp(g_ui.cursor_y + dy, 0, (int)H - 20);

    if (desktop_system_is_initialized())
        desktop_system_handle_mouse_move(g_ui.cursor_x, g_ui.cursor_y);
}

void ui_shell_click_cursor(void)
{
    if (!desktop_system_is_initialized())
        return;

    desktop_system_handle_mouse_click(g_ui.cursor_x, g_ui.cursor_y, 1);
    desktop_system_handle_mouse_release(g_ui.cursor_x, g_ui.cursor_y, 1);
}
