/*
 * =============================================================================
 * JARVIS OS — Notepad (apps/notepad.c)
 * Language: C17
 *
 * A plain-text editor.
 *
 *   File    New (Ctrl+N), Open (Ctrl+O), Save (Ctrl+S), Save As
 *           (Ctrl+Shift+S), Exit. Asks before discarding unsaved changes,
 *           including when the window is closed.
 *   Edit    Undo (Ctrl+Z), Redo (Ctrl+Y), Cut / Copy / Paste (Ctrl+X/C/V,
 *           shared clipboard), Delete, Find (Ctrl+F), Find next (F3),
 *           Replace (Ctrl+H), Go to line (Ctrl+G), Select all (Ctrl+A),
 *           Time/Date (F5)
 *   View    Word wrap, Status bar
 *   Text    mouse click / drag / double-click (word) selection, Shift+arrows,
 *           Ctrl+arrows (word jumps), Home/End, Ctrl+Home/End, Page Up/Down,
 *           Tab characters, scrollbar
 *
 * Windows files (CRLF line endings) are shown normally and saved back with
 * CRLF. Files on USB drives open read-only: Save offers Save As.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "../wm/wm.h"
#include "../desktop/ui.h"
#include <string.h>
#include <stdio.h>

void file_explorer_refresh(void);           /* apps/file_explorer.c */
void desktop_files_changed(void);           /* desktop/desktop.c    */

#define NP_MAX_TEXT   (4u << 20)            /* 4 MB */
#define NP_MENU_H     28
#define NP_STATUS_H   24
#define NP_FIND_H     40
#define NP_SCROLL_W   12
#define NP_LINE_H     18
#define NP_PAD        8
#define NP_TAB        4
#define NP_UNDO_MAX   200
#define NP_PATH_LEN   200

/* ---- Allocation: small buffers from the heap, large ones from pages ---- */
static void *np_alloc(size_t n)
{
    return n >= LARGE_ALLOC_THRESHOLD ? kmalloc_large(n) : kmalloc(n);
}

static void np_free(void *p, size_t n)
{
    if (!p) return;
    if (n >= LARGE_ALLOC_THRESHOLD) kfree_large(p, n); else kfree(p);
}

/* ---- Undo records: replace `del` at pos with `ins` ------------------------ */
typedef struct {
    int    pos;
    char  *del; int del_len;
    char  *ins; int ins_len;
    int    sel_before;                      /* caret before the edit */
} np_op_t;

enum { DLG_NONE, DLG_OPEN, DLG_SAVEAS, DLG_UNSAVED, DLG_GOTO };
enum { AFTER_NOTHING, AFTER_NEW, AFTER_OPEN_DIALOG, AFTER_OPEN_PATH, AFTER_CLOSE };

static struct {
    wm_window_t *window;

    char  *text; size_t len, cap;
    int    caret, anchor;                   /* selection = [min, max) of the two */
    int    goal_x;                          /* column kept while moving up/down  */
    int    modified, crlf, read_only, ro_warned;
    char   path[NP_PATH_LEN];

    /* layout */
    int   *rows; int row_count, row_cap;    /* index where each visual row starts */
    int    layout_cols, layout_dirty;
    int    scroll_row, scroll_x;
    int    free_scroll;         /* wheel moved the view away from the caret */
    int    wrap, status_bar;
    int    dragging, scroll_dragging;
    uint32_t last_click_tick; int last_click_pos;

    /* undo / redo */
    np_op_t undo[NP_UNDO_MAX]; int undo_n;
    np_op_t redo[NP_UNDO_MAX]; int redo_n;
    uint64_t last_edit_tick;

    /* find / replace bar */
    int    find_open, replace_mode, find_field;   /* field 0 = find, 1 = replace */
    char   find[64]; int find_len;
    char   repl[64]; int repl_len;
    int    match_case;

    /* dialogs */
    int    dialog, after;
    char   after_path[NP_PATH_LEN];
    char   dlg_dir[NP_PATH_LEN];
    char   dlg_name[96]; int dlg_name_len; int dlg_fresh;   /* fresh: typing replaces the name */
    char   dlg_items[64][96]; int dlg_is_dir[64]; int dlg_count, dlg_sel, dlg_scroll;
    char   dlg_error[96];
    uint32_t dlg_click_tick; int dlg_click_idx;
} np;

static int np_is_open(void)
{
    desktop_app_t *app = desktop_app_get(desktop_app_find("Notepad"));
    return app && app->window != NULL;
}

static int sel_start(void) { return np.caret < np.anchor ? np.caret : np.anchor; }
static int sel_end(void)   { return np.caret < np.anchor ? np.anchor : np.caret; }
static int has_sel(void)   { return np.caret != np.anchor; }

static const char *doc_name(void)
{
    if (!np.path[0]) return "Untitled";
    const char *s = strrchr(np.path, '/');
    return s ? s + 1 : np.path;
}

static void update_title(void)
{
    char title[96];
    snprintf(title, sizeof(title), "%s%s - Notepad", np.modified ? "*" : "", doc_name());
    if (np_is_open()) wm_set_title(np.window, title);
}

static void np_toast(const char *msg)
{
    notification_show("Notepad", msg, NOTIF_WARNING, 5000);
}

/* ==========================================================================
 * Text buffer
 * ========================================================================== */
static int ensure_cap(size_t need)
{
    if (need + 1 <= np.cap) return 0;
    if (need > NP_MAX_TEXT) return -1;
    size_t cap = np.cap ? np.cap : 16384;
    while (cap < need + 1) cap *= 2;
    char *t = (char *)np_alloc(cap);
    if (!t) return -1;
    if (np.text) { memcpy(t, np.text, np.len + 1); np_free(np.text, np.cap); }
    else t[0] = 0;
    np.text = t;
    np.cap = cap;
    return 0;
}

static char *dup_range(int pos, int n)
{
    char *p = (char *)np_alloc((size_t)n + 1);
    if (!p) return NULL;
    memcpy(p, np.text + pos, (size_t)n);
    p[n] = 0;
    return p;
}

static void op_free(np_op_t *o)
{
    np_free(o->del, (size_t)o->del_len + 1);
    np_free(o->ins, (size_t)o->ins_len + 1);
    memset(o, 0, sizeof(*o));
}

static void stack_clear(np_op_t *s, int *n)
{
    for (int i = 0; i < *n; i++) op_free(&s[i]);
    *n = 0;
}

static void stack_push(np_op_t *s, int *n, np_op_t *o)
{
    if (*n == NP_UNDO_MAX) {                 /* drop the oldest */
        op_free(&s[0]);
        memmove(&s[0], &s[1], sizeof(np_op_t) * (NP_UNDO_MAX - 1));
        (*n)--;
    }
    s[(*n)++] = *o;
}

/* Raw replace without undo bookkeeping. 0 on success. */
static int raw_replace(int pos, int del_len, const char *ins, int ins_len)
{
    if (pos < 0 || del_len < 0 || pos + del_len > (int)np.len) return -1;
    size_t new_len = np.len - (size_t)del_len + (size_t)ins_len;
    if (ensure_cap(new_len) != 0) return -1;
    memmove(np.text + pos + ins_len, np.text + pos + del_len, np.len - (size_t)(pos + del_len) + 1);
    memcpy(np.text + pos, ins, (size_t)ins_len);
    np.len = new_len;
    np.layout_dirty = 1;
    return 0;
}

/* The one way text changes: replaces [pos, pos+del_len) with ins, records
 * undo (merging consecutive typing), moves the caret after the insertion. */
