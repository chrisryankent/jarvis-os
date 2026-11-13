/*
 * =============================================================================
 * JARVIS OS — Apps (apps/store.c)
 * Language: C17
 *
 * The package manager window.
 *   Installed   apps installed from packages: open or uninstall them
 *   Available   .jpk packages found in Downloads, Documents, Desktop and on
 *               USB drives: install them
 * Opening a .jpk in Files shows the install confirmation here.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "../wm/wm.h"
#include "../desktop/ui.h"
#include <string.h>
#include <stdio.h>

#define ST_TAB_H   44
#define ST_ROW_H   64
#define ST_MAX     24

typedef struct { pkg_info_t info; char path[200]; } avail_t;

static struct {
    wm_window_t *window;
    int tab;                                /* 0 installed, 1 available */
    pkg_info_t installed[ST_MAX]; int installed_n;
    avail_t avail[ST_MAX]; int avail_n;
    uint32_t seen_pkg, seen_storage;
    int scroll;
    /* install confirmation */
    int confirm;
    char confirm_path[200];
    pkg_info_t confirm_info;
    char confirm_err[96];
} st;

static int store_open(void)
{
    desktop_app_t *app = desktop_app_get(desktop_app_find("Apps"));
    return app && app->window != NULL;
}

static void scan_folder(const char *dir)
{
    static char listing[8192];
    if (vfs_list(dir, listing, sizeof(listing)) < 0) return;
    size_t dlen = strlen(dir);
    for (const char *p = listing; *p && st.avail_n < ST_MAX;) {
        const char *eol = strchr(p, '\n');
        if (!eol) eol = p + strlen(p);
        const char *full = strchr(p, '/');
        size_t flen = full ? (size_t)(eol - full) : 0;
        if (!strncmp(p, "[file] ", 7) && full && flen > dlen + 5 && flen < 200 &&
            !strncmp(full, dir, dlen) && full[dlen] == '/' && !memchr(full + dlen + 1, '/', flen - dlen - 1) &&
            !strncmp(eol - 4, ".jpk", 4)) {
            avail_t *a = &st.avail[st.avail_n];
            memcpy(a->path, full, flen);
            a->path[flen] = 0;
            char err[8];
            if (pkg_inspect(a->path, &a->info, err, sizeof(err)) == 0) st.avail_n++;
        }
        p = *eol ? eol + 1 : eol;
    }
}

static void store_reload(void)
{
    st.installed_n = pkg_list(st.installed, ST_MAX);
    st.avail_n = 0;
    scan_folder("/jarvis/data/Downloads");
    scan_folder("/jarvis/data");
    scan_folder("/jarvis/desktop");
    drive_info_t drives[8];
    int n = storage_list(drives, 8);
    for (int i = 0; i < n; i++)
        if (drives[i].removable && drives[i].mount[0]) scan_folder(drives[i].mount);
    st.seen_pkg = g_pkg_generation;
    st.seen_storage = g_storage_generation;
}

/* Called by Files when a package is opened. */
void apps_store_show_install(const char *path)
{
    desktop_app_launch(desktop_app_find("Apps"));
    if (!store_open()) return;
    snprintf(st.confirm_path, sizeof(st.confirm_path), "%s", path);
    st.confirm_err[0] = 0;
    if (pkg_inspect(path, &st.confirm_info, st.confirm_err, sizeof(st.confirm_err)) != 0) {
        notification_show("Install", st.confirm_err, NOTIF_WARNING, 6000);
        return;
    }
    st.confirm = 1;
    desktop_invalidate();
}

static void do_install(const char *path)
{
    pkg_info_t info;
    char err[96], msg[128];
    if (pkg_install(path, &info, err, sizeof(err)) == 0) {
        snprintf(msg, sizeof(msg), "%s %s is installed. Find it in Start or under Installed.", info.title, info.version);
        notification_show("App installed", msg, NOTIF_INFO, 6000);
        st.tab = 0;
    } else {
        notification_show("Install failed", err, NOTIF_CRITICAL, 7000);
    }
    store_reload();
}

