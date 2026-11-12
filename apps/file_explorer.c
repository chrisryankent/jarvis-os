/*
 * =============================================================================
 * JARVIS OS — File Explorer (apps/file_explorer.c)
 * Language: C17
 *
 * Browses the kernel VFS: toolbar with Up / Refresh and the current path, a
 * quick-access sidebar, a details list, and a preview line for text files.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "../wm/wm.h"
#include "../desktop/ui.h"
#include <string.h>
#include <stdio.h>

#define FX_MAX_FILES   128
#define FX_PATH_LEN    160
#define FX_NAME_LEN    64
#define FX_TOOLBAR_H   44
#define FX_SIDEBAR_W   150
#define FX_ROW_H       26
#define FX_HEADER_H    26
#define FX_STATUS_H    26

typedef struct {
    char name[FX_NAME_LEN];
    char path[FX_PATH_LEN];
    int is_dir;
} file_entry_t;

typedef struct {
    wm_window_t *window;
    char current_path[FX_PATH_LEN];
    file_entry_t files[FX_MAX_FILES];
    int file_count;
    int selected;
    int scroll;
    uint32_t last_click_tick;
    int last_click_idx;
    char preview[96];
} explorer_context_t;

static explorer_context_t g_explorer;
static char g_fx_listing[32768];

static const struct { const char *label; const char *path; } g_fx_places[] = {
    { "Home",        "/jarvis" },
    { "Data",        "/jarvis/data" },
    { "Models",      "/jarvis/models" },
    { "Logs",        "/jarvis/logs" },
    { "Screenshots", "/jarvis/screenshots" },
    { "Programs",    "/jarvis/bin" },
};
#define FX_PLACES ((int)(sizeof(g_fx_places) / sizeof(g_fx_places[0])))

/* ---- Directory loading ---------------------------------------------------
 * vfs_list() returns every path under a prefix, one per line, as
 * "[dir] /full/path" or "[file] /full/path". Keep only direct children.
 * (The previous parser expected "name|type|size" lines, which the VFS never
 * produced, so the explorer always showed an empty folder.)
 */
void explorer_refresh_directory(const char *path)
{
    if (!path || !*path) path = "/jarvis";

    char dir[FX_PATH_LEN];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;
    size_t dlen = strlen(dir);
    while (dlen > 1 && dir[dlen - 1] == '/')
        dir[--dlen] = 0;

    strncpy(g_explorer.current_path, dir, sizeof(g_explorer.current_path) - 1);
    g_explorer.current_path[sizeof(g_explorer.current_path) - 1] = 0;
    g_explorer.file_count = 0;
    g_explorer.selected = -1;
    g_explorer.scroll = 0;
    g_explorer.preview[0] = 0;
    g_explorer.last_click_idx = -1;

    if (vfs_list(dir, g_fx_listing, sizeof(g_fx_listing)) < 0)
        return;

    const char *p = g_fx_listing;
    while (*p && g_explorer.file_count < FX_MAX_FILES) {
        const char *eol = p;
        while (*eol && *eol != '\n') eol++;

        int is_dir = strncmp(p, "[dir] ", 6) == 0;
        int is_file = strncmp(p, "[file] ", 7) == 0;
        const char *full = p + (is_dir ? 6 : 7);
        size_t flen = (size_t)(eol - full);

        if ((is_dir || is_file) && flen > dlen + 1 && flen < FX_PATH_LEN &&
            strncmp(full, dir, dlen) == 0 && full[dlen] == '/') {
            const char *name = full + dlen + 1;
            size_t nlen = flen - dlen - 1;
            int direct = 1;
            for (size_t i = 0; i < nlen; i++)
                if (name[i] == '/') { direct = 0; break; }

            if (direct && nlen > 0 && nlen < FX_NAME_LEN) {
                file_entry_t *e = &g_explorer.files[g_explorer.file_count++];
                memcpy(e->name, name, nlen);
                e->name[nlen] = 0;
                memcpy(e->path, full, flen);
                e->path[flen] = 0;
                e->is_dir = is_dir;
            }
        }
        p = *eol ? eol + 1 : eol;
    }

    /* Folders first, then files; each group alphabetical. */
    for (int i = 1; i < g_explorer.file_count; i++) {
        file_entry_t key = g_explorer.files[i];
        int j = i - 1;
        while (j >= 0) {
            file_entry_t *o = &g_explorer.files[j];
            int after = (o->is_dir != key.is_dir) ? (key.is_dir > o->is_dir)
                                                  : (strcmp(key.name, o->name) < 0);
            if (!after) break;
            g_explorer.files[j + 1] = *o;
            j--;
        }
        g_explorer.files[j + 1] = key;
    }

    if (g_explorer.window) {
        char title[64];
        const char *leaf = strrchr(dir, '/');
        snprintf(title, sizeof(title), "%s - Files", (leaf && leaf[1]) ? leaf + 1 : dir);
        wm_set_title(g_explorer.window, title);
    }
    desktop_invalidate();
}

