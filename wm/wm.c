/*
 * =============================================================================
 * JARVIS OS — Window Manager Implementation (wm/wm.c)
 * Language: C17
 *
 * Window list / z-order, focus, move + resize dragging, snapping, and input
 * routing. Drawing lives in wm/titlebar.c.
 * =============================================================================
 */

#include "wm.h"
#include "../kernel/kernel.h"
#include <string.h>

wm_context_t g_wm_ctx = {};

#define WM_MIN_W 260
#define WM_MIN_H 160

/* ---- Initialization ------------------------------------------------------ */
void wm_init(uint32_t screen_w, uint32_t screen_h)
{
    memset(&g_wm_ctx, 0, sizeof(g_wm_ctx));
    g_wm_ctx.screen_w = screen_w;
    g_wm_ctx.screen_h = screen_h;
    g_wm_ctx.work_w = (int)screen_w;
    g_wm_ctx.work_h = (int)screen_h;
    g_wm_ctx.next_wid = 1;
}

void wm_set_workarea(int x, int y, int w, int h)
{
    g_wm_ctx.work_x = x;
    g_wm_ctx.work_y = y;
    g_wm_ctx.work_w = w;
    g_wm_ctx.work_h = h;
}

int wm_count_windows(void)
{
    int n = 0;
    for (wm_window_t *win = g_wm_ctx.windows_head; win; win = win->next)
        n++;
    return n;
}

/* ---- List helpers -------------------------------------------------------- */
static void wm_unlink(wm_window_t *win)
{
    if (win->prev) win->prev->next = win->next;
    else           g_wm_ctx.windows_head = win->next;
    if (win->next) win->next->prev = win->prev;
    else           g_wm_ctx.windows_tail = win->prev;
    win->prev = win->next = NULL;
}

static void wm_link_front(wm_window_t *win)
{
    win->prev = NULL;
    win->next = g_wm_ctx.windows_head;
    if (g_wm_ctx.windows_head)
        g_wm_ctx.windows_head->prev = win;
    g_wm_ctx.windows_head = win;
    if (!g_wm_ctx.windows_tail)
        g_wm_ctx.windows_tail = win;
}

static wm_window_t *wm_top_visible(wm_window_t *except)
{
    for (wm_window_t *win = g_wm_ctx.windows_head; win; win = win->next)
        if (win != except && !win->minimized)
            return win;
    return NULL;
}

/* ---- Window Creation & Destruction --------------------------------------- */
wm_window_t *wm_create_window(const char *title, int x, int y, int w, int h,
                              uint32_t style, wm_draw_callback_t on_draw,
                              wm_event_callback_t on_event)
{
    wm_window_t *win = (wm_window_t *)kmalloc(sizeof(wm_window_t));
    if (!win) return NULL;

    memset(win, 0, sizeof(*win));
    win->wid = g_wm_ctx.next_wid++;
    win->style = style;
    win->state = WIN_STATE_NORMAL;
    win->min_w = WM_MIN_W;
    win->min_h = WM_MIN_H;
    win->on_draw = on_draw;
    win->on_event = on_event;
    wm_set_title(win, title);

    /* Keep the new window inside the work area. */
    if (w > g_wm_ctx.work_w) w = g_wm_ctx.work_w;
    if (h > g_wm_ctx.work_h) h = g_wm_ctx.work_h;
    if (x + w > g_wm_ctx.work_x + g_wm_ctx.work_w) x = g_wm_ctx.work_x + g_wm_ctx.work_w - w;
    if (y + h > g_wm_ctx.work_y + g_wm_ctx.work_h) y = g_wm_ctx.work_y + g_wm_ctx.work_h - h;
    if (x < g_wm_ctx.work_x) x = g_wm_ctx.work_x;
    if (y < g_wm_ctx.work_y) y = g_wm_ctx.work_y;

    win->x = x; win->y = y; win->w = w; win->h = h;
    win->restore_x = x; win->restore_y = y; win->restore_w = w; win->restore_h = h;

    wm_link_front(win);
    g_wm_ctx.focused = win;
    desktop_invalidate();
    return win;
}