static void edit(int pos, int del_len, const char *ins, int ins_len)
{
    if (np.read_only && !np.ro_warned) {
        np.ro_warned = 1;
        np_toast("This file is on a read-only drive: use Save As to keep your changes.");
    }
    np_op_t *last = np.undo_n ? &np.undo[np.undo_n - 1] : NULL;
    int typing = ins_len == 1 && del_len == 0 && ins[0] != '\n' && ins[0] != ' ';
    int merge = typing && last && last->del_len == 0 && last->ins_len < 512 &&
                last->pos + last->ins_len == pos && g_kstate.uptime_ticks - np.last_edit_tick < 150;

    if (merge) {
        char *grown = (char *)np_alloc((size_t)last->ins_len + 2);
        if (!grown) return;
        memcpy(grown, last->ins, (size_t)last->ins_len);
        grown[last->ins_len] = ins[0];
        grown[last->ins_len + 1] = 0;
        if (raw_replace(pos, 0, ins, 1) != 0) { np_free(grown, (size_t)last->ins_len + 2); np_toast("The document is too large."); return; }
        np_free(last->ins, (size_t)last->ins_len + 1);
        last->ins = grown;
        last->ins_len++;
    } else {
        np_op_t o = { pos, dup_range(pos, del_len), del_len, NULL, ins_len, np.caret };
        o.ins = (char *)np_alloc((size_t)ins_len + 1);
        if (!o.del || !o.ins) { op_free(&o); return; }
        memcpy(o.ins, ins, (size_t)ins_len);
        o.ins[ins_len] = 0;
        if (raw_replace(pos, del_len, ins, ins_len) != 0) { op_free(&o); np_toast("The document is too large."); return; }
        stack_push(np.undo, &np.undo_n, &o);
    }
    stack_clear(np.redo, &np.redo_n);
    np.last_edit_tick = g_kstate.uptime_ticks;
    np.caret = np.anchor = pos + ins_len;
    np.goal_x = -1;
    if (!np.modified) { np.modified = 1; update_title(); }
}

static void replace_selection(const char *ins, int ins_len)
{
    edit(sel_start(), sel_end() - sel_start(), ins, ins_len);
}

static void undo(void)
{
    if (!np.undo_n) return;
    np_op_t o = np.undo[--np.undo_n];
    raw_replace(o.pos, o.ins_len, o.del ? o.del : "", o.del_len);
    np.caret = np.anchor = o.pos + o.del_len;
    stack_push(np.redo, &np.redo_n, &o);
    np.modified = 1; np.last_edit_tick = 0;
    update_title();
}

static void redo(void)
{
    if (!np.redo_n) return;
    np_op_t o = np.redo[--np.redo_n];
    raw_replace(o.pos, o.del_len, o.ins ? o.ins : "", o.ins_len);
    np.caret = np.anchor = o.pos + o.ins_len;
    stack_push(np.undo, &np.undo_n, &o);
    np.modified = 1; np.last_edit_tick = 0;
    update_title();
}

static void copy_sel(void)
{
    if (has_sel()) clipboard_set(np.text + sel_start(), (size_t)(sel_end() - sel_start()));
}

static void paste(void)
{
    size_t n;
    const char *c = clipboard_get(&n);
    if (n) replace_selection(c, (int)n);
}

/* ==========================================================================
 * Layout: visual rows (with optional word wrap and tab stops)
 * ========================================================================== */
static int text_cols(int w)
{
    int c = (w - 2 * NP_PAD - NP_SCROLL_W) / 8;
    return c < 8 ? 8 : c;
}

static int text_rows(int h)
{
    int r = (h - NP_MENU_H - (np.find_open ? NP_FIND_H : 0) - (np.status_bar ? NP_STATUS_H : 0) - 2 * 4) / NP_LINE_H;
    return r < 1 ? 1 : r;
}

static int push_row(int start)
{
    if (np.row_count == np.row_cap) {
        int cap = np.row_cap ? np.row_cap * 2 : 1024;
        int *r = (int *)np_alloc((size_t)cap * sizeof(int));
        if (!r) return -1;
        if (np.rows) { memcpy(r, np.rows, (size_t)np.row_count * sizeof(int)); np_free(np.rows, (size_t)np.row_cap * sizeof(int)); }
        np.rows = r;
        np.row_cap = cap;
    }
    np.rows[np.row_count++] = start;
    return 0;
}

static void layout(int cols)
{
    if (!np.layout_dirty && np.layout_cols == cols) return;
    np.layout_dirty = 0;
    np.layout_cols = cols;
    np.row_count = 0;
    push_row(0);

    int col = 0, row_start = 0, last_space = -1;
    for (int i = 0; i < (int)np.len; i++) {
        char c = np.text[i];
        if (c == '\n') { push_row(i + 1); row_start = i + 1; col = 0; last_space = -1; continue; }
        int w = c == '\t' ? NP_TAB - col % NP_TAB : 1;
        if (np.wrap && col + w > cols && i > row_start) {
            int brk = (last_space >= row_start && last_space + 1 < i + 1 && last_space > row_start) ? last_space + 1 : i;
            push_row(brk);
            row_start = brk;
            col = 0;
            for (int k = brk; k < i; k++) col += np.text[k] == '\t' ? NP_TAB - col % NP_TAB : 1;
            last_space = -1;
            w = c == '\t' ? NP_TAB - col % NP_TAB : 1;
        }
        if (c == ' ' || c == '\t') last_space = i;
        col += w;
    }
}

static int row_of(int idx)
{
    int lo = 0, hi = np.row_count - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (np.rows[mid] <= idx) lo = mid; else hi = mid - 1;
    }
    /* a position exactly at a wrapped row boundary belongs to the next row */
    return lo;
}

static int row_end(int r)                   /* index just past the row's last char */
{
    int end = r + 1 < np.row_count ? np.rows[r + 1] : (int)np.len;
    if (end > np.rows[r] && end <= (int)np.len && end - 1 >= 0 && np.text[end - 1] == '\n') end--;
    return end;
}

static int col_of(int idx)
{
    int r = row_of(idx), col = 0;
    for (int i = np.rows[r]; i < idx; i++) col += np.text[i] == '\t' ? NP_TAB - col % NP_TAB : 1;
    return col;
}

static int index_at(int r, int target_col)
{
    if (r < 0) r = 0;
    if (r >= np.row_count) r = np.row_count - 1;
    int i = np.rows[r], end = row_end(r), col = 0;
    while (i < end) {
        int w = np.text[i] == '\t' ? NP_TAB - col % NP_TAB : 1;
        if (col + w / 2 + (w & 1) > target_col) break;
        col += w;
        i++;
    }
    return i;
}

static void line_col(int idx, int *line, int *col)  /* logical, 1-based */
{
    int l = 1, start = 0;
    for (int i = 0; i < idx && i < (int)np.len; i++)
        if (np.text[i] == '\n') { l++; start = i + 1; }
    *line = l;
    *col = idx - start + 1;
}

/* ==========================================================================
 * Files
 * ========================================================================== */
static void reset_doc(void)
{
    stack_clear(np.undo, &np.undo_n);
    stack_clear(np.redo, &np.redo_n);
    ensure_cap(0);
    np.len = 0;
    np.text[0] = 0;
    np.caret = np.anchor = 0;
    np.scroll_row = np.scroll_x = 0;
    np.modified = 0;
    np.crlf = 0;
    np.read_only = 0;
    np.ro_warned = 0;
    np.path[0] = 0;
    np.layout_dirty = 1;
    np.goal_x = -1;
}

