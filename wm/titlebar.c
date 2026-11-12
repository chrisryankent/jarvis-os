/*
 * =============================================================================
 * JARVIS OS — Window Titlebar Renderer (wm/titlebar.c)
 * Language: C17
 *
 * Renders window titlebars with minimize/maximize/close buttons.
 * =============================================================================
 */

#include "wm.h"
#include "../drivers/gpu.h"
#include <string.h>

/* TITLEBAR_HEIGHT/BUTTON_SIZE/BUTTON_MARGIN now come from wm.h as
 * WM_TITLEBAR_HEIGHT/WM_BUTTON_SIZE/WM_BUTTON_MARGIN — shared with wm.c's
 * wm_hit_test_button() so the drawn buttons and the clickable hit regions
 * can never drift apart again (see wm.h for the history of why this
 * mattered: they used to be separate, undrifted-on-purpose-but-fragile
 * copies, and any future edit to one without the other would silently
 * reintroduce the "close button doesn't actually close" bug).
 */
#define TITLEBAR_HEIGHT WM_TITLEBAR_HEIGHT
#define BUTTON_SIZE     WM_BUTTON_SIZE
#define BUTTON_MARGIN   WM_BUTTON_MARGIN

/* Colors (BGRA format) */
#define COL_TITLEBAR_ACTIVE   0x005BA3D6  /* Windows 11 Blue */
#define COL_TITLEBAR_INACTIVE 0x003F3F3F  /* Dark gray */
#define COL_TITLEBAR_TEXT     0x00FFFFFF  /* White */
#define COL_BUTTON_HOVER      0x00E81828  /* Bright red */
#define COL_BUTTON_PRESSED    0x00C41E1E  /* Darker red */

void titlebar_draw(wm_window_t *win, uint32_t *framebuffer)
{
    if (!win || !framebuffer) return;
    if (!(win->style & WS_TITLEBAR)) return;

    uint32_t titlebar_color = (win == g_wm_ctx.focused) ?
        COL_TITLEBAR_ACTIVE : COL_TITLEBAR_INACTIVE;

    /* Draw titlebar background */
    fb_fill_rect(win->x, win->y, win->w, TITLEBAR_HEIGHT, titlebar_color);

    /* Draw title text */
    fb_put_string(win->x + 8, win->y + 8, win->title, COL_TITLEBAR_TEXT, titlebar_color);

    /* Draw window control buttons (right side) */
    int button_x = win->x + win->w - BUTTON_SIZE - BUTTON_MARGIN;

    /* Close button (rightmost) */
    if (win->style & WS_CLOSABLE) {
        fb_fill_rect(button_x, win->y + BUTTON_MARGIN, BUTTON_SIZE, BUTTON_SIZE, COL_BUTTON_HOVER);
        fb_put_char(button_x + 10, win->y + 10, 'x', COL_TITLEBAR_TEXT, COL_BUTTON_HOVER);
        button_x -= BUTTON_SIZE + 2;
    }

    /* Maximize button */
    if (win->style & WS_MAXIMIZABLE) {
        fb_fill_rect(button_x, win->y + BUTTON_MARGIN, BUTTON_SIZE, BUTTON_SIZE, titlebar_color);
        fb_fill_rect(button_x + 2, win->y + BUTTON_MARGIN + 2, BUTTON_SIZE - 4, BUTTON_SIZE - 4, COL_TITLEBAR_TEXT);
        button_x -= BUTTON_SIZE + 2;
    }

    /* Minimize button */
    if (win->style & WS_MINIMIZABLE) {
        fb_fill_rect(button_x, win->y + BUTTON_MARGIN, BUTTON_SIZE, BUTTON_SIZE, titlebar_color);
        fb_fill_rect(button_x + 4, win->y + BUTTON_MARGIN + 14, BUTTON_SIZE - 8, 2, COL_TITLEBAR_TEXT);
    }
}

void titlebar_draw_window_frame(wm_window_t *win, uint32_t *framebuffer)
{
    if (!win || !framebuffer) return;

    /* Draw titlebar */
    titlebar_draw(win, framebuffer);

    /* Draw border if resizable */
    if (win->style & WS_RESIZABLE) {
        uint32_t border_color = (win == g_wm_ctx.focused) ?
            COL_TITLEBAR_ACTIVE : 0x00202020;

        /* Outline */
        fb_fill_rect(win->x, win->y + TITLEBAR_HEIGHT, win->w, 1, border_color);
        fb_fill_rect(win->x, win->y, 1, win->h, border_color);
        fb_fill_rect(win->x + win->w - 1, win->y, 1, win->h, border_color);
        fb_fill_rect(win->x, win->y + win->h - 1, win->w, 1, border_color);
    }

    /* Draw shadow (soft drop shadow on focused windows) */
    if (win == g_wm_ctx.focused) {
        /* Simple shadow: dark pixels around edges */
        uint32_t shadow_color = 0x00000000;
        int shadow_size = 3;

        for (int i = 1; i <= shadow_size; i++) {
            uint8_t alpha = (255 * (shadow_size - i)) / shadow_size;
            uint32_t shadow = (shadow_color & 0x00FFFFFF) | (alpha << 24);

            /* Right shadow */
            fb_fill_rect(win->x + win->w, win->y, i, win->h, shadow);
            /* Bottom shadow */
            fb_fill_rect(win->x, win->y + win->h, win->w, i, shadow);
        }
    }
}
