/*
 * =============================================================================
 * JARVIS OS — Desktop surface (desktop/desktop.c)
 * Language: C17
 *
 * Wallpaper, desktop icons and the right-click context menu.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "../wm/wm.h"
#include "ui.h"
#include <string.h>

/* ---- Wallpaper -----------------------------------------------------------
 * Rendered once into a cache and copied into the back buffer every frame;
 * regenerated only when the theme changes.
 */
static uint32_t *g_wallpaper = NULL;
static int g_wallpaper_valid = 0;

void desktop_wallpaper_invalidate(void)
{
    g_wallpaper_valid = 0;
}

static void wallpaper_render(void)
{
    int W = (int)fb_get_width();
    int H = (int)fb_get_height();

    fb_reset_clip();
    fb_gradient_v(0, 0, W, H, T(WALL_TOP), T(WALL_BOTTOM));

    if (theme_get_mode() != THEME_HIGH_CONTRAST) {
        /* Soft "bloom" rising from the bottom centre, plus a faint glow in
         * the top-right corner. */
        uint32_t glow = (theme_get_mode() == THEME_LIGHT) ? 0x00FFFFFF : T(ACCENT);
        int cx = W / 2, cy = H + H / 6;
        static const int radii_pct[4] = { 95, 72, 52, 34 };
        for (int i = 0; i < 4; i++) {
            int r = (H * radii_pct[i]) / 100;
            fb_blend_rounded_rect(cx - r, cy - r, 2 * r, 2 * r, r, glow, 16);
        }
        int r2 = H / 2;
        fb_blend_rounded_rect(W - r2 / 2, -r2, 2 * r2, 2 * r2, r2, glow, 10);
    }
}

static void wallpaper_draw(void)
{
    if (!g_wallpaper)
        g_wallpaper = (uint32_t *)kmalloc_large((size_t)fb_get_width() *
                                                fb_get_height() * 4);

    if (!g_wallpaper) {             /* no memory for a cache: flat gradient */
        fb_gradient_v(0, 0, (int)fb_get_width(), (int)fb_get_height(),
                      T(WALL_TOP), T(WALL_BOTTOM));
        return;
    }

    if (!g_wallpaper_valid) {
        wallpaper_render();
        fb_save_frame(g_wallpaper);
        g_wallpaper_valid = 1;
        return;
    }
    fb_load_frame(g_wallpaper);
}

void file_explorer_refresh(void);               /* apps/file_explorer.c */
int  file_explorer_trash_path(const char *path);

/* ---- Desktop icons -------------------------------------------------------
 * The fixed system icons come first, followed by one icon for every file and
 * folder in /jarvis/desktop. Creating something on the desktop creates it in
 * that folder; it shows up here and in Files > Desktop.
 */
#define DICON_W      84
#define DICON_H      84
#define DICON_GAP    6
#define DICON_MAX    40
#define DESKTOP_DIR  "/jarvis/desktop"

typedef struct {
    char label[64];
    char target[200];       /* app name, or a path */
    icon_id_t icon;
    int is_file_item;       /* lives in DESKTOP_DIR (can be renamed / deleted) */
} desktop_icon_t;

static desktop_icon_t g_dicons[DICON_MAX];
static int g_dicon_count = 0;
static int g_dicon_sys_count = 0;
static int g_dicon_selected = -1;
static int g_dicon_last_click = -1;
static uint32_t g_dicon_last_time = 0;
static int g_dfiles_dirty = 1;
static char g_dlisting[8192];

/* Inline rename of a desktop file icon */
static struct { int idx; char buf[64]; int len; int fresh; } g_drename = { -1, "", 0, 0 };

void desktop_files_changed(void)
{
    g_dfiles_dirty = 1;
    desktop_invalidate();
}