static void explorer_go_up(void)
{
    char parent[FX_PATH_LEN];
    strncpy(parent, g_explorer.current_path, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = 0;
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) {
        *slash = 0;
        explorer_refresh_directory(parent);
    }
}

static void explorer_select(int idx)
{
    g_explorer.selected = idx;
    g_explorer.preview[0] = 0;
    if (idx < 0 || idx >= g_explorer.file_count) return;

    file_entry_t *e = &g_explorer.files[idx];
    if (!e->is_dir) {
        char buf[96];
        int n = vfs_read_file(e->path, buf, sizeof(buf));
        if (n > 0) {
            int out = 0;
            for (int i = 0; i < n && out < (int)sizeof(g_explorer.preview) - 1; i++) {
                char c = buf[i];
                if (c == '\n' || c == '\r' || c == '\t') c = ' ';
                if ((unsigned char)c < 32 || (unsigned char)c > 126) {
                    strncpy(g_explorer.preview, "(binary file)", sizeof(g_explorer.preview) - 1);
                    return;
                }
                g_explorer.preview[out++] = c;
            }
            g_explorer.preview[out] = 0;
        } else {
            strncpy(g_explorer.preview, "(empty file)", sizeof(g_explorer.preview) - 1);
        }
    }
}

static void explorer_open(int idx)
{
    if (idx < 0 || idx >= g_explorer.file_count) return;
    file_entry_t *e = &g_explorer.files[idx];
    if (e->is_dir) {
        char path[FX_PATH_LEN];
        strncpy(path, e->path, sizeof(path) - 1);
        path[sizeof(path) - 1] = 0;
        explorer_refresh_directory(path);
    } else {
        notification_show(e->name, g_explorer.preview[0] ? g_explorer.preview
                          : "No preview available.", NOTIF_INFO, 5000);
    }
}

static int explorer_visible_rows(int client_h)
{
    int rows = (client_h - FX_TOOLBAR_H - FX_HEADER_H - FX_STATUS_H) / FX_ROW_H;
    return rows < 1 ? 1 : rows;
}

/* ---- Drawing ------------------------------------------------------------- */
void explorer_draw_callback(wm_window_t *win, uint32_t *framebuffer)
{
    (void)framebuffer;
    int x, y, w, h;
    wm_get_client(win, &x, &y, &w, &h);

    /* Toolbar */
    fb_fill_rect(x, y, w, FX_TOOLBAR_H, T(SURFACE));
    fb_fill_rect(x, y + FX_TOOLBAR_H - 1, w, 1, T(BORDER));
    fb_fill_rounded_rect(x + 8, y + 7, 30, 30, 5, T(HOVER));
    glyph_draw(GLYPH_UP, x + 15, y + 14, 16, T(TEXT), T(HOVER));
    fb_fill_rounded_rect(x + 44, y + 7, 30, 30, 5, T(HOVER));
    glyph_draw(GLYPH_REFRESH, x + 51, y + 14, 16, T(TEXT), T(HOVER));

    fb_fill_rounded_rect(x + 84, y + 7, w - 96, 30, 5, T(INPUT));
    icon_draw(ICON_FOLDER, x + 92, y + 14, 16);
    ui_text_fit(x + 116, y + 14, w - 140, g_explorer.current_path, T(TEXT));

    /* Sidebar */
    int body_y = y + FX_TOOLBAR_H;
    int body_h = h - FX_TOOLBAR_H - FX_STATUS_H;
    fb_fill_rect(x, body_y, FX_SIDEBAR_W, body_h, T(SURFACE));
    fb_fill_rect(x + FX_SIDEBAR_W - 1, body_y, 1, body_h, T(BORDER));
    fb_draw_text(x + 12, body_y + 10, "Quick access", T(TEXT_DIM));
    for (int i = 0; i < FX_PLACES; i++) {
        int iy = body_y + 34 + i * 28;
        if (strcmp(g_explorer.current_path, g_fx_places[i].path) == 0)
            fb_fill_rounded_rect(x + 6, iy, FX_SIDEBAR_W - 13, 26, 4, T(HOVER));
        icon_draw(ICON_FOLDER, x + 12, iy + 5, 16);
        fb_draw_text(x + 36, iy + 5, g_fx_places[i].label, T(TEXT));
    }

    /* List header */
    int lx = x + FX_SIDEBAR_W, lw = w - FX_SIDEBAR_W;
    fb_fill_rect(lx, body_y, lw, FX_HEADER_H, T(WINDOW));
    fb_fill_rect(lx, body_y + FX_HEADER_H - 1, lw, 1, T(BORDER));
    fb_draw_text(lx + 36, body_y + 5, "Name", T(TEXT_DIM));
    fb_draw_text(lx + lw - 110, body_y + 5, "Type", T(TEXT_DIM));

    /* Rows */
    int rows = explorer_visible_rows(h);
    int ry = body_y + FX_HEADER_H;
    for (int i = g_explorer.scroll; i < g_explorer.file_count && i < g_explorer.scroll + rows; i++) {
        file_entry_t *e = &g_explorer.files[i];
        uint32_t fg = T(TEXT);
        if (i == g_explorer.selected) {
            fb_fill_rounded_rect(lx + 4, ry + 1, lw - 8, FX_ROW_H - 2, 4, T(ACCENT));
            fg = T(ACCENT_TEXT);
        }
        icon_draw(e->is_dir ? ICON_FOLDER : ICON_FILE, lx + 12, ry + 5, 16);
        ui_text_fit(lx + 36, ry + 5, lw - 160, e->name, fg);
        fb_draw_text(lx + lw - 110, ry + 5, e->is_dir ? "Folder" : "File",
                     i == g_explorer.selected ? fg : T(TEXT_DIM));
        ry += FX_ROW_H;
    }
    if (g_explorer.file_count == 0)
        ui_text_center(lx, body_y + FX_HEADER_H + 30, lw, "This folder is empty.", T(TEXT_DIM));

    /* Status bar */
    int sy = y + h - FX_STATUS_H;
    char status[64];
    fb_fill_rect(x, sy, w, FX_STATUS_H, T(SURFACE));
    fb_fill_rect(x, sy, w, 1, T(BORDER));
    snprintf(status, sizeof(status), "%d item%s", g_explorer.file_count,
             g_explorer.file_count == 1 ? "" : "s");
    fb_draw_text(x + 12, sy + 5, status, T(TEXT_DIM));
    if (g_explorer.preview[0])
        ui_text_fit(x + 110, sy + 5, w - 122, g_explorer.preview, T(TEXT));
}