void wm_destroy_window(wm_window_t *win)
{
    if (!win) return;

    wm_unlink(win);

    if (g_wm_ctx.drag_window == win) {
        g_wm_ctx.drag_window = NULL;
        g_wm_ctx.drag_active = 0;
    }
    if (g_wm_ctx.capture == win)      g_wm_ctx.capture = NULL;
    if (g_wm_ctx.hover_window == win) g_wm_ctx.hover_window = NULL;
    if (g_wm_ctx.focused == win)      g_wm_ctx.focused = wm_top_visible(NULL);

    kfree(win);
    desktop_invalidate();
}

void wm_set_close_callback(wm_window_t *win, wm_close_callback_t cb, void *ctx)
{
    if (!win) return;
    win->on_close = cb;
    win->close_ctx = ctx;
}

void wm_close_window(wm_window_t *win)
{
    if (!win) return;
    if (win->on_close)
        win->on_close(win->close_ctx);
    wm_destroy_window(win);
}

/* ---- Focus / state ------------------------------------------------------- */
void wm_raise_window(wm_window_t *win)
{
    if (!win || g_wm_ctx.windows_head == win) return;
    wm_unlink(win);
    wm_link_front(win);
}

void wm_set_focused(wm_window_t *win)
{
    if (win) {
        win->minimized = 0;
        wm_raise_window(win);
    }
    g_wm_ctx.focused = win;
    desktop_invalidate();
}

void wm_set_minimized(wm_window_t *win, int minimized)
{
    if (!win) return;

    if (!minimized) {
        wm_set_focused(win);
        return;
    }
    win->minimized = 1;
    if (g_wm_ctx.focused == win)
        g_wm_ctx.focused = wm_top_visible(win);
    if (g_wm_ctx.capture == win)
        g_wm_ctx.capture = NULL;
    desktop_invalidate();
}

void wm_minimize_all(void)
{
    for (wm_window_t *win = g_wm_ctx.windows_head; win; win = win->next)
        win->minimized = 1;
    g_wm_ctx.focused = NULL;
    g_wm_ctx.capture = NULL;
    desktop_invalidate();
}

static void wm_save_restore_rect(wm_window_t *win)
{
    if (win->state != WIN_STATE_NORMAL) return;
    win->restore_x = win->x; win->restore_y = win->y;
    win->restore_w = win->w; win->restore_h = win->h;
}

void wm_restore_window(wm_window_t *win)
{
    if (!win || win->state == WIN_STATE_NORMAL) return;
    win->state = WIN_STATE_NORMAL;
    win->x = win->restore_x; win->y = win->restore_y;
    win->w = win->restore_w; win->h = win->restore_h;
    desktop_invalidate();
}

void wm_set_maximized(wm_window_t *win, int maximized)
{
    if (!win) return;

    if (!maximized) {
        wm_restore_window(win);
        return;
    }
    if (!(win->style & WS_MAXIMIZABLE)) return;

    wm_save_restore_rect(win);
    win->state = WIN_STATE_MAXIMIZED;
    win->x = g_wm_ctx.work_x; win->y = g_wm_ctx.work_y;
    win->w = g_wm_ctx.work_w; win->h = g_wm_ctx.work_h;
    desktop_invalidate();
}

void wm_toggle_maximized(wm_window_t *win)
{
    if (!win) return;
    wm_set_maximized(win, win->state != WIN_STATE_MAXIMIZED);
}

void wm_snap_window(wm_window_t *win, wm_window_state_t snap_state)
{
    if (!win || !(win->style & WS_RESIZABLE)) return;
    if (snap_state != WIN_STATE_SNAPPED_HALF_LEFT &&
        snap_state != WIN_STATE_SNAPPED_HALF_RIGHT)
        return;

    wm_save_restore_rect(win);
    int half = g_wm_ctx.work_w / 2;
    win->state = snap_state;
    win->y = g_wm_ctx.work_y;
    win->h = g_wm_ctx.work_h;
    win->w = half;
    win->x = g_wm_ctx.work_x +
             (snap_state == WIN_STATE_SNAPPED_HALF_RIGHT ? g_wm_ctx.work_w - half : 0);
    desktop_invalidate();
}

void wm_move_window(wm_window_t *win, int x, int y)
{
    if (!win) return;
    win->x = x;
    win->y = y;
    desktop_invalidate();
}

void wm_resize_window(wm_window_t *win, int w, int h)
{
    if (!win) return;
    if (w < win->min_w) w = win->min_w;
    if (h < win->min_h) h = win->min_h;
    win->w = w;
    win->h = h;
    desktop_invalidate();
}