void desktop_add_icon(const char *label, const char *target, icon_id_t icon)
{
    if (g_dicon_count >= DICON_MAX) return;
    desktop_icon_t *d = &g_dicons[g_dicon_count++];
    memset(d, 0, sizeof(*d));
    strncpy(d->label, label, sizeof(d->label) - 1);
    strncpy(d->target, target, sizeof(d->target) - 1);
    d->icon = icon;
}

void desktop_init(void)
{
    memset(g_dicons, 0, sizeof(g_dicons));
    g_dicon_count = 0;
    g_dicon_selected = -1;
    g_dicon_last_click = -1;
    g_drename.idx = -1;

    desktop_add_icon("This PC",     "::thispc",      ICON_PC);
    desktop_add_icon("Files",       "/jarvis",       ICON_FOLDER);
    desktop_add_icon("JARVIS",      "JARVIS",        ICON_JARVIS);
    desktop_add_icon("Terminal",    "Terminal",      ICON_TERMINAL);
    desktop_add_icon("Recycle Bin", "/jarvis/trash", ICON_TRASH);
    g_dicon_sys_count = g_dicon_count;
    g_dfiles_dirty = 1;
}

/* Re-read DESKTOP_DIR and rebuild the file icons. */
static void desktop_reload_files(void)
{
    char selected[200] = "";
    if (g_dicon_selected >= g_dicon_sys_count && g_dicon_selected < g_dicon_count)
        strncpy(selected, g_dicons[g_dicon_selected].target, sizeof(selected) - 1);

    g_dfiles_dirty = 0;
    g_dicon_count = g_dicon_sys_count;
    if (g_dicon_selected >= g_dicon_sys_count) g_dicon_selected = -1;
    if (g_drename.idx >= 0) g_drename.idx = -1;

    vfs_mkdir(DESKTOP_DIR);
    if (vfs_list(DESKTOP_DIR, g_dlisting, sizeof(g_dlisting)) < 0)
        return;

    size_t dlen = strlen(DESKTOP_DIR);
    for (const char *p = g_dlisting; *p && g_dicon_count < DICON_MAX;) {
        const char *eol = p;
        while (*eol && *eol != '\n') eol++;

        int is_dir = strncmp(p, "[dir] ", 6) == 0;
        int is_file = strncmp(p, "[file] ", 7) == 0;
        const char *full = p + (is_dir ? 6 : 7);
        size_t flen = (size_t)(eol - full);

        if ((is_dir || is_file) && flen > dlen + 1 && flen < sizeof(g_dicons[0].target) &&
            strncmp(full, DESKTOP_DIR, dlen) == 0 && full[dlen] == '/') {
            const char *name = full + dlen + 1;
            size_t nlen = flen - dlen - 1;
            int direct = 1;
            for (size_t i = 0; i < nlen; i++)
                if (name[i] == '/') { direct = 0; break; }

            if (direct && nlen > 0 && nlen < sizeof(g_dicons[0].label)) {
                desktop_icon_t *d = &g_dicons[g_dicon_count];
                memset(d, 0, sizeof(*d));
                memcpy(d->label, name, nlen);
                memcpy(d->target, full, flen);
                d->icon = is_dir ? ICON_FOLDER : ICON_FILE;
                d->is_file_item = 1;
                if (selected[0] && strcmp(selected, d->target) == 0)
                    g_dicon_selected = g_dicon_count;
                g_dicon_count++;
            }
        }
        p = *eol ? eol + 1 : eol;
    }
}

static void dicon_rect(int i, int *x, int *y)
{
    int rows = ((int)fb_get_height() - 48 - 16) / (DICON_H + DICON_GAP);
    if (rows < 1) rows = 1;
    *x = 12 + (i / rows) * (DICON_W + DICON_GAP);
    *y = 12 + (i % rows) * (DICON_H + DICON_GAP);
}

