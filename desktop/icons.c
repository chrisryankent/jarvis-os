/*
 * =============================================================================
 * JARVIS OS — Icons and small widgets (desktop/icons.c)
 *
 * Icons are drawn from primitives on a 24-unit grid and scaled to the
 * requested size, so the same definition serves the 16px titlebar icon, the
 * 24px taskbar icon and the 40px Start / desktop icon.
 * =============================================================================
 */

#include "../kernel/kernel.h"
#include "ui.h"
#include <string.h>

/* ---- Grid helpers -------------------------------------------------------- */
typedef struct { int x, y, size; } ic_t;

static int ic_s(const ic_t *c, int v)
{
    return (v * c->size) / 24;
}

static int ic_len(const ic_t *c, int v)
{
    int n = ic_s(c, v);
    return (v > 0 && n < 1) ? 1 : n;
}

static void ic_rect(const ic_t *c, int gx, int gy, int gw, int gh, uint32_t col)
{
    fb_fill_rect(c->x + ic_s(c, gx), c->y + ic_s(c, gy),
                 ic_len(c, gw), ic_len(c, gh), col);
}

static void ic_rr(const ic_t *c, int gx, int gy, int gw, int gh, int gr, uint32_t col)
{
    fb_fill_rounded_rect(c->x + ic_s(c, gx), c->y + ic_s(c, gy),
                         ic_len(c, gw), ic_len(c, gh), ic_s(c, gr), col);
}

static void ic_circle(const ic_t *c, int gx, int gy, int gr, uint32_t col)
{
    fb_fill_circle(c->x + ic_s(c, gx), c->y + ic_s(c, gy), ic_len(c, gr), col);
}

static void ic_ring(const ic_t *c, int gx, int gy, int gro, int gri, uint32_t col)
{
    int ro = ic_len(c, gro), ri = ic_s(c, gri);
    if (ri >= ro) ri = ro - 1;
    fb_fill_ring(c->x + ic_s(c, gx), c->y + ic_s(c, gy), ro, ri, col);
}

static void ic_line(const ic_t *c, int gx0, int gy0, int gx1, int gy1, uint32_t col)
{
    int x0 = c->x + ic_s(c, gx0), y0 = c->y + ic_s(c, gy0);
    int x1 = c->x + ic_s(c, gx1), y1 = c->y + ic_s(c, gy1);
    int thick = c->size >= 32 ? 3 : (c->size >= 20 ? 2 : 1);

    for (int t = 0; t < thick; t++) {
        fb_draw_line(x0 + t, y0, x1 + t, y1, col);
        if (t) fb_draw_line(x0, y0 + t, x1, y1 + t, col);
    }
}

