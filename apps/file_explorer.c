/*
 * =============================================================================
 * JARVIS OS — Files (apps/file_explorer.c)
 * Language: C17
 *
 * The file manager.
 *
 *   This PC        every drive (the JARVIS disk, each USB drive) with a
 *                  capacity bar, plus the main folders
 *   navigation     back / forward / up, a breadcrumb address bar (click a
 *                  part to jump there, or click the empty part to type a
 *                  path), a sidebar with Quick access, drives and the
 *                  Recycle Bin, and a search box that filters the folder
 *   views          details (sortable Name / Type / Size columns) or large
 *                  icons
 *   operations     new folder / text document, cut, copy, fx_paste, rename,
 *                  delete to the Recycle Bin, restore, empty, properties;
 *                  opening a package installs it, opening a program runs it
 *   keyboard       arrows, Enter, Backspace or Alt+Left (back), Alt+Right,
 *                  Alt+Up, F2 rename, Delete, F5 refresh, Ctrl+C/X/V,
 *                  Ctrl+F search
 *
 * USB drives are read-only: their files can be opened and copied onto the
 * JARVIS disk, but not changed.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "../wm/wm.h"
#include "../desktop/ui.h"
#include <string.h>
#include <stdio.h>

void notepad_open_file(const char *path);       /* apps/notepad.c */
void desktop_files_changed(void);               /* desktop/desktop.c */
void apps_store_show_install(const char *path); /* apps/store.c */

#define FX_MAX_FILES   256
#define FX_PATH_LEN    200
#define FX_NAME_LEN    96
#define FX_HISTORY     16
#define FX_TRASH       "/jarvis/trash"
#define FX_TRASH_MAX   64
#define LOC_THISPC     "::thispc"

#define TOOLBAR_H   46
#define CMDBAR_H    40
#define SIDEBAR_W   196
#define STATUS_H    26
#define ROW_H       26
#define HEADER_H    28
#define CELL_W      104
#define CELL_H      100
#define SIDE_ROW_H  28

enum { KIND_FOLDER, KIND_TEXT, KIND_PROGRAM, KIND_PACKAGE, KIND_IMAGE, KIND_AUDIO,
       KIND_MODEL, KIND_OTHER };
enum { SORT_NAME, SORT_TYPE, SORT_SIZE };
enum { VIEW_DETAILS, VIEW_ICONS };

typedef struct {
    char name[FX_NAME_LEN];
    char path[FX_PATH_LEN];
    int  is_dir;
    int  kind;
    long size;
} fx_file_entry_t;

typedef struct { int x, y, w, h; } fx_rect_t;
typedef struct { const char *label; const char *path; icon_id_t icon; fx_rect_t r; } fx_side_item_t;
typedef struct { const char *label; int id; int enabled; fx_rect_t r; } fx_cmd_t;

static struct {
    wm_window_t *window;
    char path[FX_PATH_LEN];                 /* current folder, or LOC_THISPC */
    fx_file_entry_t all[FX_MAX_FILES];
    int all_count;
    int shown[FX_MAX_FILES];                /* indices into all[] after search + sort */
    int shown_count;
    int selected;                           /* index into shown[] (or This PC tiles), -1 = none */
    int scroll;
    int view, sort, sort_desc;

    char back[FX_HISTORY][FX_PATH_LEN]; int back_n;
    char fwd[FX_HISTORY][FX_PATH_LEN];  int fwd_n;

    char search[48]; int search_len; int search_focus;
    char addr[FX_PATH_LEN]; int addr_len; int addr_focus;

    char clipboard[FX_PATH_LEN]; int clipboard_cut;

    int  renaming;
    char rename_buf[FX_NAME_LEN]; int rename_len; int rename_fresh;

    int  props_open;
    char props_path[FX_PATH_LEN];

    uint32_t last_click_tick; int last_click_idx;
    uint32_t storage_seen;

    drive_info_t drives[8]; int drive_count;

    /* hit areas from the last draw, in client coordinates */
    fx_side_item_t side[16]; int side_count;
    fx_cmd_t cmds[12]; int cmd_count;
    fx_rect_t crumbs[12]; char crumb_paths[12][FX_PATH_LEN]; int crumb_count;
    fx_rect_t pc_tiles[16]; char pc_paths[16][FX_PATH_LEN]; int pc_count;
    fx_rect_t col_name, col_type, col_size;
} fx;

static char fx_g_listing[32768];

/* Where each item in the Recycle Bin came from. */
static struct { char trash_path[FX_PATH_LEN]; char origin[FX_PATH_LEN]; } fx_g_trash[FX_TRASH_MAX];

/* ==========================================================================
 * Helpers
 * ========================================================================== */
static const char *fx_leaf_of(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static int fx_under(const char *path, const char *dir)
{
    size_t n = strlen(dir);
    return strncmp(path, dir, n) == 0 && (path[n] == '/' || path[n] == 0);
}

static int fx_at_thispc(void) { return strcmp(fx.path, LOC_THISPC) == 0; }
static int fx_in_trash(void)  { return fx_under(fx.path, FX_TRASH); }
static int fx_read_only_here(void) { return fx_at_thispc() || fatfs_owns(fx.path); }

static char fx_lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int fx_icmp(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (fx_lc(*a) != fx_lc(*b)) return (int)fx_lc(*a) - (int)fx_lc(*b);
    return (int)fx_lc(*a) - (int)fx_lc(*b);
}

static int fx_icontains(const char *hay, const char *needle)
{
    if (!*needle) return 1;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && fx_lc(*h) == fx_lc(*n)) { h++; n++; }
        if (!*n) return 1;
    }
    return 0;
}

static int fx_ext_is(const char *name, const char *ext)
{
    const char *dot = strrchr(name, '.');
    return dot && fx_icmp(dot + 1, ext) == 0;
}

/* System folders the OS depends on cannot be moved or deleted. */
static int fx_is_protected(const char *path)
{
    static const char *const fixed[] = {
        "/jarvis", "/jarvis/models", "/jarvis/bin", "/jarvis/logs", "/jarvis/data",
        "/jarvis/fonts", "/jarvis/screenshots", "/jarvis/desktop", "/jarvis/apps",
        "/jarvis/data/Downloads", FX_TRASH,
    };
    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++)
        if (strcmp(path, fixed[i]) == 0) return 1;
    return fx_under(path, "/jarvis/models") || fx_under(path, "/jarvis/apps") || fatfs_owns(path);
}

static void fx_toast(const char *title, const char *body)
{
    notification_show(title, body, NOTIF_WARNING, 4000);
}

static void fx_format_size(uint64_t bytes, char *out, size_t n)
{
    if (bytes < 1024)                       snprintf(out, n, "%u bytes", (unsigned)bytes);
    else if (bytes < 1024 * 1024)           snprintf(out, n, "%u KB", (unsigned)((bytes + 1023) / 1024));
    else if (bytes < 1024ULL * 1024 * 1024) snprintf(out, n, "%u.%u MB", (unsigned)(bytes >> 20),
                                                     (unsigned)(((bytes & 0xFFFFF) * 10) >> 20));
    else                                    snprintf(out, n, "%u.%u GB", (unsigned)(bytes >> 30),
                                                     (unsigned)(((bytes & 0x3FFFFFFF) * 10) >> 30));
}

/* dir/name, or "name (2)", "name (3)" ... if taken. 0 on success. */
static int fx_unique_path(char *out, size_t n, const char *dir, const char *name)
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

static int fx_classify(const char *path, const char *name, int is_dir)
{
    if (is_dir) return KIND_FOLDER;
    if (fx_ext_is(name, "jpk")) return KIND_PACKAGE;
    if (fx_ext_is(name, "txt") || fx_ext_is(name, "md") || fx_ext_is(name, "log") || fx_ext_is(name, "json") ||
        fx_ext_is(name, "html") || fx_ext_is(name, "htm") || fx_ext_is(name, "cfg") || fx_ext_is(name, "ini") ||
        fx_ext_is(name, "c") || fx_ext_is(name, "h") || fx_ext_is(name, "py") || fx_ext_is(name, "csv") ||
        fx_ext_is(name, "sh") || fx_ext_is(name, "xml"))
        return KIND_TEXT;
    if (fx_ext_is(name, "png") || fx_ext_is(name, "jpg") || fx_ext_is(name, "jpeg") || fx_ext_is(name, "bmp") ||
        fx_ext_is(name, "gif") || fx_ext_is(name, "webp"))
        return KIND_IMAGE;
    if (fx_ext_is(name, "wav") || fx_ext_is(name, "mp3") || fx_ext_is(name, "ogg") || fx_ext_is(name, "flac"))
        return KIND_AUDIO;
    if (fx_ext_is(name, "gguf") || fx_ext_is(name, "onnx") || fx_ext_is(name, "safetensors") ||
        fx_ext_is(name, "pth") || fx_ext_is(name, "pt") || fx_ext_is(name, "bin"))
        return KIND_MODEL;
    if (!strchr(name, '.') && user_is_program(path)) return KIND_PROGRAM;
    return KIND_OTHER;
}