void desktop_draw(void)
{
    if (g_dfiles_dirty)
        desktop_reload_files();

    wallpaper_draw();

    for (int i = 0; i < g_dicon_count; i++) {
        int x, y;
        dicon_rect(i, &x, &y);

        if (i == g_dicon_selected) {
            fb_blend_rounded_rect(x, y, DICON_W, DICON_H, 6, T(ACCENT), 60);
            fb_draw_rounded_rect(x, y, DICON_W, DICON_H, 6, T(ACCENT));
        }
        icon_draw(g_dicons[i].icon, x + (DICON_W - 40) / 2, y + 8, 40);

        if (i == g_drename.idx) {
            /* edit box, wide enough for the name being typed */
            int bw = g_drename.len * 8 + 14;
            if (bw < DICON_W + 20) bw = DICON_W + 20;
            int bx = x + (DICON_W - bw) / 2;
            if (bx < 2) bx = 2;
            fb_fill_rect(bx, y + 53, bw, 22, T(INPUT));
            fb_draw_rect(bx, y + 53, bw, 22, T(ACCENT));
            if (g_drename.fresh)
                fb_fill_rect(bx + 5, y + 56, g_drename.len * 8 + 2, 16, T(ACCENT));
            fb_draw_text(bx + 6, y + 56, g_drename.buf,
                         g_drename.fresh ? T(ACCENT_TEXT) : T(TEXT));
            if (!g_drename.fresh)
                fb_fill_rect(bx + 7 + g_drename.len * 8, y + 56, 1, 16, T(TEXT));
            continue;
        }

        /* Label (shortened to fit) with a 1px shadow so it stays readable
         * on any wallpaper */
        char label[16];
        size_t n = strlen(g_dicons[i].label);
        if (n > 11) {
            memcpy(label, g_dicons[i].label, 9);
            label[9] = '.'; label[10] = '.'; label[11] = 0;
        } else {
            memcpy(label, g_dicons[i].label, n + 1);
        }
        int tx = x + (DICON_W - fb_text_width(label)) / 2;
        fb_draw_text(tx + 1, y + 57, label, 0x00000000);
        fb_draw_text(tx, y + 56, label, 0x00FFFFFF);
    }
}

/* ---- Inline rename ------------------------------------------------------- */
int desktop_rename_active(void)
{
    return g_drename.idx >= 0;
}

static void desktop_rename_start(int idx)
{
    if (idx < g_dicon_sys_count || idx >= g_dicon_count) return;
    g_drename.idx = idx;
    strncpy(g_drename.buf, g_dicons[idx].label, sizeof(g_drename.buf) - 1);
    g_drename.buf[sizeof(g_drename.buf) - 1] = 0;
    g_drename.len = (int)strlen(g_drename.buf);
    g_drename.fresh = 1;
    g_dicon_selected = idx;
    desktop_invalidate();
}

void desktop_rename_finish(int commit)
{
    int idx = g_drename.idx;
    if (idx < 0) return;
    g_drename.idx = -1;
    desktop_invalidate();

    if (!commit || idx >= g_dicon_count || g_drename.len == 0 ||
        strcmp(g_drename.buf, g_dicons[idx].label) == 0)
        return;

    char target[200];
    snprintf(target, sizeof(target), "%s/%s", DESKTOP_DIR, g_drename.buf);
    if (vfs_exists(target) || vfs_rename(g_dicons[idx].target, target) != 0) {
        notification_show("Rename", "That name is already in use, or is not valid.",
                          NOTIF_WARNING, 4000);
        return;
    }
    strncpy(g_dicons[idx].target, target, sizeof(g_dicons[idx].target) - 1);
    desktop_files_changed();
    file_explorer_refresh();
}

