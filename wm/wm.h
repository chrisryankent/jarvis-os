/*
 * =============================================================================
 * JARVIS OS — Window Manager Header (wm/wm.h)
 * Language: C17
 *
 * Core window management system with tiling support (floating + snap modes).
 * Inspired by Windows 11 (floating) + i3 (tiling snaps).
 * =============================================================================
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/* ---- Window Style Flags -------------------------------------------------- */
#define WS_TITLEBAR       0x0001
#define WS_BORDER         0x0002
#define WS_RESIZABLE      0x0004
#define WS_MINIMIZABLE    0x0008
#define WS_MAXIMIZABLE    0x0010
#define WS_CLOSABLE       0x0020
#define WS_TOPMOST        0x0040
#define WS_DEFAULT        (WS_TITLEBAR | WS_BORDER | WS_RESIZABLE | WS_MINIMIZABLE | WS_MAXIMIZABLE | WS_CLOSABLE)

/* ---- Window States ------------------------------------------------------- */
typedef enum {
    WIN_STATE_NORMAL    = 0,
    WIN_STATE_MINIMIZED = 1,
    WIN_STATE_MAXIMIZED = 2,
    WIN_STATE_SNAPPED_HALF_LEFT = 3,
    WIN_STATE_SNAPPED_HALF_RIGHT = 4,
    WIN_STATE_SNAPPED_THIRD_LEFT = 5,
    WIN_STATE_SNAPPED_THIRD_CENTER = 6,
    WIN_STATE_SNAPPED_THIRD_RIGHT = 7,
} wm_window_state_t;

/* ---- Input Events -------------------------------------------------------- */
typedef enum {
    IE_MOUSE_MOVE,
    IE_MOUSE_DOWN,
    IE_MOUSE_UP,
    IE_MOUSE_WHEEL,
    IE_KEY_DOWN,
    IE_KEY_UP,
    IE_TEXT_INPUT,
} input_event_type_t;

typedef struct {
    input_event_type_t type;
    uint32_t timestamp;
    union {
        struct { int x, y; int button; } mouse;
        struct { uint32_t key; int mods; } keyboard;
        struct { char ch; } text;
    } data;
} wm_input_event_t;

/* ---- Window Drawing Callback --------------------------------------------- */
struct wm_window;
typedef void (*wm_draw_callback_t)(struct wm_window *win, uint32_t *framebuffer);
typedef void (*wm_event_callback_t)(struct wm_window *win, wm_input_event_t *event);
typedef void (*wm_close_callback_t)(void *ctx);

/* ---- Window Structure ---------------------------------------------------- */
typedef struct wm_window {
    uint32_t wid;                           /* unique window ID */
    char title[256];
    int x, y, w, h;                         /* position and dimensions */
    int min_w, min_h;                       /* minimum size constraints */
    int max_w, max_h;                       /* maximum size constraints */
    uint32_t z_order;                       /* depth (higher = on top) */
    wm_window_state_t state;
    uint32_t style;                         /* WS_* flags */
    uint32_t bg_color;                      /* window background color (BGRA) */

    wm_draw_callback_t on_draw;
    wm_event_callback_t on_event;
    wm_close_callback_t on_close;            /* fired just before destruction */
    void *close_ctx;                         /* opaque context for on_close   */

    void *user_data;                        /* app-specific context */

    struct wm_window *prev;
    struct wm_window *next;
} wm_window_t;

/* ---- Window Manager Context ---------------------------------------------- */
typedef struct {
    wm_window_t *windows_head;              /* linked list of windows (back to front) */
    wm_window_t *windows_tail;
    wm_window_t *focused;                   /* currently focused window */
    uint32_t next_wid;

    uint32_t screen_w, screen_h;
    uint32_t frame_count;

    /* Drag state for window moving/resizing */
    int drag_active;
    wm_window_t *drag_window;
    int drag_start_x, drag_start_y;
    int drag_start_w, drag_start_h;
    int drag_edge;                          /* EDGE_* for resizing */

    /* Double-click detection */
    uint32_t last_click_time;
    int last_click_x, last_click_y;
    int double_click_active;
} wm_context_t;

/*
 * ---- Resize Edge Constants -------------------------------------------------
 * These MUST be independent bit flags (not sequential integers) because
 * wm_handle_mouse_move()'s resize logic tests them with bitwise AND
 * (`drag_edge & EDGE_LEFT`, etc.) to decide which sides of the window move.
 * The previous sequential numbering (LEFT=1, RIGHT=2, TOP=3, BOTTOM=4,
 * TOP_LEFT=5, ...) meant e.g. EDGE_BOTTOM_LEFT=7=0b111 satisfied
 * `& EDGE_TOP(3)` (7&3=3, truthy) and `& EDGE_RIGHT(2)` (7&2=2, truthy) even
 * though the corner has nothing to do with those edges — dragging the
 * bottom-left corner would also incorrectly resize from the top and right.
 */