static const char *fx_kind_name(const fx_file_entry_t *e, char *buf, size_t n)
{
    switch (e->kind) {
    case KIND_FOLDER:  return "File folder";
    case KIND_TEXT:    return "Text document";
    case KIND_PROGRAM: return "Program";
    case KIND_PACKAGE: return "JARVIS package";
    case KIND_IMAGE:   return "Image";
    case KIND_AUDIO:   return "Audio";
    case KIND_MODEL:   return "AI model file";
    default: {
        const char *dot = strrchr(e->name, '.');
        if (!dot || !dot[1]) return "File";
        size_t i = 0;
        for (dot++; *dot && i < 6; dot++) buf[i++] = (*dot >= 'a' && *dot <= 'z') ? (char)(*dot - 32) : *dot;
        snprintf(buf + i, n - i, " file");
        return buf;
    }
    }
}

static icon_id_t fx_kind_icon(int kind)
{
    switch (kind) {
    case KIND_FOLDER:  return ICON_FOLDER;
    case KIND_PROGRAM: return ICON_PROGRAM;
    case KIND_PACKAGE: return ICON_PACKAGE;
    default:           return ICON_FILE;
    }
}

/* Friendly name for a location: drive label for a drive root, else the leaf. */
static void fx_location_name(const char *path, char *out, size_t n)
{
    if (!strcmp(path, LOC_THISPC)) { snprintf(out, n, "This PC"); return; }
    if (!strcmp(path, FX_TRASH))   { snprintf(out, n, "Recycle Bin"); return; }
    if (!strcmp(path, "/jarvis/data")) { snprintf(out, n, "Documents"); return; }
    if (!strcmp(path, "/jarvis/desktop")) { snprintf(out, n, "Desktop"); return; }
    for (int i = 0; i < fx.drive_count; i++) {
        if (fx.drives[i].mount[0] && !strcmp(fx.drives[i].mount, path)) {
            snprintf(out, n, "%s", fx.drives[i].name);
            return;
        }
    }
    snprintf(out, n, "%s", fx_leaf_of(path));
}

static int fx_window_open(void)
{
    desktop_app_t *app = desktop_app_get(desktop_app_find("Files"));
    return app && app->window != NULL;
}

/* ==========================================================================
 * Loading, sorting, filtering
 * ========================================================================== */
static int fx_entry_cmp(const fx_file_entry_t *a, const fx_file_entry_t *b)
{
    if (a->is_dir != b->is_dir) return b->is_dir - a->is_dir;          /* folders first */
    int r = 0;
    if (fx.sort == SORT_SIZE) r = (a->size > b->size) - (a->size < b->size);
    else if (fx.sort == SORT_TYPE) {
        char x[24], y[24];
        r = fx_icmp(fx_kind_name(a, x, sizeof(x)), fx_kind_name(b, y, sizeof(y)));
    }
    if (r == 0) r = fx_icmp(a->name, b->name);
    return fx.sort_desc ? -r : r;
}

static void fx_apply_filter(void)
{
    fx.shown_count = 0;
    for (int i = 0; i < fx.all_count; i++)
        if (fx_icontains(fx.all[i].name, fx.search)) fx.shown[fx.shown_count++] = i;
    for (int i = 1; i < fx.shown_count; i++) {
        int k = fx.shown[i], j = i - 1;
        while (j >= 0 && fx_entry_cmp(&fx.all[fx.shown[j]], &fx.all[k]) > 0) {
            fx.shown[j + 1] = fx.shown[j];
            j--;
        }
        fx.shown[j + 1] = k;
    }
    if (fx.selected >= fx.shown_count && !fx_at_thispc()) fx.selected = fx.shown_count - 1;
}

static void fx_load_folder(void)
{
    fx.all_count = 0;
    fx.drive_count = storage_list(fx.drives, 8);
    if (fx_at_thispc()) { fx_apply_filter(); return; }

    if (!strcmp(fx.path, FX_TRASH)) vfs_mkdir(FX_TRASH);
    if (vfs_list(fx.path, fx_g_listing, sizeof(fx_g_listing)) < 0) { fx_apply_filter(); return; }

    size_t dlen = strlen(fx.path);
    for (const char *p = fx_g_listing; *p && fx.all_count < FX_MAX_FILES;) {
        const char *eol = p;
        while (*eol && *eol != '\n') eol++;
        int is_dir = strncmp(p, "[dir] ", 6) == 0;
        int is_file = strncmp(p, "[file] ", 7) == 0;
        const char *full = p + (is_dir ? 6 : 7);
        size_t flen = (size_t)(eol - full);

        if ((is_dir || is_file) && flen > dlen + 1 && flen < FX_PATH_LEN &&
            strncmp(full, fx.path, dlen) == 0 && full[dlen] == '/') {
            const char *name = full + dlen + 1;
            size_t nlen = flen - dlen - 1;
            for (size_t i = 0; i < nlen; i++) {        /* deeper path => direct child folder */
                if (name[i] == '/') { nlen = i; flen = dlen + 1 + nlen; is_dir = 1; break; }
            }
            /* the Recycle Bin has its own sidebar entry; not listed in Local Disk */
            int known = flen == strlen(FX_TRASH) && !strncmp(full, FX_TRASH, flen);
            for (int d = 0; d < fx.drive_count && !known; d++)       /* other drives: in This PC */
                if (fx.drives[d].removable && fx.drives[d].mount[0] &&
                    strlen(fx.drives[d].mount) == flen && !strncmp(fx.drives[d].mount, full, flen))
                    known = 1;
            for (int k = 0; k < fx.all_count && !known; k++)
                if (strlen(fx.all[k].name) == nlen && !strncmp(fx.all[k].name, name, nlen)) known = 1;
            if (!known && nlen && nlen < FX_NAME_LEN) {
                fx_file_entry_t *e = &fx.all[fx.all_count++];
                memcpy(e->name, name, nlen); e->name[nlen] = 0;
                memcpy(e->path, full, flen); e->path[flen] = 0;
                e->is_dir = is_dir;
                e->kind = fx_classify(e->path, e->name, is_dir);
                e->size = is_dir ? 0 : vfs_size(e->path);
            }
        }
        p = *eol ? eol + 1 : eol;
    }
    fx_apply_filter();
}

static void fx_update_title(void)
{
    char name[64], title[96];
    fx_location_name(fx.path, name, sizeof(name));
    snprintf(title, sizeof(title), "%s - Files", name);
    if (fx_window_open()) wm_set_title(fx.window, title);
}

/* Go somewhere. record = 1 pushes the current place on the back stack. */
static void fx_navigate(const char *path, int record)
{
    if (!path || !*path) path = LOC_THISPC;
    char target[FX_PATH_LEN];
    snprintf(target, sizeof(target), "%s", path);
    size_t l = strlen(target);
    while (l > 1 && target[l - 1] == '/') target[--l] = 0;

    if (!strcmp(target, FX_TRASH)) vfs_mkdir(FX_TRASH);
    if (strcmp(target, LOC_THISPC) && !vfs_is_dir(target)) {
        fx_toast("Files", "That folder does not exist.");
        return;
    }
    if (record && strcmp(target, fx.path)) {
        if (fx.back_n == FX_HISTORY) {
            memmove(fx.back[0], fx.back[1], sizeof(fx.back[0]) * (FX_HISTORY - 1));
            fx.back_n--;
        }
        snprintf(fx.back[fx.back_n++], FX_PATH_LEN, "%s", fx.path);
        fx.fwd_n = 0;
    }
    snprintf(fx.path, sizeof(fx.path), "%s", target);
    fx.selected = -1;
    fx.scroll = 0;
    fx.renaming = -1;
    fx.props_open = 0;
    fx.search[0] = 0; fx.search_len = 0; fx.search_focus = 0;
    fx.addr_focus = 0;
    fx.last_click_idx = -1;
    fx_load_folder();
    fx_update_title();
    desktop_invalidate();
}

static void fx_go_back(void)
{
    if (!fx.back_n) return;
    if (fx.fwd_n < FX_HISTORY) snprintf(fx.fwd[fx.fwd_n++], FX_PATH_LEN, "%s", fx.path);
    char p[FX_PATH_LEN];
    snprintf(p, sizeof(p), "%s", fx.back[--fx.back_n]);
    fx_navigate(p, 0);
}

static void fx_go_forward(void)
{
    if (!fx.fwd_n) return;
    if (fx.back_n < FX_HISTORY) snprintf(fx.back[fx.back_n++], FX_PATH_LEN, "%s", fx.path);
    char p[FX_PATH_LEN];
    snprintf(p, sizeof(p), "%s", fx.fwd[--fx.fwd_n]);
    fx_navigate(p, 0);
}

static void fx_go_up(void)
{
    if (fx_at_thispc()) return;
    for (int i = 0; i < fx.drive_count; i++)            /* drive root -> This PC */
        if (fx.drives[i].mount[0] && !strcmp(fx.drives[i].mount, fx.path)) { fx_navigate(LOC_THISPC, 1); return; }
    if (!strcmp(fx.path, FX_TRASH)) { fx_navigate(LOC_THISPC, 1); return; }
    char parent[FX_PATH_LEN];
    snprintf(parent, sizeof(parent), "%s", fx.path);
    char *s = strrchr(parent, '/');
    if (s && s != parent) { *s = 0; fx_navigate(parent, 1); }
    else fx_navigate(LOC_THISPC, 1);
}