void wm_set_title(wm_window_t *win, const char *title)
{
    if (!win) return;
    strncpy(win->title, title ? title : "", sizeof(win->title) - 1);
    win->title[sizeof(win->title) - 1] = '\0';
    desktop_invalidate();
}

void wm_get_client(wm_window_t *win, int *x, int *y, int *w, int *h)
{
    int border = (win->state == WIN_STATE_MAXIMIZED) ? 0 : 1;
    *x = win->x + border;
    *y = win->y + WM_TITLEBAR_HEIGHT;
    *w = win->w - 2 * border;
    *h = win->h - WM_TITLEBAR_HEIGHT - border;
    if (*w < 0) *w = 0;
    if (*h < 0) *h = 0;
}

/* ---- Hit Testing --------------------------------------------------------- */
wm_window_t *wm_window_at_point(int x, int y)
{
    for (wm_window_t *win = g_wm_ctx.windows_head; win; win = win->next) {
        if (win->minimized) continue;
        if (x >= win->x && x < win->x + win->w &&
            y >= win->y && y < win->y + win->h)
            return win;
    }
    return NULL;
}

/* Caption buttons sit flush right: close, then maximize, then minimize. */
wm_titlebar_button_t wm_hit_test_button(wm_window_t *win, int x, int y)
{
    if (!win || !(win->style & WS_TITLEBAR))
        return WM_BTN_NONE;
    if (y < win->y || y >= win->y + WM_TITLEBAR_HEIGHT)
        return WM_BTN_NONE;

    int bx = win->x + win->w;

    if (win->style & WS_CLOSABLE) {
        bx -= WM_BUTTON_W;
        if (x >= bx && x < bx + WM_BUTTON_W) return WM_BTN_CLOSE;
    }
    if (win->style & WS_MAXIMIZABLE) {
        bx -= WM_BUTTON_W;
        if (x >= bx && x < bx + WM_BUTTON_W) return WM_BTN_MAXIMIZE;
    }
    if (win->style & WS_MINIMIZABLE) {
        bx -= WM_BUTTON_W;
        if (x >= bx && x < bx + WM_BUTTON_W) return WM_BTN_MINIMIZE;
    }
    return WM_BTN_NONE;
}

static int wm_hit_test_edge(wm_window_t *win, int x, int y)
{
    if (!(win->style & WS_RESIZABLE) || win->state == WIN_STATE_MAXIMIZED)
        return EDGE_NONE;

    int g = WM_RESIZE_GRIP, edge = EDGE_NONE;
    if (x < win->x + g)            edge |= EDGE_LEFT;
    if (x >= win->x + win->w - g)  edge |= EDGE_RIGHT;
    if (y < win->y + g)            edge |= EDGE_TOP;
    if (y >= win->y + win->h - g)  edge |= EDGE_BOTTOM;
    return edge;
}

/* ---- Snapping ------------------------------------------------------------ */
static wm_window_state_t wm_snap_zone(int x, int y)
{
    if (y <= g_wm_ctx.work_y + 1)                       return WIN_STATE_MAXIMIZED;
    if (x <= g_wm_ctx.work_x + 1)                       return WIN_STATE_SNAPPED_HALF_LEFT;
    if (x >= g_wm_ctx.work_x + g_wm_ctx.work_w - 2)     return WIN_STATE_SNAPPED_HALF_RIGHT;
    return WIN_STATE_NORMAL;
}

int wm_is_dragging(void)
{
    return g_wm_ctx.drag_active && g_wm_ctx.drag_window;
}

int wm_get_snap_target(int *x, int *y, int *w, int *h)
{
    if (!wm_is_dragging() || g_wm_ctx.drag_edge != EDGE_NONE || !g_wm_ctx.drag_moved)
        return 0;

    wm_window_t *win = g_wm_ctx.drag_window;
    wm_window_state_t zone = wm_snap_zone(g_wm_ctx.mouse_x, g_wm_ctx.mouse_y);
    int half = g_wm_ctx.work_w / 2;

    *y = g_wm_ctx.work_y;
    *h = g_wm_ctx.work_h;

    switch (zone) {
    case WIN_STATE_MAXIMIZED:
        if (!(win->style & WS_MAXIMIZABLE)) return 0;
        *x = g_wm_ctx.work_x; *w = g_wm_ctx.work_w;
        return 1;
    case WIN_STATE_SNAPPED_HALF_LEFT:
        if (!(win->style & WS_RESIZABLE)) return 0;
        *x = g_wm_ctx.work_x; *w = half;
        return 1;
    case WIN_STATE_SNAPPED_HALF_RIGHT:
        if (!(win->style & WS_RESIZABLE)) return 0;
        *x = g_wm_ctx.work_x + g_wm_ctx.work_w - half; *w = half;
        return 1;
    default:
        return 0;
    }
}

