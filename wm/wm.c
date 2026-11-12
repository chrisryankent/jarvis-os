/*
 * =============================================================================
 * JARVIS OS — Window Manager Implementation (wm/wm.c)
 * Language: C17
 *
 * Window management, z-order, tiling snaps, input routing.
 * =============================================================================
 */

#include "wm.h"
#include "../kernel/kernel.h"
#include <string.h>
#include <stdio.h>

/* ---- Global Context ------------------------------------------------------ */
wm_context_t g_wm_ctx = {};

/* ---- Forward Declarations ------------------------------------------------ */
static int wm_hit_test_titlebar(wm_window_t *win, int x, int y);
static int wm_hit_test_edge(wm_window_t *win, int x, int y);

/* ---- Initialization ------------------------------------------------------ */
void wm_init(uint32_t screen_w, uint32_t screen_h)
{
    memset(&g_wm_ctx, 0, sizeof(g_wm_ctx));
    g_wm_ctx.screen_w = screen_w;
    g_wm_ctx.screen_h = screen_h;
    g_wm_ctx.next_wid = 1;
}

int wm_count_windows(void)
{
    int n = 0;
    for (wm_window_t *win = g_wm_ctx.windows_head; win; win = win->next)
        n++;
    return n;
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
    win->x = x;
    win->y = y;
    win->w = w;
    win->h = h;
    win->style = style;
    win->state = WIN_STATE_NORMAL;
    win->bg_color = 0x00121820;  /* Dark panel color */
    win->z_order = g_wm_ctx.frame_count;

    win->on_draw = on_draw;
    win->on_event = on_event;

    strncpy(win->title, title, sizeof(win->title) - 1);
    win->title[sizeof(win->title) - 1] = '\0';

    /* Add to front of window list */
    win->next = g_wm_ctx.windows_head;
    if (g_wm_ctx.windows_head)
        g_wm_ctx.windows_head->prev = win;
    g_wm_ctx.windows_head = win;

    if (!g_wm_ctx.windows_tail)
        g_wm_ctx.windows_tail = win;

    wm_set_focused(win);

    return win;
}

void wm_destroy_window(wm_window_t *win)
{
    if (!win) return;

    if (win->prev)
        win->prev->next = win->next;
    else
        g_wm_ctx.windows_head = win->next;

    if (win->next)
        win->next->prev = win->prev;
    else
        g_wm_ctx.windows_tail = win->prev;

    if (g_wm_ctx.focused == win)
        g_wm_ctx.focused = g_wm_ctx.windows_tail;
    if (g_wm_ctx.drag_window == win) {
        g_wm_ctx.drag_window = NULL;
        g_wm_ctx.drag_active = 0;
    }

    kfree(win);
}

void wm_set_close_callback(wm_window_t *win, wm_close_callback_t cb, void *ctx)
{
    if (!win) return;
    win->on_close = cb;
    win->close_ctx = ctx;
}

/*
 * User-initiated close (clicking the titlebar X, or a future "Close" menu
 * item) — fires the registered on_close callback, if any, before freeing
 * the window. Without this distinction, closing one of the four
 * default-launched apps (File Explorer/Terminal/Settings/Calculator) would
 * free the wm_window_t while desktop/desktop_init.c's g_desktop_state still
 * held a raw pointer to it, leaving a dangling pointer that
 * desktop_system_open_app() would check against (and incorrectly treat the
 * app as "still open") forever, since nothing would ever set it back to
 * NULL. wm_destroy_window() remains available as the lower-level primitive
 * for any future programmatic/non-user-initiated teardown.
 */
void wm_close_window(wm_window_t *win)
{
    if (!win) return;
    if (win->on_close)
        win->on_close(win->close_ctx);
    wm_destroy_window(win);
}

/* ---- Window State Management ---------------------------------------------- */
void wm_set_focused(wm_window_t *win)
{
    if (!win) return;

    if (g_wm_ctx.focused == win)
        return;

    g_wm_ctx.focused = win;
    wm_raise_window(win);
}