/* Reload in place, keeping the selection by name. */
static void fx_reload(const char *select_name)
{
    char keep[FX_NAME_LEN] = "";
    if (select_name) snprintf(keep, sizeof(keep), "%s", select_name);
    else if (!fx_at_thispc() && fx.selected >= 0 && fx.selected < fx.shown_count)
        snprintf(keep, sizeof(keep), "%s", fx.all[fx.shown[fx.selected]].name);
    fx_load_folder();
    if (!fx_at_thispc()) {
        fx.selected = -1;
        for (int i = 0; keep[0] && i < fx.shown_count; i++)
            if (!strcmp(fx.all[fx.shown[i]].name, keep)) fx.selected = i;
    }
    desktop_invalidate();
}

void file_explorer_refresh(void)
{
    if (fx_window_open()) fx_reload(NULL);
}

static fx_file_entry_t *fx_selected_entry(void)
{
    if (fx_at_thispc() || fx.selected < 0 || fx.selected >= fx.shown_count) return NULL;
    return &fx.all[fx.shown[fx.selected]];
}

/* ==========================================================================
 * Operations
 * ========================================================================== */
static void fx_open_entry(fx_file_entry_t *e)
{
    if (!e) return;
    if (e->is_dir) {
        char p[FX_PATH_LEN];
        snprintf(p, sizeof(p), "%s", e->path);
        fx_navigate(p, 1);
        return;
    }
    switch (e->kind) {
    case KIND_PACKAGE:
        apps_store_show_install(e->path);
        break;
    case KIND_PROGRAM:
        desktop_app_launch(desktop_app_find("Terminal"));
        if (!user_exec(e->path)) fx_toast("Run", "The program could not be started.");
        break;
    case KIND_IMAGE: case KIND_AUDIO: case KIND_MODEL:
        fx_toast(e->name, "There is no app for this kind of file yet.");
        break;
    default:
        notepad_open_file(e->path);            /* it says so itself if it is not text */
        break;
    }
}

static void fx_start_rename(void)
{
    fx_file_entry_t *e = fx_selected_entry();
    if (!e) return;
    if (fx_is_protected(e->path)) {
        fx_toast("Rename", fatfs_owns(e->path) ? "USB drives are read-only." : "System folders cannot be renamed.");
        return;
    }
    fx.renaming = fx.selected;
    snprintf(fx.rename_buf, sizeof(fx.rename_buf), "%s", e->name);
    fx.rename_len = (int)strlen(fx.rename_buf);
    fx.rename_fresh = 1;
}

static void fx_commit_rename(void)
{
    int idx = fx.renaming;
    fx.renaming = -1;
    if (idx < 0 || idx >= fx.shown_count) return;
    fx_file_entry_t *e = &fx.all[fx.shown[idx]];
    if (!fx.rename_len || !strcmp(fx.rename_buf, e->name)) return;

    char target[FX_PATH_LEN];
    snprintf(target, sizeof(target), "%s/%s", fx.path, fx.rename_buf);
    if (vfs_exists(target)) { fx_toast("Rename", "An item with that name already exists here."); return; }
    if (vfs_rename(e->path, target) != 0) { fx_toast("Rename", "The item could not be renamed."); return; }
    desktop_files_changed();
    fx_reload(fx.rename_buf);
}

/* Move to the Recycle Bin, remembering where it came from. */
int file_explorer_trash_path(const char *path)
{
    char target[FX_PATH_LEN];
    if (fx_is_protected(path) || fx_under(path, FX_TRASH)) return -1;
    vfs_mkdir(FX_TRASH);
    if (fx_unique_path(target, sizeof(target), FX_TRASH, fx_leaf_of(path)) != 0 || vfs_rename(path, target) != 0)
        return -1;
    for (int i = 0; i < FX_TRASH_MAX; i++) {
        if (!fx_g_trash[i].trash_path[0]) {
            snprintf(fx_g_trash[i].trash_path, FX_PATH_LEN, "%s", target);
            snprintf(fx_g_trash[i].origin, FX_PATH_LEN, "%s", path);
            break;
        }
    }
    if (!strcmp(fx.clipboard, path)) fx.clipboard[0] = 0;
    file_explorer_refresh();
    desktop_files_changed();
    return 0;
}

static const char *fx_trash_origin(const char *trash_path)
{
    for (int i = 0; i < FX_TRASH_MAX; i++)
        if (fx_g_trash[i].trash_path[0] && !strcmp(fx_g_trash[i].trash_path, trash_path)) return fx_g_trash[i].origin;
    return NULL;
}

static void fx_delete_selected(void)
{
    fx_file_entry_t *e = fx_selected_entry();
    if (!e) return;
    if (fx_in_trash()) {                                  /* already binned: remove for good */
        for (int i = 0; i < FX_TRASH_MAX; i++)
            if (!strcmp(fx_g_trash[i].trash_path, e->path)) fx_g_trash[i].trash_path[0] = 0;
        vfs_delete(e->path);
        fx_reload(NULL);
        return;
    }
    if (fx_is_protected(e->path)) {
        fx_toast("Delete", fatfs_owns(e->path) ? "USB drives are read-only." : "System folders cannot be deleted.");
        return;
    }
    if (file_explorer_trash_path(e->path) != 0)
        fx_toast("Delete", "The item could not be moved to the Recycle Bin.");
}

static void fx_restore_selected(void)
{
    fx_file_entry_t *e = fx_selected_entry();
    const char *origin = e ? fx_trash_origin(e->path) : NULL;
    if (!origin) { fx_toast("Restore", "The original location of this item is not known."); return; }

    char dir[FX_PATH_LEN], target[FX_PATH_LEN];
    snprintf(dir, sizeof(dir), "%s", origin);
    char *s = strrchr(dir, '/');
    if (!s || s == dir) return;
    *s = 0;
    vfs_mkdir(dir);
    if (fx_unique_path(target, sizeof(target), dir, s + 1) != 0 || vfs_rename(e->path, target) != 0) {
        fx_toast("Restore", "The item could not be restored.");
        return;
    }
    for (int i = 0; i < FX_TRASH_MAX; i++)
        if (!strcmp(fx_g_trash[i].trash_path, e->path)) fx_g_trash[i].trash_path[0] = 0;
    desktop_files_changed();
    fx_reload(NULL);
}

int file_explorer_empty_trash(void)
{
    static char listing[8192];
    int removed = 0;
    size_t tlen = strlen(FX_TRASH);

    vfs_mkdir(FX_TRASH);
    for (int guard = 0; guard < 512; guard++) {
        if (vfs_list(FX_TRASH, listing, sizeof(listing)) < 0) break;
        char victim[FX_PATH_LEN] = "";
        for (const char *p = listing; *p && !victim[0];) {
            const char *eol = p;
            while (*eol && *eol != '\n') eol++;
            const char *full = strchr(p, '/');
            if (full && full < eol && (size_t)(eol - full) > tlen + 1 &&
                !strncmp(full, FX_TRASH, tlen) && full[tlen] == '/') {
                size_t n = tlen + 1;
                while (full + n < eol && full[n] != '/') n++;
                if (n < sizeof(victim)) { memcpy(victim, full, n); victim[n] = 0; }
            }
            p = *eol ? eol + 1 : eol;
        }
        if (!victim[0]) break;
        vfs_delete(victim);
        removed++;
    }
    for (int i = 0; i < FX_TRASH_MAX; i++) fx_g_trash[i].trash_path[0] = 0;
    notification_show("Recycle Bin", removed ? "The Recycle Bin has been emptied." : "The Recycle Bin is already empty.",
                      NOTIF_INFO, 3000);
    file_explorer_refresh();
    return removed;
}

static void fx_clip(int cut)
{
    fx_file_entry_t *e = fx_selected_entry();
    if (!e) return;
    if (cut && fx_is_protected(e->path)) {
        fx_toast("Cut", fatfs_owns(e->path) ? "USB drives are read-only: use Copy instead." : "System folders cannot be moved.");
        return;
    }
    snprintf(fx.clipboard, sizeof(fx.clipboard), "%s", e->path);
    fx.clipboard_cut = cut;
}

static void fx_paste(void)
{
    const char *src = fx.clipboard;
    if (!src[0]) return;
    if (fx_read_only_here() || fx_in_trash()) { fx_toast("Paste", "Items cannot be pasted here."); return; }
    if (!vfs_exists(src)) { fx_toast("Paste", "The copied item no longer exists."); fx.clipboard[0] = 0; return; }
    if (fx_under(fx.path, src)) { fx_toast("Paste", "A folder cannot be placed inside itself."); return; }

    char target[FX_PATH_LEN], name[FX_NAME_LEN];
    snprintf(name, sizeof(name), "%s", fx_leaf_of(src));
    snprintf(target, sizeof(target), "%s/%s", fx.path, name);
    if (fx.clipboard_cut && !strcmp(target, src)) return;
    if (fx_unique_path(target, sizeof(target), fx.path, name) != 0) { fx_toast("Paste", "No free name here."); return; }

    int rc = fx.clipboard_cut ? vfs_rename(src, target) : vfs_copy(src, target);
    if (rc != 0) { fx_toast("Paste", "The item could not be pasted (too large, or no space)."); return; }
    if (fx.clipboard_cut) fx.clipboard[0] = 0;
    desktop_files_changed();
    fx_reload(fx_leaf_of(target));
}