/* ---- Mouse --------------------------------------------------------------- */
static void wm_send_mouse(wm_window_t *win, input_event_type_t type,
                          int x, int y, int button)
{
    if (!win || !win->on_event) return;

    int cx, cy, cw, ch;
    wm_get_client(win, &cx, &cy, &cw, &ch);

    wm_input_event_t event;
    event.type = type;
    event.timestamp = (uint32_t)g_kstate.uptime_ticks;
    event.data.mouse.x = x - cx;
    event.data.mouse.y = y - cy;
    event.data.mouse.button = button;
    win->on_event(win, &event);
}

static void wm_drag_update(int x, int y)
{
    wm_window_t *win = g_wm_ctx.drag_window;
    int dx = x - g_wm_ctx.drag_start_mx;
    int dy = y - g_wm_ctx.drag_start_my;

    if (g_wm_ctx.drag_edge == EDGE_NONE) {
        /* ---- Move ---- */
        if (!g_wm_ctx.drag_moved) {
            if (dx * dx + dy * dy < 16)
                return;                 /* ignore jitter on a plain click */
            g_wm_ctx.drag_moved = 1;

            /* Dragging a maximized / snapped window pulls it back to its
             * normal size, keeping the cursor at the same relative spot. */
            if (win->state != WIN_STATE_NORMAL) {
                int old_w = win->w > 0 ? win->w : 1;
                int rel = ((x - win->x) * win->restore_w) / old_w;
                win->state = WIN_STATE_NORMAL;
                win->w = win->restore_w;
                win->h = win->restore_h;
                g_wm_ctx.drag_off_x = rel;
                if (g_wm_ctx.drag_off_x > win->w - WM_BUTTON_W * 3 - 8)
                    g_wm_ctx.drag_off_x = win->w / 2;
            }
        }

        int nx = x - g_wm_ctx.drag_off_x;
        int ny = y - g_wm_ctx.drag_off_y;
        int min_y = g_wm_ctx.work_y;
        int max_y = g_wm_ctx.work_y + g_wm_ctx.work_h - WM_TITLEBAR_HEIGHT;
        if (ny < min_y) ny = min_y;
        if (ny > max_y) ny = max_y;
        if (nx < g_wm_ctx.work_x - win->w + 80) nx = g_wm_ctx.work_x - win->w + 80;
        if (nx > g_wm_ctx.work_x + g_wm_ctx.work_w - 80) nx = g_wm_ctx.work_x + g_wm_ctx.work_w - 80;
        win->x = nx;
        win->y = ny;
        return;
    }

    /* ---- Resize: recompute from the rectangle at drag start ---- */
    g_wm_ctx.drag_moved = 1;
    int nx = g_wm_ctx.drag_start_x, ny = g_wm_ctx.drag_start_y;
    int nw = g_wm_ctx.drag_start_w, nh = g_wm_ctx.drag_start_h;

    if (g_wm_ctx.drag_edge & EDGE_RIGHT)  nw += dx;
    if (g_wm_ctx.drag_edge & EDGE_BOTTOM) nh += dy;
    if (g_wm_ctx.drag_edge & EDGE_LEFT) {
        if (nw - dx < win->min_w) dx = nw - win->min_w;
        nx += dx; nw -= dx;
    }
    if (g_wm_ctx.drag_edge & EDGE_TOP) {
        if (nh - dy < win->min_h) dy = nh - win->min_h;
        if (ny + dy < g_wm_ctx.work_y) dy = g_wm_ctx.work_y - ny;
        ny += dy; nh -= dy;
    }
    if (nw < win->min_w) nw = win->min_w;
    if (nh < win->min_h) nh = win->min_h;

    win->state = WIN_STATE_NORMAL;
    win->x = nx; win->y = ny; win->w = nw; win->h = nh;
}