static int load_file(const char *path)
{
    long size = vfs_size(path);
    if (vfs_is_private(path)) { np_toast("That file is part of the protected system folder."); return -1; }
    if (size < 0 || vfs_is_dir(path)) { np_toast("The file could not be opened."); return -1; }
    if ((size_t)size > NP_MAX_TEXT) { np_toast("The file is larger than 4 MB."); return -1; }

    char *buf = (char *)np_alloc((size_t)size + 1);
    if (!buf) { np_toast("Not enough memory to open the file."); return -1; }
    int n = size ? vfs_read_file(path, buf, (size_t)size + 1) : 0;
    if (n < 0) { np_free(buf, (size_t)size + 1); np_toast("The file could not be read."); return -1; }

    int crlf = 0, binary = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == 0 || (c < 32 && c != '\n' && c != '\r' && c != '\t' && c != 12)) { binary = 1; break; }
        if (c == '\r' && i + 1 < n && buf[i + 1] == '\n') crlf = 1;
    }
    if (binary) { np_free(buf, (size_t)size + 1); np_toast("This is not a text file, so Notepad cannot show it."); return -1; }

    reset_doc();
    if (ensure_cap((size_t)n) != 0) { np_free(buf, (size_t)size + 1); np_toast("Not enough memory."); return -1; }
    size_t out = 0;
    for (int i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\r' && i + 1 < n && buf[i + 1] == '\n') continue;     /* CRLF -> LF */
        if ((unsigned char)c > 126) c = '?';
        np.text[out++] = c;
    }
    np.text[out] = 0;
    np.len = out;
    np_free(buf, (size_t)size + 1);

    np.crlf = crlf;
    np.read_only = fatfs_owns(path);
    snprintf(np.path, sizeof(np.path), "%s", path);
    update_title();
    return 0;
}

static int save_to(const char *path)
{
    if (fatfs_owns(path)) { np_toast("USB drives are read-only. Choose a folder on the Local Disk."); return -1; }
    if (vfs_is_private(path)) { np_toast("That is the protected system folder."); return -1; }
    size_t out_len = np.len;
    if (np.crlf) for (size_t i = 0; i < np.len; i++) if (np.text[i] == '\n') out_len++;
    char *out = np.text;
    if (np.crlf) {
        out = (char *)np_alloc(out_len + 1);
        if (!out) { np_toast("Not enough memory to save."); return -1; }
        size_t k = 0;
        for (size_t i = 0; i < np.len; i++) { if (np.text[i] == '\n') out[k++] = '\r'; out[k++] = np.text[i]; }
    }
    int rc = vfs_write_file(path, out, out_len);
    if (np.crlf) np_free(out, out_len + 1);
    if (rc != 0) { np_toast("The file could not be saved (disk or file table full?)."); return -1; }

    snprintf(np.path, sizeof(np.path), "%s", path);
    np.modified = 0;
    np.read_only = 0;
    update_title();
    file_explorer_refresh();
    desktop_files_changed();
    return 0;
}

/* ---- Open / Save As dialog ----------------------------------------------- */
static void dlg_load(const char *dir)
{
    static char listing[16384];
    snprintf(np.dlg_dir, sizeof(np.dlg_dir), "%s", dir);
    np.dlg_count = 0; np.dlg_sel = -1; np.dlg_scroll = 0; np.dlg_error[0] = 0;
    if (vfs_list(dir, listing, sizeof(listing)) < 0) return;
    size_t dl = strlen(dir);
    for (const char *p = listing; *p && np.dlg_count < 64;) {
        const char *eol = strchr(p, '\n');
        if (!eol) eol = p + strlen(p);
        int is_dir = !strncmp(p, "[dir] ", 6);
        const char *full = strchr(p, '/');
        if (full && full < eol && (size_t)(eol - full) > dl + 1 && !strncmp(full, dir, dl) && full[dl] == '/') {
            const char *name = full + dl + 1;
            size_t nl = (size_t)(eol - name);
            const char *slash = memchr(name, '/', nl);
            if (slash) { nl = (size_t)(slash - name); is_dir = 1; }
            int dup = 0;
            for (int k = 0; k < np.dlg_count && !dup; k++)
                if (strlen(np.dlg_items[k]) == nl && !strncmp(np.dlg_items[k], name, nl)) dup = 1;
            if (!dup && nl && nl < 96 && !(dl == 7 && nl == 5 && !strncmp(name, "trash", 5))) {
                memcpy(np.dlg_items[np.dlg_count], name, nl);
                np.dlg_items[np.dlg_count][nl] = 0;
                np.dlg_is_dir[np.dlg_count++] = is_dir;
            }
        }
        p = *eol ? eol + 1 : eol;
    }
    /* folders first, then names (simple insertion sort) */
    for (int i = 1; i < np.dlg_count; i++) {
        char name[96]; int d = np.dlg_is_dir[i];
        memcpy(name, np.dlg_items[i], 96);
        int j = i - 1;
        while (j >= 0 && (np.dlg_is_dir[j] < d || (np.dlg_is_dir[j] == d && strcmp(np.dlg_items[j], name) > 0))) {
            memcpy(np.dlg_items[j + 1], np.dlg_items[j], 96);
            np.dlg_is_dir[j + 1] = np.dlg_is_dir[j];
            j--;
        }
        memcpy(np.dlg_items[j + 1], name, 96);
        np.dlg_is_dir[j + 1] = d;
    }
}

static void open_dialog(int which)
{
    np.dialog = which;
    const char *start = "/jarvis/data";
    char cur[NP_PATH_LEN];
    if (np.path[0]) {                                   /* start in the file's folder */
        snprintf(cur, sizeof(cur), "%s", np.path);
        char *s = strrchr(cur, '/');
        if (s && s != cur) {
            *s = 0;
            if (vfs_is_dir(cur) && (which == DLG_OPEN || !fatfs_owns(cur))) start = cur;
        }
    }
    dlg_load(start);
    if (which == DLG_SAVEAS) {
        snprintf(np.dlg_name, sizeof(np.dlg_name), "%s", np.path[0] ? doc_name() : "Untitled.txt");
    } else {
        np.dlg_name[0] = 0;
    }
    np.dlg_name_len = (int)strlen(np.dlg_name);
    np.dlg_fresh = np.dlg_name_len > 0;
}

static void continue_after(void);

static void dlg_accept(void)
{
    char path[NP_PATH_LEN];
    if (np.dlg_sel >= 0 && np.dlg_is_dir[np.dlg_sel]) {
        snprintf(path, sizeof(path), "%s/%s", np.dlg_dir, np.dlg_items[np.dlg_sel]);
        dlg_load(path);
        return;
    }
    if (!np.dlg_name_len) { snprintf(np.dlg_error, sizeof(np.dlg_error), "Type a file name."); return; }
    if (strchr(np.dlg_name, '/')) snprintf(path, sizeof(path), "%s", np.dlg_name);
    else snprintf(path, sizeof(path), "%s/%s", np.dlg_dir, np.dlg_name);

    if (np.dialog == DLG_OPEN) {
        if (!vfs_exists(path) || vfs_is_dir(path)) { snprintf(np.dlg_error, sizeof(np.dlg_error), "There is no file with that name here."); return; }
        np.dialog = DLG_NONE;
        load_file(path);
    } else {
        if (fatfs_owns(path)) { snprintf(np.dlg_error, sizeof(np.dlg_error), "USB drives are read-only."); return; }
        if (!strchr(np.dlg_name, '.') && !strchr(np.dlg_name, '/')) {      /* add .txt like Notepad */
            size_t l = strlen(path);
            if (l + 4 < sizeof(path)) memcpy(path + l, ".txt", 5);
        }
        np.dialog = DLG_NONE;
        if (save_to(path) == 0) continue_after();
    }
}

/* ---- Unsaved changes ----------------------------------------------------- */
/* Run `after` now, or ask about unsaved changes first. */
static void guard(int after, const char *path)
{
    np.after = after;
    snprintf(np.after_path, sizeof(np.after_path), "%s", path ? path : "");
    if (np.modified && (np.len || np.path[0])) { np.dialog = DLG_UNSAVED; return; }
    continue_after();
}

static void continue_after(void)
{
    int a = np.after;
    np.after = AFTER_NOTHING;
    switch (a) {
    case AFTER_NEW:         reset_doc(); update_title(); break;
    case AFTER_OPEN_DIALOG: open_dialog(DLG_OPEN); break;
    case AFTER_OPEN_PATH:   load_file(np.after_path); break;
    case AFTER_CLOSE:       np.modified = 0; wm_close_window(np.window); break;
    default: break;
    }
}

static void save(void)
{
    if (!np.path[0] || np.read_only) { open_dialog(DLG_SAVEAS); return; }
    if (save_to(np.path) == 0) continue_after();
}

