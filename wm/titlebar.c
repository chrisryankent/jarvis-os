/*
 * =============================================================================
 * Kryzen OS — Window frame renderer (wm/titlebar.c)
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
    int layers = focused ? 6 : 4;
    int r = WM_CORNER_RADIUS;
    for (int i = layers; i >= 1; i--) {
        uint8_t a = (uint8_t)(focused ? (22 - i * 3) : (12 - i * 2));
        int off_y = focused ? 3 : 2;
        fb_blend_rounded_rect(win->x - i, win->y - i + off_y,
                              win->w + 2 * i, win->h + 2 * i,
                              r + i, 0x00000000, a);
    }
}

static void draw_caption_glyph(wm_titlebar_button_t btn, int bx, int by,
                               uint32_t fg)
{
    int cx = bx + WM_BUTTON_W / 2;
    int cy = by + WM_TITLEBAR_HEIGHT / 2;

    switch (btn) {
    case WM_BTN_MINIMIZE:
        fb_draw_line_aa(cx - 5, cy, cx + 5, cy, 1, fg, 255);
        break;
    case WM_BTN_MAXIMIZE:
        fb_neon_rounded_rect(cx - 5, cy - 5, 11, 11, 2, 1, 0, fg, fg, 255);
        break;
    case WM_BTN_CLOSE:
        fb_draw_line_aa(cx - 5, cy - 5, cx + 5, cy + 5, 1, fg, 255);
        fb_draw_line_aa(cx - 5, cy + 5, cx + 5, cy - 5, 1, fg, 255);
        break;
    default:
        break;
    }
}

/* Title bar: icon and title on the left; minimize, maximize and close as
 * thin line glyphs on the right (close turns red under the pointer). */
static void draw_titlebar(wm_window_t *win, int focused, int radius)
{
    uint32_t bar = focused ? T(TITLE_ACTIVE) : T(TITLE_INACTIVE);
    uint32_t fg  = focused ? T(TITLE_TEXT) : T(TEXT_DIM);
    int border = radius ? 1 : 0;
    int x = win->x + border, y = win->y + border;
    int w = win->w - 2 * border;
    int h = WM_TITLEBAR_HEIGHT - border;

    if (radius) {
        fb_fill_rounded_rect(x, y, w, h + radius, radius - 1, bar);
        fb_blend_rect(x + radius, y, w - 2 * radius, 1, 0x00FFFFFF, focused ? 30 : 14);
    } else {
        fb_fill_rect(x, y, w, h, bar);
    }
    fb_blend_rect(x, y + h - 1, w, 1, 0x00FFFFFF, 18);

    static const struct { wm_titlebar_button_t id; uint32_t style; } buttons[3] = {
        { WM_BTN_CLOSE,    WS_CLOSABLE },
        { WM_BTN_MAXIMIZE, WS_MAXIMIZABLE },
        { WM_BTN_MINIMIZE, WS_MINIMIZABLE },
    };
    int bx = win->x + win->w - 4;

    for (int i = 0; i < 3; i++) {
        if (!(win->style & buttons[i].style)) continue;
        bx -= WM_BUTTON_W;
        int hovered = g_wm_ctx.hover_window == win &&
                      g_wm_ctx.hover_button == buttons[i].id;
        uint32_t gfg = fg;
        if (hovered) {
            uint32_t hbg = buttons[i].id == WM_BTN_CLOSE ? COL_CLOSE_HOVER : T(HOVER);
            fb_fill_rounded_rect(bx + 2, win->y + 5, WM_BUTTON_W - 4, WM_TITLEBAR_HEIGHT - 10, 5, hbg);
            if (buttons[i].id == WM_BTN_CLOSE) gfg = 0x00FFFFFF;
        }
        draw_caption_glyph(buttons[i].id, bx, win->y, gfg);
    }

    /* Icon + title */
    fb_clip_t saved = fb_get_clip();
    fb_set_clip(x, y, bx - x - 4, h);
    int tx = x + 12;
    if (win->icon) {
        icon_draw((icon_id_t)win->icon, tx, win->y + (WM_TITLEBAR_HEIGHT - 16) / 2, 16);
        tx += 24;
    }
    fb_draw_text(tx, win->y + (WM_TITLEBAR_HEIGHT - 16) / 2, win->title, fg);
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

        /* Frame: the focused window wears the cyan-to-violet outline with a
         * soft glow; the others a quiet hairline. */
        uint32_t border = focused ? UI_NEON_CYAN : T(BORDER);
        if (theme_get_mode() == THEME_HIGH_CONTRAST)
            border = focused ? T(ACCENT) : T(BORDER);
        fb_fill_rounded_rect(win->x, win->y, win->w, win->h, radius, T(WINDOW));
        if (focused && theme_get_mode() != THEME_HIGH_CONTRAST)
            fb_neon_rounded_rect(win->x - 1, win->y - 1, win->w + 2, win->h + 2, radius + 1, 2, 6,
                                 UI_NEON_CYAN, UI_NEON_VIOLET, 255);
        else
            fb_neon_rounded_rect(win->x, win->y, win->w, win->h, radius, 1, 0, border, border, 255);
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
