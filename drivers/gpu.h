/*
 * =============================================================================
 * JARVIS OS — GPU Framebuffer Driver Header (drivers/gpu.h)
 * Language: C17
 *
 * All drawing goes to an off-screen back buffer. Nothing reaches the screen
 * until fb_present() copies the finished frame to video memory in one pass,
 * so the display never shows a half-drawn frame.
 *
 * Colours are 0x00RRGGBB.
 * =============================================================================
 */

#pragma once

#include <stdint.h>

/* ---- Setup / info -------------------------------------------------------- */
void gpu_framebuffer_init(void);
uint32_t fb_get_width(void);
uint32_t fb_get_height(void);
uint32_t *fb_get_base(void);          /* back buffer, stride == width */

/* ---- Clipping ------------------------------------------------------------ */
typedef struct { int x0, y0, x1, y1; } fb_clip_t;   /* x1/y1 exclusive */
void fb_set_clip(int x, int y, int w, int h);       /* intersects with screen */
void fb_reset_clip(void);
fb_clip_t fb_get_clip(void);
void fb_restore_clip(fb_clip_t c);

/* ---- Primitives ---------------------------------------------------------- */
void fb_put_pixel(int x, int y, uint32_t color);
void fb_fill_rect(int x, int y, int w, int h, uint32_t color);
void fb_draw_rect(int x, int y, int w, int h, uint32_t color);      /* outline */
void fb_draw_line(int x0, int y0, int x1, int y1, uint32_t color);
void fb_clear(uint32_t color);
void fb_gradient_v(int x, int y, int w, int h, uint32_t top, uint32_t bottom);

uint32_t fb_alpha_blend(uint32_t fg, uint32_t bg, uint8_t alpha);
void fb_blend_pixel(int x, int y, uint32_t color, uint8_t alpha);
void fb_blend_rect(int x, int y, int w, int h, uint32_t color, uint8_t alpha);

/* Anti-aliased rounded shapes */
void fb_fill_rounded_rect(int x, int y, int w, int h, int r, uint32_t color);
void fb_blend_rounded_rect(int x, int y, int w, int h, int r,
                           uint32_t color, uint8_t alpha);
void fb_draw_rounded_rect(int x, int y, int w, int h, int r, uint32_t color);
void fb_fill_circle(int cx, int cy, int r, uint32_t color);
void fb_fill_ring(int cx, int cy, int r_out, int r_in, uint32_t color);

/* ---- Text ----------------------------------------------------------------
 * Anti-aliased fonts (see drivers/font_aa.h for the faces). y is the top of a
 * 16 px text line. fb_draw_text is proportional; *_mono and the char calls
 * use the 8 px monospace face. */
#include "font_aa.h"
int  fb_text(int px, int py, const char *str, uint32_t fg, int font);     /* returns end x */
int  fb_text_n(int px, int py, const char *str, int n, uint32_t fg, int font);
int  fb_text_width_font(const char *str, int font);
int  fb_text_width_n_font(const char *str, int n, int font);
int  fb_text_index_at(const char *str, int dx, int font);
void fb_put_char(int px, int py, char c, uint32_t fg, uint32_t bg);      /* mono */
void fb_put_string(int px, int py, const char *str, uint32_t fg, uint32_t bg);
void fb_draw_char(int px, int py, char c, uint32_t fg);                  /* mono, no bg */
void fb_draw_text(int px, int py, const char *str, uint32_t fg);
void fb_draw_text_bold(int px, int py, const char *str, uint32_t fg);
void fb_draw_text_mono(int px, int py, const char *str, uint32_t fg);
void fb_draw_text_scaled(int px, int py, const char *str, uint32_t fg, int scale);
int  fb_text_width(const char *str);
int  fb_text_width_n(const char *str, int n);       /* first n characters */
int  fb_text_width_bold(const char *str);
int  fb_text_width_scaled(const char *str, int scale);

/* ---- Whole-frame operations ---------------------------------------------- */
void fb_save_frame(uint32_t *dst);          /* back buffer -> dst (w*h px)   */
void fb_load_frame(const uint32_t *src);    /* src -> back buffer            */
void fb_present(void);                      /* back buffer -> screen         */

/* ---- Display settings ---------------------------------------------------- */
void fb_set_color_filter(int brightness, int warmth);   /* 10..100 %, 0..100 */
int  fb_mode_count(void);
void fb_mode_get(int i, int *w, int *h);
int  fb_set_mode(int w, int h);                        /* 0 = ok */

/* ---- Mouse cursor (drawn on the screen only, never in the back buffer) --- */
void fb_cursor_set_pos(int x, int y);
void fb_cursor_show(int visible);
void fb_cursor_refresh(void);               /* cheap: repaint cursor only    */

/* ---- Colour constants ---------------------------------------------------- */
#define FB_COLOR_BLACK      0x00000000
#define FB_COLOR_WHITE      0x00FFFFFF
#define FB_COLOR_RED        0x00FF0000
#define FB_COLOR_GREEN      0x0000FF00
#define FB_COLOR_BLUE       0x000000FF
#define FB_COLOR_CYAN       0x0000FFFF
#define FB_COLOR_MAGENTA    0x00FF00FF
#define FB_COLOR_YELLOW     0x00FFFF00
