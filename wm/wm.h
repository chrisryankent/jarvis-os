/*
 * =============================================================================
 * JARVIS OS — Window Manager Header (wm/wm.h)
 * Language: C17
 *
 * Floating windows with a Windows-style frame: drag by the title bar, resize
 * from any edge, minimize / maximize / close, snap to the screen edges.
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
#define WS_FIXED          (WS_TITLEBAR | WS_BORDER | WS_MINIMIZABLE | WS_CLOSABLE)

/* ---- Window layout state (minimized is a separate flag) ------------------ */
typedef enum {
    WIN_STATE_NORMAL    = 0,
    WIN_STATE_MAXIMIZED = 2,
    WIN_STATE_SNAPPED_HALF_LEFT = 3,
    WIN_STATE_SNAPPED_HALF_RIGHT = 4,
} wm_window_state_t;

/* ---- Key codes -----------------------------------------------------------
 * Printable keys arrive as ASCII. Everything else uses the codes below; they
 * mirror the KEY_* values produced by kernel/platform.c's keyboard decoder.
 */
#define WM_KEY_BACKSPACE  8
#define WM_KEY_TAB        9
#define WM_KEY_ENTER      10
#define WM_KEY_ESC        0x101
#define WM_KEY_UP         0x102
#define WM_KEY_DOWN       0x103
#define WM_KEY_LEFT       0x104
#define WM_KEY_RIGHT      0x105
#define WM_KEY_DELETE     0x106
#define WM_KEY_HOME       0x107
#define WM_KEY_END        0x108
#define WM_KEY_F1         0x110   /* F1..F12 = 0x110..0x11B */
#define WM_KEY_F4         0x113
#define WM_KEY_SUPER_TAP  0x120   /* Super/Windows key tapped on its own */

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
    uint32_t timestamp;                     /* kernel ticks (100 Hz) */
    union {
        struct { int x, y; int button; } mouse;     /* button: 1=left 2=right 3=middle */
        struct { uint32_t key; int mods; } keyboard;
        struct { char ch; } text;
    } data;
} wm_input_event_t;

/* ---- Callbacks -----------------------------------------------------------
 * on_draw: called with the clip rectangle set to the window's client area.
 *          Use wm_get_client() for the area to draw into.
 * on_event: mouse coordinates are relative to the client area's top-left.
 */
struct wm_window;
typedef void (*wm_draw_callback_t)(struct wm_window *win, uint32_t *framebuffer);
typedef void (*wm_event_callback_t)(struct wm_window *win, wm_input_event_t *event);
typedef void (*wm_close_callback_t)(void *ctx);

/* ---- Window Structure ---------------------------------------------------- */
typedef struct wm_window {
    uint32_t wid;
    char title[64];
    int x, y, w, h;                         /* outer frame */
    int restore_x, restore_y, restore_w, restore_h;
    int min_w, min_h;
    wm_window_state_t state;
    int minimized;
    uint32_t style;                         /* WS_* flags */
    int icon;                               /* icon_id_t for the title bar */

    wm_draw_callback_t on_draw;
    wm_event_callback_t on_event;
    wm_close_callback_t on_close;           /* fired just before destruction */
    void *close_ctx;
    void *user_data;

    struct wm_window *prev;                 /* towards the front */
    struct wm_window *next;                 /* towards the back  */
} wm_window_t;

typedef enum {
    WM_BTN_NONE = 0,
    WM_BTN_CLOSE,
    WM_BTN_MAXIMIZE,
    WM_BTN_MINIMIZE,
} wm_titlebar_button_t;