/* ---- App icons ----------------------------------------------------------- */
void icon_draw(icon_id_t icon, int x, int y, int size)
{
    ic_t c = { x, y, size };

    switch (icon) {
    case ICON_FOLDER:
        ic_rr(&c, 2, 4, 10, 6, 2, 0x00E0A030);
        ic_rr(&c, 2, 7, 20, 14, 2, 0x00FFC83D);
        ic_rect(&c, 2, 10, 20, 1, 0x00FFDE7A);
        break;

    case ICON_FILE:
        ic_rr(&c, 5, 2, 14, 20, 2, 0x00E8EEF2);
        ic_rect(&c, 14, 2, 5, 5, 0x00B8C4CC);
        ic_rect(&c, 8, 10, 8, 1, 0x008896A0);
        ic_rect(&c, 8, 13, 8, 1, 0x008896A0);
        ic_rect(&c, 8, 16, 6, 1, 0x008896A0);
        break;

    case ICON_TERMINAL:
        ic_rr(&c, 2, 3, 20, 18, 3, 0x004A4A4A);
        ic_rr(&c, 3, 4, 18, 16, 2, 0x00101418);
        ic_line(&c, 6, 9, 9, 12, 0x00FFFFFF);
        ic_line(&c, 9, 12, 6, 15, 0x00FFFFFF);
        ic_rect(&c, 11, 15, 6, 2, 0x004EE0A0);
        break;

    case ICON_JARVIS:
        ic_circle(&c, 12, 12, 11, 0x000B3A5C);
        ic_ring(&c, 12, 12, 10, 8, 0x0046D3C6);
        ic_ring(&c, 12, 12, 6, 5, 0x0060CDFF);
        ic_circle(&c, 12, 12, 3, 0x00FFFFFF);
        break;

    case ICON_SHIELD: {
        ic_rr(&c, 4, 2, 16, 12, 2, 0x003FB950);
        /* tapering lower half */
        int rows = ic_len(&c, 9);
        int top = y + ic_s(&c, 13);
        int half = ic_len(&c, 8);
        int cx = x + ic_s(&c, 12);
        for (int r = 0; r < rows; r++) {
            int hw = half - (r * half) / rows;
            fb_fill_rect(cx - hw, top + r, hw * 2, 1, 0x003FB950);
        }
        ic_line(&c, 8, 11, 11, 14, 0x00FFFFFF);
        ic_line(&c, 11, 14, 16, 8, 0x00FFFFFF);
        break;
    }

    case ICON_GEAR:
        ic_rect(&c, 10, 1, 4, 22, 0x009AA7B0);
        ic_rect(&c, 1, 10, 22, 4, 0x009AA7B0);
        ic_rr(&c, 4, 4, 5, 5, 1, 0x009AA7B0);
        ic_rr(&c, 15, 4, 5, 5, 1, 0x009AA7B0);
        ic_rr(&c, 4, 15, 5, 5, 1, 0x009AA7B0);
        ic_rr(&c, 15, 15, 5, 5, 1, 0x009AA7B0);
        ic_circle(&c, 12, 12, 8, 0x009AA7B0);
        ic_circle(&c, 12, 12, 4, 0x00323C44);
        break;

    case ICON_CALC:
        ic_rr(&c, 4, 2, 16, 20, 2, 0x00344654);
        ic_rr(&c, 6, 4, 12, 5, 1, 0x009BE7C4);
        for (int r = 0; r < 3; r++)
            for (int k = 0; k < 3; k++)
                ic_rect(&c, 6 + k * 4 + (k == 2 ? 1 : 0), 11 + r * 4, 3, 3,
                        (k == 2) ? 0x00F2A33A : 0x00E8EEF2);
        break;

    case ICON_PC:
        ic_rr(&c, 2, 3, 20, 14, 2, 0x003A7BD5);
        ic_rect(&c, 4, 5, 16, 10, 0x009CD0FF);
        ic_rect(&c, 10, 17, 4, 3, 0x009AA7B0);
        ic_rr(&c, 7, 20, 10, 2, 1, 0x009AA7B0);
        break;

    case ICON_TRASH:
        ic_rect(&c, 9, 2, 6, 2, 0x008896A0);
        ic_rr(&c, 4, 4, 16, 3, 1, 0x00B8C4CC);
        ic_rr(&c, 6, 8, 12, 14, 2, 0x00A8B4BD);
        ic_rect(&c, 9, 11, 1, 8, 0x00647078);
        ic_rect(&c, 12, 11, 1, 8, 0x00647078);
        ic_rect(&c, 15, 11, 1, 8, 0x00647078);
        break;

    case ICON_CHART:
        ic_rr(&c, 2, 2, 20, 20, 3, 0x00243442);
        ic_rect(&c, 5, 13, 3, 6, 0x0060CDFF);
        ic_rect(&c, 10, 9, 3, 10, 0x0046D3C6);
        ic_rect(&c, 15, 5, 3, 14, 0x006CCB5F);
        break;

    case ICON_START:
        ic_rr(&c, 2, 2, 9, 9, 1, T(ACCENT));
        ic_rr(&c, 13, 2, 9, 9, 1, T(ACCENT));
        ic_rr(&c, 2, 13, 9, 9, 1, T(ACCENT));
        ic_rr(&c, 13, 13, 9, 9, 1, T(ACCENT));
        break;

    default:
        ic_rr(&c, 3, 3, 18, 18, 3, T(HOVER));
        break;
    }
}