int file_explorer_create_in(const char *dir, int folder, char *out, size_t n)
{
    vfs_mkdir(dir);
    if (fx_unique_path(out, n, dir, folder ? "New folder" : "New Text Document.txt") != 0) return -1;
    if ((folder ? vfs_mkdir(out) : vfs_create_file(out)) != 0) return -1;
    file_explorer_refresh();
    desktop_files_changed();
    return 0;
}

static void fx_new_item(int folder)
{
    char path[FX_PATH_LEN];
    if (fx_read_only_here() || fx_in_trash()) { fx_toast("New", "New items cannot be created here."); return; }
    if (file_explorer_create_in(fx.path, folder, path, sizeof(path)) != 0) {
        fx_toast("New", "The item could not be created.");
        return;
    }
    fx_reload(fx_leaf_of(path));
    fx_start_rename();
}

/* ==========================================================================
 * Commands and menus
 * ========================================================================== */
enum {
    C_NEW_FOLDER = 1, C_NEW_TEXT, C_CUT, C_COPY, C_PASTE, C_RENAME, C_DELETE, C_PROPS,
    C_OPEN, C_RESTORE, C_EMPTY, C_REFRESH, C_INSTALL, C_RUN, C_NEW_MENU,
    C_SORT_MENU, C_SORT_NAME, C_SORT_TYPE, C_SORT_SIZE, C_VIEW_TOGGLE, C_OPEN_TERMINAL,
};

static void fx_do_command(int id, void *ctx);

static void fx_open_dropdown(int id)
{
    int x = 0, y = 0, wx, wy, ww, wh;
    for (int i = 0; i < fx.cmd_count; i++)
        if (fx.cmds[i].id == id) { x = fx.cmds[i].r.x; y = fx.cmds[i].r.y + fx.cmds[i].r.h; }
    wm_get_client(fx.window, &wx, &wy, &ww, &wh);
    if (id == C_NEW_MENU) {
        ui_menu_item_t items[2] = { { "Folder", C_NEW_FOLDER, 0, 0 }, { "Text document", C_NEW_TEXT, 0, 0 } };
        ui_menu_open(wx + x, wy + y + 2, items, 2, fx_do_command, NULL);
    } else {
        ui_menu_item_t items[3] = { { "Name", C_SORT_NAME, 0, 0 }, { "Type", C_SORT_TYPE, 0, 0 },
                                    { "Size", C_SORT_SIZE, 0, 0 } };
        ui_menu_open(wx + x, wy + y + 2, items, 3, fx_do_command, NULL);
    }
}

static void fx_do_command(int id, void *ctx)
{
    (void)ctx;
    fx_file_entry_t *e = fx_selected_entry();
    switch (id) {
    case C_NEW_FOLDER: fx_new_item(1); break;
    case C_NEW_TEXT:   fx_new_item(0); break;
    case C_CUT:        fx_clip(1); break;
    case C_COPY:       fx_clip(0); break;
    case C_PASTE:      fx_paste(); break;
    case C_RENAME:     fx_start_rename(); break;
    case C_DELETE:     fx_delete_selected(); break;
    case C_RESTORE:    fx_restore_selected(); break;
    case C_EMPTY:      file_explorer_empty_trash(); break;
    case C_REFRESH:    fx_reload(NULL); break;
    case C_OPEN: case C_INSTALL: case C_RUN: fx_open_entry(e); break;
    case C_PROPS:
        if (fx_at_thispc()) break;
        snprintf(fx.props_path, sizeof(fx.props_path), "%s", e ? e->path : fx.path);
        fx.props_open = 1;
        break;
    case C_SORT_NAME: case C_SORT_TYPE: case C_SORT_SIZE: {
        int s = id - C_SORT_NAME;
        if (fx.sort == s) fx.sort_desc = !fx.sort_desc; else { fx.sort = s; fx.sort_desc = 0; }
        fx_apply_filter();
        break;
    }
    case C_VIEW_TOGGLE: fx.view = fx.view == VIEW_DETAILS ? VIEW_ICONS : VIEW_DETAILS; fx.scroll = 0; break;
    case C_OPEN_TERMINAL: desktop_app_launch(desktop_app_find("Terminal")); break;
    case C_NEW_MENU: case C_SORT_MENU: fx_open_dropdown(id); break;
    }
    desktop_invalidate();
}

static void fx_build_commands(void)
{
    fx_file_entry_t *e = fx_selected_entry();
    int ro = fx_read_only_here(), sel = e != NULL;
    int n = 0;
#define CMD(l, i, en) do { fx.cmds[n].label = (l); fx.cmds[n].id = (i); fx.cmds[n].enabled = (en); n++; } while (0)
    if (fx_at_thispc()) {
        fx.cmd_count = 0;                       /* nothing to edit on This PC */
        return;
    }
    if (fx_in_trash()) {
        CMD("Restore", C_RESTORE, sel);
        CMD("Delete", C_DELETE, sel);
        CMD("Empty Recycle Bin", C_EMPTY, fx.all_count > 0);
    } else {
        CMD("New", C_NEW_MENU, !ro);
        CMD("Cut", C_CUT, sel && !ro);
        CMD("Copy", C_COPY, sel);
        CMD("Paste", C_PASTE, fx.clipboard[0] && !ro);
        CMD("Rename", C_RENAME, sel && !ro);
        CMD("Delete", C_DELETE, sel && !ro);
        if (e && e->kind == KIND_PACKAGE) CMD("Install", C_INSTALL, 1);
        if (e && e->kind == KIND_PROGRAM) CMD("Run", C_RUN, 1);
    }
    CMD("Sort", C_SORT_MENU, !fx_at_thispc());
    CMD(fx.view == VIEW_DETAILS ? "Large icons" : "Details", C_VIEW_TOGGLE, !fx_at_thispc());
#undef CMD
    fx.cmd_count = n;
}

static void fx_context_menu(int sx, int sy)
{
    ui_menu_item_t items[12];
    int n = 0;
    fx_file_entry_t *e = fx_selected_entry();
    int ro = fx_read_only_here(), noclip = !fx.clipboard[0];

    if (e && fx_in_trash()) {
        items[n++] = (ui_menu_item_t){ "Restore", C_RESTORE, fx_trash_origin(e->path) == NULL, 0 };
        items[n++] = (ui_menu_item_t){ "Delete permanently", C_DELETE, 0, 0 };
        items[n++] = (ui_menu_item_t){ "Properties", C_PROPS, 0, 1 };
    } else if (e) {
        items[n++] = (ui_menu_item_t){ e->kind == KIND_PACKAGE ? "Install" : e->kind == KIND_PROGRAM ? "Run" : "Open",
                                       C_OPEN, 0, 0 };
        items[n++] = (ui_menu_item_t){ "Cut", C_CUT, ro, 1 };
        items[n++] = (ui_menu_item_t){ "Copy", C_COPY, 0, 0 };
        items[n++] = (ui_menu_item_t){ "Paste", C_PASTE, noclip || ro, 0 };
        items[n++] = (ui_menu_item_t){ "Rename", C_RENAME, ro, 1 };
        items[n++] = (ui_menu_item_t){ "Delete", C_DELETE, ro, 0 };
        items[n++] = (ui_menu_item_t){ "Properties", C_PROPS, 0, 1 };
    } else if (fx_in_trash()) {
        items[n++] = (ui_menu_item_t){ "Empty Recycle Bin", C_EMPTY, fx.all_count == 0, 0 };
        items[n++] = (ui_menu_item_t){ "Refresh", C_REFRESH, 0, 0 };
    } else if (!fx_at_thispc()) {
        items[n++] = (ui_menu_item_t){ "New folder", C_NEW_FOLDER, ro, 0 };
        items[n++] = (ui_menu_item_t){ "New text document", C_NEW_TEXT, ro, 0 };
        items[n++] = (ui_menu_item_t){ "Paste", C_PASTE, noclip || ro, 1 };
        items[n++] = (ui_menu_item_t){ fx.view == VIEW_DETAILS ? "View: large icons" : "View: details", C_VIEW_TOGGLE, 0, 1 };
        items[n++] = (ui_menu_item_t){ "Sort by name", C_SORT_NAME, 0, 0 };
        items[n++] = (ui_menu_item_t){ "Sort by size", C_SORT_SIZE, 0, 0 };
        items[n++] = (ui_menu_item_t){ "Refresh", C_REFRESH, 0, 0 };
        items[n++] = (ui_menu_item_t){ "Open Terminal", C_OPEN_TERMINAL, 0, 1 };
        items[n++] = (ui_menu_item_t){ "Properties", C_PROPS, 0, 0 };
    } else {
        items[n++] = (ui_menu_item_t){ "Refresh", C_REFRESH, 0, 0 };
    }
    ui_menu_open(sx, sy, items, n, fx_do_command, NULL);
}

/* ==========================================================================
 * Drawing (all hit rectangles are stored relative to the client origin)
 * ========================================================================== */
static fx_rect_t fx_content_rect(int w, int h)
{
    return (fx_rect_t){ SIDEBAR_W, TOOLBAR_H + CMDBAR_H, w - SIDEBAR_W, h - TOOLBAR_H - CMDBAR_H - STATUS_H };
}

static int fx_addr_width(int w) { return w - 148 - 222; }

