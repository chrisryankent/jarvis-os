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
void ui_shell_login_click(int x, int y);

#include "../input/input_events.c"
#include "theme.c"
#include "sysutil.c"
#include "icons.c"
#include "notifications.c"
#include "wallpaper.c"
#include "prefs.c"
#include "desktop.c"
#include "taskbar.c"
#include "flyouts.c"
#include "../apps/file_explorer.c"
#include "../apps/shell.c"
#include "../apps/terminal.c"
#include "../apps/settings.c"
#include "../apps/calculator.c"
#include "../apps/assistant.c"
#include "../apps/security_center.c"
#include "../apps/sysmon.c"
#include "../apps/notepad.c"
#include "../apps/store.c"
#include "../apps/disks.c"
#include "../apps/devices.c"
#include "../apps/photos.c"
#include "../apps/clock.c"
#include "../apps/regedit.c"
#include "../apps/browser.c"

/* ---- App registry -------------------------------------------------------- */
typedef wm_window_t *(*app_open_fn)(int x, int y, int w, int h);

typedef struct {
    desktop_app_t pub;
    app_open_fn open;
    int w, h;
    const char *aliases;        /* space-separated, lower case */
} app_entry_t;

static app_entry_t g_apps[] = {
    { { "Files",      ICON_FOLDER,   NULL }, file_explorer_open,  920, 560, "files file explorer this pc computer drives" },
    { { "Terminal",   ICON_TERMINAL, NULL }, terminal_app_open,   820, 480, "terminal term console shell" },
    { { "JARVIS",     ICON_JARVIS,   NULL }, assistant_app_open,  460, 500, "jarvis ai assistant voice" },
    { { "Security",   ICON_SHIELD,   NULL }, security_app_open,   470, 440, "security guard" },
    { { "Settings",   ICON_GEAR,     NULL }, settings_app_open,   1000, 600, "settings personalize" },
    { { "Calculator", ICON_CALC,     NULL }, calculator_app_open, 300, 430, "calculator calc" },
    { { "Tasks",      ICON_CHART,    NULL }, sysmon_app_open,     900, 500, "system monitor taskmgr processes" },
    { { "Notepad",    ICON_FILE,     NULL }, notepad_app_open,    780, 520, "notepad editor text" },
    { { "Apps",       ICON_STORE,    NULL }, store_app_open,      760, 520, "apps store install installer packages" },
    { { "Disks",      ICON_DISKS,    NULL }, disks_app_open,      960, 600, "disks disk management manager drives partitions storage" },
    { { "Device Manager", ICON_DEVICES, NULL }, devices_app_open, 940, 600, "device manager devices hardware drivers devmgmt" },
    { { "Photos",     ICON_PHOTOS,   NULL }, photos_app_open,     900, 600, "photos pictures images viewer gallery picture" },
    { { "Clock",      ICON_CLOCK,    NULL }, clock_app_open,      520, 460, "clock alarm alarms timer stopwatch" },
    { { "Run",        ICON_TERMINAL, NULL }, run_app_open,        480, 170, "run" },
    { { "Registry Editor", ICON_REGEDIT, NULL }, regedit_app_open, 1000, 600, "regedit registry editor reg" },
    { { "Browser",    ICON_BROWSER,  NULL }, browser_app_open,    1060, 640, "browser web internet chrome edge firefox www" },
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

/* ---- Clipboard ----------------------------------------------------------- */
#define CLIPBOARD_MAX (256 * 1024)
static char   g_clipboard[CLIPBOARD_MAX + 1];
static size_t g_clipboard_len = 0;

void clipboard_set(const char *text, size_t len)
{
    if (len > CLIPBOARD_MAX) len = CLIPBOARD_MAX;
    memcpy(g_clipboard, text, len);
    g_clipboard[len] = 0;
    g_clipboard_len = len;
}

const char *clipboard_get(size_t *len)
{
    if (len) *len = g_clipboard_len;
    return g_clipboard;
}

/* ---- Apps ----------------------------------------------------------------
 * Built-in apps (g_apps) come first, then one entry per installed package,
 * rebuilt whenever a package is installed or removed. Package entries show in
 * Start and search, not on the taskbar.
 */
#define PKG_APPS_MAX 24
static struct { desktop_app_t pub; char title[48]; char name[32]; } g_pkg_apps[PKG_APPS_MAX];
static int g_pkg_app_count = 0;
static uint32_t g_pkg_seen = 0;

static void pkg_apps_refresh(void)
{
    if (g_pkg_seen == g_pkg_generation) return;
    g_pkg_seen = g_pkg_generation;
    static pkg_info_t list[PKG_APPS_MAX];
    int n = pkg_list(list, PKG_APPS_MAX);
    for (int i = 0; i < n; i++) {
        snprintf(g_pkg_apps[i].title, sizeof(g_pkg_apps[i].title), "%s", list[i].title);
        snprintf(g_pkg_apps[i].name, sizeof(g_pkg_apps[i].name), "%s", list[i].name);
        g_pkg_apps[i].pub.name = g_pkg_apps[i].title;
        g_pkg_apps[i].pub.icon = ICON_PROGRAM;
        g_pkg_apps[i].pub.window = NULL;
    }
    g_pkg_app_count = n;
}

int desktop_app_count(void)
{
    pkg_apps_refresh();
    return APP_COUNT + g_pkg_app_count;
}

int desktop_app_builtin_count(void)
{
    return APP_COUNT;
}

desktop_app_t *desktop_app_get(int index)
{
    if (index >= APP_COUNT && index < APP_COUNT + g_pkg_app_count) return &g_pkg_apps[index - APP_COUNT].pub;
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
    if (index >= APP_COUNT && index < APP_COUNT + g_pkg_app_count && g_ds.initialized) {
        /* installed package: a console program, shown in the Terminal */
        const char *name = g_pkg_apps[index - APP_COUNT].name;
        desktop_app_launch(desktop_app_find("Terminal"));
        if (!pkg_run(name))
            notification_show(g_pkg_apps[index - APP_COUNT].title, "The app could not be started.", NOTIF_WARNING, 4000);
        return;
    }
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
    if (index >= APP_COUNT) { desktop_app_launch(index); return; }
    if (index < 0) return;
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

/* ---- Settings that take effect outside the Settings app ----------------- */
void desktop_apply_settings(void)
{
    theme_set_mode((theme_mode_t)setting_get_int("theme", THEME_DARK));
    theme_set_accent(setting_get_int("accent", 0));
    theme_set_wallpaper(setting_get_int("wallpaper", 0));
    sys_set_clock_24h(setting_get_int("clock_24h", 1));
    ui_shell_set_mouse(setting_get_int("mouse_speed", 2), setting_get_int("mouse_swap", 0));
    audio_set_volume(setting_get_int("volume", 80));
    ai_tools_set_ask_first(setting_get_int("ai_ask_first", 0));
    if (setting_get_int("airplane", 0)) net_set_offline(1);
    if (setting_get_int("dev_net_off", 0)) net_set_adapter_enabled(0);
    if (setting_get_int("dev_audio_off", 0)) audio_set_enabled(0);
    if (setting_get_int("mute", 0)) audio_set_volume(0);
    prefs_apply_display();
    static int res_done;
    if (!res_done) {                    /* saved resolution: once, at start-up */
        res_done = 1;
        char r[16], msg[160];
        setting_get("resolution", r, sizeof(r));
        if (r[0]) prefs_apply_resolution(r, msg, sizeof(msg));
    }
}

/* The screen mode changed: re-fit the work area, windows and wallpaper. */
static void desktop_display_changed(void)
{
    int W = (int)fb_get_width(), H = (int)fb_get_height();
    g_wm_ctx.screen_w = (uint32_t)W;
    g_wm_ctx.screen_h = (uint32_t)H;
    wm_set_workarea(0, 0, W, H - TASKBAR_H);
    for (wm_window_t *w = g_wm_ctx.windows_head; w; w = w->next) {
        if (w->state != WIN_STATE_NORMAL) {           /* maximized or snapped: re-apply */
            wm_window_state_t st = w->state;
            w->state = WIN_STATE_NORMAL;
            if (st == WIN_STATE_MAXIMIZED) wm_set_maximized(w, 1);
            else wm_snap_window(w, st);
            continue;
        }
        if (w->w > W) w->w = W;
        if (w->h > H - TASKBAR_H) w->h = H - TASKBAR_H;
        if (w->x + w->w > W) w->x = W - w->w;
        if (w->y + w->h > H - TASKBAR_H) w->y = H - TASKBAR_H - w->h;
        if (w->x < 0) w->x = 0;
        if (w->y < 0) w->y = 0;
    }
    desktop_wallpaper_invalidate();
    desktop_files_changed();
    desktop_invalidate();
}

void desktop_send_host_prefs(void)
{
    char p[32];
    snprintf(p, sizeof(p), "web=%d", setting_get_int("ai_web", 1));
    ai_host_pref(p);
    snprintf(p, sizeof(p), "volume=%d", setting_get_int("voice_volume", 100));
    ai_host_pref(p);
}

/* Apps chosen in Settings > Apps > Startup open after each sign-in. */
static void run_startup_apps(void)
{
    for (int i = 0; i < APP_COUNT; i++) {
        char key[48];
        snprintf(key, sizeof(key), "startup_%s", g_apps[i].pub.name);
        if (setting_get_int(key, 0)) desktop_app_launch(i);
    }
}

static uint64_t g_last_input_tick = 0;
void desktop_note_input(void) { g_last_input_tick = g_kstate.uptime_ticks; }

void desktop_lock(void)
{
    start_menu_close();
    notifications_close_center();
    desktop_context_close();
    ui_shell_show_lock_screen();
}

/* Print Screen / Win+Shift+S / "screenshot": the current frame as a PNG in
 * Pictures/Screenshots. */
void desktop_take_screenshot(char *saved, size_t n)
{
    saved[0] = 0;
    const char *dir = "/jarvis/data/Pictures/Screenshots";
    if (!vfs_is_dir("/jarvis/data/Pictures")) vfs_mkdir("/jarvis/data/Pictures");
    if (!vfs_is_dir(dir)) vfs_mkdir(dir);
    sys_time_t t;
    sys_get_time(&t);
    char path[160];
    snprintf(path, sizeof(path), "%s/Screenshot %04d-%02d-%02d %02d%02d%02d.png", dir, t.year, t.month, t.day, t.hour, t.minute, t.second);
    if (image_save_png(path, fb_get_base(), (int)fb_get_width(), (int)fb_get_height()) != 0) return;
    snprintf(saved, n, "%s", path);
    notification_show("Screenshot saved", "Saved in Pictures > Screenshots. Open Photos to see it.", NOTIF_INFO, 4000);
    desktop_files_changed();
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
    reg_flush();
    diskfs_sync();          /* nothing unsaved is lost */
    sys_poweroff();
}

void desktop_system_restart(void)
{
    power_screen("Restarting...");
    reg_flush();
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
        reg_flush();
        diskfs_sync();
        /* hardware or apps changed: rebuild the live registry keys */
        static uint32_t seen_storage, seen_pkg;
        if (seen_storage != g_storage_generation || seen_pkg != g_pkg_generation) {
            seen_storage = g_storage_generation;
            seen_pkg = g_pkg_generation;
            reg_refresh_live();
        }
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
    if (scheduler_take_event(sched_msg, sizeof(sched_msg))) {
        int usb = !strncmp(sched_msg, "USB drive", 9);
        if (!strcmp(sched_msg, "Disk check finished")) {
            diskfs_check_t ck;
            diskfs_check_status(&ck);
            notification_show("Disk check finished", ck.summary, ck.problems ? NOTIF_WARNING : NOTIF_INFO, 8000);
            ai_tools_disk_check_done();
        } else
            notification_show(usb ? "USB" : "Process manager", sched_msg, usb ? NOTIF_INFO : NOTIF_WARNING, 8000);
        /* AutoPlay: show the new drive in This PC */
        if (!strncmp(sched_msg, "USB drive connected", 19) && setting_get_int("autoplay", 1) && ui_shell_is_desktop()) {
            desktop_app_launch(desktop_app_find("Files"));
            file_explorer_navigate_to("::thispc");
        }
    }

    /* Sign-in after each unlock: startup apps; idle: lock automatically. */
    static int was_desktop = 0;
    int is_desktop = ui_shell_is_desktop();
    if (is_desktop && !was_desktop) { g_last_input_tick = now; run_startup_apps(); }
    was_desktop = is_desktop;
    int idle_min = setting_get_int("lock_after_min", 0);
    if (is_desktop && idle_min > 0 && account_has_sign_in() && now - g_last_input_tick > (uint64_t)idle_min * 6000)
        desktop_lock();

    /* Screen off after inactivity (Settings > Display > Power): a black
     * frame until the next key press or mouse movement. */
    static int screen_off = 0;
    int off_min = setting_get_int("screen_off_min", 0);
    if (off_min > 0 && now - g_last_input_tick > (uint64_t)off_min * 6000) {
        if (!screen_off) {
            screen_off = 1;
            fb_reset_clip();
            fb_clear(0);
            fb_cursor_show(0);
            fb_present();
        }
        return;
    }
    if (screen_off) { screen_off = 0; fb_cursor_show(1); g_ds.dirty = 1; }

    if (notifications_tick())
        g_ds.dirty = 1;
    shell_poll_downloads();
    static uint64_t last_pref_tick;
    if (now - last_pref_tick >= 50) { last_pref_tick = now; prefs_tick(); clock_tick(); }
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
    shell_flyout_draw();
    desktop_context_draw();
    fb_present();
}

/* ---- Mouse --------------------------------------------------------------- */
void desktop_system_handle_mouse_move(int x, int y)
{
    g_last_input_tick = g_kstate.uptime_ticks;
    fb_cursor_set_pos(x, y);
    g_ds.cursor_dirty = 1;

    if (!g_ds.initialized || !ui_shell_is_desktop())
        return;

    int changed = 0;
    if (shell_flyout_dragging()) {
        if (shell_flyout_hover(x, y)) g_ds.dirty = 1;
        return;
    }
    if (wm_is_dragging()) {
        changed = wm_handle_mouse_move(x, y);
    } else {
        changed |= desktop_context_hover(x, y);
        changed |= start_menu_hover(x, y);
        changed |= taskbar_hover(x, y);
        changed |= shell_flyout_hover(x, y);
        changed |= wm_handle_mouse_move(x, y);
    }
    if (changed)
        g_ds.dirty = 1;
}

void desktop_open_icon(int icon)
{
    const char *target = desktop_get_icon_target(icon);

    if (!strncmp(target, "::", 2) || !strcmp(target, "/jarvis/trash") ||
        (target[0] == '/' && vfs_is_dir(target))) {
        /* This PC, the Recycle Bin (even while it is still empty) or a folder */
        desktop_app_launch(desktop_app_find("Files"));
        file_explorer_navigate_to(target);
    } else if (target[0] == '/') {
        shell_open_path(target);
    } else if (target[0]) {
        desktop_system_open_app(target);
    }
}

void desktop_system_handle_mouse_click(int x, int y, int button)
{
    if (!g_ds.initialized)
        return;
    g_ds.dirty = 1;

    if (!ui_shell_is_desktop()) {          /* sign-in or first-run setup screen */
        if (button == 1) ui_shell_login_click(x, y);
        return;
    }

    /* Clicking anywhere accepts a desktop rename in progress. */
    if (desktop_rename_active())
        desktop_rename_finish(1);

    /* Top-most layers first. */
    if (desktop_context_click(x, y))
        return;
    if (shell_flyout_is_open()) {
        if (shell_flyout_click(x, y)) return;
        if (y < (int)fb_get_height() - TASKBAR_H) { shell_flyout_close(); return; }  /* click outside dismisses it */
    }
    if (button == 2 && taskbar_handle_right_click(x, y))
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
    shell_flyout_release();
    wm_handle_mouse_release(x, y, button);
    g_ds.dirty = 1;
}

/* The wheel goes to the window under the pointer (as on Windows 10/11),
 * one event per notch; delta > 0 = scroll up. */
void desktop_system_handle_mouse_wheel(int x, int y, int delta)
{
    g_last_input_tick = g_kstate.uptime_ticks;
    if (!g_ds.initialized || !ui_shell_is_desktop()) return;
    if (taskbar_is_start_menu_open() || desktop_context_is_open()) return;
    if (shell_flyout_wheel(x, y, delta)) { g_ds.dirty = 1; return; }
    wm_window_t *win = wm_window_at_point(x, y);
    if (!win || !win->on_event) return;
    int cx, cy, cw, ch;
    wm_get_client(win, &cx, &cy, &cw, &ch);
    wm_input_event_t ev;
    ev.type = IE_MOUSE_WHEEL;
    ev.timestamp = (uint32_t)g_kstate.uptime_ticks;
    ev.data.mouse.x = x - cx;
    ev.data.mouse.y = y - cy;
    int lines = setting_get_int("scroll_lines", 3);
    for (int i = 0; i < (delta < 0 ? -delta : delta); i++) {
        ev.data.mouse.button = delta > 0 ? lines : -lines;
        win->on_event(win, &ev);
    }
    g_ds.dirty = 1;
}

/* ---- Keyboard ------------------------------------------------------------ */
/* Global shortcuts. Returns 1 if the key was consumed. */
static int desktop_shortcut(uint32_t key, int mods)
{
    if (key == WM_KEY_PRTSC || ((mods & KMOD_SUPER) && (mods & KMOD_SHIFT) && lower((char)key) == 's')) {
        char saved[160];
        desktop_take_screenshot(saved, sizeof(saved));
        return 1;
    }
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
        case 'a': shell_flyout_toggle(FLY_QUICK);                      return 1;
        case 'n': shell_flyout_toggle(FLY_CALENDAR);                   return 1;
        case 'x': desktop_app_launch(desktop_app_find("Device Manager")); return 1;
        case 'r': desktop_app_launch(desktop_app_find("Run"));         return 1;
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
    g_last_input_tick = g_kstate.uptime_ticks;
    if (!g_ds.initialized || !event)
        return;

    if (!ui_shell_is_desktop()) {          /* sign-in or first-run setup screen */
        if (event->type == IE_KEY_DOWN)
            ui_shell_lock_input(event->data.keyboard.key, 0);
        else if (event->type == IE_TEXT_INPUT)
            ui_shell_lock_input(0, event->data.text.ch);
        g_ds.dirty = 1;
        return;
    }

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
        if (key == WM_KEY_ESC && (desktop_context_is_open() || shell_flyout_is_open() ||
                                  notifications_center_is_visible())) {
            shell_flyout_close();
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