void wm_set_minimized(wm_window_t *win, int minimized)
{
    if (!win) return;

    if (minimized)
        win->state = WIN_STATE_MINIMIZED;
    else
        win->state = WIN_STATE_NORMAL;
}

void wm_set_maximized(wm_window_t *win, int maximized)
{
    if (!win) return;

    if (maximized) {
        if (win->state == WIN_STATE_MAXIMIZED) {
            /* Restore from maximize */
            win->state = WIN_STATE_NORMAL;
        } else {
            /* Save current state before maximizing */
            win->state = WIN_STATE_MAXIMIZED;
            /* Maximize to screen size */
            win->x = 0;
            win->y = 40;  /* Below top taskbar */
            win->w = g_wm_ctx.screen_w;
            win->h = g_wm_ctx.screen_h - 40 - 64;  /* Above taskbar */
        }
    }
}

void wm_snap_window(wm_window_t *win, wm_window_state_t snap_state)
{
    if (!win) return;

    int taskbar_h = 64;
    int top_bar_h = 40;
    int available_h = g_wm_ctx.screen_h - top_bar_h - taskbar_h;

    switch (snap_state) {
    case WIN_STATE_SNAPPED_HALF_LEFT:
        win->state = WIN_STATE_SNAPPED_HALF_LEFT;
        win->x = 0;
        win->y = top_bar_h;
        win->w = g_wm_ctx.screen_w / 2;
        win->h = available_h;
        break;

    case WIN_STATE_SNAPPED_HALF_RIGHT:
        win->state = WIN_STATE_SNAPPED_HALF_RIGHT;
        win->x = g_wm_ctx.screen_w / 2;
        win->y = top_bar_h;
        win->w = g_wm_ctx.screen_w / 2;
        win->h = available_h;
        break;

    case WIN_STATE_SNAPPED_THIRD_LEFT:
        win->state = WIN_STATE_SNAPPED_THIRD_LEFT;
        win->x = 0;
        win->y = top_bar_h;
        win->w = g_wm_ctx.screen_w / 3;
        win->h = available_h;
        break;

    case WIN_STATE_SNAPPED_THIRD_CENTER:
        win->state = WIN_STATE_SNAPPED_THIRD_CENTER;
        win->x = g_wm_ctx.screen_w / 3;
        win->y = top_bar_h;
        win->w = g_wm_ctx.screen_w / 3;
        win->h = available_h;
        break;

    case WIN_STATE_SNAPPED_THIRD_RIGHT:
        win->state = WIN_STATE_SNAPPED_THIRD_RIGHT;
        win->x = (g_wm_ctx.screen_w * 2) / 3;
        win->y = top_bar_h;
        win->w = g_wm_ctx.screen_w / 3;
        win->h = available_h;
        break;

    default:
        break;
    }
}

void wm_restore_window(wm_window_t *win)
{
    if (!win) return;
    win->state = WIN_STATE_NORMAL;
}

/* ---- Window Positioning -------------------------------------------------- */
void wm_move_window(wm_window_t *win, int x, int y)
{
    if (!win) return;
    win->x = x;
    win->y = y;
}

void wm_resize_window(wm_window_t *win, int w, int h)
{
    if (!win) return;

    /* Respect minimum and maximum constraints */
    if (w < 200) w = 200;
    if (h < 150) h = 150;
    if (w > (int)g_wm_ctx.screen_w - 20) w = g_wm_ctx.screen_w - 20;
    if (h > (int)g_wm_ctx.screen_h - 20) h = g_wm_ctx.screen_h - 20;

    win->w = w;
    win->h = h;
}

void wm_set_title(wm_window_t *win, const char *title)
{
    if (!win) return;
    strncpy(win->title, title, sizeof(win->title) - 1);
    win->title[sizeof(win->title) - 1] = '\0';
}

