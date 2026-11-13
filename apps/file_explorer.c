/*
 * =============================================================================
 * JARVIS OS — File Explorer (apps/file_explorer.c)
 * Language: C17
 *
 * Browses the kernel VFS and manages files:
 *   - open folders; open files in Notepad
 *   - right-click menu: Open, Cut, Copy, Paste, Rename, Delete, Properties,
 *     New folder, New text document
 *   - Delete moves items to the Recycle Bin (/jarvis/trash); from there they
 *     can be restored or deleted permanently
 *   - keyboard: arrows, Enter, Backspace (up), F2 rename, Delete,
 *     Ctrl+C / Ctrl+X / Ctrl+V
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "../wm/wm.h"
#include "../desktop/ui.h"
#include <string.h>
#include <stdio.h>

void notepad_open_file(const char *path);       /* apps/notepad.c */
void desktop_files_changed(void);                /* desktop/desktop.c */
static int fx_window_open(void);

#define FX_MAX_FILES   128
#define FX_PATH_LEN    200
#define FX_NAME_LEN    64
#define FX_TOOLBAR_H   44
#define FX_SIDEBAR_W   150
#define FX_ROW_H       26
#define FX_HEADER_H    26
#define FX_STATUS_H    26
#define FX_TRASH       "/jarvis/trash"
#define FX_TRASH_MAX   32

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

    char clipboard[FX_PATH_LEN];        /* path waiting to be pasted */
    int clipboard_cut;

    int renaming;                       /* row being renamed, or -1 */
    int rename_fresh;                   /* whole name selected: typing replaces it */
    char rename_buf[FX_NAME_LEN];
    int rename_len;

    int props_open;
    char props_path[FX_PATH_LEN];
} explorer_context_t;

static explorer_context_t g_explorer;
static char g_fx_listing[32768];

/* Where each item in the Recycle Bin came from, so it can be restored. */
static struct { char trash_path[FX_PATH_LEN]; char origin[FX_PATH_LEN]; }
    g_fx_trash[FX_TRASH_MAX];

static const struct { const char *label; const char *path; icon_id_t icon; } g_fx_places[] = {
    { "Home",        "/jarvis",             ICON_FOLDER },
    { "Desktop",     "/jarvis/desktop",     ICON_PC },
    { "Data",        "/jarvis/data",        ICON_FOLDER },
    { "Models",      "/jarvis/models",      ICON_FOLDER },
    { "Logs",        "/jarvis/logs",        ICON_FOLDER },
    { "Screenshots", "/jarvis/screenshots", ICON_FOLDER },
    { "Programs",    "/jarvis/bin",         ICON_FOLDER },
    { "Recycle Bin", FX_TRASH,              ICON_TRASH },
};
#define FX_PLACES ((int)(sizeof(g_fx_places) / sizeof(g_fx_places[0])))

/* ---- Path helpers -------------------------------------------------------- */
static const char *fx_leaf(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int fx_under(const char *path, const char *dir)
{
    size_t n = strlen(dir);
    return strncmp(path, dir, n) == 0 && (path[n] == '/' || path[n] == 0);
}

static int fx_in_trash(void)
{
    return fx_under(g_explorer.current_path, FX_TRASH);
}

/* System folders the OS itself depends on cannot be moved or deleted. */
static int fx_is_protected(const char *path)
{
    static const char *const fixed[] = {
        "/jarvis", "/jarvis/models", "/jarvis/bin", "/jarvis/logs", "/jarvis/data",
        "/jarvis/fonts", "/jarvis/screenshots", "/jarvis/desktop", FX_TRASH,
    };
    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++)
        if (strcmp(path, fixed[i]) == 0) return 1;
    return fx_under(path, "/jarvis/models") || fatfs_owns(path);   /* USB drives are read-only */
}

/* Builds dir/name; if that exists, "name (2)", "name (3)"... keeping the
 * extension. Returns 0 on success. */
static int fx_unique(char *out, size_t n, const char *dir, const char *name)
{
    snprintf(out, n, "%s/%s", dir, name);
    if (!vfs_exists(out)) return 0;

    char stem[FX_NAME_LEN];
    const char *ext = strrchr(name, '.');
    size_t slen = (ext && ext != name) ? (size_t)(ext - name) : strlen(name);
    if (slen >= sizeof(stem)) slen = sizeof(stem) - 1;
    memcpy(stem, name, slen);
    stem[slen] = 0;
    if (!ext || ext == name) ext = "";

    for (int i = 2; i < 100; i++) {
        snprintf(out, n, "%s/%s (%d)%s", dir, stem, i, ext);
        if (!vfs_exists(out)) return 0;
    }
    return -1;
}

