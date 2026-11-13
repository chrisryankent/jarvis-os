/*
 * =============================================================================
 * JARVIS OS — Notepad (apps/notepad.c)
 * Language: C17
 *
 * Plain-text editor for files in the VFS. Opened by double-clicking a file in
 * Files. Caret movement with the arrow keys / Home / End / mouse click,
 * insert and delete anywhere, Ctrl+S (or the Save button) writes the file.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "../wm/wm.h"
#include "../desktop/ui.h"
#include <string.h>
#include <stdio.h>

void file_explorer_refresh(void);           /* apps/file_explorer.c */

#define NP_MAX        16384
#define NP_TOOLBAR_H  40
#define NP_STATUS_H   24
#define NP_LINE_H     18

static struct {
    wm_window_t *window;
    char path[200];
    char text[NP_MAX];
    int len;
    int caret;              /* index into text[] */
    int scroll;             /* first visible visual row */
    int modified;
} g_np;

static int np_is_open(void)
{
    desktop_app_t *app = desktop_app_get(desktop_app_find("Notepad"));
    return app && app->window != NULL;
}

static void np_update_title(void)
{
    char title[64];
    const char *leaf = strrchr(g_np.path, '/');
    snprintf(title, sizeof(title), "%s%s - Notepad", g_np.modified ? "*" : "",
             g_np.path[0] ? (leaf ? leaf + 1 : g_np.path) : "Untitled");
    if (np_is_open())
        wm_set_title(g_np.window, title);
}

/* ---- Layout: text is wrapped at `cols` characters ------------------------ */
static void np_locate(int index, int cols, int *row, int *col)
{
    int r = 0, c = 0;
    for (int i = 0; i < index && i < g_np.len; i++) {
        if (g_np.text[i] == '\n') { r++; c = 0; }
        else {
            if (c == cols) { r++; c = 0; }
            c++;
        }
    }
    if (c == cols && index < g_np.len && g_np.text[index] != '\n') { r++; c = 0; }
    *row = r; *col = c;
}

/* Index of the character closest to visual (row, col). */
static int np_index_at(int row, int col, int cols)
{
    int r = 0, c = 0, i = 0;
    for (; i < g_np.len; i++) {
        if (r == row && c >= col) return i;
        if (g_np.text[i] == '\n') {
            if (r == row) return i;
            r++; c = 0;
        } else {
            if (c == cols) {
                if (r == row) return i;
                r++; c = 0;
            }
            c++;
        }
        if (r > row) return i;
    }
    return g_np.len;
}

static void np_text_area(wm_window_t *win, int *x, int *y, int *cols, int *rows)
{
    int cx, cy, cw, ch;
    wm_get_client(win, &cx, &cy, &cw, &ch);
    *x = cx + 10;
    *y = cy + NP_TOOLBAR_H + 6;
    *cols = (cw - 20) / 8;
    *rows = (ch - NP_TOOLBAR_H - NP_STATUS_H - 10) / NP_LINE_H;
    if (*cols < 8) *cols = 8;
    if (*rows < 1) *rows = 1;
}

/* ---- Editing ------------------------------------------------------------- */
static void np_insert(char ch)
{
    if (g_np.len >= NP_MAX - 1) return;
    memmove(&g_np.text[g_np.caret + 1], &g_np.text[g_np.caret],
            (size_t)(g_np.len - g_np.caret));
    g_np.text[g_np.caret++] = ch;
    g_np.text[++g_np.len] = 0;
    if (!g_np.modified) { g_np.modified = 1; np_update_title(); }
}

static void np_delete_at(int index)
{
    if (index < 0 || index >= g_np.len) return;
    memmove(&g_np.text[index], &g_np.text[index + 1], (size_t)(g_np.len - index - 1));
    g_np.text[--g_np.len] = 0;
    if (!g_np.modified) { g_np.modified = 1; np_update_title(); }
}

