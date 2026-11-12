/*
 * =============================================================================
 * JARVIS OS — File Explorer Application (apps/file_explorer.c)
 * Language: C17
 *
 * Full-featured file browser with list/grid views, breadcrumb navigation,
 * search integration, and VFS integration.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "../wm/wm.h"
#include "../drivers/gpu.h"
#include <string.h>
#include <stdio.h>

#define MAX_FILES      256
#define MAX_PATH_LEN   256
#define MAX_FILENAME   128

typedef enum {
    VIEW_LIST = 0,
    VIEW_GRID = 1,
    VIEW_DETAILS = 2,
} view_mode_t;

typedef struct {
    char name[MAX_FILENAME];
    char path[MAX_PATH_LEN];
    int is_dir;
    uint32_t size;
    uint32_t modified;
} file_entry_t;

typedef struct {
    wm_window_t *window;
    char current_path[MAX_PATH_LEN];
    file_entry_t files[MAX_FILES];
    int file_count;
    int selected_idx;
    view_mode_t view_mode;
    char search_query[128];
    int search_active;
    int scroll_offset;
    uint32_t last_click_timestamp;
} explorer_context_t;

static explorer_context_t g_explorer = {};

/* ---- File Operations ----------------------------------------------------- */
void explorer_refresh_directory(const char *path)
{
    if (!path) path = "/jarvis";

    strncpy(g_explorer.current_path, path, sizeof(g_explorer.current_path) - 1);
    g_explorer.current_path[sizeof(g_explorer.current_path) - 1] = '\0';

    /* Clear file list */
    memset(g_explorer.files, 0, sizeof(g_explorer.files));
    g_explorer.file_count = 0;
    g_explorer.selected_idx = 0;

    /* Read directory using kernel VFS */
    char buffer[2048];
    if (vfs_list(path, buffer, sizeof(buffer)) < 0) {
        terminal_printf("[EXPLORER] Failed to list: %s\n", path);
        return;
    }

    /* Parse VFS output (simple format: "name|type|size\nname|type|size\n...").
     * Keep this freestanding: sscanf is not available in the kernel link. */
    const char *p = buffer;
    while (*p && g_explorer.file_count < MAX_FILES) {
        char name[MAX_FILENAME];
        char type_str[16];
        int size = 0;
        int name_len = 0;
        int type_len = 0;

        memset(name, 0, sizeof(name));
        memset(type_str, 0, sizeof(type_str));

        while (*p && *p != '|' && *p != '\n') {
            if (name_len + 1 < (int)sizeof(name))
                name[name_len++] = *p;
            p++;
        }
        if (*p != '|')
            break;
        p++;

        while (*p && *p != '|' && *p != '\n') {
            if (type_len + 1 < (int)sizeof(type_str))
                type_str[type_len++] = *p;
            p++;
        }
        if (*p != '|')
            break;
        p++;

        while (*p >= '0' && *p <= '9') {
            size = size * 10 + (*p - '0');
            p++;
        }

        file_entry_t *entry = &g_explorer.files[g_explorer.file_count];
        strncpy(entry->name, name, sizeof(entry->name) - 1);
        snprintf(entry->path, sizeof(entry->path), "%s/%s", path, name);
        entry->is_dir = (strcmp(type_str, "dir") == 0);
        entry->size = size;
        g_explorer.file_count++;

        /* Skip to next line */
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }

    terminal_printf("[EXPLORER] Loaded %d entries from %s\n", g_explorer.file_count, path);
}

