/*
 * =============================================================================
 * JARVIS OS — Desktop system (desktop/desktop_init.c)
 * Language: C17
 *
 * Ties the desktop together: the app registry, the frame compositor, and
 * mouse / keyboard routing between the shell (taskbar, Start menu,
 * notifications, context menu), the window manager and the lock screen.
 *
 * This file is a unity build: it #includes the rest of the shell and the
 * apps so they share one translation unit.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "../wm/wm.h"
#include "ui.h"
#include <string.h>
#include <stdio.h>

void ui_shell_lock_input(uint32_t key, char ch);    /* desktop/ui_shell.c */

#include "../input/input_events.c"
#include "theme.c"
#include "sysutil.c"
#include "icons.c"
#include "notifications.c"
#include "desktop.c"
#include "taskbar.c"
#include "../apps/file_explorer.c"
#include "../apps/terminal.c"
#include "../apps/settings.c"
#include "../apps/calculator.c"
#include "../apps/assistant.c"
#include "../apps/security_center.c"
#include "../apps/sysmon.c"
#include "../apps/notepad.c"

/* ---- App registry -------------------------------------------------------- */
typedef wm_window_t *(*app_open_fn)(int x, int y, int w, int h);

typedef struct {
    desktop_app_t pub;
    app_open_fn open;
    int w, h;
    const char *aliases;        /* space-separated, lower case */
} app_entry_t;

static app_entry_t g_apps[] = {
    { { "Files",      ICON_FOLDER,   NULL }, file_explorer_open,  700, 460, "files file explorer" },
    { { "Terminal",   ICON_TERMINAL, NULL }, terminal_app_open,   640, 400, "terminal term console shell" },
    { { "JARVIS",     ICON_JARVIS,   NULL }, assistant_app_open,  460, 500, "jarvis ai assistant voice" },
    { { "Security",   ICON_SHIELD,   NULL }, security_app_open,   470, 440, "security guard" },
    { { "Settings",   ICON_GEAR,     NULL }, settings_app_open,   680, 400, "settings personalize" },
    { { "Calculator", ICON_CALC,     NULL }, calculator_app_open, 300, 430, "calculator calc" },
    { { "Tasks",      ICON_CHART,    NULL }, sysmon_app_open,     640, 470, "system monitor taskmgr processes" },
    { { "Notepad",    ICON_FILE,     NULL }, notepad_app_open,    600, 420, "notepad editor text" },
};
#define APP_COUNT ((int)(sizeof(g_apps) / sizeof(g_apps[0])))

/* ---- Desktop state ------------------------------------------------------- */
static struct {
    int initialized;
    int dirty;                  /* scene changed: recompose the frame */
    int cursor_dirty;           /* only the pointer moved             */
    uint64_t last_draw_tick;
    uint32_t frame_count;
    int launch_count;
} g_ds;

void desktop_invalidate(void)
{
    g_ds.dirty = 1;
}

uint32_t desktop_frame_count(void)
{
    return g_ds.frame_count;
}

int desktop_system_is_initialized(void)
{
    return g_ds.initialized;
}

/* ---- Apps ---------------------------------------------------------------- */
int desktop_app_count(void)
{
    return APP_COUNT;
}

desktop_app_t *desktop_app_get(int index)
{
    if (index < 0 || index >= APP_COUNT) return NULL;
    return &g_apps[index].pub;
}

static int word_ieq(const char *a, const char *b, size_t blen)
{
    size_t i = 0;
    for (; i < blen; i++) {
        if (!a[i] || lower(a[i]) != lower(b[i])) return 0;
    }
    return a[i] == 0;
}

int desktop_app_find(const char *name)
{
    if (!name || !*name) return -1;

    for (int i = 0; i < APP_COUNT; i++) {
        if (word_ieq(name, g_apps[i].pub.name, strlen(g_apps[i].pub.name)))
            return i;
        for (const char *p = g_apps[i].aliases; *p;) {
            const char *end = p;
            while (*end && *end != ' ') end++;
            if (word_ieq(name, p, (size_t)(end - p)))
                return i;
            p = *end ? end + 1 : end;
        }
    }
    return -1;
}

static void app_closed(void *ctx)
{
    ((app_entry_t *)ctx)->pub.window = NULL;
}