static void fx_draw_toolbar(int x, int y, int w)
{
    fb_fill_rect(x, y, w, TOOLBAR_H, T(SURFACE));
    struct { glyph_id_t g; int on; } nav[4] = {
        { GLYPH_BACK, fx.back_n > 0 }, { GLYPH_FORWARD, fx.fwd_n > 0 },
        { GLYPH_UP, !fx_at_thispc() }, { GLYPH_REFRESH, 1 },
    };
    for (int i = 0; i < 4; i++)
        glyph_draw(nav[i].g, x + 12 + i * 34, y + 14, 18, nav[i].on ? T(TEXT) : T(TEXT_DIM), T(SURFACE));

    int ax = x + 148, aw = fx_addr_width(w);
    fb_fill_rounded_rect(ax, y + 8, aw, 30, 5, T(INPUT));
    fb_draw_rounded_rect(ax, y + 8, aw, 30, 5, fx.addr_focus ? T(ACCENT) : T(BORDER));
    fx.crumb_count = 0;
    if (fx.addr_focus) {
        ui_text_fit(ax + 10, y + 15, aw - 20, fx.addr, T(TEXT));
        int cx = ax + 10 + fx.addr_len * 8;
        if (cx < ax + aw - 6) fb_fill_rect(cx, y + 15, 1, 16, T(TEXT));
    } else {
        /* This PC > Local Disk > data > projects */
        static char parts[12][FX_PATH_LEN];
        int np = 0;
        snprintf(parts[np++], FX_PATH_LEN, "%s", LOC_THISPC);
        if (!fx_at_thispc()) {
            const char *root = "/jarvis";
            for (int i = 0; i < fx.drive_count; i++)                /* deepest matching drive root */
                if (fx.drives[i].mount[0] && fx_under(fx.path, fx.drives[i].mount) &&
                    strlen(fx.drives[i].mount) > strlen(root)) root = fx.drives[i].mount;
            if (fx_under(fx.path, FX_TRASH)) root = FX_TRASH;
            snprintf(parts[np++], FX_PATH_LEN, "%s", root);
            const char *p = fx.path + strlen(root);
            while (*p == '/' && np < 12) {
                const char *slash = strchr(p + 1, '/');
                size_t len = slash ? (size_t)(slash - fx.path) : strlen(fx.path);
                memcpy(parts[np], fx.path, len);
                parts[np][len] = 0;
                np++;
                p = slash ? slash : fx.path + strlen(fx.path);
            }
        }
        int cx = ax + 8;
        icon_draw(fx_at_thispc() ? ICON_PC : fatfs_owns(fx.path) ? ICON_USB : fx_in_trash() ? ICON_TRASH : ICON_DRIVE,
                  cx, y + 15, 16);
        cx += 24;
        for (int i = 0; i < np && cx < ax + aw - 30; i++) {
            char name[64];
            fx_location_name(parts[i], name, sizeof(name));
            int tw = fb_text_width(name);
            if (i) { fb_draw_text(cx, y + 15, ">", T(TEXT_DIM)); cx += 16; }
            fb_draw_text(cx, y + 15, name, i == np - 1 ? T(TEXT) : T(TEXT_DIM));
            fx.crumbs[fx.crumb_count] = (fx_rect_t){ cx - 4 - x, 8, tw + 8, 30 };
            snprintf(fx.crumb_paths[fx.crumb_count++], FX_PATH_LEN, "%s", parts[i]);
            cx += tw + 8;
        }
    }

    int sx = x + w - 212;
    fb_fill_rounded_rect(sx, y + 8, 200, 30, 5, T(INPUT));
    fb_draw_rounded_rect(sx, y + 8, 200, 30, 5, fx.search_focus ? T(ACCENT) : T(BORDER));
    glyph_draw(GLYPH_SEARCH, sx + 8, y + 15, 16, T(TEXT_DIM), T(INPUT));
    if (fx.search_len || fx.search_focus) {
        ui_text_fit(sx + 30, y + 15, 160, fx.search, T(TEXT));
        if (fx.search_focus) fb_fill_rect(sx + 31 + fx.search_len * 8, y + 15, 1, 16, T(TEXT));
    } else {
        char name[64], hint[80];
        fx_location_name(fx.path, name, sizeof(name));
        snprintf(hint, sizeof(hint), "Search %s", name);
        ui_text_fit(sx + 30, y + 15, 160, hint, T(TEXT_DIM));
    }
}

static void fx_draw_cmdbar(int x, int y, int w)
{
    fx_build_commands();
    fb_fill_rect(x, y, w, CMDBAR_H, T(WINDOW));
    fb_fill_rect(x, y + CMDBAR_H - 1, w, 1, T(BORDER));
    int cx = x + 10;
    for (int i = 0; i < fx.cmd_count; i++) {
        fx_cmd_t *c = &fx.cmds[i];
        int dropdown = c->id == C_NEW_MENU || c->id == C_SORT_MENU;
        int tw = fb_text_width(c->label) + 20 + (dropdown ? 14 : 0);
        if (c->id == C_SORT_MENU && i > 0) { fb_fill_rect(cx + 2, y + 10, 1, 20, T(BORDER)); cx += 10; }
        if (cx + tw > x + w - 8) { c->r = (fx_rect_t){ -100, -100, 0, 0 }; continue; }
        c->r = (fx_rect_t){ cx - x, TOOLBAR_H + 6, tw, 28 };
        if ((c->id == C_NEW_MENU || c->id == C_INSTALL || c->id == C_RUN) && c->enabled)
            fb_fill_rounded_rect(cx, y + 6, tw, 28, 5, T(HOVER));
        fb_draw_text(cx + 10, y + 12, c->label, c->enabled ? T(TEXT) : T(TEXT_DIM));
        if (dropdown) fb_draw_text(cx + tw - 18, y + 12, "v", T(TEXT_DIM));
        cx += tw + 4;
    }
}

static void fx_draw_sidebar(int x, int y, int h)
{
    static const struct { const char *label; const char *path; icon_id_t icon; } quick[] = {
        { "Desktop",   "/jarvis/desktop",        ICON_PC },
        { "Documents", "/jarvis/data",           ICON_FOLDER },
        { "Downloads", "/jarvis/data/Downloads", ICON_FOLDER },
        { "Programs",  "/jarvis/bin",            ICON_PROGRAM },
        { "Apps",      "/jarvis/apps",           ICON_STORE },
    };
    int top = TOOLBAR_H + CMDBAR_H;           /* sidebar origin in client coordinates */

    fb_fill_rect(x, y, SIDEBAR_W, h, T(SURFACE));
    fb_fill_rect(x + SIDEBAR_W - 1, y, 1, h, T(BORDER));

    fx.side_count = 0;
    int ry = 8;
    fb_draw_text(x + 14, y + ry, "Quick access", T(TEXT_DIM));
    ry += 22;
    for (size_t i = 0; i < sizeof(quick) / sizeof(quick[0]); i++, ry += SIDE_ROW_H)
        fx.side[fx.side_count++] = (fx_side_item_t){ quick[i].label, quick[i].path, quick[i].icon,
                                                  { 0, top + ry, SIDEBAR_W, SIDE_ROW_H } };
    ry += 8;
    fx.side[fx.side_count++] = (fx_side_item_t){ "This PC", LOC_THISPC, ICON_PC, { 0, top + ry, SIDEBAR_W, SIDE_ROW_H } };
    ry += SIDE_ROW_H;
    for (int i = 0; i < fx.drive_count && fx.side_count < 14; i++) {
        if (!fx.drives[i].mount[0]) continue;
        fx.side[fx.side_count++] = (fx_side_item_t){ fx.drives[i].name, fx.drives[i].mount,
                                                  fx.drives[i].removable ? ICON_USB : ICON_DRIVE,
                                                  { 14, top + ry, SIDEBAR_W - 14, SIDE_ROW_H } };
        ry += SIDE_ROW_H;
    }
    ry += 8;
    fx.side[fx.side_count++] = (fx_side_item_t){ "Recycle Bin", FX_TRASH, ICON_TRASH, { 0, top + ry, SIDEBAR_W, SIDE_ROW_H } };

    for (int i = 0; i < fx.side_count; i++) {
        fx_side_item_t *s = &fx.side[i];
        int sy = y + s->r.y - top;
        if (sy + SIDE_ROW_H > y + h) { s->r.w = 0; continue; }
        if (!strcmp(fx.path, s->path)) {
            fb_fill_rounded_rect(x + 6 + s->r.x, sy + 1, SIDEBAR_W - 13 - s->r.x, SIDE_ROW_H - 2, 4, T(HOVER));
            fb_fill_rounded_rect(x + 6 + s->r.x, sy + 8, 3, 12, 1, T(ACCENT));
        }
        icon_draw(s->icon, x + 16 + s->r.x, sy + 6, 16);
        ui_text_fit(x + 40 + s->r.x, sy + 6, SIDEBAR_W - 48 - s->r.x, s->label, T(TEXT));
    }
}