/* ---- Drawing ------------------------------------------------------------- */
static void draw_row(int x, int y, int w, const pkg_info_t *p, const char *b1, const char *b2, int hl)
{
    char line[160], size[24];
    ui_card(x, y, w, ST_ROW_H - 6);
    icon_draw(ICON_PROGRAM, x + 12, y + 12, 34);
    snprintf(line, sizeof(line), "%s  %s", p->title, p->version);
    ui_text_fit(x + 58, y + 8, w - 300, line, T(TEXT));
    if (p->bytes < 1024 * 1024) snprintf(size, sizeof(size), "%u KB", (unsigned)((p->bytes + 1023) / 1024));
    else snprintf(size, sizeof(size), "%u MB", (unsigned)(p->bytes >> 20));
    snprintf(line, sizeof(line), "%s%s%s", p->publisher[0] ? p->publisher : "Unknown publisher", "  -  ", size);
    ui_text_fit(x + 58, y + 26, w - 300, line, T(TEXT_DIM));
    ui_text_fit(x + 58, y + 42, w - 300, p->description, T(TEXT_DIM));
    if (b1) ui_button(x + w - 220, y + 14, 100, 30, b1, hl ? T(ACCENT) : T(HOVER), hl ? T(ACCENT_TEXT) : T(TEXT));
    if (b2) ui_button(x + w - 112, y + 14, 100, 30, b2, T(HOVER), T(TEXT));
}

static void draw_confirm(int x, int y, int w, int h)
{
    int pw = 440, ph = 230, px = x + (w - pw) / 2, py = y + (h - ph) / 2;
    pkg_info_t *p = &st.confirm_info;
    char line[200];

    fb_blend_rect(x, y, w, h, 0x00000000, 110);
    fb_fill_rounded_rect(px, py, pw, ph, 8, T(BORDER));
    fb_fill_rounded_rect(px + 1, py + 1, pw - 2, ph - 2, 7, T(MENU));
    icon_draw(ICON_PACKAGE, px + 20, py + 20, 40);
    snprintf(line, sizeof(line), "%s %s %s?", p->installed ? "Reinstall" : "Install", p->title, p->version);
    ui_text_fit(px + 76, py + 22, pw - 96, line, T(TEXT));
    snprintf(line, sizeof(line), "From %s", p->publisher[0] ? p->publisher : "an unknown publisher");
    ui_text_fit(px + 76, py + 42, pw - 96, line, T(TEXT_DIM));
    ui_text_fit(px + 20, py + 80, pw - 40, p->description, T(TEXT));
    snprintf(line, sizeof(line), "%u file(s) will be installed in /jarvis/apps/%s", p->files, p->name);
    ui_text_fit(px + 20, py + 108, pw - 40, line, T(TEXT_DIM));
    ui_text_fit(px + 20, py + 128, pw - 40, "Packages are not signed: only install apps you trust.", T(WARN));
    ui_button(px + pw - 220, py + ph - 48, 100, 32, p->installed ? "Reinstall" : "Install", T(ACCENT), T(ACCENT_TEXT));
    ui_button(px + pw - 112, py + ph - 48, 92, 32, "Cancel", T(HOVER), T(TEXT));
}