/* ---- Z-Order Management -------------------------------------------------- */
void wm_raise_window(wm_window_t *win)
{
    if (!win) return;

    /* Remove from current position */
    if (win->prev)
        win->prev->next = win->next;
    else
        g_wm_ctx.windows_head = win->next;

    if (win->next)
        win->next->prev = win->prev;
    else
        g_wm_ctx.windows_tail = win->prev;

    /* Insert at head (front/top) */
    win->prev = NULL;
    win->next = g_wm_ctx.windows_head;
    if (g_wm_ctx.windows_head)
        g_wm_ctx.windows_head->prev = win;
    g_wm_ctx.windows_head = win;

    if (!g_wm_ctx.windows_tail)
        g_wm_ctx.windows_tail = win;

    win->z_order = ++g_wm_ctx.frame_count;
}

void wm_lower_window(wm_window_t *win)
{
    if (!win) return;

    /* Remove from current position */
    if (win->prev)
        win->prev->next = win->next;
    else
        g_wm_ctx.windows_head = win->next;

    if (win->next)
        win->next->prev = win->prev;
    else
        g_wm_ctx.windows_tail = win->prev;

    /* Insert at tail (back/bottom) */
    win->next = NULL;
    win->prev = g_wm_ctx.windows_tail;
    if (g_wm_ctx.windows_tail)
        g_wm_ctx.windows_tail->next = win;
    g_wm_ctx.windows_tail = win;

    if (!g_wm_ctx.windows_head)
        g_wm_ctx.windows_head = win;
}

/* ---- Hit Testing --------------------------------------------------------- */
wm_window_t *wm_window_at_point(int x, int y)
{
    /* Iterate from front (head) to back, return first hit */
    for (wm_window_t *win = g_wm_ctx.windows_head; win; win = win->next) {
        if (win->state == WIN_STATE_MINIMIZED)
            continue;
        if (x >= win->x && x < win->x + win->w &&
            y >= win->y && y < win->y + win->h)
            return win;
    }
    return NULL;
}

static int wm_hit_test_titlebar(wm_window_t *win, int x, int y)
{
    if (!(win->style & WS_TITLEBAR))
        return 0;

    return (y >= win->y && y < win->y + WM_TITLEBAR_HEIGHT &&
            x >= win->x && x < win->x + win->w);
}

/*
 * Mirrors titlebar.c's right-to-left button layout exactly: close drawn
 * first (rightmost), then maximize, then minimize — each only if its style
 * flag is set, each stepping the cursor left by WM_BUTTON_SIZE+2 in turn.
 * Without this, every click anywhere on the titlebar (including squarely on
 * the close button) fell through to wm_hit_test_titlebar()'s "start
 * dragging the window" branch, and the buttons — though correctly drawn —
 * were completely inert.
 */
wm_titlebar_button_t wm_hit_test_button(wm_window_t *win, int x, int y)
{
    if (!win || !(win->style & WS_TITLEBAR))
        return WM_BTN_NONE;
    if (y < win->y || y >= win->y + WM_TITLEBAR_HEIGHT)
        return WM_BTN_NONE;

    int button_x = win->x + win->w - WM_BUTTON_SIZE - WM_BUTTON_MARGIN;

    if (win->style & WS_CLOSABLE) {
        if (x >= button_x && x < button_x + WM_BUTTON_SIZE)
            return WM_BTN_CLOSE;
        button_x -= (WM_BUTTON_SIZE + 2);
    }
    if (win->style & WS_MAXIMIZABLE) {
        if (x >= button_x && x < button_x + WM_BUTTON_SIZE)
            return WM_BTN_MAXIMIZE;
        button_x -= (WM_BUTTON_SIZE + 2);
    }
    if (win->style & WS_MINIMIZABLE) {
        if (x >= button_x && x < button_x + WM_BUTTON_SIZE)
            return WM_BTN_MINIMIZE;
    }
    return WM_BTN_NONE;
}