void desktop_app_launch(int index)
{
    if (index < 0 || index >= APP_COUNT || !g_ds.initialized) return;
    app_entry_t *app = &g_apps[index];

    if (app->pub.window) {
        wm_set_focused(app->pub.window);
        return;
    }

    /* Cascade new windows from the top-left of the work area. */
    int step = g_ds.launch_count++ % 6;
    int x = 110 + step * 34;
    int y = 28 + step * 30;

    app->pub.window = app->open(x, y, app->w, app->h);
    if (app->pub.window)
        wm_set_close_callback(app->pub.window, app_closed, app);
    desktop_invalidate();
}

/* Taskbar icon behaviour: launch, bring forward, or minimize if already
 * in front. */
void desktop_app_activate(int index)
{
    if (index < 0 || index >= APP_COUNT) return;
    wm_window_t *win = g_apps[index].pub.window;

    if (!win)
        desktop_app_launch(index);
    else if (win == g_wm_ctx.focused && !win->minimized)
        wm_set_minimized(win, 1);
    else
        wm_set_focused(win);
}

void desktop_system_open_app(const char *app)
{
    if (!g_ds.initialized || !app) return;

    int idx = desktop_app_find(app);
    if (idx >= 0) {
        desktop_app_launch(idx);
    } else if (word_ieq(app, "browser", 7) || word_ieq(app, "web", 3)) {
        notification_show("Browser", "No web browser yet. In Terminal, 'http <url>' fetches plain http pages.",
                          NOTIF_WARNING, 5000);
    } else {
        terminal_printf("[DESKTOP] Unknown app: %s\n", app);
    }
}

void desktop_system_close_all(void)
{
    while (g_wm_ctx.windows_head)
        wm_close_window(g_wm_ctx.windows_head);
}

/* Typed questions take the same route as recognised speech. */
void desktop_ask_jarvis(const char *text)
{
    if (!text || !*text) return;

    desktop_app_launch(desktop_app_find("JARVIS"));

    static ipc_message_t msg;       /* 4 KB payload: keep it off the stack */
    size_t len = strlen(text);
    if (len >= sizeof(msg.payload)) len = sizeof(msg.payload) - 1;

    memset(&msg, 0, sizeof(msg));
    msg.type = IPC_VOICE_INPUT;
    memcpy(msg.payload, text, len);
    msg.payload_len = len;
    ipc_send(&msg);
}

void desktop_lock(void)
{
    start_menu_close();
    notifications_close_center();
    desktop_context_close();
    ui_shell_show_lock_screen();
}

static void power_screen(const char *text)
{
    fb_reset_clip();
    fb_clear(0x00000000);
    ui_text_center(0, (int)fb_get_height() / 2 - 8, (int)fb_get_width(), text, 0x00FFFFFF);
    fb_cursor_show(0);
    fb_present();
}

void desktop_system_shutdown(void)
{
    power_screen("Shutting down...");
    diskfs_sync();          /* nothing unsaved is lost */
    sys_poweroff();
}

void desktop_system_restart(void)
{
    power_screen("Restarting...");
    diskfs_sync();          /* nothing unsaved is lost */
    sys_reboot();
}

/* ---- Initialization ------------------------------------------------------ */
void desktop_system_init(void)
{
    if (g_ds.initialized)
        return;

    uint32_t W = fb_get_width(), H = fb_get_height();
    terminal_printf("[DESKTOP] Initializing desktop at %ux%u\n", W, H);

    if (W == 0 || H == 0) {
        terminal_puts("[DESKTOP] No framebuffer; desktop disabled\n");
        return;
    }

    wm_init(W, H);
    wm_set_workarea(0, 0, (int)W, (int)H - TASKBAR_H);
    theme_init();
    input_queue_init();
    notifications_init();
    taskbar_init();
    desktop_init();

    g_ds.initialized = 1;
    g_ds.dirty = 1;

    notification_show("Welcome to JARVIS OS",
                      "Click Start, or open JARVIS and type 'help'.",
                      NOTIF_INFO, 6000);
    terminal_puts("[DESKTOP] Desktop system ready\n");
}

/* ---- Frame compositor ----------------------------------------------------
 * Called from the kernel main loop on every wake-up. A full frame is only
 * composed when something changed (or twice a second for the clock and the
 * terminal caret); plain pointer movement repaints just the pointer.
 */