static void fx_toast(const char *title, const char *body)
{
    notification_show(title, body, NOTIF_WARNING, 4000);
}

/* ---- Directory loading ---------------------------------------------------
 * vfs_list() returns every path under a prefix, one per line, as
 * "[dir] /full/path" or "[file] /full/path". Keep only direct children.
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
    g_explorer.last_click_idx = -1;
    g_explorer.renaming = -1;
    g_explorer.props_open = 0;

    if (strcmp(dir, FX_TRASH) == 0)
        vfs_mkdir(FX_TRASH);
    desktop_files_changed();    /* every file operation ends in a reload */

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

            /* A deeper path ("models/stt/x.bin" while listing the parent)
             * still tells us the folder "models" exists here, even when the
             * VFS has no node of its own for it. */
            for (size_t i = 0; i < nlen; i++) {
                if (name[i] == '/') {
                    nlen = i;
                    flen = dlen + 1 + nlen;
                    is_dir = 1;
                    break;
                }
            }

            int known = 0;
            for (int k = 0; k < g_explorer.file_count; k++) {
                if (strlen(g_explorer.files[k].name) == nlen &&
                    strncmp(g_explorer.files[k].name, name, nlen) == 0) {
                    known = 1;
                    break;
                }
            }

            /* The Recycle Bin has its own entry in the sidebar; don't list
             * its backing folder among the user's files. */
            if (flen == strlen(FX_TRASH) && strncmp(full, FX_TRASH, flen) == 0)
                known = 1;

            if (!known && nlen > 0 && nlen < FX_NAME_LEN) {
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

    if (fx_window_open()) {
        char title[96];
        if (strcmp(dir, FX_TRASH) == 0)
            snprintf(title, sizeof(title), "Recycle Bin - Files");
        else
            snprintf(title, sizeof(title), "%s - Files", fx_leaf(dir)[0] ? fx_leaf(dir) : dir);
        wm_set_title(g_explorer.window, title);
    }
    desktop_invalidate();
}