static void np_save(void)
{
    if (!g_np.path[0]) {
        /* A new document is saved into Data under the first free name. */
        for (int i = 1; i < 100; i++) {
            if (i == 1) snprintf(g_np.path, sizeof(g_np.path), "/jarvis/data/Untitled.txt");
            else        snprintf(g_np.path, sizeof(g_np.path), "/jarvis/data/Untitled (%d).txt", i);
            if (!vfs_exists(g_np.path)) break;
        }
    }

    if (vfs_write_file(g_np.path, g_np.text, (size_t)g_np.len) != 0) {
        notification_show("Notepad", "The file could not be saved.", NOTIF_CRITICAL, 5000);
        return;
    }
    g_np.modified = 0;
    np_update_title();
    file_explorer_refresh();
    desktop_files_changed();
}

/* ---- Drawing ------------------------------------------------------------- */
void notepad_draw_callback(wm_window_t *win, uint32_t *framebuffer)
{
    (void)framebuffer;
    int x, y, w, h, tx, ty, cols, rows, crow, ccol;
    wm_get_client(win, &x, &y, &w, &h);
    np_text_area(win, &tx, &ty, &cols, &rows);

    /* Toolbar */
    fb_fill_rect(x, y, w, NP_TOOLBAR_H, T(SURFACE));
    fb_fill_rect(x, y + NP_TOOLBAR_H - 1, w, 1, T(BORDER));
    ui_button(x + 8, y + 6, 64, 28, "Save",
              g_np.modified ? T(ACCENT) : T(HOVER),
              g_np.modified ? T(ACCENT_TEXT) : T(TEXT));
    ui_text_fit(x + 84, y + 12, w - 96, g_np.path[0] ? g_np.path : "New document (not saved yet)",
                T(TEXT_DIM));

    /* Keep the caret on screen */
    np_locate(g_np.caret, cols, &crow, &ccol);
    if (crow < g_np.scroll) g_np.scroll = crow;
    if (crow >= g_np.scroll + rows) g_np.scroll = crow - rows + 1;

    /* Text */
    fb_fill_rect(x, y + NP_TOOLBAR_H, w, h - NP_TOOLBAR_H - NP_STATUS_H, T(INPUT));
    int r = 0, c = 0;
    for (int i = 0; i < g_np.len; i++) {
        char ch = g_np.text[i];
        if (ch == '\n') { r++; c = 0; continue; }
        if (c == cols) { r++; c = 0; }
        if (r >= g_np.scroll + rows) break;
        if (r >= g_np.scroll && (unsigned char)ch >= 32)
            fb_draw_char(tx + c * 8, ty + (r - g_np.scroll) * NP_LINE_H, ch, T(TEXT));
        c++;
    }

    /* Caret (blinks while focused) */
    if (g_wm_ctx.focused == win && (g_kstate.uptime_ticks / 50) % 2 == 0)
        fb_fill_rect(tx + ccol * 8, ty + (crow - g_np.scroll) * NP_LINE_H, 2, 16, T(ACCENT));

    /* Status bar */
    int sy = y + h - NP_STATUS_H;
    char status[80];
    fb_fill_rect(x, sy, w, NP_STATUS_H, T(SURFACE));
    fb_fill_rect(x, sy, w, 1, T(BORDER));
    snprintf(status, sizeof(status), "Ln %d, Col %d    %d characters%s", crow + 1, ccol + 1,
             g_np.len, g_np.modified ? "    Modified (Ctrl+S to save)" : "");
    ui_text_fit(x + 10, sy + 4, w - 20, status, T(TEXT_DIM));
}

