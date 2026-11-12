/*
 * =============================================================================
 * JARVIS OS — Window frame renderer (wm/titlebar.c)
 * Language: C17
 *
 * Draws each window: soft shadow, 1px border, title bar with icon, title and
 * caption buttons, then the app's client area (clipped so an app can never
 * paint outside its own window).
 * =============================================================================
 */

#include "wm.h"
#include "../kernel/kernel.h"
#include "../desktop/ui.h"
#include <string.h>

#define COL_CLOSE_HOVER 0x00C42B1C

static void draw_shadow(wm_window_t *win, int focused)
{
    int layers = focused ? 7 : 4;
    for (int i = 1; i <= layers; i++) {
        uint8_t a = (uint8_t)((layers + 1 - i) * (focused ? 5 : 4));
        int x = win->x - i, y = win->y - i + 2;
        int w = win->w + 2 * i, h = win->h + 2 * i;
        fb_blend_rect(x, y, w, 1, 0x00000000, a);
        fb_blend_rect(x, y + h - 1, w, 1, 0x00000000, a);
        fb_blend_rect(x, y + 1, 1, h - 2, 0x00000000, a);
        fb_blend_rect(x + w - 1, y + 1, 1, h - 2, 0x00000000, a);
    }
}

static void draw_caption_glyph(wm_titlebar_button_t btn, int bx, int by,
                               uint32_t fg)
{
    int cx = bx + WM_BUTTON_W / 2;
    int cy = by + WM_TITLEBAR_HEIGHT / 2;

    switch (btn) {
    case WM_BTN_MINIMIZE:
        fb_fill_rect(cx - 5, cy, 10, 1, fg);
        break;
    case WM_BTN_MAXIMIZE:
        fb_draw_rect(cx - 5, cy - 5, 10, 10, fg);
        break;
    case WM_BTN_CLOSE:
        fb_draw_line(cx - 5, cy - 5, cx + 5, cy + 5, fg);
        fb_draw_line(cx - 5, cy + 5, cx + 5, cy - 5, fg);
        break;
    default:
        break;
    }
}

static void draw_titlebar(wm_window_t *win, int focused, int radius)
{
    uint32_t bar = focused ? T(TITLE_ACTIVE) : T(TITLE_INACTIVE);
    uint32_t fg  = focused ? T(TITLE_TEXT) : T(TEXT_DIM);
    int border = radius ? 1 : 0;
    int x = win->x + border, y = win->y + border;
    int w = win->w - 2 * border;
    int h = WM_TITLEBAR_HEIGHT - border;

    /* Bar background with rounded top corners */
    if (radius) {
        fb_fill_rounded_rect(x, y, w, h + radius, radius - 1, bar);
    } else {
        fb_fill_rect(x, y, w, h, bar);
    }

    /* Caption buttons */
    static const struct { wm_titlebar_button_t id; uint32_t style; } buttons[3] = {
        { WM_BTN_CLOSE,    WS_CLOSABLE },
        { WM_BTN_MAXIMIZE, WS_MAXIMIZABLE },
        { WM_BTN_MINIMIZE, WS_MINIMIZABLE },
    };
    int bx = win->x + win->w;
    int maximized = win->state == WIN_STATE_MAXIMIZED;

    for (int i = 0; i < 3; i++) {
        if (!(win->style & buttons[i].style)) continue;
        bx -= WM_BUTTON_W;

        int hovered = g_wm_ctx.hover_window == win &&
                      g_wm_ctx.hover_button == buttons[i].id;
        uint32_t bbg = bar, bfg = fg;

        if (hovered) {
            int is_close = buttons[i].id == WM_BTN_CLOSE;
            bbg = is_close ? COL_CLOSE_HOVER : T(HOVER);
            bfg = is_close ? 0x00FFFFFF : T(TEXT);
            int bw = WM_BUTTON_W - (is_close ? border : 0);
            if (is_close && radius) {
                /* keep the window's rounded top-right corner */
                fb_fill_rounded_rect(bx, y, bw, h, radius - 1, bbg);
                fb_fill_rect(bx, y, bw - radius, h, bbg);
                fb_fill_rect(bx, y + radius, bw, h - radius, bbg);
            } else {
                fb_fill_rect(bx, y, bw, h, bbg);
            }
        }

        if (buttons[i].id == WM_BTN_MAXIMIZE && maximized) {
            /* "restore" glyph: two overlapping squares */
            int cx = bx + WM_BUTTON_W / 2, cy = win->y + WM_TITLEBAR_HEIGHT / 2;
            fb_draw_rect(cx - 3, cy - 5, 8, 8, bfg);
            fb_fill_rect(cx - 5, cy - 3, 8, 8, bbg);
            fb_draw_rect(cx - 5, cy - 3, 8, 8, bfg);
        } else {
            draw_caption_glyph(buttons[i].id, bx, win->y, bfg);
        }
    }

    /* Icon + title, clipped so long titles never run under the buttons */
    fb_clip_t saved = fb_get_clip();
    fb_set_clip(x, y, bx - x - 4, h);
    int tx = x + 10;
    if (win->icon) {
        icon_draw((icon_id_t)win->icon, tx, win->y + 8, 16);
        tx += 24;
    }
    fb_draw_text(tx, win->y + 8, win->title, fg);
    fb_restore_clip(saved);
}

void wm_draw_window(wm_window_t *win, uint32_t *framebuffer)
{
    if (!win || win->minimized)
        return;

    int focused = (win == g_wm_ctx.focused);
    int maximized = (win->state == WIN_STATE_MAXIMIZED);
    int radius = maximized ? 0 : WM_CORNER_RADIUS;

    if (!maximized) {
        draw_shadow(win, focused);

        /* Frame: rounded at the top, square at the bottom */
        uint32_t border = focused ? fb_alpha_blend(T(ACCENT), T(BORDER), 90) : T(BORDER);
        if (theme_get_mode() == THEME_HIGH_CONTRAST)
            border = focused ? T(ACCENT) : T(BORDER);
        fb_fill_rounded_rect(win->x, win->y, win->w, win->h, radius, border);
        fb_fill_rect(win->x, win->y + win->h - radius, win->w, radius, border);
    }

    if (win->style & WS_TITLEBAR)
        draw_titlebar(win, focused, radius);

    /* Client area */
    int cx, cy, cw, ch;
    wm_get_client(win, &cx, &cy, &cw, &ch);
    fb_fill_rect(cx, cy, cw, ch, T(WINDOW));

    if (win->on_draw && cw > 0 && ch > 0) {
        fb_clip_t saved = fb_get_clip();
        fb_set_clip(cx, cy, cw, ch);
        win->on_draw(win, framebuffer);
        fb_restore_clip(saved);
    }
}

void wm_draw_all(uint32_t *framebuffer)
{
    for (wm_window_t *win = g_wm_ctx.windows_tail; win; win = win->prev)
        wm_draw_window(win, framebuffer);
}

/* Translucent outline of where the dragged window will land if released. */
void wm_draw_snap_preview(void)
{
    int x, y, w, h;
    if (!wm_get_snap_target(&x, &y, &w, &h))
        return;

    fb_blend_rounded_rect(x + 8, y + 8, w - 16, h - 16, 8, T(ACCENT), 50);
    fb_draw_rounded_rect(x + 8, y + 8, w - 16, h - 16, 8, T(ACCENT));
}