static void fx_draw_drive_tile(int x, int y, int w, drive_info_t *d, int selected)
{
    char buf[96], a[24], b[24];
    if (selected) fb_fill_rounded_rect(x, y, w, 70, 6, T(HOVER));
    icon_draw(d->removable ? ICON_USB : ICON_DRIVE, x + 10, y + 13, 44);
    snprintf(buf, sizeof(buf), "%s (%s)", d->name, d->device[0] ? d->device : "memory");
    ui_text_fit(x + 66, y + 8, w - 74, buf, T(TEXT));
    if (d->total && d->mount[0]) {
        int pct = (int)((d->total - d->free) * 100 / d->total);
        ui_progress(x + 66, y + 30, w - 80, 10, pct, T(BORDER), pct >= 90 ? T(DANGER) : T(ACCENT));
        fx_format_size(d->free, a, sizeof(a));
        fx_format_size(d->total, b, sizeof(b));
        snprintf(buf, sizeof(buf), "%s free of %s%s", a, b, d->read_only ? "  (read-only)" : "");
        ui_text_fit(x + 66, y + 46, w - 74, buf, T(TEXT_DIM));
    } else if (!d->mount[0]) {
        ui_text_fit(x + 66, y + 32, w - 74, "Format not supported (needs FAT32)", T(WARN));
    } else {
        ui_text_fit(x + 66, y + 32, w - 74, "No disk: files are not saved", T(TEXT_DIM));
    }
}

static void fx_draw_thispc(fx_rect_t c, int ox, int oy)
{
    static const struct { const char *label; const char *path; icon_id_t icon; } folders[] = {
        { "Desktop", "/jarvis/desktop", ICON_PC }, { "Documents", "/jarvis/data", ICON_FOLDER },
        { "Downloads", "/jarvis/data/Downloads", ICON_FOLDER }, { "Programs", "/jarvis/bin", ICON_PROGRAM },
        { "Apps", "/jarvis/apps", ICON_STORE }, { "Recycle Bin", FX_TRASH, ICON_TRASH },
    };
    int x = ox + c.x + 20, y = oy + c.y + 14, w = c.w - 40;
    int cols = w >= 600 ? 2 : 1, tw = (w - (cols - 1) * 12) / cols;
    fx.pc_count = 0;

    fb_draw_text_bold(x, y, "Devices and drives", T(TEXT));
    y += 26;
    for (int i = 0; i < fx.drive_count && fx.pc_count < 16; i++) {
        int tx = x + (i % cols) * (tw + 12), ty = y + (i / cols) * 78;
        fx_draw_drive_tile(tx, ty, tw, &fx.drives[i], fx.selected == fx.pc_count);
        fx.pc_tiles[fx.pc_count] = (fx_rect_t){ tx - ox, ty - oy, tw, 70 };
        snprintf(fx.pc_paths[fx.pc_count++], FX_PATH_LEN, "%s", fx.drives[i].mount);
    }
    y += ((fx.drive_count + cols - 1) / cols) * 78 + 12;

    if (y + 40 >= oy + c.y + c.h) return;
    fb_draw_text_bold(x, y, "Folders", T(TEXT));
    y += 26;
    int fcols = w / CELL_W;
    if (fcols < 1) fcols = 1;
    for (size_t i = 0; i < sizeof(folders) / sizeof(folders[0]) && fx.pc_count < 16; i++) {
        int fx0 = x + (int)(i % (size_t)fcols) * CELL_W, fy0 = y + (int)(i / (size_t)fcols) * 84;
        if (fy0 + 80 > oy + c.y + c.h) break;
        if (fx.selected == fx.pc_count) fb_fill_rounded_rect(fx0, fy0, CELL_W - 8, 80, 6, T(HOVER));
        icon_draw(folders[i].icon, fx0 + (CELL_W - 8 - 40) / 2, fy0 + 8, 40);
        ui_text_center(fx0, fy0 + 56, CELL_W - 8, folders[i].label, T(TEXT));
        fx.pc_tiles[fx.pc_count] = (fx_rect_t){ fx0 - ox, fy0 - oy, CELL_W - 8, 80 };
        snprintf(fx.pc_paths[fx.pc_count++], FX_PATH_LEN, "%s", folders[i].path);
    }
}

static int fx_visible_rows(fx_rect_t c)
{
    int r = (c.h - HEADER_H) / ROW_H;
    return r < 1 ? 1 : r;
}

static int fx_icon_cols(fx_rect_t c)
{
    int n = (c.w - 16) / CELL_W;
    return n < 1 ? 1 : n;
}

/* Name, or the inline rename box when this item is being renamed. */
static void fx_draw_name(int x, int y, int box_w, fx_file_entry_t *e, int idx, uint32_t fg, int centered)
{
    if (idx != fx.renaming) {
        if (centered) {
            char s[16];
            size_t n = strlen(e->name);
            if (n > 12) { memcpy(s, e->name, 10); s[10] = '.'; s[11] = '.'; s[12] = 0; }
            else memcpy(s, e->name, n + 1);
            ui_text_center(x, y, box_w, s, fg);
        } else {
            ui_text_fit(x, y, box_w, e->name, fg);
        }
        return;
    }
    fb_fill_rect(x - 3, y - 3, box_w, 22, T(INPUT));
    fb_draw_rect(x - 3, y - 3, box_w, 22, T(ACCENT));
    if (fx.rename_fresh) {
        int sw = fx.rename_len * 8 + 2;
        fb_fill_rect(x - 1, y, sw < box_w - 4 ? sw : box_w - 4, 16, T(ACCENT));
    }
    ui_text_fit(x, y, box_w - 8, fx.rename_buf, fx.rename_fresh ? T(ACCENT_TEXT) : T(TEXT));
    if (!fx.rename_fresh) fb_fill_rect(x + 1 + fx.rename_len * 8, y, 1, 16, T(TEXT));
}

static void fx_draw_details(fx_rect_t c, int ox, int oy)
{
    int x = ox + c.x, y = oy + c.y;
    int type_x = c.w - 290, size_x = c.w - 110;
    char buf[32];

    fb_fill_rect(x, y + HEADER_H - 1, c.w, 1, T(BORDER));
    fx.col_name = (fx_rect_t){ c.x, c.y, type_x - 8, HEADER_H };
    fx.col_type = (fx_rect_t){ c.x + type_x - 8, c.y, size_x - type_x, HEADER_H };
    fx.col_size = (fx_rect_t){ c.x + size_x - 8, c.y, c.w - size_x + 8, HEADER_H };
    const char *names[3] = { "Name", "Type", "Size" };
    int xs[3] = { 40, type_x, size_x };
    for (int i = 0; i < 3; i++) {
        fb_draw_text(x + xs[i], y + 6, names[i], T(TEXT_DIM));
        if (fx.sort == i)
            fb_draw_text(x + xs[i] + fb_text_width(names[i]) + 6, y + 6, fx.sort_desc ? "v" : "^", T(ACCENT));
    }

    int rows = fx_visible_rows(c);
    if (fx.selected >= 0) {
        if (fx.selected < fx.scroll) fx.scroll = fx.selected;
        if (fx.selected >= fx.scroll + rows) fx.scroll = fx.selected - rows + 1;
    }
    int ry = y + HEADER_H;
    for (int i = fx.scroll; i < fx.shown_count && i < fx.scroll + rows; i++, ry += ROW_H) {
        fx_file_entry_t *e = &fx.all[fx.shown[i]];
        int cut = fx.clipboard_cut && !strcmp(fx.clipboard, e->path);
        uint32_t fg = cut ? T(TEXT_DIM) : T(TEXT), dim = T(TEXT_DIM);
        if (i == fx.selected) {
            fb_fill_rounded_rect(x + 4, ry + 1, c.w - 8, ROW_H - 2, 4, T(ACCENT));
            fg = dim = T(ACCENT_TEXT);
        }
        icon_draw(fx_kind_icon(e->kind), x + 14, ry + 5, 16);
        fx_draw_name(x + 40, ry + 5, type_x - 52, e, i, fg, 0);
        fb_draw_text(x + type_x, ry + 5, fx_kind_name(e, buf, sizeof(buf)), dim);
        if (!e->is_dir && e->size >= 0) {
            fx_format_size((uint64_t)e->size, buf, sizeof(buf));
            fb_draw_text(x + size_x, ry + 5, buf, dim);
        }
    }
}

static void fx_draw_icons(fx_rect_t c, int ox, int oy)
{
    int cols = fx_icon_cols(c), rows = c.h / CELL_H;
    if (rows < 1) rows = 1;
    if (fx.selected >= 0) {
        int r = fx.selected / cols;
        if (r < fx.scroll) fx.scroll = r;
        if (r >= fx.scroll + rows) fx.scroll = r - rows + 1;
    }
    for (int i = fx.scroll * cols; i < fx.shown_count && i < (fx.scroll + rows) * cols; i++) {
        fx_file_entry_t *e = &fx.all[fx.shown[i]];
        int cx = ox + c.x + 8 + (i % cols) * CELL_W, cy = oy + c.y + 8 + (i / cols - fx.scroll) * CELL_H;
        if (i == fx.selected) fb_fill_rounded_rect(cx, cy, CELL_W - 6, CELL_H - 6, 6, T(HOVER));
        icon_draw(fx_kind_icon(e->kind), cx + (CELL_W - 6 - 48) / 2, cy + 8, 48);
        fx_draw_name(cx + 2, cy + 66, CELL_W - 10, e, i, T(TEXT), 1);
    }
}

