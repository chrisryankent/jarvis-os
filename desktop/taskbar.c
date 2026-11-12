/*
 * =============================================================================
 * JARVIS OS — Taskbar System (desktop/taskbar.c)
 * Language: C17
 *
 * Bottom panel with Start button, app launcher, pinned apps, system tray, clock.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "../wm/wm.h"
#include "../drivers/gpu.h"
#include <string.h>
#include <stdio.h>

/* Defined later in desktop_init.c (the outermost file in this unity
 * build); a plain forward declaration here is safe since no enum/typedef
 * is involved (unlike notification_priority_t — see desktop.c's history
 * with that one). */
void desktop_system_open_app(const char *app);

#define TASKBAR_HEIGHT     64
#define TASKBAR_Y          (fb_get_height() - TASKBAR_HEIGHT)
#define START_BUTTON_W     90
#define SEARCH_BOX_W       220
#define ICON_SIZE          48
#define ICON_SPACING       12

/* Colors (BGRA) */
#define COL_TASKBAR_BG     0x0010191D
#define COL_TASKBAR_BORDER 0x002F454A
#define COL_START_BG       0x0046D3C6
#define COL_START_TEXT     0x00FFFFFF
#define COL_ICON_BG        0x00282828
#define COL_ICON_HOVER     0x003A3A3A
#define COL_TEXT           0x00E0E0E0
#define COL_DIM            0x00808080

typedef struct {
    char label[64];
    wm_window_t *window;
    int icon_index;
    int active;
} taskbar_app_t;

typedef struct {
    taskbar_app_t apps[32];
    int app_count;
    int start_menu_open;
} taskbar_context_t;

static taskbar_context_t g_taskbar = {};

/* Click-hit bounds, recomputed every draw call alongside the actual
 * drawing so taskbar_handle_click() can never drift from what's on
 * screen (the same class of bug fixed for titlebar buttons in wm.c). */
static int g_tb_start_bounds[4];          /* x,y,w,h */
static int g_tb_search_bounds[4];
static int g_tb_pinned_bounds[8][4];
static int g_tb_pinned_count = 0;
static const char *g_tb_pinned_app_names[8];
static int g_tb_window_bounds[32][4];
static int g_tb_window_count = 0;

void taskbar_init(void)
{
    memset(&g_taskbar, 0, sizeof(g_taskbar));
}

void taskbar_draw_start_button(void)
{
    int y = TASKBAR_Y;
    int x = 12;
    int bw = START_BUTTON_W, bh = 48;

    uint32_t bg = g_taskbar.start_menu_open ? 0x006BE585 : COL_START_BG;
    fb_fill_rounded_rect(x, y + 8, bw, bh, 8, bg);

    /* Windows-11-style four-coloured grid icon (2×2 blocks, 6px each) */
    int gx = x + 10, gy = y + 18;
    fb_fill_rect(gx,     gy,     6, 6, 0x00F35325); /* red    (top-left)     */
    fb_fill_rect(gx + 8, gy,     6, 6, 0x0081BC06); /* green  (top-right)    */
    fb_fill_rect(gx,     gy + 8, 6, 6, 0x05A6F0FF); /* blue   (bottom-left)  */
    fb_fill_rect(gx + 8, gy + 8, 6, 6, 0x00FFBA08); /* yellow (bottom-right) */

    fb_put_string(x + 28, y + 22, "Start", COL_START_TEXT, bg);

    /* Record hit bounds */
    g_tb_start_bounds[0] = x; g_tb_start_bounds[1] = y + 8;
    g_tb_start_bounds[2] = bw; g_tb_start_bounds[3] = bh;
}

