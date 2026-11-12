/*
 * =============================================================================
 * JARVIS OS — Desktop System Initialization (desktop/desktop_init.c)
 * Language: C17
 *
 * Main desktop initialization and main loop integration.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "../wm/wm.h"
#include "../input/input_events.c"
#include "../desktop/theme.c"
#include "../desktop/taskbar.c"
#include "../desktop/desktop.c"
#include "../desktop/notifications.c"
#include "../wm/animations.c"
#include "../apps/file_explorer.c"
#include "../apps/terminal.c"
#include "../apps/settings.c"
#include "../apps/calculator.c"

/* ---- Desktop State ------------------------------------------------------- */
typedef struct {
    int initialized;
    uint32_t frame_count;
    wm_window_t *file_explorer;
    wm_window_t *terminal;
    wm_window_t *settings;
    wm_window_t *calculator;
} desktop_state_t;

static desktop_state_t g_desktop_state = {};

/* Registered as each default app's wm_close_callback_t so that clicking the
 * titlebar X actually clears the tracked pointer in g_desktop_state.
 * Without this, wm_close_window() would free the wm_window_t while
 * g_desktop_state.file_explorer/terminal/settings/calculator still held the
 * now-dangling pointer — desktop_system_open_app()'s `if (!g_desktop_state.X)`
 * guard would then think the app was still open forever, and the user could
 * never relaunch it after closing it once. */
static void desktop_clear_window_ptr(void *ctx)
{
    wm_window_t **slot = (wm_window_t **)ctx;
    if (*slot)
        taskbar_remove_window(*slot);
    *slot = NULL;
}

static void desktop_launch_file_explorer(void)
{
    if (g_desktop_state.file_explorer) {
        wm_set_focused(g_desktop_state.file_explorer);
        return;
    }
    uint32_t sw = fb_get_width();
    uint32_t sh = fb_get_height();
    int w = sw >= 1200 ? 700 : (int)sw - 420;
    int h = sh >= 760 ? 500 : (int)sh - 220;
    if (w < 460) w = 460;
    if (h < 360) h = 360;

    g_desktop_state.file_explorer = file_explorer_open(28, 58, w, h);
    wm_set_close_callback(g_desktop_state.file_explorer, desktop_clear_window_ptr,
                           &g_desktop_state.file_explorer);
    taskbar_add_window(g_desktop_state.file_explorer, "Files");
}

static void desktop_launch_terminal(void)
{
    if (g_desktop_state.terminal) {
        wm_set_focused(g_desktop_state.terminal);
        return;
    }
    uint32_t sw = fb_get_width();
    uint32_t sh = fb_get_height();
    int w = sw >= 1200 ? 500 : 380;
    int h = sh >= 760 ? 400 : 320;
    int x = (int)sw - w - 28;
    int y = 76;
    if (x < 40) x = 40;

    g_desktop_state.terminal = terminal_app_open(x, y, w, h);
    wm_set_close_callback(g_desktop_state.terminal, desktop_clear_window_ptr,
                           &g_desktop_state.terminal);
    taskbar_add_window(g_desktop_state.terminal, "Term");
}

static void desktop_launch_settings(void)
{
    if (g_desktop_state.settings) {
        wm_set_focused(g_desktop_state.settings);
        return;
    }
    uint32_t sw = fb_get_width();
    uint32_t sh = fb_get_height();
    int w = sw >= 1200 ? 600 : 520;
    int h = sh >= 760 ? 380 : 330;
    int x = 58;
    int y = (int)sh - h - 92;
    if (y < 96) y = 96;

    g_desktop_state.settings = settings_app_open(x, y, w, h);
    wm_set_close_callback(g_desktop_state.settings, desktop_clear_window_ptr,
                           &g_desktop_state.settings);
    taskbar_add_window(g_desktop_state.settings, "Settings");
}

static void desktop_launch_calculator(void)
{
    if (g_desktop_state.calculator) {
        wm_set_focused(g_desktop_state.calculator);
        return;
    }
    uint32_t sw = fb_get_width();
    int w = 350;
    int h = 400;
    int x = (int)sw - w - 40;
    if (x < 60) x = 60;

    g_desktop_state.calculator = calculator_app_open(x, 120, w, h);
    wm_set_close_callback(g_desktop_state.calculator, desktop_clear_window_ptr,
                           &g_desktop_state.calculator);
    taskbar_add_window(g_desktop_state.calculator, "Calculator");
}

/* ---- Initialization ------------------------------------------------------ */
void desktop_system_init(void)
{
    if (g_desktop_state.initialized)
        return;

    terminal_puts("[DESKTOP] Initializing desktop system...\n");

    uint32_t screen_w = fb_get_width();
    uint32_t screen_h = fb_get_height();

    terminal_printf("[DESKTOP] Screen resolution: %ux%u\n", screen_w, screen_h);

    /* Initialize subsystems */
    wm_init(screen_w, screen_h);
    theme_init();
    input_queue_init();
    animation_init();
    taskbar_init();
    desktop_init();
    notifications_init();

    terminal_puts("[DESKTOP] All subsystems initialized\n");

    /* Launch default applications */
    terminal_puts("[DESKTOP] Launching default applications...\n");

    desktop_launch_file_explorer();
    desktop_launch_terminal();

    terminal_puts("[DESKTOP] Default applications launched\n");

    g_desktop_state.initialized = 1;

    /* Show desktop startup message */
    notification_show(
        "JARVIS Desktop Ready",
        "Window manager, File Explorer, Terminal active",
        NOTIF_INFO,
        3000
    );

    terminal_puts("[DESKTOP] Desktop system ready!\n");
}