/* Reload the current folder, optionally re-selecting an item by name. */
static void explorer_reload(const char *select_name)
{
    char path[FX_PATH_LEN], name[FX_NAME_LEN] = "";
    int scroll = g_explorer.scroll;

    strncpy(path, g_explorer.current_path, sizeof(path) - 1);
    path[sizeof(path) - 1] = 0;
    if (select_name) {
        strncpy(name, select_name, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
    }

    explorer_refresh_directory(path);
    g_explorer.scroll = scroll < g_explorer.file_count ? scroll : 0;

    for (int i = 0; name[0] && i < g_explorer.file_count; i++)
        if (strcmp(g_explorer.files[i].name, name) == 0)
            g_explorer.selected = i;
}

/* g_explorer.window is stale once the window has been closed; the app
 * registry is the source of truth for whether Files is open. */
static int fx_window_open(void)
{
    desktop_app_t *app = desktop_app_get(desktop_app_find("Files"));
    return app && app->window != NULL;
}

/* Called by other apps (Notepad save, desktop menu) after changing files. */
void file_explorer_refresh(void)
{
    if (fx_window_open())
        explorer_reload(NULL);
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

static file_entry_t *explorer_selected(void)
{
    if (g_explorer.selected < 0 || g_explorer.selected >= g_explorer.file_count)
        return NULL;
    return &g_explorer.files[g_explorer.selected];
}

static int explorer_visible_rows(int client_h)
{
    int rows = (client_h - FX_TOOLBAR_H - FX_HEADER_H - FX_STATUS_H) / FX_ROW_H;
    return rows < 1 ? 1 : rows;
}

/* ---- File operations ----------------------------------------------------- */
static void explorer_open(int idx)
{
    if (idx < 0 || idx >= g_explorer.file_count) return;
    file_entry_t *e = &g_explorer.files[idx];

    if (e->is_dir) {
        char path[FX_PATH_LEN];
        strncpy(path, e->path, sizeof(path) - 1);
        path[sizeof(path) - 1] = 0;
        explorer_refresh_directory(path);
    } else if (user_is_program(e->path)) {
        /* A program: run it in user mode; its output goes to the Terminal. */
        desktop_app_launch(desktop_app_find("Terminal"));
        if (!user_exec(e->path))
            fx_toast("Run", "The program could not be started.");
    } else {
        notepad_open_file(e->path);
    }
}

static void explorer_start_rename(int idx)
{
    if (idx < 0 || idx >= g_explorer.file_count) return;
    if (fx_is_protected(g_explorer.files[idx].path)) {
        fx_toast("Rename", "This is a system folder and cannot be renamed.");
        return;
    }
    g_explorer.selected = idx;
    g_explorer.renaming = idx;
    strncpy(g_explorer.rename_buf, g_explorer.files[idx].name, sizeof(g_explorer.rename_buf) - 1);
    g_explorer.rename_buf[sizeof(g_explorer.rename_buf) - 1] = 0;
    g_explorer.rename_len = (int)strlen(g_explorer.rename_buf);
    g_explorer.rename_fresh = 1;
}

static void explorer_commit_rename(void)
{
    int idx = g_explorer.renaming;
    g_explorer.renaming = -1;
    if (idx < 0 || idx >= g_explorer.file_count) return;

    file_entry_t *e = &g_explorer.files[idx];
    if (g_explorer.rename_len == 0 || strcmp(g_explorer.rename_buf, e->name) == 0)
        return;

    char target[FX_PATH_LEN];
    snprintf(target, sizeof(target), "%s/%s", g_explorer.current_path, g_explorer.rename_buf);
    if (vfs_exists(target)) {
        fx_toast("Rename", "An item with that name already exists here.");
        return;
    }
    if (vfs_rename(e->path, target) != 0) {
        fx_toast("Rename", "The item could not be renamed.");
        return;
    }
    explorer_reload(g_explorer.rename_buf);
}

static void explorer_delete(int idx)
{
    if (idx < 0 || idx >= g_explorer.file_count) return;
    file_entry_t *e = &g_explorer.files[idx];

    if (fx_is_protected(e->path)) {
        fx_toast("Delete", "This is a system folder and cannot be deleted.");
        return;
    }

    if (fx_in_trash()) {                        /* already binned: remove for good */
        for (int i = 0; i < FX_TRASH_MAX; i++)
            if (strcmp(g_fx_trash[i].trash_path, e->path) == 0)
                g_fx_trash[i].trash_path[0] = 0;
        vfs_delete(e->path);
    } else {
        char target[FX_PATH_LEN];
        vfs_mkdir(FX_TRASH);
        if (fx_unique(target, sizeof(target), FX_TRASH, e->name) != 0 ||
            vfs_rename(e->path, target) != 0) {
            fx_toast("Delete", "The item could not be moved to the Recycle Bin.");
            return;
        }
        for (int i = 0; i < FX_TRASH_MAX; i++) {
            if (!g_fx_trash[i].trash_path[0]) {
                strncpy(g_fx_trash[i].trash_path, target, FX_PATH_LEN - 1);
                strncpy(g_fx_trash[i].origin, e->path, FX_PATH_LEN - 1);
                break;
            }
        }
    }
    if (strcmp(g_explorer.clipboard, e->path) == 0)
        g_explorer.clipboard[0] = 0;
    explorer_reload(NULL);
}

static const char *explorer_trash_origin(const char *trash_path)
{
    for (int i = 0; i < FX_TRASH_MAX; i++)
        if (g_fx_trash[i].trash_path[0] && strcmp(g_fx_trash[i].trash_path, trash_path) == 0)
            return g_fx_trash[i].origin;
    return NULL;
}

static void explorer_restore(int idx)
{
    if (idx < 0 || idx >= g_explorer.file_count) return;
    file_entry_t *e = &g_explorer.files[idx];
    const char *origin = explorer_trash_origin(e->path);
    if (!origin) return;

    /* Put it back where it was; pick a free name if that spot is taken. */
    char dir[FX_PATH_LEN], target[FX_PATH_LEN];
    strncpy(dir, origin, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;
    char *slash = strrchr(dir, '/');
    if (!slash || slash == dir) return;
    *slash = 0;

    if (fx_unique(target, sizeof(target), dir, slash + 1) != 0 ||
        vfs_rename(e->path, target) != 0) {
        fx_toast("Restore", "The item could not be restored.");
        return;
    }
    for (int i = 0; i < FX_TRASH_MAX; i++)
        if (strcmp(g_fx_trash[i].trash_path, e->path) == 0)
            g_fx_trash[i].trash_path[0] = 0;
    explorer_reload(NULL);
}

/* Permanently removes everything in the Recycle Bin. Returns the number of
 * top-level items removed. */
int file_explorer_empty_trash(void)
{
    static char listing[8192];
    int removed = 0;

    vfs_mkdir(FX_TRASH);
    size_t tlen = strlen(FX_TRASH);

    /* Each pass deletes one top-level item (and its contents). */
    for (int guard = 0; guard < 256; guard++) {
        if (vfs_list(FX_TRASH, listing, sizeof(listing)) < 0) break;

        char victim[FX_PATH_LEN] = "";
        for (const char *p = listing; *p && !victim[0];) {
            const char *eol = p;
            while (*eol && *eol != '\n') eol++;
            const char *full = strchr(p, '/');
            if (full && full < eol && (size_t)(eol - full) > tlen + 1 &&
                strncmp(full, FX_TRASH, tlen) == 0 && full[tlen] == '/') {
                size_t n = tlen + 1;
                while (full + n < eol && full[n] != '/') n++;
                if (n < sizeof(victim)) {
                    memcpy(victim, full, n);
                    victim[n] = 0;
                }
            }
            p = *eol ? eol + 1 : eol;
        }
        if (!victim[0]) break;
        vfs_delete(victim);
        removed++;
    }

    for (int i = 0; i < FX_TRASH_MAX; i++)
        g_fx_trash[i].trash_path[0] = 0;

    notification_show("Recycle Bin",
                      removed ? "The Recycle Bin has been emptied." : "The Recycle Bin is already empty.",
                      NOTIF_INFO, 3000);
    file_explorer_refresh();
    return removed;
}

static void explorer_clip(int idx, int cut)
{
    if (idx < 0 || idx >= g_explorer.file_count) return;
    if (cut && fx_is_protected(g_explorer.files[idx].path)) {
        fx_toast("Cut", "This is a system folder and cannot be moved.");
        return;
    }
    strncpy(g_explorer.clipboard, g_explorer.files[idx].path, sizeof(g_explorer.clipboard) - 1);
    g_explorer.clipboard[sizeof(g_explorer.clipboard) - 1] = 0;
    g_explorer.clipboard_cut = cut;
}

static void explorer_paste(void)
{
    const char *src = g_explorer.clipboard;
    if (!src[0]) return;

    if (!vfs_exists(src)) {
        fx_toast("Paste", "The copied item no longer exists.");
        g_explorer.clipboard[0] = 0;
        return;
    }
    if (fx_under(g_explorer.current_path, src)) {
        fx_toast("Paste", "A folder cannot be placed inside itself.");
        return;
    }

    char target[FX_PATH_LEN], name[FX_NAME_LEN];
    strncpy(name, fx_leaf(src), sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;

    /* Moving something onto its own location is a no-op. */
    snprintf(target, sizeof(target), "%s/%s", g_explorer.current_path, name);
    if (g_explorer.clipboard_cut && strcmp(target, src) == 0)
        return;

    if (fx_unique(target, sizeof(target), g_explorer.current_path, name) != 0) {
        fx_toast("Paste", "Could not find a free name here.");
        return;
    }

    int rc = g_explorer.clipboard_cut ? vfs_rename(src, target) : vfs_copy(src, target);
    if (rc != 0) {
        fx_toast("Paste", "The item could not be pasted (out of space or name too long).");
        return;
    }
    if (g_explorer.clipboard_cut)
        g_explorer.clipboard[0] = 0;
    explorer_reload(fx_leaf(target));
}

/* Creates "New folder" / "New Text Document.txt" in dir and starts renaming
 * it. Opens the Files window on that folder if needed. */
void file_explorer_new_item(const char *dir, int folder)
{
    char target[FX_PATH_LEN];

    desktop_app_launch(desktop_app_find("Files"));
    if (!fx_window_open()) return;
    if (strcmp(g_explorer.current_path, dir) != 0)
        explorer_refresh_directory(dir);

    if (fx_in_trash()) {
        fx_toast("New", "Items cannot be created inside the Recycle Bin.");
        return;
    }
    if (fx_unique(target, sizeof(target), g_explorer.current_path,
                  folder ? "New folder" : "New Text Document.txt") != 0)
        return;

    int rc = folder ? vfs_mkdir(target) : vfs_create_file(target);
    if (rc != 0) {
        fx_toast("New", "The item could not be created (file table is full).");
        return;
    }
    explorer_reload(fx_leaf(target));
    explorer_start_rename(g_explorer.selected);
}

/* ---- Right-click menu ---------------------------------------------------- */
enum {
    FXM_OPEN = 1, FXM_CUT, FXM_COPY, FXM_PASTE, FXM_RENAME, FXM_DELETE,
    FXM_PROPERTIES, FXM_NEW_FOLDER, FXM_NEW_TEXT, FXM_REFRESH, FXM_RESTORE,
    FXM_EMPTY_TRASH,
};

static void explorer_show_properties(const char *path)
{
    strncpy(g_explorer.props_path, path, sizeof(g_explorer.props_path) - 1);
    g_explorer.props_path[sizeof(g_explorer.props_path) - 1] = 0;
    g_explorer.props_open = 1;
}

static void explorer_menu_action(int id, void *ctx)
{
    (void)ctx;
    int idx = g_explorer.selected;

    switch (id) {
    case FXM_OPEN:        explorer_open(idx);            break;
    case FXM_CUT:         explorer_clip(idx, 1);         break;
    case FXM_COPY:        explorer_clip(idx, 0);         break;
    case FXM_PASTE:       explorer_paste();              break;
    case FXM_RENAME:      explorer_start_rename(idx);    break;
    case FXM_DELETE:      explorer_delete(idx);          break;
    case FXM_RESTORE:     explorer_restore(idx);         break;
    case FXM_EMPTY_TRASH: file_explorer_empty_trash();   break;
    case FXM_REFRESH:     explorer_reload(NULL);         break;
    case FXM_NEW_FOLDER:  file_explorer_new_item(g_explorer.current_path, 1); break;
    case FXM_NEW_TEXT:    file_explorer_new_item(g_explorer.current_path, 0); break;
    case FXM_PROPERTIES: {
        file_entry_t *e = explorer_selected();
        explorer_show_properties(e ? e->path : g_explorer.current_path);
        break;
    }
    }
    desktop_invalidate();
}

static void explorer_open_menu(int screen_x, int screen_y)
{
    ui_menu_item_t items[10];
    int n = 0;
    file_entry_t *e = explorer_selected();
    int trash = fx_in_trash();
    int no_clip = !g_explorer.clipboard[0];

    if (e && trash) {
        items[n++] = (ui_menu_item_t){ "Restore", FXM_RESTORE,
                                       explorer_trash_origin(e->path) == NULL, 0 };
        items[n++] = (ui_menu_item_t){ "Cut", FXM_CUT, 0, 0 };
        items[n++] = (ui_menu_item_t){ "Delete permanently", FXM_DELETE, 0, 0 };
        items[n++] = (ui_menu_item_t){ "Properties", FXM_PROPERTIES, 0, 1 };
    } else if (e) {
        items[n++] = (ui_menu_item_t){ "Open", FXM_OPEN, 0, 0 };
        items[n++] = (ui_menu_item_t){ "Cut", FXM_CUT, 0, 1 };
        items[n++] = (ui_menu_item_t){ "Copy", FXM_COPY, 0, 0 };
        items[n++] = (ui_menu_item_t){ "Paste", FXM_PASTE, no_clip, 0 };
        items[n++] = (ui_menu_item_t){ "Rename", FXM_RENAME, 0, 1 };
        items[n++] = (ui_menu_item_t){ "Delete", FXM_DELETE, 0, 0 };
        items[n++] = (ui_menu_item_t){ "Properties", FXM_PROPERTIES, 0, 1 };
    } else if (trash) {
        items[n++] = (ui_menu_item_t){ "Empty Recycle Bin", FXM_EMPTY_TRASH,
                                       g_explorer.file_count == 0, 0 };
        items[n++] = (ui_menu_item_t){ "Refresh", FXM_REFRESH, 0, 0 };
    } else {
        items[n++] = (ui_menu_item_t){ "New folder", FXM_NEW_FOLDER, 0, 0 };
        items[n++] = (ui_menu_item_t){ "New text document", FXM_NEW_TEXT, 0, 0 };
        items[n++] = (ui_menu_item_t){ "Paste", FXM_PASTE, no_clip, 1 };
        items[n++] = (ui_menu_item_t){ "Refresh", FXM_REFRESH, 0, 0 };
        items[n++] = (ui_menu_item_t){ "Properties", FXM_PROPERTIES, 0, 1 };
    }
    ui_menu_open(screen_x, screen_y, items, n, explorer_menu_action, NULL);
}

/* ---- Drawing ------------------------------------------------------------- */
static void fx_format_size(long bytes, char *out, size_t n)
{
    if (bytes < 0)                  snprintf(out, n, "-");
    else if (bytes < 1024)          snprintf(out, n, "%d bytes", (int)bytes);
    else if (bytes < 1024 * 1024)   snprintf(out, n, "%d KB", (int)(bytes / 1024));
    else                            snprintf(out, n, "%d MB", (int)(bytes / (1024 * 1024)));
}

static void explorer_draw_properties(int x, int y, int w, int h)
{
    int pw = 340, ph = 214;
    if (pw > w - 16) pw = w - 16;
    int px = x + (w - pw) / 2, py = y + (h - ph) / 2;
    const char *path = g_explorer.props_path;
    int is_dir = vfs_is_dir(path);
    char buf[64], dir[FX_PATH_LEN];

    fb_blend_rect(x, y, w, h, 0x00000000, 90);
    fb_blend_rounded_rect(px + 2, py + 5, pw, ph, 8, 0x00000000, 80);
    fb_fill_rounded_rect(px, py, pw, ph, 8, T(BORDER));
    fb_fill_rounded_rect(px + 1, py + 1, pw - 2, ph - 2, 7, T(MENU));

    icon_draw(is_dir ? ICON_FOLDER : ICON_FILE, px + 20, py + 18, 32);
    ui_text_fit(px + 64, py + 18, pw - 80, fx_leaf(path)[0] ? fx_leaf(path) : path, T(TEXT));
    fb_draw_text(px + 64, py + 38, is_dir ? "File folder" : "File", T(TEXT_DIM));
    fb_fill_rect(px + 16, py + 66, pw - 32, 1, T(BORDER));

    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) *slash = 0;

    fb_draw_text(px + 20, py + 80, "Location", T(TEXT_DIM));
    ui_text_fit(px + 110, py + 80, pw - 126, dir, T(TEXT));
    fb_draw_text(px + 20, py + 104, "Size", T(TEXT_DIM));
    fx_format_size(vfs_size(path), buf, sizeof(buf));
    fb_draw_text(px + 110, py + 104, buf, T(TEXT));
    fb_draw_text(px + 20, py + 128, "Contains", T(TEXT_DIM));
    if (is_dir) snprintf(buf, sizeof(buf), "%d item(s)", vfs_count_children(path));
    else        snprintf(buf, sizeof(buf), "-");
    fb_draw_text(px + 110, py + 128, buf, T(TEXT));

    ui_button(px + pw - 100, py + ph - 46, 84, 30, "OK", T(ACCENT), T(ACCENT_TEXT));
}

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
    icon_draw(fx_in_trash() ? ICON_TRASH : ICON_FOLDER, x + 92, y + 14, 16);
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
        icon_draw(g_fx_places[i].icon, x + 12, iy + 5, 16);
        fb_draw_text(x + 36, iy + 5, g_fx_places[i].label, T(TEXT));
    }

    /* List header */
    int lx = x + FX_SIDEBAR_W, lw = w - FX_SIDEBAR_W;
    fb_fill_rect(lx, body_y + FX_HEADER_H - 1, lw, 1, T(BORDER));
    fb_draw_text(lx + 36, body_y + 5, "Name", T(TEXT_DIM));
    fb_draw_text(lx + lw - 180, body_y + 5, "Type", T(TEXT_DIM));
    fb_draw_text(lx + lw - 90, body_y + 5, "Size", T(TEXT_DIM));

    /* Rows */
    int rows = explorer_visible_rows(h);
    int ry = body_y + FX_HEADER_H;
    for (int i = g_explorer.scroll; i < g_explorer.file_count && i < g_explorer.scroll + rows; i++) {
        file_entry_t *e = &g_explorer.files[i];
        int selected = (i == g_explorer.selected);
        int cut = g_explorer.clipboard_cut && strcmp(g_explorer.clipboard, e->path) == 0;
        uint32_t fg = cut ? T(TEXT_DIM) : T(TEXT);
        uint32_t dim = T(TEXT_DIM);

        if (selected) {
            fb_fill_rounded_rect(lx + 4, ry + 1, lw - 8, FX_ROW_H - 2, 4, T(ACCENT));
            fg = dim = T(ACCENT_TEXT);
        }
        icon_draw(e->is_dir ? ICON_FOLDER : ICON_FILE, lx + 12, ry + 5, 16);

        if (i == g_explorer.renaming) {
            int ew = lw - 230;
            if (ew < 120) ew = 120;
            fb_fill_rect(lx + 32, ry + 2, ew, FX_ROW_H - 4, T(INPUT));
            fb_draw_rect(lx + 32, ry + 2, ew, FX_ROW_H - 4, T(ACCENT));
            if (g_explorer.rename_fresh) {
                int sw = g_explorer.rename_len * 8 + 2;
                fb_fill_rect(lx + 35, ry + 5, sw < ew - 8 ? sw : ew - 8, 16, T(ACCENT));
            }
            ui_text_fit(lx + 36, ry + 5, ew - 10, g_explorer.rename_buf,
                        g_explorer.rename_fresh ? T(ACCENT_TEXT) : T(TEXT));
            if (!g_explorer.rename_fresh)
                fb_fill_rect(lx + 37 + g_explorer.rename_len * 8, ry + 5, 1, 16, T(TEXT));
        } else {
            ui_text_fit(lx + 36, ry + 5, lw - 230, e->name, fg);
        }

        fb_draw_text(lx + lw - 180, ry + 5, e->is_dir ? "Folder" : "File", dim);
        if (!e->is_dir) {
            char size[24];
            fx_format_size(vfs_size(e->path), size, sizeof(size));
            fb_draw_text(lx + lw - 90, ry + 5, size, dim);
        }
        ry += FX_ROW_H;
    }
    if (g_explorer.file_count == 0)
        ui_text_center(lx, body_y + FX_HEADER_H + 30, lw,
                       fx_in_trash() ? "The Recycle Bin is empty." : "This folder is empty.",
                       T(TEXT_DIM));

    /* Status bar */
    int sy = y + h - FX_STATUS_H;
    char status[96];
    fb_fill_rect(x, sy, w, FX_STATUS_H, T(SURFACE));
    fb_fill_rect(x, sy, w, 1, T(BORDER));
    snprintf(status, sizeof(status), "%d item%s", g_explorer.file_count,
             g_explorer.file_count == 1 ? "" : "s");
    fb_draw_text(x + 12, sy + 5, status, T(TEXT_DIM));
    if (g_explorer.clipboard[0]) {
        snprintf(status, sizeof(status), "%s: %s", g_explorer.clipboard_cut ? "Cut" : "Copied",
                 fx_leaf(g_explorer.clipboard));
        ui_text_fit(x + 110, sy + 5, w - 122, status, T(TEXT));
    }

    if (g_explorer.props_open)
        explorer_draw_properties(x, y, w, h);
}

/* ---- Events -------------------------------------------------------------- */
static void explorer_keep_visible(int rows)
{
    if (g_explorer.selected < 0) return;
    if (g_explorer.selected < g_explorer.scroll)
        g_explorer.scroll = g_explorer.selected;
    if (g_explorer.selected >= g_explorer.scroll + rows)
        g_explorer.scroll = g_explorer.selected - rows + 1;
}

void explorer_event_callback(wm_window_t *win, wm_input_event_t *event)
{
    int x, y, w, h;
    wm_get_client(win, &x, &y, &w, &h);
    int rows = explorer_visible_rows(h);

    /* The properties card is modal: any click or key dismisses it. */
    if (g_explorer.props_open) {
        if (event->type == IE_MOUSE_DOWN || event->type == IE_KEY_DOWN)
            g_explorer.props_open = 0;
        return;
    }

    /* ---- Renaming: the keyboard edits the name ---- */
    if (g_explorer.renaming >= 0) {
        if (event->type == IE_TEXT_INPUT) {
            char ch = event->data.text.ch;
            if (ch >= 32 && ch < 127 && ch != '/' && g_explorer.rename_fresh) {
                g_explorer.rename_len = 0;      /* typing replaces the selected name */
                g_explorer.rename_fresh = 0;
            }
            if (ch >= 32 && ch < 127 && ch != '/' &&
                g_explorer.rename_len < FX_NAME_LEN - 1) {
                g_explorer.rename_buf[g_explorer.rename_len++] = ch;
                g_explorer.rename_buf[g_explorer.rename_len] = 0;
            }
            return;
        }
        if (event->type == IE_KEY_DOWN) {
            uint32_t key = event->data.keyboard.key;
            if (key == WM_KEY_ENTER)          explorer_commit_rename();
            else if (key == WM_KEY_ESC)       g_explorer.renaming = -1;
            else if (key == WM_KEY_BACKSPACE) {
                if (g_explorer.rename_fresh)
                    g_explorer.rename_len = 0;
                else if (g_explorer.rename_len > 0)
                    g_explorer.rename_len--;
                g_explorer.rename_buf[g_explorer.rename_len] = 0;
                g_explorer.rename_fresh = 0;
            } else if (key == WM_KEY_RIGHT || key == WM_KEY_END) {
                g_explorer.rename_fresh = 0;    /* keep the name, edit at the end */
            }
            return;
        }
        if (event->type == IE_MOUSE_DOWN)
            explorer_commit_rename();           /* clicking away accepts the name */
        else
            return;
    }

    if (event->type == IE_MOUSE_DOWN) {
        int mx = event->data.mouse.x, my = event->data.mouse.y;
        int button = event->data.mouse.button;

        if (my < FX_TOOLBAR_H) {
            if (button != 1) return;
            if (ui_hit(mx, my, 8, 7, 30, 30))       explorer_go_up();
            else if (ui_hit(mx, my, 44, 7, 30, 30)) explorer_reload(NULL);
            return;
        }
        if (my >= h - FX_STATUS_H) return;

        if (mx < FX_SIDEBAR_W) {
            int i = (my - FX_TOOLBAR_H - 34) / 28;
            if (button == 1 && my >= FX_TOOLBAR_H + 34 && i >= 0 && i < FX_PLACES)
                explorer_refresh_directory(g_fx_places[i].path);
            return;
        }

        int row = my - FX_TOOLBAR_H - FX_HEADER_H;
        int idx = row >= 0 ? g_explorer.scroll + row / FX_ROW_H : -1;
        if (idx >= g_explorer.file_count) idx = -1;

        if (button == 2) {
            g_explorer.selected = idx;
            explorer_open_menu(x + mx, y + my);
            return;
        }
        if (button != 1) return;

        int is_double = idx >= 0 && idx == g_explorer.last_click_idx &&
                        (event->timestamp - g_explorer.last_click_tick <= 50);
        g_explorer.last_click_idx = is_double ? -1 : idx;
        g_explorer.last_click_tick = event->timestamp;
        g_explorer.selected = idx;
        if (is_double) explorer_open(idx);
        return;
    }

    if (event->type == IE_KEY_DOWN) {
        uint32_t key = event->data.keyboard.key;
        int ctrl = event->data.keyboard.mods & KMOD_CTRL;

        if (ctrl && (key == 'c' || key == 'C'))      explorer_clip(g_explorer.selected, 0);
        else if (ctrl && (key == 'x' || key == 'X')) explorer_clip(g_explorer.selected, 1);
        else if (ctrl && (key == 'v' || key == 'V')) explorer_paste();
        else switch (key) {
        case WM_KEY_UP:
            if (g_explorer.selected > 0) g_explorer.selected--;
            else if (g_explorer.selected < 0 && g_explorer.file_count) g_explorer.selected = 0;
            break;
        case WM_KEY_DOWN:
            if (g_explorer.selected < g_explorer.file_count - 1) g_explorer.selected++;
            break;
        case WM_KEY_ENTER:      explorer_open(g_explorer.selected);          break;
        case WM_KEY_BACKSPACE:  explorer_go_up();                            break;
        case WM_KEY_DELETE:     explorer_delete(g_explorer.selected);        break;
        case WM_KEY_F1 + 1:     explorer_start_rename(g_explorer.selected);  break;  /* F2 */
        case WM_KEY_F1 + 4:     explorer_reload(NULL);                       break;  /* F5 */
        default: break;
        }
        explorer_keep_visible(rows);
    }
}

/* ---- Public API ---------------------------------------------------------- */
wm_window_t *file_explorer_open(int x, int y, int w, int h)
{
    /* The clipboard survives closing and reopening the window. */
    char clip[FX_PATH_LEN];
    int cut = g_explorer.clipboard_cut;
    memcpy(clip, g_explorer.clipboard, sizeof(clip));

    memset(&g_explorer, 0, sizeof(g_explorer));
    memcpy(g_explorer.clipboard, clip, sizeof(clip));
    g_explorer.clipboard_cut = cut;
    g_explorer.renaming = -1;

    wm_window_t *win = wm_create_window("Files", x, y, w, h, WS_DEFAULT,
                                        explorer_draw_callback,
                                        explorer_event_callback);
    if (!win) return NULL;

    win->icon = ICON_FOLDER;
    win->min_w = 480;
    win->min_h = 280;
    g_explorer.window = win;
    explorer_refresh_directory("/jarvis");
    return win;
}

void file_explorer_navigate_to(const char *path)
{
    explorer_refresh_directory(path);
}

/* ---- Operations usable without the Files window (desktop, other apps) ---- */

/* Creates "New folder" / "New Text Document.txt" (or the next free name) in
 * dir. Writes the new path to out. Returns 0 on success. */
int file_explorer_create_in(const char *dir, int folder, char *out, size_t n)
{
    vfs_mkdir(dir);
    if (fx_unique(out, n, dir, folder ? "New folder" : "New Text Document.txt") != 0)
        return -1;
    if ((folder ? vfs_mkdir(out) : vfs_create_file(out)) != 0)
        return -1;
    file_explorer_refresh();
    desktop_files_changed();
    return 0;
}

/* Moves a file or folder to the Recycle Bin, remembering where it was. */
int file_explorer_trash_path(const char *path)
{
    char target[FX_PATH_LEN];

    if (fx_is_protected(path) || fx_under(path, FX_TRASH))
        return -1;
    vfs_mkdir(FX_TRASH);
    if (fx_unique(target, sizeof(target), FX_TRASH, fx_leaf(path)) != 0 ||
        vfs_rename(path, target) != 0)
        return -1;

    for (int i = 0; i < FX_TRASH_MAX; i++) {
        if (!g_fx_trash[i].trash_path[0]) {
            strncpy(g_fx_trash[i].trash_path, target, FX_PATH_LEN - 1);
            strncpy(g_fx_trash[i].origin, path, FX_PATH_LEN - 1);
            break;
        }
    }
    file_explorer_refresh();
    desktop_files_changed();
    return 0;
}