void desktop_system_render(void)
{
    if (!g_ds.initialized)
        return;

    uint64_t now = g_kstate.uptime_ticks;

    /* Scheduler events raised inside other processes (they must not touch
     * the UI themselves) are shown from here, on the desktop process. */
    governor_tick();        /* resource policy: AI advice or built-in rules */

    /* Save changed files to disk every two seconds. */
    static uint64_t last_sync;
    if (now - last_sync >= 200) {
        last_sync = now;
        diskfs_sync();
    }

    /* Output printed by user programs appears in the Terminal. */
    static char prog_out[512];
    size_t out_len = user_console_read(prog_out, sizeof(prog_out) - 1);
    if (out_len) {
        prog_out[out_len] = 0;
        terminal_puts(prog_out);                            /* serial log */
        desktop_app_t *term = desktop_app_get(desktop_app_find("Terminal"));
        if (term && term->window)
            terminal_app_write(prog_out);
        g_ds.dirty = 1;
    }

    char sched_msg[96];
    if (scheduler_take_event(sched_msg, sizeof(sched_msg)))
        notification_show("Process manager", sched_msg, NOTIF_WARNING, 8000);

    if (notifications_tick())
        g_ds.dirty = 1;
    if (now - g_ds.last_draw_tick >= 50)
        g_ds.dirty = 1;

    if (!g_ds.dirty) {
        if (g_ds.cursor_dirty) {
            fb_cursor_refresh();
            g_ds.cursor_dirty = 0;
        }
        return;
    }

    g_ds.dirty = 0;
    g_ds.cursor_dirty = 0;
    g_ds.last_draw_tick = now;
    g_ds.frame_count++;

    if (!ui_shell_is_desktop()) {
        ui_shell_render_fullscreen();       /* boot / setup / lock screen */
        return;
    }

    fb_reset_clip();
    desktop_draw();
    wm_draw_all(fb_get_base());
    wm_draw_snap_preview();
    taskbar_draw();
    start_menu_draw();
    notifications_draw();
    desktop_context_draw();
    fb_present();
}

/* ---- Mouse --------------------------------------------------------------- */
void desktop_system_handle_mouse_move(int x, int y)
{
    fb_cursor_set_pos(x, y);
    g_ds.cursor_dirty = 1;

    if (!g_ds.initialized || !ui_shell_is_desktop())
        return;

    int changed = 0;
    if (wm_is_dragging()) {
        changed = wm_handle_mouse_move(x, y);
    } else {
        changed |= desktop_context_hover(x, y);
        changed |= start_menu_hover(x, y);
        changed |= taskbar_hover(x, y);
        changed |= wm_handle_mouse_move(x, y);
    }
    if (changed)
        g_ds.dirty = 1;
}

void desktop_open_icon(int icon)
{
    const char *target = desktop_get_icon_target(icon);

    if (target[0] == '/' && !vfs_is_dir(target)) {
        notepad_open_file(target);
    } else if (target[0] == '/') {
        desktop_app_launch(desktop_app_find("Files"));
        file_explorer_navigate_to(target);
    } else if (target[0]) {
        desktop_system_open_app(target);
    }
}

void desktop_system_handle_mouse_click(int x, int y, int button)
{
    if (!g_ds.initialized)
        return;
    g_ds.dirty = 1;

    if (ui_shell_is_locked()) {
        ui_shell_lock_input(0, 0);
        return;
    }
    if (!ui_shell_is_desktop())
        return;

    /* Clicking anywhere accepts a desktop rename in progress. */
    if (desktop_rename_active())
        desktop_rename_finish(1);

    /* Top-most layers first. */
    if (desktop_context_click(x, y))
        return;
    if (taskbar_handle_click(x, y))
        return;
    if (taskbar_is_start_menu_open()) {
        if (!start_menu_click(x, y))
            start_menu_close();             /* click outside dismisses it */
        return;
    }
    if (notifications_center_is_visible()) {
        if (!notifications_click(x, y))
            notifications_close_center();
        return;
    }

    /* A right-click anywhere first dismisses a menu that is already open
     * (handled above), then goes to the window or desktop under it. */
    if (wm_handle_mouse_click(x, y, button))
        return;

    /* Bare desktop */
    if (button == 2) {
        desktop_context_open(x, y);
    } else if (button == 1) {
        int icon = desktop_handle_click(x, y);
        if (icon >= 0)
            desktop_open_icon(icon);
    }
}