/* ---- Events -------------------------------------------------------------- */
void notepad_event_callback(wm_window_t *win, wm_input_event_t *event)
{
    int tx, ty, cols, rows, crow, ccol;
    np_text_area(win, &tx, &ty, &cols, &rows);

    if (event->type == IE_TEXT_INPUT) {
        char ch = event->data.text.ch;
        if (ch >= 32 && ch < 127) np_insert(ch);
        return;
    }

    if (event->type == IE_MOUSE_DOWN && event->data.mouse.button == 1) {
        int mx = event->data.mouse.x, my = event->data.mouse.y;
        if (my < NP_TOOLBAR_H) {
            if (ui_hit(mx, my, 8, 6, 64, 28)) np_save();
            return;
        }
        int row = g_np.scroll + (my - NP_TOOLBAR_H - 6) / NP_LINE_H;
        int col = (mx - 10 + 4) / 8;
        if (row < 0) row = 0;
        if (col < 0) col = 0;
        g_np.caret = np_index_at(row, col, cols);
        return;
    }

    if (event->type != IE_KEY_DOWN)
        return;

    uint32_t key = event->data.keyboard.key;
    int ctrl = event->data.keyboard.mods & KMOD_CTRL;
    np_locate(g_np.caret, cols, &crow, &ccol);

    if (ctrl && (key == 's' || key == 'S')) { np_save(); return; }

    switch (key) {
    case WM_KEY_ENTER:      np_insert('\n'); break;
    case WM_KEY_TAB:        for (int i = 0; i < 4; i++) np_insert(' '); break;
    case WM_KEY_BACKSPACE:  if (g_np.caret > 0) np_delete_at(--g_np.caret); break;
    case WM_KEY_DELETE:     np_delete_at(g_np.caret); break;
    case WM_KEY_LEFT:       if (g_np.caret > 0) g_np.caret--; break;
    case WM_KEY_RIGHT:      if (g_np.caret < g_np.len) g_np.caret++; break;
    case WM_KEY_UP:         if (crow > 0) g_np.caret = np_index_at(crow - 1, ccol, cols); break;
    case WM_KEY_DOWN:       g_np.caret = np_index_at(crow + 1, ccol, cols); break;
    case WM_KEY_HOME:       g_np.caret = np_index_at(crow, 0, cols); break;
    case WM_KEY_END:        g_np.caret = np_index_at(crow, cols, cols); break;
    default: break;
    }
}

/* ---- Public API ---------------------------------------------------------- */
wm_window_t *notepad_app_open(int x, int y, int w, int h)
{
    memset(&g_np, 0, sizeof(g_np));

    wm_window_t *win = wm_create_window("Untitled - Notepad", x, y, w, h, WS_DEFAULT,
                                        notepad_draw_callback, notepad_event_callback);
    if (!win) return NULL;

    win->icon = ICON_FILE;
    win->min_w = 320;
    win->min_h = 220;
    g_np.window = win;
    return win;
}

/* Opens (or focuses) Notepad and loads a file into it. */
void notepad_open_file(const char *path)
{
    desktop_app_launch(desktop_app_find("Notepad"));
    if (!np_is_open()) return;

    int n = vfs_read_file(path, g_np.text, NP_MAX);
    if (n < 0) {
        notification_show("Notepad", "The file could not be opened.", NOTIF_WARNING, 4000);
        return;
    }

    /* Binary data cannot be edited as text. */
    for (int i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)g_np.text[i];
        if (ch == '\r') g_np.text[i] = ' ';
        else if (ch != '\n' && ch != '\t' && (ch < 32 || ch > 126)) {
            g_np.text[0] = 0;
            g_np.len = 0;
            g_np.caret = 0;
            notification_show("Notepad", "This is not a text file and cannot be shown.",
                              NOTIF_WARNING, 4000);
            return;
        }
        if (ch == '\t') g_np.text[i] = ' ';
    }

    g_np.len = n;
    g_np.text[n] = 0;
    g_np.caret = 0;
    g_np.scroll = 0;
    g_np.modified = 0;
    strncpy(g_np.path, path, sizeof(g_np.path) - 1);
    g_np.path[sizeof(g_np.path) - 1] = 0;
    np_update_title();
}