/* ---- Drawing Functions --------------------------------------------------- */
void explorer_draw_toolbar(wm_window_t *win, uint32_t *framebuffer)
{
    int x = win->x + 8;
    int y = win->y + 40;
    int w = win->w - 16;

    /* Breadcrumb navigation */
    fb_fill_rect(x, y, w, 30, 0x00282828);
    fb_put_string(x + 4, y + 8, "Path: ", 0x00808080, 0x00282828);
    fb_put_string(x + 40, y + 8, g_explorer.current_path, 0x00E0E0E0, 0x00282828);

    /* View mode buttons */
    int btn_y = y + 35;
    fb_fill_rect(x, btn_y, 60, 28, 0x00353535);
    fb_put_string(x + 12, btn_y + 8, "List", 0x00E0E0E0, 0x00353535);

    fb_fill_rect(x + 65, btn_y, 60, 28, 0x005BA3D6);
    fb_put_string(x + 77, btn_y + 8, "Grid", 0x00FFFFFF, 0x005BA3D6);

    fb_fill_rect(x + 130, btn_y, 60, 28, 0x00353535);
    fb_put_string(x + 140, btn_y + 8, "Details", 0x00E0E0E0, 0x00353535);

    /* Search box */
    int search_x = x + w - 250;
    fb_fill_rect(search_x, btn_y, 240, 28, 0x00353535);
    fb_put_string(search_x + 4, btn_y + 8, "Search files...", 0x00808080, 0x00353535);
}

void explorer_draw_file_list(wm_window_t *win, uint32_t *framebuffer)
{
    int x = win->x + 8;
    int y = win->y + 110;
    int row_h = 24;
    int visible_rows = (win->h - 140) / row_h;

    /* Column headers */
    fb_fill_rect(x, y, win->w - 16, row_h, 0x003A3A3A);
    fb_put_string(x + 4, y + 6, "Name", 0x00A0A0A0, 0x003A3A3A);
    fb_put_string(x + 300, y + 6, "Type", 0x00A0A0A0, 0x003A3A3A);
    fb_put_string(x + 450, y + 6, "Size", 0x00A0A0A0, 0x003A3A3A);

    /* File entries */
    y += row_h;
    for (int i = g_explorer.scroll_offset;
         i < g_explorer.file_count && i < g_explorer.scroll_offset + visible_rows;
         i++) {
        file_entry_t *entry = &g_explorer.files[i];

        /* Row background */
        uint32_t bg = (i == g_explorer.selected_idx) ? 0x005BA3D6 : 0x001F1F1F;
        uint32_t fg = (i == g_explorer.selected_idx) ? 0x00FFFFFF : 0x00E0E0E0;

        fb_fill_rect(x, y, win->w - 16, row_h, bg);

        /* File icon + name */
        const char *icon = entry->is_dir ? "[D]" : "[F]";
        fb_put_string(x + 4, y + 6, icon, 0x0000D9FF, bg);
        fb_put_string(x + 40, y + 6, entry->name, fg, bg);

        /* Type */
        const char *type_str = entry->is_dir ? "Folder" : "File";
        fb_put_string(x + 300, y + 6, type_str, 0x00808080, bg);

        /* Size */
        if (!entry->is_dir) {
            char size_str[32];
            if (entry->size > 1024 * 1024)
                snprintf(size_str, sizeof(size_str), "%u MB", entry->size / (1024 * 1024));
            else if (entry->size > 1024)
                snprintf(size_str, sizeof(size_str), "%u KB", entry->size / 1024);
            else
                snprintf(size_str, sizeof(size_str), "%u B", entry->size);
            fb_put_string(x + 450, y + 6, size_str, 0x00808080, bg);
        }

        y += row_h;
    }

    /* Status bar */
    int status_y = win->y + win->h - 28;
    char status[128];
    snprintf(status, sizeof(status), "%d files total | %d selected",
        g_explorer.file_count, g_explorer.selected_idx + 1);
    fb_fill_rect(win->x, status_y, win->w, 28, 0x00282828);
    fb_put_string(win->x + 8, status_y + 6, status, 0x00808080, 0x00282828);
}