int wm_handle_mouse_move(int x, int y)
{
    g_wm_ctx.mouse_x = x;
    g_wm_ctx.mouse_y = y;

    if (wm_is_dragging()) {
        wm_drag_update(x, y);
        return 1;
    }

    if (g_wm_ctx.capture) {
        wm_send_mouse(g_wm_ctx.capture, IE_MOUSE_MOVE, x, y, 1);
        return 1;
    }

    /* Track which caption button is under the cursor for hover highlight. */
    wm_window_t *win = wm_window_at_point(x, y);
    wm_titlebar_button_t btn = win ? wm_hit_test_button(win, x, y) : WM_BTN_NONE;
    if (btn == WM_BTN_NONE) win = NULL;

    if (win != g_wm_ctx.hover_window || btn != g_wm_ctx.hover_button) {
        g_wm_ctx.hover_window = win;
        g_wm_ctx.hover_button = btn;
        return 1;
    }
    return 0;
}

#define WM_DOUBLE_CLICK_TICKS 40    /* 400 ms at 100 Hz */
#define WM_DOUBLE_CLICK_DIST  6

int wm_handle_mouse_click(int x, int y, int button)
{
    g_wm_ctx.mouse_x = x;
    g_wm_ctx.mouse_y = y;

    wm_window_t *win = wm_window_at_point(x, y);
    if (!win) {
        if (g_wm_ctx.focused) {
            g_wm_ctx.focused = NULL;
            desktop_invalidate();
        }
        return 0;
    }

    wm_set_focused(win);

    int in_title = (win->style & WS_TITLEBAR) && y < win->y + WM_TITLEBAR_HEIGHT;

    if (button == 1) {
        wm_titlebar_button_t btn = wm_hit_test_button(win, x, y);
        if (btn != WM_BTN_NONE) {
            g_wm_ctx.hover_window = NULL;
            g_wm_ctx.hover_button = WM_BTN_NONE;
            switch (btn) {
            case WM_BTN_CLOSE:    wm_close_window(win);       break;
            case WM_BTN_MAXIMIZE: wm_toggle_maximized(win);   break;
            case WM_BTN_MINIMIZE: wm_set_minimized(win, 1);   break;
            default: break;
            }
            return 1;
        }

        int edge = wm_hit_test_edge(win, x, y);
        if (edge != EDGE_NONE || in_title) {
            uint32_t now = (uint32_t)g_kstate.uptime_ticks;

            /* Double-click on the title bar toggles maximize. */
            if (edge == EDGE_NONE) {
                int ddx = x - g_wm_ctx.last_click_x;
                int ddy = y - g_wm_ctx.last_click_y;
                if (g_wm_ctx.last_click_wid == win->wid &&
                    now - g_wm_ctx.last_click_time <= WM_DOUBLE_CLICK_TICKS &&
                    ddx * ddx + ddy * ddy <= WM_DOUBLE_CLICK_DIST * WM_DOUBLE_CLICK_DIST) {
                    g_wm_ctx.last_click_wid = 0;
                    wm_toggle_maximized(win);
                    return 1;
                }
                g_wm_ctx.last_click_time = now;
                g_wm_ctx.last_click_x = x;
                g_wm_ctx.last_click_y = y;
                g_wm_ctx.last_click_wid = win->wid;
            }

            g_wm_ctx.drag_active = 1;
            g_wm_ctx.drag_window = win;
            g_wm_ctx.drag_edge = edge;
            g_wm_ctx.drag_moved = 0;
            g_wm_ctx.drag_off_x = x - win->x;
            g_wm_ctx.drag_off_y = y - win->y;
            g_wm_ctx.drag_start_mx = x;
            g_wm_ctx.drag_start_my = y;
            g_wm_ctx.drag_start_x = win->x;
            g_wm_ctx.drag_start_y = win->y;
            g_wm_ctx.drag_start_w = win->w;
            g_wm_ctx.drag_start_h = win->h;
            return 1;
        }
    }

    if (in_title)
        return 1;

    /* Client area: hand the click to the app. */
    if (button == 1)
        g_wm_ctx.capture = win;
    wm_send_mouse(win, IE_MOUSE_DOWN, x, y, button);
    return 1;
}