/* ---- Single-colour glyphs ------------------------------------------------ */
void glyph_draw(glyph_id_t glyph, int x, int y, int size, uint32_t fg, uint32_t bg)
{
    ic_t c = { x, y, size };

    switch (glyph) {
    case GLYPH_SEARCH:
        ic_ring(&c, 10, 10, 7, 5, fg);
        ic_line(&c, 15, 15, 21, 21, fg);
        break;

    case GLYPH_POWER:
        ic_ring(&c, 12, 13, 9, 7, fg);
        ic_rect(&c, 9, 2, 6, 8, bg);
        ic_rect(&c, 11, 2, 2, 10, fg);
        break;

    case GLYPH_LOCK:
        ic_ring(&c, 12, 9, 6, 4, fg);
        ic_rr(&c, 5, 10, 14, 11, 2, fg);
        ic_rect(&c, 11, 14, 2, 4, bg);
        break;

    case GLYPH_RESTART:
        ic_ring(&c, 12, 12, 9, 7, fg);
        ic_rect(&c, 12, 1, 10, 9, bg);
        ic_line(&c, 13, 2, 19, 5, fg);
        ic_line(&c, 19, 5, 15, 10, fg);
        break;

    case GLYPH_BELL:
        ic_rr(&c, 6, 4, 12, 14, 5, fg);
        ic_rect(&c, 4, 16, 16, 2, fg);
        ic_rr(&c, 10, 19, 4, 3, 1, fg);
        break;

    case GLYPH_NET:
        ic_rect(&c, 3, 16, 3, 5, fg);
        ic_rect(&c, 8, 12, 3, 9, fg);
        ic_rect(&c, 13, 8, 3, 13, fg);
        ic_rect(&c, 18, 3, 3, 18, fg);
        break;

    case GLYPH_VOLUME:
        ic_rect(&c, 3, 9, 5, 6, fg);
        ic_rect(&c, 8, 6, 2, 12, fg);
        ic_rect(&c, 10, 4, 2, 16, fg);
        ic_rect(&c, 15, 9, 2, 6, fg);
        ic_rect(&c, 19, 6, 2, 12, fg);
        break;

    case GLYPH_MIC:
        ic_rr(&c, 9, 2, 6, 12, 3, fg);
        ic_ring(&c, 12, 11, 7, 6, fg);
        ic_rect(&c, 4, 3, 16, 8, bg);
        ic_rr(&c, 9, 2, 6, 12, 3, fg);
        ic_rect(&c, 11, 17, 2, 4, fg);
        ic_rect(&c, 8, 21, 8, 1, fg);
        break;

    case GLYPH_USER:
        ic_circle(&c, 12, 8, 5, fg);
        ic_rr(&c, 3, 14, 18, 10, 6, fg);
        break;

    case GLYPH_BACK:
        ic_line(&c, 4, 12, 20, 12, fg);
        ic_line(&c, 4, 12, 10, 6, fg);
        ic_line(&c, 4, 12, 10, 18, fg);
        break;

    case GLYPH_UP:
        ic_line(&c, 12, 4, 12, 20, fg);
        ic_line(&c, 12, 4, 6, 10, fg);
        ic_line(&c, 12, 4, 18, 10, fg);
        break;

    case GLYPH_REFRESH:
        ic_ring(&c, 12, 12, 8, 6, fg);
        ic_rect(&c, 12, 2, 9, 8, bg);
        ic_line(&c, 13, 3, 18, 6, fg);
        ic_line(&c, 18, 6, 14, 10, fg);
        break;
    }
}

/* ---- Widgets ------------------------------------------------------------- */
int ui_hit(int px, int py, int x, int y, int w, int h)
{
    return px >= x && px < x + w && py >= y && py < y + h;
}

void ui_text_center(int x, int y, int w, const char *text, uint32_t fg)
{
    fb_draw_text(x + (w - fb_text_width(text)) / 2, y, text, fg);
}

void ui_text_fit(int x, int y, int max_w, const char *text, uint32_t fg)
{
    char buf[160];
    int max_chars = max_w / 8;
    int len = (int)strlen(text);

    if (max_chars <= 0)
        return;
    if (len <= max_chars) {
        fb_draw_text(x, y, text, fg);
        return;
    }
    if (max_chars > (int)sizeof(buf) - 1)
        max_chars = (int)sizeof(buf) - 1;

    memcpy(buf, text, (size_t)max_chars);
    if (max_chars >= 3) {
        buf[max_chars - 2] = '.';
        buf[max_chars - 1] = '.';
    }
    buf[max_chars] = 0;
    fb_draw_text(x, y, buf, fg);
}

void ui_button(int x, int y, int w, int h, const char *label,
               uint32_t bg, uint32_t fg)
{
    fb_fill_rounded_rect(x, y, w, h, 5, bg);
    ui_text_center(x, y + (h - 16) / 2, w, label, fg);
}

void ui_card(int x, int y, int w, int h)
{
    fb_fill_rounded_rect(x, y, w, h, 6, T(BORDER));
    fb_fill_rounded_rect(x + 1, y + 1, w - 2, h - 2, 5, T(SURFACE));
}

void ui_progress(int x, int y, int w, int h, int percent,
                 uint32_t track, uint32_t fill)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    int filled = (w * percent) / 100;

    fb_fill_rounded_rect(x, y, w, h, h / 2, track);
    if (filled >= h)
        fb_fill_rounded_rect(x, y, filled, h, h / 2, fill);
    else if (filled > 0)
        fb_fill_rounded_rect(x, y, h, h, h / 2, fill);
}