/* ---- Events -------------------------------------------------------------- */
void explorer_event_callback(wm_window_t *win, wm_input_event_t *event)
{
    int x, y, w, h;
    wm_get_client(win, &x, &y, &w, &h);
    int rows = explorer_visible_rows(h);

    if (event->type == IE_MOUSE_DOWN && event->data.mouse.button == 1) {
        int mx = event->data.mouse.x, my = event->data.mouse.y;

        if (my < FX_TOOLBAR_H) {
            if (ui_hit(mx, my, 8, 7, 30, 30))       explorer_go_up();
            else if (ui_hit(mx, my, 44, 7, 30, 30)) {
                char path[FX_PATH_LEN];
                strncpy(path, g_explorer.current_path, sizeof(path) - 1);
                path[sizeof(path) - 1] = 0;
                explorer_refresh_directory(path);
            }
            return;
        }
        if (my >= h - FX_STATUS_H) return;

        if (mx < FX_SIDEBAR_W) {
            int i = (my - FX_TOOLBAR_H - 34) / 28;
            if (my >= FX_TOOLBAR_H + 34 && i >= 0 && i < FX_PLACES)
                explorer_refresh_directory(g_fx_places[i].path);
            return;
        }

        int row = (my - FX_TOOLBAR_H - FX_HEADER_H);
        if (row < 0) return;
        int idx = g_explorer.scroll + row / FX_ROW_H;
        if (idx >= g_explorer.file_count) {
            explorer_select(-1);
            return;
        }

        int is_double = (idx == g_explorer.last_click_idx) &&
                        (event->timestamp - g_explorer.last_click_tick <= 50);
        g_explorer.last_click_idx = is_double ? -1 : idx;
        g_explorer.last_click_tick = event->timestamp;
        explorer_select(idx);
        if (is_double) explorer_open(idx);
        return;
    }

    if (event->type == IE_KEY_DOWN) {
        switch (event->data.keyboard.key) {
        case WM_KEY_UP:
            if (g_explorer.selected > 0) explorer_select(g_explorer.selected - 1);
            break;
        case WM_KEY_DOWN:
            if (g_explorer.selected < g_explorer.file_count - 1)
                explorer_select(g_explorer.selected + 1);
            break;
        case WM_KEY_ENTER:
            explorer_open(g_explorer.selected);
            break;
        case WM_KEY_BACKSPACE:
            explorer_go_up();
            break;
        default:
            break;
        }
        if (g_explorer.selected >= 0) {
            if (g_explorer.selected < g_explorer.scroll)
                g_explorer.scroll = g_explorer.selected;
            if (g_explorer.selected >= g_explorer.scroll + rows)
                g_explorer.scroll = g_explorer.selected - rows + 1;
        }
    }
}

/* ---- Public API ---------------------------------------------------------- */
wm_window_t *file_explorer_open(int x, int y, int w, int h)
{
    memset(&g_explorer, 0, sizeof(g_explorer));

    wm_window_t *win = wm_create_window("Files", x, y, w, h, WS_DEFAULT,
                                        explorer_draw_callback,
                                        explorer_event_callback);
    if (!win) return NULL;

    win->icon = ICON_FOLDER;
    win->min_w = 420;
    win->min_h = 260;
    g_explorer.window = win;
    explorer_refresh_directory("/jarvis");
    return win;
}

void file_explorer_navigate_to(const char *path)
{
    explorer_refresh_directory(path);
}