#define EDGE_NONE           0
#define EDGE_LEFT           0x1
#define EDGE_RIGHT          0x2
#define EDGE_TOP            0x4
#define EDGE_BOTTOM         0x8
#define EDGE_TOP_LEFT       (EDGE_TOP | EDGE_LEFT)
#define EDGE_TOP_RIGHT      (EDGE_TOP | EDGE_RIGHT)
#define EDGE_BOTTOM_LEFT    (EDGE_BOTTOM | EDGE_LEFT)
#define EDGE_BOTTOM_RIGHT   (EDGE_BOTTOM | EDGE_RIGHT)

/*
 * Titlebar button geometry — shared between wm/titlebar.c (drawing) and
 * wm/wm.c (click hit-testing). Previously these lived only as file-local
 * #defines inside titlebar.c, so wm.c's wm_hit_test_titlebar() had no way
 * to tell a click on the close/maximize/minimize button apart from a click
 * on the plain drag area of the titlebar — every titlebar click, including
 * ones square on the close button, was treated as "start dragging the
 * window", and the buttons (though correctly drawn) were completely inert.
 */
#define WM_TITLEBAR_HEIGHT  32
#define WM_BUTTON_SIZE      28
#define WM_BUTTON_MARGIN    6

typedef enum {
    WM_BTN_NONE = 0,
    WM_BTN_CLOSE,
    WM_BTN_MAXIMIZE,
    WM_BTN_MINIMIZE,
} wm_titlebar_button_t;

/* Returns which titlebar button (if any) screen point (x,y) lands on for
 * window `win`, using the exact same right-to-left layout math titlebar.c
 * uses to draw them (close, then maximize, then minimize). */
wm_titlebar_button_t wm_hit_test_button(wm_window_t *win, int x, int y);

/* ---- Public API ---------------------------------------------------------- */

/* Initialize window manager */
void wm_init(uint32_t screen_w, uint32_t screen_h);

/* Create and destroy windows */
wm_window_t *wm_create_window(const char *title, int x, int y, int w, int h,
                              uint32_t style, wm_draw_callback_t on_draw,
                              wm_event_callback_t on_event);
void wm_destroy_window(wm_window_t *win);
void wm_set_close_callback(wm_window_t *win, wm_close_callback_t cb, void *ctx);
void wm_close_window(wm_window_t *win);  /* user-initiated close: fires on_close, then destroys */

/* Window state management */
void wm_set_focused(wm_window_t *win);
void wm_set_minimized(wm_window_t *win, int minimized);
void wm_set_maximized(wm_window_t *win, int maximized);
void wm_snap_window(wm_window_t *win, wm_window_state_t snap_state);
void wm_restore_window(wm_window_t *win);

/* Window positioning */
void wm_move_window(wm_window_t *win, int x, int y);
void wm_resize_window(wm_window_t *win, int w, int h);
void wm_set_title(wm_window_t *win, const char *title);

/* Rendering */
void wm_draw_all(uint32_t *framebuffer);
void wm_draw_window(wm_window_t *win, uint32_t *framebuffer);
void titlebar_draw(wm_window_t *win, uint32_t *framebuffer);
void titlebar_draw_window_frame(wm_window_t *win, uint32_t *framebuffer);

/* Input routing */
void wm_handle_input(wm_input_event_t *event);
void wm_handle_mouse_move(int x, int y);
void wm_handle_mouse_click(int x, int y, int button);
void wm_handle_mouse_release(int x, int y, int button);
void wm_handle_mouse_wheel(int x, int y, int delta);
void wm_handle_key(uint32_t key, int mods);
void wm_handle_key_up(uint32_t key, int mods);
void wm_handle_text_input(char ch);

/* Utilities */
wm_window_t *wm_window_at_point(int x, int y);
void wm_raise_window(wm_window_t *win);
void wm_lower_window(wm_window_t *win);
int  wm_count_windows(void);                 /* live (non-destroyed) window count */

/* Alt+Tab window switching. Each call raises and focuses the next
 * non-minimized window in z-order after the currently focused one
 * (wrapping around) — call once per Alt+Tab keydown. */
void wm_alt_tab_next(void);

/* Win+Left / Win+Right / Win+Up / Win+Down snap shortcuts (Windows 11
 * parity). Operates on the currently focused window. */
void wm_snap_shortcut(int direction); /* 0=left 1=right 2=up(maximize) 3=down(minimize) */

/* Global context access */
extern wm_context_t g_wm_ctx;