void explorer_draw_grid(wm_window_t *win, uint32_t *framebuffer)
{
    int x = win->x + 8;
    int y = win->y + 110;
    int icon_w = 100;
    int icon_h = 100;
    int cols = (win->w - 16) / (icon_w + 8);

    for (int i = 0; i < g_explorer.file_count; i++) {
        file_entry_t *entry = &g_explorer.files[i];

        int col = i % cols;
        int row = i / cols;

        int icon_x = x + col * (icon_w + 8);
        int icon_y = y + row * (icon_h + 8);

        if (icon_y > win->y + win->h - 40)
            break;  /* Out of bounds */

        /* Icon background */
        uint32_t bg = (i == g_explorer.selected_idx) ? 0x005BA3D6 : 0x003A3A3A;
        fb_fill_rect(icon_x, icon_y, icon_w, icon_h, bg);

        /* Icon indicator */
        const char *icon_char = entry->is_dir ? "📁" : "📄";
        fb_put_string(icon_x + 35, icon_y + 20, icon_char, 0x0000D9FF, bg);

        /* Name label */
        fb_put_string(icon_x + 4, icon_y + 75, entry->name, 0x00E0E0E0, bg);
    }
}

void explorer_draw_callback(wm_window_t *win, uint32_t *framebuffer)
{
    if (!win) return;

    /* Window background */
    fb_fill_rect(win->x, win->y + 32, win->w, win->h - 32, 0x001F1F1F);

    /* Draw titlebar (handled by window manager) */

    /* Draw toolbar */
    explorer_draw_toolbar(win, framebuffer);

    /* Draw file list/grid */
    if (g_explorer.view_mode == VIEW_LIST)
        explorer_draw_file_list(win, framebuffer);
    else
        explorer_draw_grid(win, framebuffer);
}

/* ---- Event Handling ------------------------------------------------------ */
void explorer_event_callback(wm_window_t *win, wm_input_event_t *event)
{
    if (!event) return;

    switch (event->type) {
    case IE_MOUSE_DOWN:
        /* Handle clicks on files */
        {
            int row_y = event->data.mouse.y - 110;
            if (row_y >= 0) {
                int row = row_y / 24;
                int clicked_idx = g_explorer.scroll_offset + row;
                if (clicked_idx < g_explorer.file_count) {
                    g_explorer.selected_idx = clicked_idx;

                    /* Double-click to open */
                    if (event->timestamp - g_explorer.last_click_timestamp < 500) {
                        file_entry_t *entry = &g_explorer.files[clicked_idx];
                        if (entry->is_dir) {
                            explorer_refresh_directory(entry->path);
                        }
                    }
                    g_explorer.last_click_timestamp = event->timestamp;
                }
            }
        }
        break;

    case IE_KEY_DOWN:
        /* Handle keyboard navigation */
        switch (event->data.keyboard.key) {
        case 0x48:  /* Up arrow */
            if (g_explorer.selected_idx > 0)
                g_explorer.selected_idx--;
            break;
        case 0x50:  /* Down arrow */
            if (g_explorer.selected_idx < g_explorer.file_count - 1)
                g_explorer.selected_idx++;
            break;
        case 0x1C:  /* Enter - open directory */
            if (g_explorer.selected_idx < g_explorer.file_count) {
                file_entry_t *entry = &g_explorer.files[g_explorer.selected_idx];
                if (entry->is_dir)
                    explorer_refresh_directory(entry->path);
            }
            break;
        case 0x0E:  /* Backspace - go up directory */
            {
                /* Simple: remove last path component */
                char *last_slash = strrchr(g_explorer.current_path, '/');
                if (last_slash && last_slash != g_explorer.current_path) {
                    *last_slash = '\0';
                    explorer_refresh_directory(g_explorer.current_path);
                }
            }
            break;
        }
        break;

    default:
        break;
    }
}

/* ---- Public API ---------------------------------------------------------- */
wm_window_t *file_explorer_open(int x, int y, int w, int h)
{
    memset(&g_explorer, 0, sizeof(g_explorer));
    g_explorer.view_mode = VIEW_LIST;
    explorer_refresh_directory("/jarvis");

    wm_window_t *win = wm_create_window(
        "File Explorer",
        x, y, w, h,
        WS_DEFAULT,
        explorer_draw_callback,
        explorer_event_callback
    );

    g_explorer.window = win;
    return win;
}

void file_explorer_navigate_to(const char *path)
{
    explorer_refresh_directory(path);
}

void file_explorer_set_view_mode(view_mode_t mode)
{
    g_explorer.view_mode = mode;
}