static int wm_hit_test_edge(wm_window_t *win, int x, int y)
{
    if (!(win->style & WS_RESIZABLE))
        return EDGE_NONE;

    int edge_size = 8;
    int on_left   = (x >= win->x && x < win->x + edge_size);
    int on_right  = (x >= win->x + win->w - edge_size && x < win->x + win->w);
    int on_top    = (y >= win->y && y < win->y + edge_size);
    int on_bottom = (y >= win->y + win->h - edge_size && y < win->y + win->h);

    if (on_left && on_top) return EDGE_TOP_LEFT;
    if (on_right && on_top) return EDGE_TOP_RIGHT;
    if (on_left && on_bottom) return EDGE_BOTTOM_LEFT;
    if (on_right && on_bottom) return EDGE_BOTTOM_RIGHT;
    if (on_left) return EDGE_LEFT;
    if (on_right) return EDGE_RIGHT;
    if (on_top) return EDGE_TOP;
    if (on_bottom) return EDGE_BOTTOM;

    return EDGE_NONE;
}

/* ---- Input Handling ------------------------------------------------------ */
void wm_handle_mouse_move(int x, int y)
{
    if (g_wm_ctx.drag_active && g_wm_ctx.drag_window) {
        wm_window_t *win = g_wm_ctx.drag_window;
        int dx = x - g_wm_ctx.drag_start_x;
        int dy = y - g_wm_ctx.drag_start_y;

        if (g_wm_ctx.drag_edge == EDGE_NONE) {
            /* Moving window */
            wm_move_window(win, win->x + dx, win->y + dy);
        } else {
            /* Resizing window */
            int new_w = win->w;
            int new_h = win->h;
            int new_x = win->x;
            int new_y = win->y;

            if (g_wm_ctx.drag_edge & EDGE_LEFT) {
                new_x += dx;
                new_w -= dx;
            }
            if (g_wm_ctx.drag_edge & EDGE_RIGHT)
                new_w += dx;
            if (g_wm_ctx.drag_edge & EDGE_TOP) {
                new_y += dy;
                new_h -= dy;
            }
            if (g_wm_ctx.drag_edge & EDGE_BOTTOM)
                new_h += dy;

            wm_move_window(win, new_x, new_y);
            wm_resize_window(win, new_w, new_h);
        }

        g_wm_ctx.drag_start_x = x;
        g_wm_ctx.drag_start_y = y;
    }
}

/* Double-click window: 400ms at the kernel's fixed 100 Hz tick rate
 * (see pit_set_frequency(100) in kernel.c's interrupt_init()). */
#define WM_DOUBLE_CLICK_TICKS 40
#define WM_DOUBLE_CLICK_DIST  6