/* Keyboard while renaming. key is a WM_KEY_* code (0 for a text character). */
void desktop_rename_input(uint32_t key, char ch)
{
    if (g_drename.idx < 0) return;

    if (ch >= 32 && ch < 127 && ch != '/') {
        if (g_drename.fresh) { g_drename.len = 0; g_drename.fresh = 0; }
        if (g_drename.len < (int)sizeof(g_drename.buf) - 1) {
            g_drename.buf[g_drename.len++] = ch;
            g_drename.buf[g_drename.len] = 0;
        }
    } else if (key == WM_KEY_ENTER) {
        desktop_rename_finish(1);
    } else if (key == WM_KEY_ESC) {
        desktop_rename_finish(0);
    } else if (key == WM_KEY_BACKSPACE) {
        if (g_drename.fresh) g_drename.len = 0;
        else if (g_drename.len > 0) g_drename.len--;
        g_drename.buf[g_drename.len] = 0;
        g_drename.fresh = 0;
    } else if (key == WM_KEY_RIGHT || key == WM_KEY_END) {
        g_drename.fresh = 0;
    }
    desktop_invalidate();
}

/*
 * Returns the icon index on a completed double-click, -2 if an icon was
 * clicked once (selected), -1 if the click missed every icon.
 */
int desktop_handle_click(int x, int y)
{
    for (int i = 0; i < g_dicon_count; i++) {
        int ix, iy;
        dicon_rect(i, &ix, &iy);
        if (!ui_hit(x, y, ix, iy, DICON_W, DICON_H)) continue;

        uint32_t now = (uint32_t)g_kstate.uptime_ticks;
        int is_double = (g_dicon_last_click == i) && (now - g_dicon_last_time <= 50);

        g_dicon_selected = i;
        g_dicon_last_click = is_double ? -1 : i;
        g_dicon_last_time = now;
        return is_double ? i : -2;
    }
    g_dicon_selected = -1;
    g_dicon_last_click = -1;
    return -1;
}

const char *desktop_get_icon_target(int i)
{
    if (i < 0 || i >= g_dicon_count) return "";
    return g_dicons[i].target;
}

/* Deletes the selected desktop file icon (to the Recycle Bin). */
void desktop_delete_selected(void)
{
    if (g_dicon_selected < g_dicon_sys_count || g_dicon_selected >= g_dicon_count)
        return;
    if (file_explorer_trash_path(g_dicons[g_dicon_selected].target) != 0)
        notification_show("Delete", "The item could not be moved to the Recycle Bin.",
                          NOTIF_WARNING, 4000);
    g_dicon_selected = -1;
}

/* ==========================================================================
 * POPUP MENU
 *
 * One menu can be open at a time, anywhere on screen. Used for the desktop's
 * right-click menu and by apps (Files). The owner supplies the items and a
 * callback that receives the chosen item's id.
 * ========================================================================== */
#define MENU_ITEM_H   30
#define MENU_SEP_H    9
#define MENU_MAX      12

static struct {
    int open;
    int x, y, w, h;
    ui_menu_item_t items[MENU_MAX];
    int count;
    int hover;
    ui_menu_cb callback;
    void *ctx;
} g_menu;

static int menu_item_y(int index)
{
    int y = g_menu.y + 4;
    for (int i = 0; i <= index && i < g_menu.count; i++) {
        if (g_menu.items[i].separator_before) y += MENU_SEP_H;
        if (i < index) y += MENU_ITEM_H;
    }
    return y;
}

void ui_menu_open(int x, int y, const ui_menu_item_t *items, int count,
                  ui_menu_cb callback, void *ctx)
{
    if (count > MENU_MAX) count = MENU_MAX;
    if (count <= 0) return;

    int widest = 0;
    g_menu.count = count;
    for (int i = 0; i < count; i++) {
        g_menu.items[i] = items[i];
        int tw = fb_text_width(items[i].label);
        if (tw > widest) widest = tw;
    }
    g_menu.w = widest + 44;
    if (g_menu.w < 180) g_menu.w = 180;

    g_menu.x = 0; g_menu.y = 0;
    g_menu.h = menu_item_y(count - 1) + MENU_ITEM_H + 4;

    int W = (int)fb_get_width(), H = (int)fb_get_height() - TASKBAR_H;
    if (x + g_menu.w > W) x = W - g_menu.w;
    if (y + g_menu.h > H) y = H - g_menu.h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    g_menu.x = x; g_menu.y = y;

    g_menu.callback = callback;
    g_menu.ctx = ctx;
    g_menu.hover = -1;
    g_menu.open = 1;
    desktop_invalidate();
}