void taskbar_draw_search_box(void)
{
    int y = TASKBAR_Y;
    int x = START_BUTTON_W + 24;
    int sw = SEARCH_BOX_W, sh = 48;

    fb_fill_rounded_rect(x, y + 8, sw, sh, 8, COL_ICON_BG);
    fb_fill_rect(x + 8, y + 8, sw - 16, 1, COL_TASKBAR_BORDER);

    /* Magnifying glass icon (3×3 circle + 2px handle) */
    fb_fill_rect(x + 8, y + 18, 10, 10, COL_ICON_BG);
    fb_fill_rect(x + 9, y + 17, 8,  1, COL_DIM);
    fb_fill_rect(x + 9, y + 28, 8,  1, COL_DIM);
    fb_fill_rect(x + 8, y + 18, 1,  10, COL_DIM);
    fb_fill_rect(x + 17,y + 18, 1,  10, COL_DIM);
    fb_fill_rect(x + 17,y + 27, 4,  1, COL_DIM);
    fb_fill_rect(x + 20,y + 27, 1,  4, COL_DIM);

    const char *search_text = g_kstate.ai_ready ?
        "Ask JARVIS" : "Loading AI";
    fb_put_string(x + 26, y + 18, search_text, COL_DIM, COL_ICON_BG);

    g_tb_search_bounds[0] = x; g_tb_search_bounds[1] = y + 8;
    g_tb_search_bounds[2] = sw; g_tb_search_bounds[3] = sh;
}

void taskbar_draw_pinned_apps(void)
{
    uint32_t W = fb_get_width();
    int y = TASKBAR_Y;

    /* Windows 11 centres the pinned icons in the middle of the taskbar —
     * calculate from screen centre, not from the left edge */
    static const struct { const char *label; const char *app; uint32_t col; } pinned[] = {
        { "Files",  "Files",      0x0000D9FF },
        { "Term",   "Terminal",   0x0000E676 },
        { "Web",    "Browser",    0x00BB86FC },
        { "Guard",  "Security",   0x00FF6B6B },
        { "AI",     "AI",         0x00FFB703 },
    };
    int n = 5;
    int total_w = n * (ICON_SIZE + ICON_SPACING) - ICON_SPACING;
    int start_x = ((int)W - total_w) / 2;
    int x = start_x;

    g_tb_pinned_count = 0;
    for (int i = 0; i < n; i++) {
        uint32_t bg = COL_ICON_BG;
        /* Highlight if this app's window is the focused one */
        wm_window_t *focused = g_wm_ctx.focused;
        for (int k = 0; k < g_taskbar.app_count; k++) {
            if (g_taskbar.apps[k].window == focused &&
                strcmp(g_taskbar.apps[k].label, pinned[i].label) == 0) {
                bg = COL_ICON_HOVER;
                break;
            }
        }

        fb_fill_rounded_rect(x, y + 8, ICON_SIZE, ICON_SIZE, 8, bg);
        /* 2-px accent underline if running */
        for (int k = 0; k < g_taskbar.app_count; k++) {
            if (strcmp(g_taskbar.apps[k].label, pinned[i].label) == 0) {
                fb_fill_rect(x + ICON_SIZE/2 - 8, y + ICON_SIZE + 7, 16, 2, 0x0046D3C6);
                break;
            }
        }
        fb_put_string(x + 4, y + 28, pinned[i].label, 0x00FFFFFF, bg);

        if (g_tb_pinned_count < 8) {
            g_tb_pinned_bounds[g_tb_pinned_count][0] = x;
            g_tb_pinned_bounds[g_tb_pinned_count][1] = y + 8;
            g_tb_pinned_bounds[g_tb_pinned_count][2] = ICON_SIZE;
            g_tb_pinned_bounds[g_tb_pinned_count][3] = ICON_SIZE;
            g_tb_pinned_app_names[g_tb_pinned_count] = pinned[i].app;
            g_tb_pinned_count++;
        }
        x += ICON_SIZE + ICON_SPACING;
    }

    /* Open-window buttons (right of pinned, left of tray) — show running
     * apps that are NOT in the pinned list so the user can click to
     * bring them forward. Previously these were tracked (taskbar_add_window
     * existed) but never drawn or click-tested, so the taskbar always
     * appeared to show only pinned icons regardless of what was open. */
    g_tb_window_count = 0;
    int wx = x + 24;
    for (int i = 0; i < g_taskbar.app_count && g_tb_window_count < 32; i++) {
        if (wx + ICON_SIZE >= (int)W - 260) break;
        int pinned_match = 0;
        for (int k = 0; k < n; k++) {
            if (strcmp(g_taskbar.apps[i].label, pinned[k].label) == 0) {
                pinned_match = 1; break;
            }
        }
        if (pinned_match) continue;

        uint32_t bg = (g_taskbar.apps[i].window == g_wm_ctx.focused)
                      ? COL_ICON_HOVER : COL_ICON_BG;
        fb_fill_rounded_rect(wx, y + 8, ICON_SIZE, ICON_SIZE, 8, bg);
        /* Truncate label to 4 chars to fit the icon */
        char short_label[5]; short_label[4] = 0;
        strncpy(short_label, g_taskbar.apps[i].label, 4);
        fb_put_string(wx + 4, y + 28, short_label, COL_TEXT, bg);
        if (g_taskbar.apps[i].window == g_wm_ctx.focused)
            fb_fill_rect(wx + ICON_SIZE/2 - 8, y + ICON_SIZE + 7, 16, 2, 0x0046D3C6);

        g_tb_window_bounds[g_tb_window_count][0] = wx;
        g_tb_window_bounds[g_tb_window_count][1] = y + 8;
        g_tb_window_bounds[g_tb_window_count][2] = ICON_SIZE;
        g_tb_window_bounds[g_tb_window_count][3] = ICON_SIZE;
        g_tb_window_count++;
        wx += ICON_SIZE + ICON_SPACING;
    }
}