/* ---- Window Manager Context ---------------------------------------------- */
typedef struct {
    wm_window_t *windows_head;              /* front-most window */
    wm_window_t *windows_tail;              /* back-most window  */
    wm_window_t *focused;
    uint32_t next_wid;

    uint32_t screen_w, screen_h;
    int work_x, work_y, work_w, work_h;     /* screen minus the taskbar */

    /* Move / resize drag */
    int drag_active;
    wm_window_t *drag_window;
    int drag_edge;                          /* EDGE_* (EDGE_NONE = moving) */
    int drag_moved;
    int drag_off_x, drag_off_y;             /* cursor offset inside window */
    int drag_start_mx, drag_start_my;
    int drag_start_x, drag_start_y, drag_start_w, drag_start_h;

    wm_window_t *capture;                   /* window holding the mouse button */
    int mouse_x, mouse_y;

    wm_window_t *hover_window;              /* title-bar button under cursor */
    wm_titlebar_button_t hover_button;

    uint32_t last_click_time;
    int last_click_x, last_click_y;
    uint32_t last_click_wid;
} wm_context_t;

/* Resize edges: independent bits so corners are simple ORs. */
#define EDGE_NONE           0
#define EDGE_LEFT           0x1
#define EDGE_RIGHT          0x2
#define EDGE_TOP            0x4
#define EDGE_BOTTOM         0x8

/* Frame geometry shared by wm.c (hit-testing) and titlebar.c (drawing). */
#define WM_TITLEBAR_HEIGHT  32
#define WM_BUTTON_W         46
#define WM_RESIZE_GRIP      5
#define WM_CORNER_RADIUS    8

/* ---- Public API ---------------------------------------------------------- */
void wm_init(uint32_t screen_w, uint32_t screen_h);
void wm_set_workarea(int x, int y, int w, int h);

wm_window_t *wm_create_window(const char *title, int x, int y, int w, int h,
                              uint32_t style, wm_draw_callback_t on_draw,
                              wm_event_callback_t on_event);
void wm_destroy_window(wm_window_t *win);
void wm_set_close_callback(wm_window_t *win, wm_close_callback_t cb, void *ctx);
void wm_close_window(wm_window_t *win);      /* fires on_close, then destroys */

void wm_set_focused(wm_window_t *win);       /* NULL clears focus; restores if minimized */
void wm_set_minimized(wm_window_t *win, int minimized);
void wm_set_maximized(wm_window_t *win, int maximized);
void wm_toggle_maximized(wm_window_t *win);
void wm_snap_window(wm_window_t *win, wm_window_state_t snap_state);
void wm_restore_window(wm_window_t *win);
void wm_move_window(wm_window_t *win, int x, int y);
void wm_resize_window(wm_window_t *win, int w, int h);
void wm_set_title(wm_window_t *win, const char *title);
void wm_raise_window(wm_window_t *win);

/* Client area (inside the frame, below the title bar), in screen coords. */
void wm_get_client(wm_window_t *win, int *x, int *y, int *w, int *h);

/* Rendering (wm/titlebar.c) */
void wm_draw_all(uint32_t *framebuffer);
void wm_draw_window(wm_window_t *win, uint32_t *framebuffer);
void wm_draw_snap_preview(void);

/* Mouse. Each returns 1 if the desktop needs to be redrawn. */
int  wm_handle_mouse_move(int x, int y);
int  wm_handle_mouse_click(int x, int y, int button);   /* 1 if a window was hit */
void wm_handle_mouse_release(int x, int y, int button);

/* Keyboard */
void wm_handle_input(wm_input_event_t *event);
void wm_handle_key(uint32_t key, int mods);
void wm_handle_key_up(uint32_t key, int mods);
void wm_handle_text_input(char ch);

/* Utilities */
wm_window_t *wm_window_at_point(int x, int y);
wm_titlebar_button_t wm_hit_test_button(wm_window_t *win, int x, int y);
int  wm_count_windows(void);
int  wm_is_dragging(void);
int  wm_get_snap_target(int *x, int *y, int *w, int *h);  /* 1 while a drag would snap */
void wm_alt_tab_next(void);
void wm_snap_shortcut(int direction);   /* 0=left 1=right 2=up 3=down */
void wm_minimize_all(void);

extern wm_context_t g_wm_ctx;
