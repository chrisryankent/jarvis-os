/*
 * =============================================================================
 * JARVIS OS — UI shell state + full-screen screens (desktop/ui_shell.c)
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
    fb_gradient_v(0, 0, (int)fb_get_width(), (int)fb_get_height(),
                  T(WALL_TOP), T(WALL_BOTTOM));
}

static void draw_loading(void)
{
    int W = (int)fb_get_width(), H = (int)fb_get_height();
    int cx = W / 2, cy = H / 2;

    draw_backdrop();
    icon_draw(ICON_JARVIS, cx - 48, cy - 130, 96);
    fb_draw_text_scaled(cx - fb_text_width("JARVIS OS"), cy - 10, "JARVIS OS", 0x00FFFFFF, 2);

    int progress = 15 + (g_ui.line_count * 12) % 80;
    ui_progress(cx - 120, cy + 50, 240, 6, progress, 0x00203040, T(ACCENT));
    ui_text_center(0, cy + 72, W,
                   g_ui.mode == UI_MODE_SETUP ? g_ui.setup_title : g_ui.status,
                   0x00C0D0E0);
}

#include "login.c"

void ui_shell_render_fullscreen(void)
{
    if (!fb_get_base())
        return;
    ui_shell_ensure_init();

    switch (g_ui.mode) {
    case UI_MODE_LOADING: draw_loading(); break;
    case UI_MODE_SETUP:   login_draw_setup(); break;
    case UI_MODE_LOCK:    login_draw_lock();  break;
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

void ui_shell_unlock_desktop(void)
{
    ui_shell_ensure_init();
    g_ui.mode = UI_MODE_DESKTOP;
    ui_copy(g_ui.status, sizeof(g_ui.status), "READY");
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
    desktop_invalidate();
}

void ui_shell_login_click(int x, int y)
{
    if (g_ui.mode == UI_MODE_LOCK)       login_lock_click(x, y);
    else if (g_ui.mode == UI_MODE_SETUP) login_setup_click(x, y);
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