void wm_handle_mouse_click(int x, int y, int button)
{
    wm_window_t *win = wm_window_at_point(x, y);
    if (!win) return;

    wm_set_focused(win);

    /* Titlebar buttons take priority over both dragging and double-click */
    wm_titlebar_button_t btn = wm_hit_test_button(win, x, y);
    if (btn != WM_BTN_NONE) {
        switch (btn) {
        case WM_BTN_CLOSE:
            wm_close_window(win);
            break;
        case WM_BTN_MAXIMIZE:
            wm_set_maximized(win, 1); /* toggles: wm_set_maximized handles restore-if-already-maxed */
            break;
        case WM_BTN_MINIMIZE:
            wm_set_minimized(win, 1);
            break;
        default:
            break;
        }
        return;
    }

    /* Titlebar drag area: double-click toggles maximize (Windows 11
     * behaviour), single click (or a click that doesn't complete a
     * double-click) starts a drag. */
    if (wm_hit_test_titlebar(win, x, y)) {
        uint32_t now = (uint32_t)g_kstate.uptime_ticks;
        int dx = x - g_wm_ctx.last_click_x;
        int dy = y - g_wm_ctx.last_click_y;
        int dist2 = dx * dx + dy * dy;

        if (g_wm_ctx.double_click_active == 0 &&
            (now - g_wm_ctx.last_click_time) <= WM_DOUBLE_CLICK_TICKS &&
            dist2 <= (WM_DOUBLE_CLICK_DIST * WM_DOUBLE_CLICK_DIST)) {
            if (win->style & WS_MAXIMIZABLE)
                wm_set_maximized(win, 1);
            g_wm_ctx.double_click_active = 1; /* swallow the 3rd click of a rapid triple */
            g_wm_ctx.last_click_time = now;
            return;
        }

        g_wm_ctx.last_click_time = now;
        g_wm_ctx.last_click_x = x;
        g_wm_ctx.last_click_y = y;
        g_wm_ctx.double_click_active = 0;

        g_wm_ctx.drag_active = 1;
        g_wm_ctx.drag_window = win;
        g_wm_ctx.drag_start_x = x;
        g_wm_ctx.drag_start_y = y;
        g_wm_ctx.drag_edge = EDGE_NONE;
        return;
    }

    /* Check for edge click (resize) */
    int edge = wm_hit_test_edge(win, x, y);
    if (edge != EDGE_NONE) {
        g_wm_ctx.drag_active = 1;
        g_wm_ctx.drag_window = win;
        g_wm_ctx.drag_start_x = x;
        g_wm_ctx.drag_start_y = y;
        g_wm_ctx.drag_edge = edge;
        return;
    }

    /* Forward to window event handler */
    if (win->on_event) {
        wm_input_event_t event;
        event.type = IE_MOUSE_DOWN;
        event.data.mouse.x = x - win->x;
        event.data.mouse.y = y - win->y;
        event.data.mouse.button = button;
        event.timestamp = g_kstate.uptime_ticks;
        win->on_event(win, &event);
    }
}

void wm_handle_mouse_release(int x, int y, int button)
{
    g_wm_ctx.drag_active = 0;
    g_wm_ctx.drag_window = NULL;

    wm_window_t *win = wm_window_at_point(x, y);
    if (win && win->on_event) {
        wm_input_event_t event;
        event.type = IE_MOUSE_UP;
        event.data.mouse.x = x - win->x;
        event.data.mouse.y = y - win->y;
        event.data.mouse.button = button;
        event.timestamp = g_kstate.uptime_ticks;
        win->on_event(win, &event);
    }
}

/* Non-printable key codes — must match the KEY_* constants in
 * kernel/platform.c's irq1_keyboard_handler (duplicated here rather than
 * shared via a header since wm.c has no dependency on platform.c, and
 * these are a stable, documented part of the keyboard->WM contract). */
#define WM_KEY_TAB    9
#define WM_KEY_LEFT   0x104
#define WM_KEY_RIGHT  0x105
#define WM_KEY_UP     0x102
#define WM_KEY_DOWN   0x103

void wm_alt_tab_next(void)
{
    if (!g_wm_ctx.windows_head) return;

    wm_window_t *start = g_wm_ctx.focused ? g_wm_ctx.focused : g_wm_ctx.windows_head;
    wm_window_t *candidate = start->next ? start->next : g_wm_ctx.windows_head;

    /* Walk forward at most wm_count_windows() steps looking for the next
     * non-minimized window, wrapping at the tail. Bounded so a desktop
     * full of minimized windows can't spin forever. */
    int max_steps = wm_count_windows();
    for (int i = 0; i < max_steps && candidate; i++) {
        if (candidate != start && candidate->state != WIN_STATE_MINIMIZED) {
            wm_set_focused(candidate);
            return;
        }
        candidate = candidate->next ? candidate->next : g_wm_ctx.windows_head;
    }
    /* Only one focusable window (or none) — nothing to switch to. */
}