int desktop_context_is_open(void) { return g_menu.open; }

void desktop_context_close(void)
{
    if (g_menu.open) {
        g_menu.open = 0;
        desktop_invalidate();
    }
}

static int menu_item_at(int x, int y)
{
    if (!g_menu.open || x < g_menu.x || x >= g_menu.x + g_menu.w) return -1;
    for (int i = 0; i < g_menu.count; i++) {
        int iy = menu_item_y(i);
        if (y >= iy && y < iy + MENU_ITEM_H)
            return g_menu.items[i].disabled ? -1 : i;
    }
    return -1;
}

/* Returns 1 if the hover highlight changed. */
int desktop_context_hover(int x, int y)
{
    if (!g_menu.open) return 0;
    int item = menu_item_at(x, y);
    if (item == g_menu.hover) return 0;
    g_menu.hover = item;
    return 1;
}

void desktop_context_draw(void)
{
    if (!g_menu.open) return;

    int x = g_menu.x, y = g_menu.y, w = g_menu.w, h = g_menu.h;
    fb_blend_rounded_rect(x + 2, y + 4, w, h, 8, 0x00000000, 70);
    fb_fill_rounded_rect(x, y, w, h, 8, T(BORDER));
    fb_fill_rounded_rect(x + 1, y + 1, w - 2, h - 2, 7, T(MENU));

    for (int i = 0; i < g_menu.count; i++) {
        int iy = menu_item_y(i);
        if (g_menu.items[i].separator_before)
            fb_fill_rect(x + 10, iy - MENU_SEP_H / 2 - 1, w - 20, 1, T(BORDER));
        if (i == g_menu.hover)
            fb_fill_rounded_rect(x + 4, iy + 1, w - 8, MENU_ITEM_H - 2, 4, T(HOVER));
        fb_draw_text(x + 16, iy + 7, g_menu.items[i].label,
                     g_menu.items[i].disabled ? T(TEXT_DIM) : T(TEXT));
    }
}

/* Any click while a menu is open closes it; a click on an item runs it.
 * Returns 1 if a menu was open (the click is consumed). */
int desktop_context_click(int x, int y)
{
    if (!g_menu.open) return 0;

    int item = menu_item_at(x, y);
    g_menu.open = 0;
    desktop_invalidate();

    if (item >= 0 && g_menu.callback)
        g_menu.callback(g_menu.items[item].id, g_menu.ctx);
    return 1;
}

/* ---- Desktop right-click menus ------------------------------------------- */
enum {
    CTX_REFRESH = 1, CTX_OPEN_TERMINAL, CTX_OPEN_FILES, CTX_ASK_JARVIS,
    CTX_TOGGLE_THEME, CTX_SETTINGS, CTX_SHOW_DESKTOP, CTX_NEW_FOLDER,
    CTX_NEW_TEXT, CTX_ICON_OPEN, CTX_EMPTY_TRASH, CTX_ICON_RENAME, CTX_ICON_DELETE,
};

void desktop_open_icon(int icon);                       /* desktop_init.c */
int  file_explorer_empty_trash(void);                   /* apps/file_explorer.c */
int  file_explorer_create_in(const char *dir, int folder, char *out, size_t n);
void file_explorer_refresh(void);

/* Create a new item on the desktop itself and start naming it. */
static void desktop_new_item(int folder)
{
    char path[200];
    if (file_explorer_create_in(DESKTOP_DIR, folder, path, sizeof(path)) != 0) {
        notification_show("New", "The item could not be created.", NOTIF_WARNING, 4000);
        return;
    }
    desktop_reload_files();
    for (int i = g_dicon_sys_count; i < g_dicon_count; i++)
        if (strcmp(g_dicons[i].target, path) == 0)
            desktop_rename_start(i);
}