/* ---- Main Desktop Render Loop -------------------------------------------- */
void desktop_system_render(void)
{
    if (!g_desktop_state.initialized)
        return;

    /* Update animations */
    animation_tick();

    /* Clear framebuffer to dark background */
    fb_clear(0x00050510);

    /* Draw desktop components */
    desktop_draw();
    wm_draw_all(fb_get_base());
    taskbar_draw();
    if (taskbar_is_start_menu_open())
        start_menu_draw();
    notifications_draw();
    ui_shell_draw_frame();

    g_desktop_state.frame_count++;
}

/* ---- Input Integration --------------------------------------------------- */
void desktop_system_handle_mouse_move(int x, int y)
{
    input_mouse_move(x, y);
    input_process_queue();
}

void desktop_system_handle_mouse_click(int x, int y, int button)
{
    /* Taskbar gets first refusal — Start, pinned icons, and open-window
     * buttons must intercept clicks before the WM drag logic sees them. */
    if (taskbar_handle_click(x, y))
        return;

    /* Desktop icons (This PC, Recycle Bin, ...) live "behind" every window
     * in z-order — a click only reaches them if it didn't land on any
     * actual window. */
    if (!wm_window_at_point(x, y)) {
        int icon = desktop_handle_click(x, y);
        if (icon >= 0) {
            const char *path = desktop_get_icon_path(icon);
            if (strcmp(path, "/jarvis/trash") == 0) {
                notification_show("Recycle Bin", "Recycle Bin is empty.",
                                   NOTIF_INFO, 3000);
            } else {
                desktop_launch_file_explorer();
                file_explorer_navigate_to(path);
            }
            return;
        }
        if (icon == -2)
            return; /* hit an icon but not (yet) a double-click; consume it */
    }
    input_mouse_click(x, y, button);
    input_process_queue();
}

void desktop_system_handle_mouse_release(int x, int y, int button)
{
    input_mouse_release(x, y, button);
    input_process_queue();
}

void desktop_system_handle_mouse_wheel(int x, int y, int delta)
{
    wm_input_event_t event;
    event.type = IE_MOUSE_WHEEL;
    event.data.mouse.x = x;
    event.data.mouse.y = y;
    event.data.mouse.button = delta;
    event.timestamp = g_kstate.uptime_ticks;
    input_post_event(&event);
    input_process_queue();
}

void desktop_system_handle_key(uint32_t key, int mods)
{
    input_key_press(key, mods);
    input_process_queue();
}

void desktop_system_handle_key_up(uint32_t key, int mods)
{
    input_key_release(key, mods);
    input_process_queue();
}

void desktop_system_handle_text_input(char ch)
{
    input_text_char(ch);
    input_process_queue();
}

/* ---- Statistics & Diagnostics -------------------------------------------- */
void desktop_system_status(void)
{
    terminal_printf("[DESKTOP STATUS]\n");
    terminal_printf("  Frames rendered: %u\n", g_desktop_state.frame_count);
    terminal_printf("  Windows created: %u\n", g_wm_ctx.next_wid - 1);
    terminal_printf("  Active windows: %d\n", wm_count_windows());
    terminal_printf("  Notifications: %d/%d\n",
        notifications_get_unread_count(), notifications_get_count());
    terminal_printf("  Animations: %u\n", g_anim_ctx.animation_count);
    terminal_printf("  AI Core: %s\n", g_kstate.ai_ready ? "READY" : "LOADING");
    terminal_printf("  Speech: %s\n", g_kstate.speech_ready ? "READY" : "LOADING");
    terminal_printf("  Security: %s\n", g_kstate.security_enabled ? "ENABLED" : "DISABLED");
}

int desktop_system_is_initialized(void)
{
    return g_desktop_state.initialized;
}

/* Closes every open window (used by the "clear" debug command and
 * available for a future "log off"/"close all windows" desktop action).
 * Declared in kernel.h since the project's first revision but never
 * actually implemented or wired to anything — the "clear" debug command
 * printed "Desktop cleared." without doing anything. */
void desktop_system_close_all(void)
{
    wm_window_t *win = g_wm_ctx.windows_head;
    while (win) {
        wm_window_t *next = win->next;
        wm_close_window(win);
        win = next;
    }
}

void desktop_system_open_app(const char *app)
{
    if (!desktop_system_is_initialized())
        return;

    if (!app) return;

    terminal_printf("[DESKTOP] Voice command: opening %s\n", app);

    if (strcmp(app, "Files") == 0 || strcmp(app, "files") == 0) {
        desktop_launch_file_explorer();
    } else if (strcmp(app, "Terminal") == 0 || strcmp(app, "terminal") == 0) {
        desktop_launch_terminal();
    } else if (strcmp(app, "Settings") == 0 || strcmp(app, "settings") == 0) {
        desktop_launch_settings();
    } else if (strcmp(app, "Calculator") == 0 || strcmp(app, "calculator") == 0) {
        desktop_launch_calculator();
    } else if (strcmp(app, "Browser") == 0 || strcmp(app, "browser") == 0) {
        terminal_printf("[DESKTOP] Browser app not yet implemented\n");
    }
}