void taskbar_draw_system_tray(void)
{
    uint32_t W = fb_get_width();
    int y = TASKBAR_Y;
    int tray_x = (int)W - 220;

    /* System tray pill background */
    fb_fill_rounded_rect(tray_x - 4, y + 10, 218, 44, 8, COL_ICON_BG);

    /* Wi-Fi icon (3 stacked arcs, simplified as bars) */
    int ix = tray_x;
    fb_fill_rect(ix,     y + 40, 3, 4,  COL_TEXT);
    fb_fill_rect(ix + 4, y + 36, 3, 8,  COL_TEXT);
    fb_fill_rect(ix + 8, y + 32, 3, 12, COL_TEXT);
    fb_fill_rect(ix+12,  y + 28, 3, 16, 0x0046D3C6); /* brightest bar = connected */
    ix += 20;

    /* Volume icon (simplified speaker + wave) */
    fb_fill_rect(ix,     y + 22, 6,  20, COL_TEXT);
    fb_fill_rect(ix + 6, y + 18, 4,  28, COL_TEXT);
    fb_fill_rect(ix +10, y + 26, 2,  12, COL_TEXT);
    fb_fill_rect(ix +14, y + 28, 2,   8, 0x0046D3C6);
    ix += 22;

    /* Battery (thin rect + nub) */
    fb_fill_rect(ix, y + 22, 24, 20, COL_TEXT);
    fb_fill_rect(ix + 24, y + 27, 3, 10, COL_TEXT);  /* nub */
    fb_fill_rect(ix + 2, y + 24, 20, 16, 0x0034C759); /* green fill = 100% */
    ix += 32;

    /* Clock: HH:MM on top, day-counter on bottom */
    char time_buf[16], day_buf[16];
    uint64_t total_secs  = g_kstate.uptime_ticks / 100;
    uint64_t total_mins  = total_secs / 60;
    uint64_t display_min = total_mins % 60;
    uint64_t display_hr  = (total_mins / 60) % 24;
    snprintf(time_buf, sizeof(time_buf), "%02lu:%02lu",
             (unsigned long)display_hr, (unsigned long)display_min);
    snprintf(day_buf,  sizeof(day_buf),  "Day %lu",
             (unsigned long)(total_mins / 1440 + 1));
    fb_put_string(ix, y + 14, time_buf, COL_TEXT, COL_ICON_BG);
    fb_put_string(ix, y + 30, day_buf,  COL_DIM,  COL_ICON_BG);
}

void taskbar_draw(void)
{
    uint32_t W = fb_get_width();
    int y = TASKBAR_Y;

    /* Taskbar background */
    fb_blend_rect(0, y, W, TASKBAR_HEIGHT, 0x00000000, 110);
    fb_fill_rect(0, y + 1, W, TASKBAR_HEIGHT - 1, COL_TASKBAR_BG);
    fb_fill_rect(0, y, W, 1, COL_TASKBAR_BORDER);

    taskbar_draw_start_button();
    taskbar_draw_search_box();
    taskbar_draw_pinned_apps();
    taskbar_draw_system_tray();
}