static void fx_draw_properties(int x, int y, int w, int h)
{
    int pw = 380, ph = 236;
    int px = x + (w - pw) / 2, py = y + (h - ph) / 2;
    const char *path = fx.props_path;
    int is_dir = vfs_is_dir(path);
    char buf[96], dir[FX_PATH_LEN], name[64];
    fx_file_entry_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    snprintf(tmp.name, sizeof(tmp.name), "%s", fx_leaf_of(path));
    tmp.is_dir = is_dir;
    tmp.kind = fx_classify(path, tmp.name, is_dir);

    fb_blend_rect(x, y, w, h, 0x00000000, 90);
    fb_blend_rounded_rect(px + 2, py + 5, pw, ph, 8, 0x00000000, 80);
    fb_fill_rounded_rect(px, py, pw, ph, 8, T(BORDER));
    fb_fill_rounded_rect(px + 1, py + 1, pw - 2, ph - 2, 7, T(MENU));

    icon_draw(fx_kind_icon(tmp.kind), px + 20, py + 18, 32);
    fx_location_name(path, name, sizeof(name));
    ui_text_fit(px + 64, py + 18, pw - 80, name, T(TEXT));
    fb_draw_text(px + 64, py + 38, fx_kind_name(&tmp, buf, sizeof(buf)), T(TEXT_DIM));
    fb_fill_rect(px + 16, py + 66, pw - 32, 1, T(BORDER));

    snprintf(dir, sizeof(dir), "%s", path);
    char *s = strrchr(dir, '/');
    if (s && s != dir) *s = 0;
    fb_draw_text(px + 20, py + 80, "Location", T(TEXT_DIM));
    ui_text_fit(px + 110, py + 80, pw - 126, dir, T(TEXT));
    fb_draw_text(px + 20, py + 104, "Size", T(TEXT_DIM));
    long size = vfs_size(path);
    if (size >= 0) fx_format_size((uint64_t)size, buf, sizeof(buf)); else snprintf(buf, sizeof(buf), "-");
    fb_draw_text(px + 110, py + 104, buf, T(TEXT));
    fb_draw_text(px + 20, py + 128, "Contains", T(TEXT_DIM));
    if (is_dir) snprintf(buf, sizeof(buf), "%d item(s)", vfs_count_children(path));
    else snprintf(buf, sizeof(buf), "-");
    fb_draw_text(px + 110, py + 128, buf, T(TEXT));
    fb_draw_text(px + 20, py + 152, "Access", T(TEXT_DIM));
    fb_draw_text(px + 110, py + 152, fatfs_owns(path) ? "Read-only (USB drive)" :
                 fx_is_protected(path) ? "System item" : "Read and write", T(TEXT));
    ui_button(px + pw - 100, py + ph - 46, 84, 30, "OK", T(ACCENT), T(ACCENT_TEXT));
}

static void fx_draw_status(int x, int y, int w)
{
    char left[96], right[96], a[24], b[24];
    fb_fill_rect(x, y, w, STATUS_H, T(SURFACE));
    fb_fill_rect(x, y, w, 1, T(BORDER));

    if (fx_at_thispc()) {
        snprintf(left, sizeof(left), "%d drive%s", fx.drive_count, fx.drive_count == 1 ? "" : "s");
    } else {
        int n = snprintf(left, sizeof(left), "%d item%s", fx.shown_count, fx.shown_count == 1 ? "" : "s");
        fx_file_entry_t *e = fx_selected_entry();
        if (e && !e->is_dir && e->size >= 0) {
            fx_format_size((uint64_t)e->size, a, sizeof(a));
            snprintf(left + n, sizeof(left) - (size_t)n, "    1 selected, %s", a);
        } else if (e) {
            snprintf(left + n, sizeof(left) - (size_t)n, "    1 selected");
        }
    }
    fb_draw_text(x + 12, y + 5, left, T(TEXT_DIM));

    right[0] = 0;
    if (fx.clipboard[0]) {
        snprintf(right, sizeof(right), "%s: %s", fx.clipboard_cut ? "Cut" : "Copied", fx_leaf_of(fx.clipboard));
    } else if (!fx_at_thispc()) {
        for (int i = fx.drive_count - 1; i >= 0; i--) {
            if (fx.drives[i].mount[0] && fx_under(fx.path, fx.drives[i].mount) && fx.drives[i].total) {
                fx_format_size(fx.drives[i].free, a, sizeof(a));
                fx_format_size(fx.drives[i].total, b, sizeof(b));
                snprintf(right, sizeof(right), "%s free of %s", a, b);
                break;
            }
        }
    }
    if (right[0]) fb_draw_text(x + w - 12 - fb_text_width(right), y + 5, right, T(TEXT_DIM));
}

void explorer_draw_callback(wm_window_t *win, uint32_t *framebuffer)
{
    (void)framebuffer;
    int x, y, w, h;
    wm_get_client(win, &x, &y, &w, &h);

    /* A drive was plugged in or removed: refresh; leave a drive that is gone. */
    if (fx.storage_seen != g_storage_generation) {
        fx.storage_seen = g_storage_generation;
        if (!fx_at_thispc() && !vfs_is_dir(fx.path) && !fx_in_trash()) {
            fx_toast("Files", "The drive was removed.");
            fx.back_n = fx.fwd_n = 0;
            fx_navigate(LOC_THISPC, 0);
        } else {
            fx_reload(NULL);
        }
    }

    fx_rect_t c = fx_content_rect(w, h);
    fx_draw_toolbar(x, y, w);
    fx_draw_cmdbar(x, y + TOOLBAR_H, w);
    fx_draw_sidebar(x, y + TOOLBAR_H + CMDBAR_H, c.h);

    fb_fill_rect(x + c.x, y + c.y, c.w, c.h, T(WINDOW));
    fb_clip_t saved = fb_get_clip();
    fb_set_clip(x + c.x, y + c.y, c.w, c.h);
    if (fx_at_thispc())                fx_draw_thispc(c, x, y);
    else if (fx.view == VIEW_ICONS) fx_draw_icons(c, x, y);
    else                            fx_draw_details(c, x, y);
    if (!fx_at_thispc() && fx.shown_count == 0) {
        const char *msg = fx.search_len ? "No items match your search." :
                          fx_in_trash() ? "The Recycle Bin is empty." : "This folder is empty.";
        ui_text_center(x + c.x, y + c.y + 60, c.w, msg, T(TEXT_DIM));
    }
    fb_restore_clip(saved);

    fx_draw_status(x, y + h - STATUS_H, w);
    if (fx.props_open) fx_draw_properties(x, y, w, h);
}

/* ==========================================================================
 * Input
 * ========================================================================== */
static int fx_rhit(fx_rect_t r, int mx, int my) { return ui_hit(mx, my, r.x, r.y, r.w, r.h); }

static int fx_item_at(fx_rect_t c, int mx, int my)
{
    if (fx_at_thispc()) {
        for (int i = 0; i < fx.pc_count; i++) if (fx_rhit(fx.pc_tiles[i], mx, my)) return i;
        return -1;
    }
    if (fx.view == VIEW_ICONS) {
        int cols = fx_icon_cols(c);
        if (mx < c.x + 8 || my < c.y + 8) return -1;
        int col = (mx - c.x - 8) / CELL_W, row = (my - c.y - 8) / CELL_H;
        if (col >= cols) return -1;
        int i = (row + fx.scroll) * cols + col;
        return i < fx.shown_count ? i : -1;
    }
    if (my < c.y + HEADER_H) return -1;
    int i = fx.scroll + (my - c.y - HEADER_H) / ROW_H;
    return i < fx.shown_count ? i : -1;
}

static void fx_activate(int idx)
{
    if (fx_at_thispc()) {
        if (idx < 0 || idx >= fx.pc_count) return;
        if (!fx.pc_paths[idx][0]) {
            fx_toast("Files", "This drive's format is not supported yet (only FAT32 and FAT16).");
            return;
        }
        char p[FX_PATH_LEN];
        snprintf(p, sizeof(p), "%s", fx.pc_paths[idx]);
        fx_navigate(p, 1);
        return;
    }
    if (idx >= 0 && idx < fx.shown_count) fx_open_entry(&fx.all[fx.shown[idx]]);
}

static int fx_text_edit(char *buf, int *len, int max, wm_input_event_t *ev)
{
    if (ev->type == IE_TEXT_INPUT) {
        char ch = ev->data.text.ch;
        if (ch >= 32 && ch < 127 && *len < max - 1) { buf[(*len)++] = ch; buf[*len] = 0; return 1; }
        return 0;
    }
    if (ev->type == IE_KEY_DOWN && ev->data.keyboard.key == WM_KEY_BACKSPACE) {
        if (*len) buf[--(*len)] = 0;
        return 1;
    }
    return 0;
}