void desktop_system_handle_mouse_release(int x, int y, int button)
{
    if (!g_ds.initialized || !ui_shell_is_desktop())
        return;
    wm_handle_mouse_release(x, y, button);
    g_ds.dirty = 1;
}

void desktop_system_handle_mouse_wheel(int x, int y, int delta)
{
    (void)x; (void)y; (void)delta;      /* no wheel-capable mouse driver yet */
}

/* ---- Keyboard ------------------------------------------------------------ */
/* Global shortcuts. Returns 1 if the key was consumed. */
static int desktop_shortcut(uint32_t key, int mods)
{
    if (key == WM_KEY_SUPER_TAP || ((mods & KMOD_CTRL) && key == WM_KEY_ESC)) {
        taskbar_toggle_start_menu();
        return 1;
    }

    if (mods & KMOD_SUPER) {
        switch (lower((char)key)) {
        case 'd': wm_minimize_all();                                   return 1;
        case 'e': desktop_app_launch(desktop_app_find("Files"));       return 1;
        case 'i': desktop_app_launch(desktop_app_find("Settings"));    return 1;
        case 'j': desktop_app_launch(desktop_app_find("JARVIS"));      return 1;
        case 'l': desktop_lock();                                      return 1;
        default: break;
        }
    }
    if ((mods & KMOD_CTRL) && (mods & KMOD_ALT) && lower((char)key) == 't') {
        desktop_app_launch(desktop_app_find("Terminal"));
        return 1;
    }
    return 0;
}

void desktop_dispatch_event(wm_input_event_t *event)
{
    if (!g_ds.initialized || !event)
        return;

    if (ui_shell_is_locked()) {
        if (event->type == IE_KEY_DOWN)
            ui_shell_lock_input(event->data.keyboard.key, 0);
        else if (event->type == IE_TEXT_INPUT)
            ui_shell_lock_input(0, event->data.text.ch);
        g_ds.dirty = 1;
        return;
    }
    if (!ui_shell_is_desktop())
        return;

    switch (event->type) {
    case IE_KEY_DOWN: {
        uint32_t key = event->data.keyboard.key;
        int mods = event->data.keyboard.mods;
        g_ds.dirty = 1;

        if (desktop_rename_active()) {
            desktop_rename_input(key, 0);
            return;
        }
        if (desktop_shortcut(key, mods))
            return;
        if (start_menu_key(key, mods))
            return;
        /* With no window focused, Delete / F2 act on the selected desktop icon. */
        if (!g_wm_ctx.focused && key == WM_KEY_DELETE) {
            desktop_delete_selected();
            return;
        }
        if (key == WM_KEY_ESC && (desktop_context_is_open() ||
                                  notifications_center_is_visible())) {
            desktop_context_close();
            notifications_close_center();
            return;
        }
        wm_handle_input(event);
        break;
    }
    case IE_TEXT_INPUT:
        g_ds.dirty = 1;
        if (desktop_rename_active()) {
            desktop_rename_input(0, event->data.text.ch);
            return;
        }
        if (start_menu_text(event->data.text.ch))
            return;
        wm_handle_input(event);
        break;
    case IE_KEY_UP:
        wm_handle_input(event);
        break;
    default:
        break;
    }
}

/* Kept for callers that inject input directly (voice commands, tests). */
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

/* ---- Diagnostics --------------------------------------------------------- */
void desktop_system_status(void)
{
    terminal_printf("[DESKTOP STATUS]\n");
    terminal_printf("  Frames rendered: %u\n", g_ds.frame_count);
    terminal_printf("  Open windows: %d\n", wm_count_windows());
    terminal_printf("  Notifications: %d (%d unread)\n",
        notifications_get_count(), notifications_get_unread_count());
    terminal_printf("  Theme: %s\n", theme_get_name());
    terminal_printf("  AI Core: %s\n", g_kstate.ai_ready ? "READY" : "LOADING");
    terminal_printf("  Speech: %s\n", g_kstate.speech_ready ? "READY" : "LOADING");
    terminal_printf("  Security: %s\n", g_kstate.security_enabled ? "ENABLED" : "DISABLED");
}