void store_draw_callback(wm_window_t *win, uint32_t *fb)
{
    (void)fb;
    int x, y, w, h;
    wm_get_client(win, &x, &y, &w, &h);
    if (st.seen_pkg != g_pkg_generation || st.seen_storage != g_storage_generation) store_reload();

    static const char *const tabs[2] = { "Installed", "Available" };
    fb_fill_rect(x, y, w, ST_TAB_H, T(SURFACE));
    fb_fill_rect(x, y + ST_TAB_H - 1, w, 1, T(BORDER));
    for (int i = 0; i < 2; i++) {
        int tx = x + 12 + i * 124;
        char label[32];
        snprintf(label, sizeof(label), "%s (%d)", tabs[i], i ? st.avail_n : st.installed_n);
        if (st.tab == i) {
            fb_fill_rounded_rect(tx, y + 8, 116, 28, 5, T(HOVER));
            fb_fill_rounded_rect(tx + 42, y + 33, 32, 3, 1, T(ACCENT));
        }
        ui_text_center(tx, y + 14, 116, label, T(TEXT));
    }

    fb_fill_rect(x, y + ST_TAB_H, w, h - ST_TAB_H, T(WINDOW));
    int ry = y + ST_TAB_H + 12;
    if (st.tab == 0) {
        for (int i = 0; i < st.installed_n && ry + ST_ROW_H < y + h; i++, ry += ST_ROW_H)
            draw_row(x + 12, ry, w - 24, &st.installed[i], "Open", "Uninstall", 1);
        if (!st.installed_n)
            ui_text_center(x, y + ST_TAB_H + 60, w, "No apps installed yet. See Available.", T(TEXT_DIM));
    } else {
        for (int i = 0; i < st.avail_n && ry + ST_ROW_H < y + h; i++, ry += ST_ROW_H)
            draw_row(x + 12, ry, w - 24, &st.avail[i].info,
                     st.avail[i].info.installed ? "Reinstall" : "Install", NULL, !st.avail[i].info.installed);
        if (!st.avail_n)
            ui_text_center(x, y + ST_TAB_H + 60, w, "No .jpk packages found in Downloads, Documents, Desktop or on USB drives.",
                           T(TEXT_DIM));
    }
    if (st.confirm) draw_confirm(x, y, w, h);
}

void store_event_callback(wm_window_t *win, wm_input_event_t *ev)
{
    int x, y, w, h;
    wm_get_client(win, &x, &y, &w, &h);
    if (ev->type == IE_KEY_DOWN && st.confirm) {
        if (ev->data.keyboard.key == WM_KEY_ESC) st.confirm = 0;
        else if (ev->data.keyboard.key == WM_KEY_ENTER) { st.confirm = 0; do_install(st.confirm_path); }
        return;
    }
    if (ev->type != IE_MOUSE_DOWN || ev->data.mouse.button != 1) return;
    int mx = ev->data.mouse.x, my = ev->data.mouse.y;

    if (st.confirm) {
        int pw = 440, ph = 230, px = (w - pw) / 2, py = (h - ph) / 2;
        if (ui_hit(mx, my, px + pw - 220, py + ph - 48, 100, 32)) { st.confirm = 0; do_install(st.confirm_path); }
        else if (ui_hit(mx, my, px + pw - 112, py + ph - 48, 92, 32)) st.confirm = 0;
        return;
    }
    if (my < ST_TAB_H) {
        for (int i = 0; i < 2; i++) if (ui_hit(mx, my, 12 + i * 124, 8, 116, 28)) st.tab = i;
        return;
    }
    int row = (my - ST_TAB_H - 12) / ST_ROW_H, ry = ST_TAB_H + 12 + row * ST_ROW_H;
    int bx1 = 12 + (w - 24) - 220, bx2 = 12 + (w - 24) - 112;
    if (st.tab == 0 && row >= 0 && row < st.installed_n) {
        pkg_info_t *p = &st.installed[row];
        if (ui_hit(mx, my, bx1, ry + 14, 100, 30)) {
            desktop_app_launch(desktop_app_find("Terminal"));
            if (!pkg_run(p->name)) notification_show(p->title, "The app could not be started.", NOTIF_WARNING, 4000);
        } else if (ui_hit(mx, my, bx2, ry + 14, 100, 30)) {
            char msg[96];
            snprintf(msg, sizeof(msg), pkg_uninstall(p->name) == 0 ? "%s was uninstalled." : "%s could not be uninstalled.", p->title);
            notification_show("Apps", msg, NOTIF_INFO, 4000);
            store_reload();
        }
    } else if (st.tab == 1 && row >= 0 && row < st.avail_n) {
        if (ui_hit(mx, my, bx1, ry + 14, 100, 30)) do_install(st.avail[row].path);
    }
}

wm_window_t *store_app_open(int x, int y, int w, int h)
{
    memset(&st, 0, sizeof(st));
    wm_window_t *win = wm_create_window("Apps", x, y, w, h, WS_DEFAULT, store_draw_callback, store_event_callback);
    if (!win) return NULL;
    win->icon = ICON_STORE;
    win->min_w = 560;
    win->min_h = 320;
    st.window = win;
    store_reload();
    st.tab = st.installed_n ? 0 : 1;
    return win;
}