static int on_close_request(wm_window_t *win)
{
    (void)win;
    if (!np.modified || (!np.len && !np.path[0])) return 1;
    guard(AFTER_CLOSE, NULL);
    desktop_invalidate();
    return 0;
}

/* ==========================================================================
 * Find / replace / go to
 * ========================================================================== */
static int match_at(int i, const char *s, int n)
{
    if (i + n > (int)np.len) return 0;
    for (int k = 0; k < n; k++) {
        char a = np.text[i + k], b = s[k];
        if (!np.match_case) {
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        }
        if (a != b) return 0;
    }
    return 1;
}

static int find_next(int from_end_of_sel)
{
    if (!np.find_len) return 0;
    int start = from_end_of_sel ? sel_end() : sel_start();
    for (int pass = 0; pass < 2; pass++) {              /* wraps around once */
        for (int i = pass ? 0 : start; i <= (int)np.len - np.find_len; i++) {
            if (match_at(i, np.find, np.find_len)) {
                np.anchor = i;
                np.caret = i + np.find_len;
                np.goal_x = -1;
                return 1;
            }
        }
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "Cannot find \"%s\".", np.find);
    notification_show("Notepad", msg, NOTIF_INFO, 3000);
    return 0;
}

static void replace_one(void)
{
    if (has_sel() && sel_end() - sel_start() == np.find_len && match_at(sel_start(), np.find, np.find_len))
        replace_selection(np.repl, np.repl_len);
    find_next(1);
}

static void replace_all(void)
{
    int count = 0;
    if (!np.find_len) return;
    for (int i = 0; i <= (int)np.len - np.find_len;) {
        if (match_at(i, np.find, np.find_len)) {
            np.last_edit_tick = 0;                      /* each is its own undo step */
            edit(i, np.find_len, np.repl, np.repl_len);
            i += np.repl_len;
            count++;
        } else {
            i++;
        }
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "Replaced %d occurrence%s.", count, count == 1 ? "" : "s");
    notification_show("Notepad", msg, NOTIF_INFO, 3000);
}

static void goto_line(int line)
{
    int l = 1, i = 0;
    while (l < line && i < (int)np.len) { if (np.text[i] == '\n') l++; i++; }
    np.caret = np.anchor = i;
    np.goal_x = -1;
}

/* ==========================================================================
 * Menus
 * ========================================================================== */
enum {
    M_NEW = 1, M_OPEN, M_SAVE, M_SAVEAS, M_EXIT,
    M_UNDO, M_REDO, M_CUT, M_COPY, M_PASTE, M_DELETE, M_FIND, M_FINDNEXT, M_REPLACE, M_GOTO,
    M_SELALL, M_TIMEDATE, M_WRAP, M_STATUS, M_ABOUT,
};

static void insert_time_date(void)
{
    char clock[12], date[32], both[48];
    sys_format_clock(clock, sizeof(clock));
    sys_format_date(date, sizeof(date));
    snprintf(both, sizeof(both), "%s %s", clock, date);
    replace_selection(both, (int)strlen(both));
}

static void do_menu(int id, void *ctx)
{
    (void)ctx;
    switch (id) {
    case M_NEW:      guard(AFTER_NEW, NULL); break;
    case M_OPEN:     guard(AFTER_OPEN_DIALOG, NULL); break;
    case M_SAVE:     save(); break;
    case M_SAVEAS:   open_dialog(DLG_SAVEAS); break;
    case M_EXIT:     wm_request_close(np.window); break;
    case M_UNDO:     undo(); break;
    case M_REDO:     redo(); break;
    case M_CUT:      copy_sel(); if (has_sel()) replace_selection("", 0); break;
    case M_COPY:     copy_sel(); break;
    case M_PASTE:    paste(); break;
    case M_DELETE:   if (has_sel()) replace_selection("", 0); else if (np.caret < (int)np.len) edit(np.caret, 1, "", 0); break;
    case M_FIND:     np.find_open = 1; np.replace_mode = 0; np.find_field = 0; break;
    case M_REPLACE:  np.find_open = 1; np.replace_mode = 1; np.find_field = 0; break;
    case M_FINDNEXT: if (np.find_len) find_next(1); else { np.find_open = 1; np.find_field = 0; } break;
    case M_GOTO:     np.dialog = DLG_GOTO; np.dlg_name[0] = 0; np.dlg_name_len = 0; np.dlg_error[0] = 0; break;
    case M_SELALL:   np.anchor = 0; np.caret = (int)np.len; break;
    case M_TIMEDATE: insert_time_date(); break;
    case M_WRAP:     np.wrap = !np.wrap; np.scroll_x = 0; np.layout_dirty = 1; break;
    case M_STATUS:   np.status_bar = !np.status_bar; break;
    case M_ABOUT:    notification_show("Notepad", "JARVIS OS Notepad: plain-text editor.", NOTIF_INFO, 4000); break;
    }
    desktop_invalidate();
}

static const char *const g_menus[4] = { "File", "Edit", "View", "Help" };

static int menu_x(int i)
{
    int x = 8;
    for (int k = 0; k < i; k++) x += fb_text_width(g_menus[k]) + 20;
    return x;
}

static void open_menu(int which)
{
    int wx, wy, ww, wh;
    wm_get_client(np.window, &wx, &wy, &ww, &wh);
    int x = wx + menu_x(which), y = wy + NP_MENU_H;
    int sel = has_sel(), ro = 0;
    (void)ro;
    if (which == 0) {
        ui_menu_item_t it[] = {
            { "New               Ctrl+N", M_NEW, 0, 0 },
            { "Open...           Ctrl+O", M_OPEN, 0, 0 },
            { "Save              Ctrl+S", M_SAVE, 0, 0 },
            { "Save as...  Ctrl+Shift+S", M_SAVEAS, 0, 0 },
            { "Exit",                     M_EXIT, 0, 1 },
        };
        ui_menu_open(x, y, it, 5, do_menu, NULL);
    } else if (which == 1) {
        ui_menu_item_t it[] = {
            { "Undo              Ctrl+Z", M_UNDO, !np.undo_n, 0 },
            { "Redo              Ctrl+Y", M_REDO, !np.redo_n, 0 },
            { "Cut               Ctrl+X", M_CUT, !sel, 1 },
            { "Copy              Ctrl+C", M_COPY, !sel, 0 },
            { "Paste             Ctrl+V", M_PASTE, 0, 0 },
            { "Delete               Del", M_DELETE, 0, 0 },
            { "Find...           Ctrl+F", M_FIND, 0, 1 },
            { "Find next             F3", M_FINDNEXT, 0, 0 },
            { "Replace...        Ctrl+H", M_REPLACE, 0, 0 },
            { "Go to...          Ctrl+G", M_GOTO, 0, 0 },
            { "Select all        Ctrl+A", M_SELALL, 0, 1 },
            { "Time/Date             F5", M_TIMEDATE, 0, 0 },
        };
        ui_menu_open(x, y, it, 12, do_menu, NULL);
    } else if (which == 2) {
        ui_menu_item_t it[] = {
            { np.wrap ? "[x] Word wrap" : "[ ] Word wrap", M_WRAP, 0, 0 },
            { np.status_bar ? "[x] Status bar" : "[ ] Status bar", M_STATUS, 0, 0 },
        };
        ui_menu_open(x, y, it, 2, do_menu, NULL);
    } else {
        ui_menu_item_t it[] = { { "About Notepad", M_ABOUT, 0, 0 } };
        ui_menu_open(x, y, it, 1, do_menu, NULL);
    }
}

/* ==========================================================================
 * Drawing
 * ========================================================================== */
typedef struct { int x, y, w, h; } np_rect_t;

static np_rect_t text_area(int w, int h)
{
    int top = NP_MENU_H + (np.find_open ? NP_FIND_H : 0);
    int bottom = np.status_bar ? NP_STATUS_H : 0;
    return (np_rect_t){ 0, top, w - NP_SCROLL_W, h - top - bottom };
}