static int g_ctx_icon = -1;

static void desktop_menu_action(int id, void *ctx)
{
    (void)ctx;
    switch (id) {
    case CTX_REFRESH:       desktop_wallpaper_invalidate(); break;
    case CTX_SHOW_DESKTOP:  wm_minimize_all(); break;
    case CTX_OPEN_TERMINAL: desktop_app_launch(desktop_app_find("Terminal")); break;
    case CTX_OPEN_FILES:    desktop_app_launch(desktop_app_find("Files")); break;
    case CTX_ASK_JARVIS:    desktop_app_launch(desktop_app_find("JARVIS")); break;
    case CTX_SETTINGS:      desktop_app_launch(desktop_app_find("Settings")); break;
    case CTX_TOGGLE_THEME:
        theme_set_mode(theme_get_mode() == THEME_DARK ? THEME_LIGHT : THEME_DARK);
        break;
    case CTX_NEW_FOLDER:    desktop_new_item(1); break;
    case CTX_NEW_TEXT:      desktop_new_item(0); break;
    case CTX_ICON_RENAME:   desktop_rename_start(g_ctx_icon); break;
    case CTX_ICON_DELETE:   g_dicon_selected = g_ctx_icon; desktop_delete_selected(); break;
    case CTX_ICON_OPEN:     desktop_open_icon(g_ctx_icon); break;
    case CTX_EMPTY_TRASH:   file_explorer_empty_trash(); break;
    }
}

/* Index of the desktop icon under (x,y), or -1. */
int desktop_icon_at(int x, int y)
{
    for (int i = 0; i < g_dicon_count; i++) {
        int ix, iy;
        dicon_rect(i, &ix, &iy);
        if (ui_hit(x, y, ix, iy, DICON_W, DICON_H)) return i;
    }
    return -1;
}

void desktop_context_open(int x, int y)
{
    int icon = desktop_icon_at(x, y);

    if (icon >= 0) {
        ui_menu_item_t items[4] = { { "Open", CTX_ICON_OPEN, 0, 0 } };
        int n = 1;
        g_ctx_icon = icon;
        g_dicon_selected = icon;
        if (g_dicons[icon].is_file_item) {
            items[n++] = (ui_menu_item_t){ "Rename", CTX_ICON_RENAME, 0, 1 };
            items[n++] = (ui_menu_item_t){ "Delete", CTX_ICON_DELETE, 0, 0 };
        }
        if (strcmp(g_dicons[icon].target, "/jarvis/trash") == 0)
            items[n++] = (ui_menu_item_t){ "Empty Recycle Bin", CTX_EMPTY_TRASH, 0, 1 };
        ui_menu_open(x, y, items, n, desktop_menu_action, NULL);
        return;
    }

    static const ui_menu_item_t items[] = {
        { "Refresh",              CTX_REFRESH,       0, 0 },
        { "Show desktop",         CTX_SHOW_DESKTOP,  0, 0 },
        { "New folder",           CTX_NEW_FOLDER,    0, 1 },
        { "New text document",    CTX_NEW_TEXT,      0, 0 },
        { "Open Terminal",        CTX_OPEN_TERMINAL, 0, 1 },
        { "Open Files",           CTX_OPEN_FILES,    0, 0 },
        { "Ask JARVIS",           CTX_ASK_JARVIS,    0, 0 },
        { "Switch light / dark",  CTX_TOGGLE_THEME,  0, 1 },
        { "Personalize",          CTX_SETTINGS,      0, 0 },
    };
    g_dicon_selected = -1;
    ui_menu_open(x, y, items, (int)(sizeof(items) / sizeof(items[0])),
                 desktop_menu_action, NULL);
}