void taskbar_add_window(wm_window_t *win, const char *label)
{
    if (g_taskbar.app_count < 32) {
        strncpy(g_taskbar.apps[g_taskbar.app_count].label, label,
                sizeof(g_taskbar.apps[0].label) - 1);
        g_taskbar.apps[g_taskbar.app_count].window = win;
        g_taskbar.apps[g_taskbar.app_count].active = 1;
        g_taskbar.app_count++;
    }
}

void taskbar_remove_window(wm_window_t *win)
{
    for (int i = 0; i < g_taskbar.app_count; i++) {
        if (g_taskbar.apps[i].window == win) {
            for (int j = i; j < g_taskbar.app_count - 1; j++)
                g_taskbar.apps[j] = g_taskbar.apps[j + 1];
            g_taskbar.app_count--;
            break;
        }
    }
}

/* ---- Start Menu ---------------------------------------------------------- */
void start_menu_draw(void)
{
    uint32_t H = fb_get_height();
    int menu_w = 640;
    int menu_h = (int)H - 80;
    int x = 12;
    int y = TASKBAR_Y - menu_h - 8;  /* 8px gap above taskbar */

    /* Semi-transparent dark panel (acrylic effect via a flat dark bg) */
    fb_fill_rounded_rect(x, y, menu_w, menu_h, 8, 0x0010191D);
    /* Top border accent */
    fb_fill_rect(x, y, menu_w, 1, COL_TASKBAR_BORDER);
    /* Right border */
    fb_fill_rect(x + menu_w - 1, y, 1, menu_h, COL_TASKBAR_BORDER);

    /* ---- User banner ---------------------------------------------------- */
    fb_fill_rounded_rect(x + 10, y + 10, menu_w - 20, 50, 8, 0x0018242B);
    /* User icon circle */
    fb_fill_rounded_rect(x + 20, y + 17, 36, 36, 8, 0x0046D3C6);
    fb_put_string(x + 30, y + 27, "JV", 0x00FFFFFF, 0x0046D3C6);
    fb_put_string(x + 66, y + 20, "JARVIS User", COL_TEXT, 0x0018242B);
    fb_put_string(x + 66, y + 37, "Administrator", COL_DIM, 0x0018242B);

    /* ---- Search box ----------------------------------------------------- */
    int sy = y + 72;
    fb_fill_rounded_rect(x + 16, sy, menu_w - 32, 42, 8, COL_ICON_BG);
    fb_fill_rect(x + 24, sy, menu_w - 48, 1, 0x0046D3C6);
    fb_put_string(x + 28, sy + 12, "Search apps, files, and settings...", COL_DIM, COL_ICON_BG);

    /* ---- Pinned section -------------------------------------------------- */
    int py = sy + 56;
    fb_put_string(x + 16, py, "Pinned", COL_TEXT, 0x00151515);
    fb_fill_rect(x + 16, py + 16, 60, 1, COL_TASKBAR_BORDER);

    static const struct { const char *label; uint32_t col; } pinned[] = {
        { "Files",      0x0000D9FF }, { "Terminal",  0x0000E676 },
        { "Settings",   0x00BB86FC }, { "Security",  0x00FF6B6B },
        { "Calculator", 0x00FFB703 }, { "Browser",   0x00FF9500 },
    };
    int icon_x = x + 20, icon_y = py + 28;
    int icons_per_row = 5;
    for (int i = 0; i < 6; i++) {
        fb_fill_rounded_rect(icon_x, icon_y, 72, 72, 8, pinned[i].col);
        /* First letter as large icon glyph */
        char first[2] = { pinned[i].label[0], 0 };
        fb_put_string(icon_x + 28, icon_y + 24, first, 0x00FFFFFF, pinned[i].col);
        fb_put_string(icon_x + 4,  icon_y + 56, pinned[i].label, 0x00FFFFFF, pinned[i].col);
        icon_x += 82;
        if ((i + 1) % icons_per_row == 0) { icon_x = x + 20; icon_y += 86; }
    }

    /* ---- Recommended / Recent ------------------------------------------- */
    int ry = icon_y + 18;
    fb_put_string(x + 16, ry, "Recommended", COL_TEXT, 0x00151515);
    fb_fill_rect(x + 16, ry + 16, 90, 1, COL_TASKBAR_BORDER);

    static const char *recent[] = { "kernel_core.o", "brain.py", "speech_engine.cpp",
                                    "vfs.c", "desktop_init.c", "CHANGELOG.md" };
    int rx = x + 20, rrow_y = ry + 28;
    for (int i = 0; i < 6; i++) {
        fb_fill_rect(rx, rrow_y, 12, 16, 0x005BA3D6);
        fb_put_string(rx + 16, rrow_y, recent[i], COL_TEXT, 0x00151515);
        rrow_y += 22;
        if (rrow_y > y + menu_h - 50) break;
    }

    /* ---- Power strip (bottom) ------------------------------------------- */
    int bot_y = y + menu_h - 40;
    fb_fill_rect(x, bot_y, menu_w, 40, 0x0018242B);
    fb_put_string(x + 16,  bot_y + 12, "All Apps >",   COL_TEXT, 0x0018242B);
    fb_put_string(x + menu_w - 180, bot_y + 12, "[Sleep]  [Restart]  [Shutdown]",
                  COL_DIM, 0x0018242B);
}