static void keep_caret_visible(int rows, int cols)
{
    int r = row_of(np.caret);
    if (r < np.scroll_row) np.scroll_row = r;
    if (r >= np.scroll_row + rows) np.scroll_row = r - rows + 1;
    if (!np.wrap) {
        int c = col_of(np.caret);
        if (c < np.scroll_x) np.scroll_x = c > 8 ? c - 8 : 0;
        if (c >= np.scroll_x + cols) np.scroll_x = c - cols + 8;
    }
    int max_scroll = np.row_count - 1;
    if (np.scroll_row > max_scroll) np.scroll_row = max_scroll;
    if (np.scroll_row < 0) np.scroll_row = 0;
}

static void draw_field(int x, int y, int w, const char *label, const char *text, int len, int focused)
{
    fb_draw_text(x, y + 6, label, T(TEXT_DIM));
    int fx = x + fb_text_width(label) + 8;
    fb_fill_rounded_rect(fx, y, w, 26, 4, T(INPUT));
    fb_draw_rounded_rect(fx, y, w, 26, 4, focused ? T(ACCENT) : T(BORDER));
    ui_text_fit(fx + 6, y + 5, w - 12, text, T(TEXT));
    if (focused && 7 + fb_text_width_n(text, len) < w) fb_fill_rect(fx + 7 + fb_text_width_n(text, len), y + 5, 1, 16, T(TEXT));
}

static void draw_find_bar(int x, int y, int w)
{
    fb_fill_rect(x, y, w, NP_FIND_H, T(SURFACE));
    fb_fill_rect(x, y + NP_FIND_H - 1, w, 1, T(BORDER));
    draw_field(x + 10, y + 7, 200, "Find", np.find, np.find_len, np.find_field == 0);
    int bx = x + 10 + 40 + 200 + 10;
    if (np.replace_mode) {
        draw_field(bx, y + 7, 180, "Replace", np.repl, np.repl_len, np.find_field == 1);
        bx += 70 + 180 + 10;
    }
    ui_button(bx, y + 7, 70, 26, "Next", T(HOVER), T(TEXT));
    bx += 76;
    if (np.replace_mode) {
        ui_button(bx, y + 7, 80, 26, "Replace", T(HOVER), T(TEXT)); bx += 86;
        ui_button(bx, y + 7, 60, 26, "All", T(HOVER), T(TEXT)); bx += 66;
    }
    ui_button(bx, y + 7, 36, 26, np.match_case ? "Aa" : "aa", np.match_case ? T(ACCENT) : T(HOVER),
              np.match_case ? T(ACCENT_TEXT) : T(TEXT_DIM));
    fb_draw_text(x + w - 24, y + 12, "x", T(TEXT_DIM));
}

static void draw_dialog(int x, int y, int w, int h)
{
    char line[160];
    fb_blend_rect(x, y, w, h, 0x00000000, 110);

    if (np.dialog == DLG_UNSAVED || np.dialog == DLG_GOTO) {
        int pw = 420, ph = np.dialog == DLG_GOTO ? 150 : 140, px = x + (w - pw) / 2, py = y + (h - ph) / 2;
        fb_fill_rounded_rect(px, py, pw, ph, 8, T(BORDER));
        fb_fill_rounded_rect(px + 1, py + 1, pw - 2, ph - 2, 7, T(MENU));
        if (np.dialog == DLG_UNSAVED) {
            fb_draw_text_bold(px + 20, py + 20, "Notepad", T(TEXT));
            snprintf(line, sizeof(line), "Do you want to save changes to %s?", doc_name());
            ui_text_fit(px + 20, py + 50, pw - 40, line, T(TEXT));
            ui_button(px + pw - 290, py + ph - 46, 80, 30, "Save", T(ACCENT), T(ACCENT_TEXT));
            ui_button(px + pw - 202, py + ph - 46, 100, 30, "Don't save", T(HOVER), T(TEXT));
            ui_button(px + pw - 94, py + ph - 46, 76, 30, "Cancel", T(HOVER), T(TEXT));
        } else {
            fb_draw_text_bold(px + 20, py + 18, "Go to line", T(TEXT));
            draw_field(px + 20, py + 50, 200, "Line number", np.dlg_name, np.dlg_name_len, 1);
            if (np.dlg_error[0]) fb_draw_text(px + 20, py + 84, np.dlg_error, T(DANGER));
            ui_button(px + pw - 196, py + ph - 46, 90, 30, "Go to", T(ACCENT), T(ACCENT_TEXT));
            ui_button(px + pw - 98, py + ph - 46, 80, 30, "Cancel", T(HOVER), T(TEXT));
        }
        return;
    }

    /* Open / Save As */
    int pw = w - 80 < 560 ? w - 80 : 560, ph = h - 60 < 380 ? h - 60 : 380;
    int px = x + (w - pw) / 2, py = y + (h - ph) / 2;
    fb_fill_rounded_rect(px, py, pw, ph, 8, T(BORDER));
    fb_fill_rounded_rect(px + 1, py + 1, pw - 2, ph - 2, 7, T(MENU));
    fb_draw_text_bold(px + 16, py + 14, np.dialog == DLG_OPEN ? "Open" : "Save as", T(TEXT));

    fb_fill_rounded_rect(px + 16, py + 40, 30, 26, 4, T(HOVER));
    glyph_draw(GLYPH_UP, px + 23, py + 45, 16, T(TEXT), T(HOVER));
    fb_fill_rounded_rect(px + 52, py + 40, pw - 68, 26, 4, T(INPUT));
    ui_text_fit(px + 60, py + 45, pw - 84, np.dlg_dir, T(TEXT));

    int ly = py + 74, lh = ph - 74 - 92, rows = lh / 22;
    fb_fill_rect(px + 16, ly, pw - 32, lh, T(WINDOW));
    if (np.dlg_sel >= 0) {
        if (np.dlg_sel < np.dlg_scroll) np.dlg_scroll = np.dlg_sel;
        if (np.dlg_sel >= np.dlg_scroll + rows) np.dlg_scroll = np.dlg_sel - rows + 1;
    }
    for (int i = np.dlg_scroll; i < np.dlg_count && i < np.dlg_scroll + rows; i++) {
        int ry = ly + (i - np.dlg_scroll) * 22;
        uint32_t fg = T(TEXT);
        if (i == np.dlg_sel) { fb_fill_rect(px + 16, ry, pw - 32, 22, T(ACCENT)); fg = T(ACCENT_TEXT); }
        icon_draw(np.dlg_is_dir[i] ? ICON_FOLDER : ICON_FILE, px + 22, ry + 3, 16);
        ui_text_fit(px + 46, ry + 3, pw - 80, np.dlg_items[i], fg);
    }
    if (!np.dlg_count) fb_draw_text(px + 26, ly + 8, "This folder is empty.", T(TEXT_DIM));

    if (np.dlg_fresh) {
        int fx = px + 16 + fb_text_width("File name") + 8;
        fb_fill_rounded_rect(fx, py + ph - 82, pw - 120, 26, 4, T(INPUT));
        fb_draw_rounded_rect(fx, py + ph - 82, pw - 120, 26, 4, T(ACCENT));
        fb_draw_text(px + 16, py + ph - 76, "File name", T(TEXT_DIM));
        fb_fill_rect(fx + 5, py + ph - 77, fb_text_width_n(np.dlg_name, np.dlg_name_len) + 2, 16, T(ACCENT));
        ui_text_fit(fx + 6, py + ph - 77, pw - 132, np.dlg_name, T(ACCENT_TEXT));
    } else {
        draw_field(px + 16, py + ph - 82, pw - 120, "File name", np.dlg_name, np.dlg_name_len, 1);
    }
    if (np.dlg_error[0]) fb_draw_text(px + 16, py + ph - 48, np.dlg_error, T(DANGER));
    ui_button(px + pw - 196, py + ph - 44, 90, 30, np.dialog == DLG_OPEN ? "Open" : "Save", T(ACCENT), T(ACCENT_TEXT));
    ui_button(px + pw - 98, py + ph - 44, 80, 30, "Cancel", T(HOVER), T(TEXT));
}