void wm_handle_mouse_release(int x, int y, int button)
{
    g_wm_ctx.mouse_x = x;
    g_wm_ctx.mouse_y = y;

    if (button == 1 && wm_is_dragging()) {
        wm_window_t *win = g_wm_ctx.drag_window;

        if (g_wm_ctx.drag_edge == EDGE_NONE && g_wm_ctx.drag_moved) {
            wm_window_state_t zone = wm_snap_zone(x, y);
            if (zone == WIN_STATE_MAXIMIZED)
                wm_set_maximized(win, 1);
            else if (zone != WIN_STATE_NORMAL)
                wm_snap_window(win, zone);
        }
        g_wm_ctx.drag_active = 0;
        g_wm_ctx.drag_window = NULL;
        desktop_invalidate();
        return;
    }

    if (button == 1 && g_wm_ctx.capture) {
        wm_window_t *win = g_wm_ctx.capture;
        g_wm_ctx.capture = NULL;
        wm_send_mouse(win, IE_MOUSE_UP, x, y, button);
        desktop_invalidate();
    }
}

/* ---- Keyboard ------------------------------------------------------------ */
void wm_alt_tab_next(void)
{
    /* Bring the back-most visible window to the front: repeated presses
     * cycle through every window. */
    for (wm_window_t *win = g_wm_ctx.windows_tail; win; win = win->prev) {
        if (!win->minimized && win != g_wm_ctx.focused) {
            wm_set_focused(win);
            return;
        }
    }
    /* Everything else is minimized: restore the most recent one. */
    for (wm_window_t *win = g_wm_ctx.windows_head; win; win = win->next) {
        if (win != g_wm_ctx.focused) {
            wm_set_focused(win);
            return;
        }
    }
}

void wm_snap_shortcut(int direction)
{
    wm_window_t *win = g_wm_ctx.focused;
    if (!win) return;

    switch (direction) {
    case 0: wm_snap_window(win, WIN_STATE_SNAPPED_HALF_LEFT);  break;
    case 1: wm_snap_window(win, WIN_STATE_SNAPPED_HALF_RIGHT); break;
    case 2: wm_set_maximized(win, 1); break;
    case 3:
        if (win->state != WIN_STATE_NORMAL) wm_restore_window(win);
        else                                wm_set_minimized(win, 1);
        break;
    default: break;
    }
}

static void wm_send_key(input_event_type_t type, uint32_t key, int mods, char ch)
{
    wm_window_t *win = g_wm_ctx.focused;
    if (!win || win->minimized || !win->on_event) return;

    wm_input_event_t event;
    event.type = type;
    event.timestamp = (uint32_t)g_kstate.uptime_ticks;
    if (type == IE_TEXT_INPUT) {
        event.data.text.ch = ch;
    } else {
        event.data.keyboard.key = key;
        event.data.keyboard.mods = mods;
    }
    win->on_event(win, &event);
    desktop_invalidate();
}

void wm_handle_key(uint32_t key, int mods)
{
    if ((mods & KMOD_ALT) && key == WM_KEY_TAB) {
        wm_alt_tab_next();
        return;
    }
    if ((mods & KMOD_ALT) && key == WM_KEY_F4) {
        if (g_wm_ctx.focused && (g_wm_ctx.focused->style & WS_CLOSABLE))
            wm_close_window(g_wm_ctx.focused);
        return;
    }
    if (mods & KMOD_SUPER) {
        switch (key) {
        case WM_KEY_LEFT:  wm_snap_shortcut(0); return;
        case WM_KEY_RIGHT: wm_snap_shortcut(1); return;
        case WM_KEY_UP:    wm_snap_shortcut(2); return;
        case WM_KEY_DOWN:  wm_snap_shortcut(3); return;
        default: break;
        }
    }
    wm_send_key(IE_KEY_DOWN, key, mods, 0);
}

void wm_handle_key_up(uint32_t key, int mods)
{
    wm_send_key(IE_KEY_UP, key, mods, 0);
}

void wm_handle_text_input(char ch)
{
    wm_send_key(IE_TEXT_INPUT, 0, 0, ch);
}

void wm_handle_input(wm_input_event_t *event)
{
    if (!event) return;

    switch (event->type) {
    case IE_KEY_DOWN:
        wm_handle_key(event->data.keyboard.key, event->data.keyboard.mods);
        break;
    case IE_KEY_UP:
        wm_handle_key_up(event->data.keyboard.key, event->data.keyboard.mods);
        break;
    case IE_TEXT_INPUT:
        wm_handle_text_input(event->data.text.ch);
        break;
    default:
        break;      /* mouse input is routed by desktop/desktop_init.c */
    }
}