/* ---- Click Handler (was completely absent — taskbar was draw-only) ------- */

static int point_in(int px, int py, int *bounds)
{
    return (px >= bounds[0] && px < bounds[0] + bounds[2] &&
            py >= bounds[1] && py < bounds[1] + bounds[3]);
}

/*
 * Returns 1 if the click was handled by the taskbar (so the WM layer
 * knows not to forward it to a window), 0 otherwise. Previously the
 * taskbar was draw-only — nothing called click-test functions because none
 * existed; Start, pinned icons, and running-window buttons were inert.
 */
int taskbar_handle_click(int x, int y)
{
    /* Start button */
    if (point_in(x, y, g_tb_start_bounds)) {
        taskbar_toggle_start_menu();
        return 1;
    }

    /* Clicking anywhere while start menu is open and NOT on the taskbar
     * should dismiss the start menu — handled upstream in the desktop
     * click router. Here: clicking the search box while start is open. */
    if (point_in(x, y, g_tb_search_bounds)) {
        if (g_taskbar.start_menu_open)
            g_taskbar.start_menu_open = 0;
        /* TODO: open a global search/spotlight overlay */
        return 1;
    }

    /* Pinned icons */
    for (int i = 0; i < g_tb_pinned_count; i++) {
        if (point_in(x, y, g_tb_pinned_bounds[i])) {
            desktop_system_open_app(g_tb_pinned_app_names[i]);
            if (g_taskbar.start_menu_open)
                g_taskbar.start_menu_open = 0;
            return 1;
        }
    }

    /* Open-window (non-pinned) buttons */
    for (int i = 0; i < g_tb_window_count; i++) {
        if (point_in(x, y, g_tb_window_bounds[i])) {
            wm_window_t *win = g_taskbar.apps[i].window;
            if (win) {
                if (win->state == WIN_STATE_MINIMIZED)
                    wm_set_minimized(win, 0);
                else if (g_wm_ctx.focused == win)
                    wm_set_minimized(win, 1);  /* click running app = minimize */
                else
                    wm_set_focused(win);
            }
            return 1;
        }
    }

    /* Any click on the taskbar bar itself (background) */
    if (y >= (int)TASKBAR_Y && y < (int)(TASKBAR_Y + TASKBAR_HEIGHT)) {
        /* Dismiss start menu on background click */
        if (g_taskbar.start_menu_open)
            g_taskbar.start_menu_open = 0;
        return 1;
    }

    /* Click outside taskbar when start menu is open → dismiss */
    if (g_taskbar.start_menu_open) {
        g_taskbar.start_menu_open = 0;
        return 0;  /* don't consume — also let the click reach the window below */
    }

    return 0;
}

void taskbar_toggle_start_menu(void)
{
    g_taskbar.start_menu_open = !g_taskbar.start_menu_open;
}

int taskbar_is_start_menu_open(void)
{
    return g_taskbar.start_menu_open;
}