void notepad_draw_callback(wm_window_t *win, uint32_t *framebuffer)
{
    (void)framebuffer;
    int x, y, w, h;
    wm_get_client(win, &x, &y, &w, &h);
    np_rect_t ta = text_area(w, h);
    int cols = text_cols(w), rows = text_rows(h);
    layout(cols);

    /* menu bar */
    fb_fill_rect(x, y, w, NP_MENU_H, T(SURFACE));
    for (int i = 0; i < 4; i++) fb_draw_text(x + menu_x(i) + 6, y + 6, g_menus[i], T(TEXT));
    fb_fill_rect(x, y + NP_MENU_H - 1, w, 1, T(BORDER));
    if (np.find_open) draw_find_bar(x, y + NP_MENU_H, w);

    /* text */
    if (!np.free_scroll) keep_caret_visible(rows, cols);
    if (np.scroll_row > np.row_count - 1) np.scroll_row = np.row_count > 0 ? np.row_count - 1 : 0;
    if (np.scroll_row < 0) np.scroll_row = 0;
    fb_fill_rect(x + ta.x, y + ta.y, ta.w + NP_SCROLL_W, ta.h, T(INPUT));
    fb_clip_t saved = fb_get_clip();
    fb_set_clip(x + ta.x, y + ta.y, ta.w, ta.h);
    int s0 = sel_start(), s1 = sel_end();
    int tx = x + NP_PAD, ty = y + ta.y + 4;
    for (int r = np.scroll_row; r < np.row_count && r < np.scroll_row + rows; r++) {
        int ry = ty + (r - np.scroll_row) * NP_LINE_H, col = 0, end = row_end(r);
        for (int i = np.rows[r]; i <= end; i++) {
            int wch = (i < end && np.text[i] == '\t') ? NP_TAB - col % NP_TAB : 1;
            int vx = col - np.scroll_x;
            int selected = i >= s0 && i < s1;
            if (selected && i < (int)np.len && vx + wch > 0)
                fb_fill_rect(tx + vx * 8, ry, wch * 8 + (i == end ? 4 : 0), NP_LINE_H, T(ACCENT));
            if (i < end && vx >= 0 && vx < cols + 2 && np.text[i] != '\t')
                fb_draw_char(tx + vx * 8, ry + 1, np.text[i], selected ? T(ACCENT_TEXT) : T(TEXT));
            col += wch;
        }
    }
    /* caret */
    if (g_wm_ctx.focused == win && !np.dialog && !(np.find_open && np.find_field >= 0) &&
        (g_kstate.uptime_ticks / 50) % 2 == 0) {
        int r = row_of(np.caret), c = col_of(np.caret) - np.scroll_x;
        if (r >= np.scroll_row && r < np.scroll_row + rows && c >= 0)
            fb_fill_rect(tx + c * 8, ty + (r - np.scroll_row) * NP_LINE_H, 2, 17, T(TEXT));
    }
    fb_restore_clip(saved);

    /* scrollbar */
    int sx = x + w - NP_SCROLL_W, sy = y + ta.y;
    fb_fill_rect(sx, sy, NP_SCROLL_W, ta.h, T(SURFACE));
    if (np.row_count > rows) {
        int th = ta.h * rows / np.row_count;
        if (th < 24) th = 24;
        int max = np.row_count - rows;
        int ty2 = sy + (ta.h - th) * (np.scroll_row > max ? max : np.scroll_row) / (max ? max : 1);
        fb_fill_rounded_rect(sx + 2, ty2, NP_SCROLL_W - 4, th, 3, np.scroll_dragging ? T(ACCENT) : T(BORDER));
    }

    /* status bar */
    if (np.status_bar) {
        char buf[96];
        int line, col;
        line_col(np.caret, &line, &col);
        int sy2 = y + h - NP_STATUS_H;
        fb_fill_rect(x, sy2, w, NP_STATUS_H, T(SURFACE));
        fb_fill_rect(x, sy2, w, 1, T(BORDER));
        snprintf(buf, sizeof(buf), "Ln %d, Col %d", line, col);
        fb_draw_text(x + 12, sy2 + 4, buf, T(TEXT_DIM));
        if (has_sel()) {
            snprintf(buf, sizeof(buf), "%d selected", s1 - s0);
            fb_draw_text(x + 160, sy2 + 4, buf, T(TEXT_DIM));
        }
        snprintf(buf, sizeof(buf), "%u characters%s   %s   ASCII", (unsigned)np.len,
                 np.read_only ? "   Read-only" : "", np.crlf ? "Windows (CRLF)" : "Unix (LF)");
        fb_draw_text(x + w - 12 - fb_text_width(buf), sy2 + 4, buf, T(TEXT_DIM));
    }

    if (np.dialog) draw_dialog(x, y, w, h);
}

/* ==========================================================================
 * Input
 * ========================================================================== */