void explorer_event_callback(wm_window_t *win, wm_input_event_t *event)
{
    int x, y, w, h;
    wm_get_client(win, &x, &y, &w, &h);
    fx_rect_t c = fx_content_rect(w, h);

    if (fx.props_open) {
        if (event->type == IE_MOUSE_DOWN || event->type == IE_KEY_DOWN) fx.props_open = 0;
        return;
    }

    /* ---- text fields: rename, address, search ---- */
    if (fx.renaming >= 0) {
        if (event->type == IE_TEXT_INPUT) {
            char ch = event->data.text.ch;
            if (ch >= 32 && ch < 127 && ch != '/') {
                if (fx.rename_fresh) { fx.rename_len = 0; fx.rename_fresh = 0; }
                if (fx.rename_len < FX_NAME_LEN - 1) { fx.rename_buf[fx.rename_len++] = ch; fx.rename_buf[fx.rename_len] = 0; }
            }
            return;
        }
        if (event->type == IE_KEY_DOWN) {
            uint32_t k = event->data.keyboard.key;
            if (k == WM_KEY_ENTER) fx_commit_rename();
            else if (k == WM_KEY_ESC) fx.renaming = -1;
            else if (k == WM_KEY_BACKSPACE) {
                if (fx.rename_fresh) fx.rename_len = 0; else if (fx.rename_len) fx.rename_len--;
                fx.rename_buf[fx.rename_len] = 0;
                fx.rename_fresh = 0;
            } else if (k == WM_KEY_RIGHT || k == WM_KEY_END) fx.rename_fresh = 0;
            return;
        }
        if (event->type == IE_MOUSE_DOWN) fx_commit_rename();
        else return;
    }
    if (fx.addr_focus && (event->type == IE_TEXT_INPUT || event->type == IE_KEY_DOWN)) {
        uint32_t k = event->type == IE_KEY_DOWN ? event->data.keyboard.key : 0;
        if (k == WM_KEY_ENTER) {
            char p[FX_PATH_LEN];
            snprintf(p, sizeof(p), "%s", fx.addr);
            fx.addr_focus = 0;
            if (!*p || !fx_icmp(p, "This PC")) fx_navigate(LOC_THISPC, 1);
            else if (p[0] == '/') fx_navigate(p, 1);
            else { char q[FX_PATH_LEN]; snprintf(q, sizeof(q), "/jarvis/%s", p); fx_navigate(q, 1); }
        } else if (k == WM_KEY_ESC) {
            fx.addr_focus = 0;
        } else {
            fx_text_edit(fx.addr, &fx.addr_len, (int)sizeof(fx.addr), event);
        }
        return;
    }
    if (fx.search_focus && (event->type == IE_TEXT_INPUT || event->type == IE_KEY_DOWN)) {
        uint32_t k = event->type == IE_KEY_DOWN ? event->data.keyboard.key : 0;
        if (k == WM_KEY_ESC) { fx.search_focus = 0; fx.search[0] = 0; fx.search_len = 0; fx_apply_filter(); }
        else if (k == WM_KEY_ENTER || k == WM_KEY_DOWN) { fx.search_focus = 0; if (fx.shown_count) fx.selected = 0; }
        else if (fx_text_edit(fx.search, &fx.search_len, (int)sizeof(fx.search), event)) {
            fx.selected = -1; fx.scroll = 0; fx_apply_filter();
        }
        return;
    }

    /* ---- mouse ---- */
    if (event->type == IE_MOUSE_DOWN) {
        int mx = event->data.mouse.x, my = event->data.mouse.y, button = event->data.mouse.button;
        fx.search_focus = 0;
        fx.addr_focus = 0;

        if (my < TOOLBAR_H) {
            if (button != 1) return;
            if (ui_hit(mx, my, 6, 6, 32, 34))        fx_go_back();
            else if (ui_hit(mx, my, 40, 6, 32, 34))  fx_go_forward();
            else if (ui_hit(mx, my, 74, 6, 32, 34))  fx_go_up();
            else if (ui_hit(mx, my, 108, 6, 32, 34)) fx_reload(NULL);
            else if (ui_hit(mx, my, w - 212, 8, 200, 30)) {
                if (!fx_at_thispc()) fx.search_focus = 1;
            } else if (ui_hit(mx, my, 148, 8, fx_addr_width(w), 30)) {
                for (int i = 0; i < fx.crumb_count; i++) {
                    if (fx_rhit(fx.crumbs[i], mx, my)) {
                        char p[FX_PATH_LEN];
                        snprintf(p, sizeof(p), "%s", fx.crumb_paths[i]);
                        fx_navigate(p, 1);
                        return;
                    }
                }
                fx.addr_focus = 1;                  /* the empty part: type a path */
                snprintf(fx.addr, sizeof(fx.addr), "%s", fx_at_thispc() ? "" : fx.path);
                fx.addr_len = (int)strlen(fx.addr);
            }
            return;
        }
        if (my < TOOLBAR_H + CMDBAR_H) {
            if (button != 1) return;
            for (int i = 0; i < fx.cmd_count; i++)
                if (fx.cmds[i].enabled && fx_rhit(fx.cmds[i].r, mx, my)) { fx_do_command(fx.cmds[i].id, NULL); return; }
            return;
        }
        if (my >= h - STATUS_H) return;
        if (mx < SIDEBAR_W) {
            if (button != 1) return;
            for (int i = 0; i < fx.side_count; i++)
                if (fx.side[i].r.w && fx_rhit(fx.side[i].r, mx, my)) { fx_navigate(fx.side[i].path, 1); return; }
            return;
        }
        if (!fx_at_thispc() && fx.view == VIEW_DETAILS && my < c.y + HEADER_H) {
            if (button != 1) return;
            if (fx_rhit(fx.col_name, mx, my))      fx_do_command(C_SORT_NAME, NULL);
            else if (fx_rhit(fx.col_type, mx, my)) fx_do_command(C_SORT_TYPE, NULL);
            else if (fx_rhit(fx.col_size, mx, my)) fx_do_command(C_SORT_SIZE, NULL);
            return;
        }

        int idx = fx_item_at(c, mx, my);
        if (button == 2) {
            fx.selected = idx;
            fx_context_menu(x + mx, y + my);
            return;
        }
        if (button != 1) return;
        int dbl = idx >= 0 && idx == fx.last_click_idx && event->timestamp - fx.last_click_tick <= 50;
        fx.last_click_idx = dbl ? -1 : idx;
        fx.last_click_tick = event->timestamp;
        fx.selected = idx;
        if (dbl) fx_activate(idx);
        return;
    }

    /* ---- keyboard ---- */
    if (event->type != IE_KEY_DOWN) return;
    uint32_t k = event->data.keyboard.key;
    int mods = event->data.keyboard.mods, ctrl = mods & KMOD_CTRL, alt = mods & KMOD_ALT;
    int count = fx_at_thispc() ? fx.pc_count : fx.shown_count;
    int step = (!fx_at_thispc() && fx.view == VIEW_ICONS) ? fx_icon_cols(c) : 1;

    if (ctrl && (k == 'c' || k == 'C'))      fx_clip(0);
    else if (ctrl && (k == 'x' || k == 'X')) fx_clip(1);
    else if (ctrl && (k == 'v' || k == 'V')) fx_paste();
    else if (ctrl && (k == 'f' || k == 'F')) { if (!fx_at_thispc()) fx.search_focus = 1; }
    else if (alt && k == WM_KEY_LEFT)  fx_go_back();
    else if (alt && k == WM_KEY_RIGHT) fx_go_forward();
    else if (alt && k == WM_KEY_UP)    fx_go_up();
    else switch (k) {
    case WM_KEY_UP:
        if (fx.selected - step >= 0) fx.selected -= step;
        else if (fx.selected < 0 && count) fx.selected = 0;
        break;
    case WM_KEY_DOWN:
        if (fx.selected < 0 && count) fx.selected = 0;
        else if (fx.selected + step < count) fx.selected += step;
        break;
    case WM_KEY_LEFT:  if (step > 1 && fx.selected > 0) fx.selected--; break;
    case WM_KEY_RIGHT: if (step > 1 && fx.selected + 1 < count) fx.selected++; break;
    case WM_KEY_HOME:  if (count) fx.selected = 0; break;
    case WM_KEY_END:   if (count) fx.selected = count - 1; break;
    case WM_KEY_ENTER: fx_activate(fx.selected); break;
    case WM_KEY_BACKSPACE: fx_go_back(); break;
    case WM_KEY_DELETE: if (!fx_at_thispc()) fx_delete_selected(); break;
    case WM_KEY_F1 + 1: if (!fx_at_thispc()) fx_start_rename(); break;      /* F2 */
    case WM_KEY_F1 + 4: fx_reload(NULL); break;                          /* F5 */
    default: break;
    }
}

/* ==========================================================================
 * Public API
 * ========================================================================== */
wm_window_t *file_explorer_open(int x, int y, int w, int h)
{
    /* the clipboard and view choices survive closing the window */
    char clipb[FX_PATH_LEN];
    int cut = fx.clipboard_cut, view = fx.view, sort = fx.sort;
    memcpy(clipb, fx.clipboard, sizeof(clipb));
    memset(&fx, 0, sizeof(fx));
    memcpy(fx.clipboard, clipb, sizeof(clipb));
    fx.clipboard_cut = cut;
    fx.view = view;
    fx.sort = sort;
    fx.renaming = -1;
    fx.selected = -1;
    fx.storage_seen = g_storage_generation;
    snprintf(fx.path, sizeof(fx.path), "%s", LOC_THISPC);

    wm_window_t *win = wm_create_window("This PC - Files", x, y, w, h, WS_DEFAULT,
                                        explorer_draw_callback, explorer_event_callback);
    if (!win) return NULL;
    win->icon = ICON_FOLDER;
    win->min_w = 640;
    win->min_h = 360;
    fx.window = win;
    fx_navigate(LOC_THISPC, 0);
    return win;
}

void file_explorer_navigate_to(const char *path)
{
    fx.back_n = 0;
    fx.fwd_n = 0;
    fx_navigate(path, 0);
}