void wm_snap_shortcut(int direction)
{
    wm_window_t *win = g_wm_ctx.focused;
    if (!win) return;

    switch (direction) {
    case 0: wm_snap_window(win, WIN_STATE_SNAPPED_HALF_LEFT);  break;
    case 1: wm_snap_window(win, WIN_STATE_SNAPPED_HALF_RIGHT); break;
    case 2: wm_set_maximized(win, 1); break;
    case 3: wm_set_minimized(win, 1); break;
    default: break;
    }
}

void wm_handle_key(uint32_t key, int mods)
{
    /* System-level shortcuts: intercepted here, never forwarded to apps. */
    if ((mods & KMOD_ALT) && key == WM_KEY_TAB) {
        wm_alt_tab_next();
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

    if (g_wm_ctx.focused && g_wm_ctx.focused->on_event) {
        wm_input_event_t event;
        event.type = IE_KEY_DOWN;
        event.data.keyboard.key = key;
        event.data.keyboard.mods = mods;
        event.timestamp = g_kstate.uptime_ticks;
        g_wm_ctx.focused->on_event(g_wm_ctx.focused, &event);
    }
}

void wm_handle_key_up(uint32_t key, int mods)
{
    if (g_wm_ctx.focused && g_wm_ctx.focused->on_event) {
        wm_input_event_t event;
        event.type = IE_KEY_UP;
        event.data.keyboard.key = key;
        event.data.keyboard.mods = mods;
        event.timestamp = g_kstate.uptime_ticks;
        g_wm_ctx.focused->on_event(g_wm_ctx.focused, &event);
    }
}

void wm_handle_text_input(char ch)
{
    if (g_wm_ctx.focused && g_wm_ctx.focused->on_event) {
        wm_input_event_t event;
        event.type = IE_TEXT_INPUT;
        event.data.text.ch = ch;
        event.timestamp = g_kstate.uptime_ticks;
        g_wm_ctx.focused->on_event(g_wm_ctx.focused, &event);
    }
}

void wm_handle_mouse_wheel(int x, int y, int delta)
{
    wm_window_t *win = wm_window_at_point(x, y);
    if (win && win->on_event) {
        wm_input_event_t event;
        event.type = IE_MOUSE_WHEEL;
        event.data.mouse.x = x - win->x;
        event.data.mouse.y = y - win->y;
        event.data.mouse.button = delta; /* wheel delta reuses the button field; see wm.h */
        event.timestamp = g_kstate.uptime_ticks;
        win->on_event(win, &event);
    }
}

void wm_handle_input(wm_input_event_t *event)
{
    if (!event) return;

    switch (event->type) {
    case IE_MOUSE_MOVE:
        wm_handle_mouse_move(event->data.mouse.x, event->data.mouse.y);
        break;
    case IE_MOUSE_DOWN:
        wm_handle_mouse_click(event->data.mouse.x, event->data.mouse.y, event->data.mouse.button);
        break;
    case IE_MOUSE_UP:
        wm_handle_mouse_release(event->data.mouse.x, event->data.mouse.y, event->data.mouse.button);
        break;
    case IE_MOUSE_WHEEL:
        wm_handle_mouse_wheel(event->data.mouse.x, event->data.mouse.y, event->data.mouse.button);
        break;
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
        break;
    }
}

/* ---- Rendering ----------------------------------------------------------- */
void wm_draw_window(wm_window_t *win, uint32_t *framebuffer)
{
    if (!win || !framebuffer || win->state == WIN_STATE_MINIMIZED)
        return;

    /* Call window's custom draw callback */
    if (win->on_draw) {
        win->on_draw(win, framebuffer);
    }

    titlebar_draw_window_frame(win, framebuffer);
}

void wm_draw_all(uint32_t *framebuffer)
{
    if (!framebuffer) return;

    /* Draw all windows from back to front */
    for (wm_window_t *win = g_wm_ctx.windows_tail; win; win = win->prev) {
        wm_draw_window(win, framebuffer);
    }

    g_wm_ctx.frame_count++;
}