static int is_word(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static int word_left(int i)
{
    while (i > 0 && !is_word(np.text[i - 1])) i--;
    while (i > 0 && is_word(np.text[i - 1])) i--;
    return i;
}

static int word_right(int i)
{
    while (i < (int)np.len && is_word(np.text[i])) i++;
    while (i < (int)np.len && !is_word(np.text[i])) i++;
    return i;
}

static int pos_from_mouse(int mx, int my, np_rect_t ta)
{
    int r = np.scroll_row + (my - ta.y - 4) / NP_LINE_H;
    if (my < ta.y + 4) r = np.scroll_row - 1;
    int c = (mx - NP_PAD + 4) / 8 + np.scroll_x;
    if (c < 0) c = 0;
    if (r < 0) return r == -1 && np.scroll_row == 0 ? 0 : index_at(r < 0 ? 0 : r, c);
    if (r >= np.row_count) return (int)np.len;
    return index_at(r, c);
}

static int field_edit(char *buf, int *len, int max, wm_input_event_t *ev)
{
    if (ev->type == IE_TEXT_INPUT) {
        char ch = ev->data.text.ch;
        if (ch >= 32 && ch < 127 && *len < max - 1) { buf[(*len)++] = ch; buf[*len] = 0; }
        return 1;
    }
    if (ev->type == IE_KEY_DOWN && ev->data.keyboard.key == WM_KEY_BACKSPACE) {
        if (*len) buf[--(*len)] = 0;
        return 1;
    }
    return 0;
}

static void dialog_event(wm_input_event_t *ev, int w, int h)
{
    uint32_t key = ev->type == IE_KEY_DOWN ? ev->data.keyboard.key : 0;

    if (np.dialog == DLG_UNSAVED) {
        int pw = 420, ph = 140, px = (w - pw) / 2, py = (h - ph) / 2;
        int choice = -1;
        if (key == WM_KEY_ENTER) choice = 0;
        else if (key == WM_KEY_ESC) choice = 2;
        else if (ev->type == IE_MOUSE_DOWN) {
            int mx = ev->data.mouse.x, my = ev->data.mouse.y;
            if (ui_hit(mx, my, px + pw - 290, py + ph - 46, 80, 30)) choice = 0;
            else if (ui_hit(mx, my, px + pw - 202, py + ph - 46, 100, 30)) choice = 1;
            else if (ui_hit(mx, my, px + pw - 94, py + ph - 46, 76, 30)) choice = 2;
        }
        if (choice == 0) { np.dialog = DLG_NONE; save(); }
        else if (choice == 1) { np.dialog = DLG_NONE; np.modified = 0; continue_after(); }
        else if (choice == 2) { np.dialog = DLG_NONE; np.after = AFTER_NOTHING; }
        return;
    }

    if (np.dialog == DLG_GOTO) {
        int pw = 420, ph = 150, px = (w - pw) / 2, py = (h - ph) / 2;
        int go = key == WM_KEY_ENTER, cancel = key == WM_KEY_ESC;
        if (ev->type == IE_MOUSE_DOWN) {
            int mx = ev->data.mouse.x, my = ev->data.mouse.y;
            go = ui_hit(mx, my, px + pw - 196, py + ph - 46, 90, 30);
            cancel = ui_hit(mx, my, px + pw - 98, py + ph - 46, 80, 30);
        }
        if (ev->type == IE_TEXT_INPUT && (ev->data.text.ch < '0' || ev->data.text.ch > '9')) return;
        field_edit(np.dlg_name, &np.dlg_name_len, 8, ev);
        if (cancel) { np.dialog = DLG_NONE; return; }
        if (go) {
            int line = 0, lines = 1;
            for (int i = 0; i < np.dlg_name_len; i++) line = line * 10 + (np.dlg_name[i] - '0');
            for (size_t i = 0; i < np.len; i++) if (np.text[i] == '\n') lines++;
            if (line < 1 || line > lines) { snprintf(np.dlg_error, sizeof(np.dlg_error), "The line number is beyond the total number of lines."); return; }
            np.dialog = DLG_NONE;
            goto_line(line);
        }
        return;
    }

    /* Open / Save As */
    int pw = w - 80 < 560 ? w - 80 : 560, ph = h - 60 < 380 ? h - 60 : 380;
    int px = (w - pw) / 2, py = (h - ph) / 2;
    int ly = py + 74, lh = ph - 74 - 92;
    if (key == WM_KEY_ESC) { np.dialog = DLG_NONE; np.after = AFTER_NOTHING; return; }
    if (key == WM_KEY_ENTER) { dlg_accept(); return; }
    if (key == WM_KEY_UP && np.dlg_sel > 0) { np.dlg_sel--; goto picked; }
    if (key == WM_KEY_DOWN && np.dlg_sel + 1 < np.dlg_count) { np.dlg_sel++; goto picked; }
    if (np.dlg_fresh && (ev->type == IE_TEXT_INPUT ||
                         (ev->type == IE_KEY_DOWN && key == WM_KEY_BACKSPACE))) {
        np.dlg_name[0] = 0;                     /* the suggested name is replaced */
        np.dlg_name_len = 0;
    }
    if (ev->type == IE_TEXT_INPUT || key == WM_KEY_BACKSPACE || ev->type == IE_MOUSE_DOWN) np.dlg_fresh = 0;
    if (field_edit(np.dlg_name, &np.dlg_name_len, (int)sizeof(np.dlg_name), ev)) { np.dlg_error[0] = 0; return; }
    if (ev->type != IE_MOUSE_DOWN || ev->data.mouse.button != 1) return;
    {
        int mx = ev->data.mouse.x, my = ev->data.mouse.y;
        if (ui_hit(mx, my, px + 16, py + 40, 30, 26)) {            /* up */
            char up[NP_PATH_LEN];
            snprintf(up, sizeof(up), "%s", np.dlg_dir);
            char *s = strrchr(up, '/');
            if (s && s != up) { *s = 0; dlg_load(up); }
            return;
        }
        if (ui_hit(mx, my, px + pw - 196, py + ph - 44, 90, 30)) { dlg_accept(); return; }
        if (ui_hit(mx, my, px + pw - 98, py + ph - 44, 80, 30)) { np.dialog = DLG_NONE; np.after = AFTER_NOTHING; return; }
        if (ui_hit(mx, my, px + 16, ly, pw - 32, lh)) {
            int i = np.dlg_scroll + (my - ly) / 22;
            if (i >= np.dlg_count) return;
            int dbl = i == np.dlg_click_idx && ev->timestamp - np.dlg_click_tick <= 50;
            np.dlg_click_idx = dbl ? -1 : i;
            np.dlg_click_tick = ev->timestamp;
            np.dlg_sel = i;
            if (dbl) { dlg_accept(); return; }
            goto picked;
        }
        return;
    }
picked:
    if (np.dlg_sel >= 0 && !np.dlg_is_dir[np.dlg_sel]) {
        snprintf(np.dlg_name, sizeof(np.dlg_name), "%s", np.dlg_items[np.dlg_sel]);
        np.dlg_name_len = (int)strlen(np.dlg_name);
        np.dlg_fresh = 1;
    }
}

static void move_caret(int to, int extend)
{
    if (to < 0) to = 0;
    if (to > (int)np.len) to = (int)np.len;
    np.caret = to;
    if (!extend) np.anchor = to;
}

void notepad_event_callback(wm_window_t *win, wm_input_event_t *event)
{
    int x, y, w, h;
    wm_get_client(win, &x, &y, &w, &h);
    np_rect_t ta = text_area(w, h);
    int cols = text_cols(w), rows = text_rows(h);
    layout(cols);
    if (event->type == IE_MOUSE_WHEEL && !np.dialog) {
        np.scroll_row -= event->data.mouse.button;
        if (np.scroll_row > np.row_count - rows) np.scroll_row = np.row_count - rows;
        if (np.scroll_row < 0) np.scroll_row = 0;
        np.free_scroll = 1;
        return;
    }
    if (event->type == IE_KEY_DOWN || event->type == IE_TEXT_INPUT || event->type == IE_MOUSE_DOWN) np.free_scroll = 0;

    if (np.dialog) { dialog_event(event, w, h); return; }

    /* ---- find bar has the keyboard while open and focused ---- */
    if (np.find_open && np.find_field >= 0 && (event->type == IE_TEXT_INPUT || event->type == IE_KEY_DOWN)) {
        uint32_t key = event->type == IE_KEY_DOWN ? event->data.keyboard.key : 0;
        if (key == WM_KEY_ESC) { np.find_open = 0; return; }
        if (key == WM_KEY_ENTER) { if (np.find_field == 1) replace_one(); else find_next(1); return; }
        if (key == WM_KEY_TAB && np.replace_mode) { np.find_field ^= 1; return; }
        if (key == WM_KEY_F1 + 2) { find_next(1); return; }
        if (np.find_field == 0 ? field_edit(np.find, &np.find_len, (int)sizeof(np.find), event)
                               : field_edit(np.repl, &np.repl_len, (int)sizeof(np.repl), event))
            return;
        if (key == WM_KEY_DOWN || key == WM_KEY_UP) { np.find_field = -1; }
        else if (event->type == IE_KEY_DOWN && !(event->data.keyboard.mods & KMOD_CTRL)) return;
    }

    /* ---- mouse ---- */
    if (event->type == IE_MOUSE_DOWN) {
        int mx = event->data.mouse.x, my = event->data.mouse.y, button = event->data.mouse.button;
        if (my < NP_MENU_H) {
            for (int i = 0; i < 4; i++)
                if (button == 1 && ui_hit(mx, my, menu_x(i), 2, fb_text_width(g_menus[i]) + 12, NP_MENU_H - 4)) open_menu(i);
            return;
        }
        if (np.find_open && my < NP_MENU_H + NP_FIND_H) {           /* find bar */
            int fy = NP_MENU_H + 7, bx = 10 + 40 + 200 + 10;
            if (ui_hit(mx, my, 10, fy, 40 + 200, 26)) np.find_field = 0;
            if (np.replace_mode) {
                if (ui_hit(mx, my, bx, fy, 70 + 180, 26)) np.find_field = 1;
                bx += 70 + 180 + 10;
            }
            if (ui_hit(mx, my, bx, fy, 70, 26)) find_next(1);
            bx += 76;
            if (np.replace_mode) {
                if (ui_hit(mx, my, bx, fy, 80, 26)) replace_one();
                bx += 86;
                if (ui_hit(mx, my, bx, fy, 60, 26)) replace_all();
                bx += 66;
            }
            if (ui_hit(mx, my, bx, fy, 36, 26)) np.match_case = !np.match_case;
            if (ui_hit(mx, my, w - 32, fy, 28, 26)) np.find_open = 0;
            return;
        }
        if (my >= ta.y && my < ta.y + ta.h && mx >= w - NP_SCROLL_W) {   /* scrollbar */
            np.scroll_dragging = 1;
            int max = np.row_count - rows;
            if (max > 0) np.scroll_row = (my - ta.y) * max / (ta.h ? ta.h : 1);
            return;
        }
        if (my < ta.y || my >= ta.y + ta.h) return;
        if (np.find_open) np.find_field = -1;                        /* typing goes to the text */

        int pos = pos_from_mouse(mx, my, ta);
        if (button == 2) {
            if (pos < sel_start() || pos > sel_end() || !has_sel()) move_caret(pos, 0);
            ui_menu_item_t it[] = {
                { "Undo", M_UNDO, !np.undo_n, 0 }, { "Cut", M_CUT, !has_sel(), 1 }, { "Copy", M_COPY, !has_sel(), 0 },
                { "Paste", M_PASTE, 0, 0 }, { "Delete", M_DELETE, !has_sel(), 0 }, { "Select all", M_SELALL, 0, 1 },
            };
            ui_menu_open(x + mx, y + my, it, 6, do_menu, NULL);
            return;
        }
        if (button != 1) return;
        int dbl = pos == np.last_click_pos && event->timestamp - np.last_click_tick <= 50;
        np.last_click_pos = dbl ? -1 : pos;
        np.last_click_tick = event->timestamp;
        if (dbl) {                                                  /* select the word */
            int a = pos, b = pos;
            while (a > 0 && is_word(np.text[a - 1])) a--;
            while (b < (int)np.len && is_word(np.text[b])) b++;
            np.anchor = a; np.caret = b;
            return;
        }
        move_caret(pos, (input_current_mods() & KMOD_SHIFT) ? 1 : 0);
        np.goal_x = -1;
        np.dragging = 1;
        return;
    }
    if (event->type == IE_MOUSE_MOVE) {
        int mx = event->data.mouse.x, my = event->data.mouse.y;
        if (np.scroll_dragging) {
            int max = np.row_count - rows;
            if (max > 0) {
                int r = (my - ta.y) * max / (ta.h ? ta.h : 1);
                np.scroll_row = r < 0 ? 0 : r > max ? max : r;
            }
        } else if (np.dragging) {
            if (my < ta.y && np.scroll_row > 0) np.scroll_row--;     /* auto-scroll */
            if (my >= ta.y + ta.h && np.scroll_row + rows < np.row_count) np.scroll_row++;
            np.caret = pos_from_mouse(mx, my, ta);
        }
        return;
    }
    if (event->type == IE_MOUSE_UP) { np.dragging = 0; np.scroll_dragging = 0; return; }

    /* ---- typing ---- */
    if (event->type == IE_TEXT_INPUT) {
        char ch = event->data.text.ch;
        if (ch >= 32 && ch < 127) replace_selection(&ch, 1);
        return;
    }
    if (event->type != IE_KEY_DOWN) return;

    uint32_t key = event->data.keyboard.key;
    int mods = event->data.keyboard.mods;
    int ctrl = mods & KMOD_CTRL, shift = mods & KMOD_SHIFT;
    char lk = (key < 128) ? (char)((key >= 'A' && key <= 'Z') ? key + 32 : key) : 0;

    if (ctrl && lk) {
        switch (lk) {
        case 'n': do_menu(M_NEW, NULL); return;
        case 'o': do_menu(M_OPEN, NULL); return;
        case 's': do_menu(shift ? M_SAVEAS : M_SAVE, NULL); return;
        case 'z': undo(); return;
        case 'y': redo(); return;
        case 'x': do_menu(M_CUT, NULL); return;
        case 'c': copy_sel(); return;
        case 'v': paste(); return;
        case 'a': do_menu(M_SELALL, NULL); return;
        case 'f': do_menu(M_FIND, NULL); return;
        case 'h': do_menu(M_REPLACE, NULL); return;
        case 'g': do_menu(M_GOTO, NULL); return;
        default: break;
        }
    }

    int r = row_of(np.caret);
    if (np.goal_x < 0) np.goal_x = col_of(np.caret);
    int keep_goal = 0;

    switch (key) {
    case WM_KEY_ENTER:     replace_selection("\n", 1); break;
    case WM_KEY_TAB:       replace_selection("\t", 1); break;
    case WM_KEY_BACKSPACE:
        if (has_sel()) replace_selection("", 0);
        else if (np.caret > 0) {
            int from = ctrl ? word_left(np.caret) : np.caret - 1;
            np.last_edit_tick = 0;
            edit(from, np.caret - from, "", 0);
        }
        break;
    case WM_KEY_DELETE:
        if (has_sel()) replace_selection("", 0);
        else if (np.caret < (int)np.len) edit(np.caret, (ctrl ? word_right(np.caret) : np.caret + 1) - np.caret, "", 0);
        break;
    case WM_KEY_LEFT:
        if (has_sel() && !shift) move_caret(sel_start(), 0);
        else move_caret(ctrl ? word_left(np.caret) : np.caret - 1, shift);
        break;
    case WM_KEY_RIGHT:
        if (has_sel() && !shift) move_caret(sel_end(), 0);
        else move_caret(ctrl ? word_right(np.caret) : np.caret + 1, shift);
        break;
    case WM_KEY_UP:
        if (r > 0) move_caret(index_at(r - 1, np.goal_x), shift); else move_caret(0, shift);
        keep_goal = 1;
        break;
    case WM_KEY_DOWN:
        if (r + 1 < np.row_count) move_caret(index_at(r + 1, np.goal_x), shift); else move_caret((int)np.len, shift);
        keep_goal = 1;
        break;
    case WM_KEY_PGUP:
        move_caret(index_at(r - rows, np.goal_x), shift);
        np.scroll_row -= rows;
        keep_goal = 1;
        break;
    case WM_KEY_PGDN:
        move_caret(index_at(r + rows, np.goal_x), shift);
        np.scroll_row += rows;
        keep_goal = 1;
        break;
    case WM_KEY_HOME:      move_caret(ctrl ? 0 : np.rows[r], shift); break;
    case WM_KEY_END:       move_caret(ctrl ? (int)np.len : row_end(r), shift); break;
    case WM_KEY_F1 + 2:    do_menu(M_FINDNEXT, NULL); break;      /* F3 */
    case WM_KEY_F1 + 4:    insert_time_date(); break;             /* F5 */
    case WM_KEY_ESC:       if (np.find_open) np.find_open = 0; else np.anchor = np.caret; break;
    default: break;
    }
    if (!keep_goal) np.goal_x = -1;
}

/* ==========================================================================
 * Public API
 * ========================================================================== */
wm_window_t *notepad_app_open(int x, int y, int w, int h)
{
    /* free the previous session's buffers */
    stack_clear(np.undo, &np.undo_n);
    stack_clear(np.redo, &np.redo_n);
    np_free(np.text, np.cap);
    np_free(np.rows, (size_t)np.row_cap * sizeof(int));
    int wrap = np.window ? np.wrap : 1, status = np.window ? np.status_bar : 1;
    memset(&np, 0, sizeof(np));
    np.wrap = wrap;
    np.status_bar = status;
    np.find_field = -1;
    np.last_click_pos = -1;
    reset_doc();

    wm_window_t *win = wm_create_window("Untitled - Notepad", x, y, w, h, WS_DEFAULT,
                                        notepad_draw_callback, notepad_event_callback);
    if (!win) return NULL;
    win->icon = ICON_FILE;
    win->min_w = 420;
    win->min_h = 240;
    win->on_close_request = on_close_request;
    np.window = win;
    return win;
}

/* Opens (or focuses) Notepad and loads a file, asking first if the current
 * document has unsaved changes. */
void notepad_open_file(const char *path)
{
    desktop_app_launch(desktop_app_find("Notepad"));
    if (!np_is_open()) return;
    guard(AFTER_OPEN_PATH, path);
    desktop_invalidate();
}
