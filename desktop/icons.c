/*
 * =============================================================================
 * Kryzen OS — Icons and small widgets (desktop/icons.c)
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
/* The Kryzen K: an outlined letter in the cyan-to-violet gradient, the same
 * shape as the boot-menu logo (scripts/kryzen_art.py). */
void icon_kryzen_logo(int x, int y, int size)
{
    static const int8_t pts[12][2] = {
        { 12, 6 }, { 34, 6 }, { 34, 40 }, { 66, 6 }, { 93, 6 }, { 53, 49 },
        { 95, 94 }, { 68, 94 }, { 40, 62 }, { 34, 69 }, { 34, 94 }, { 12, 94 },
    };
    int w16 = size >= 64 ? size * 16 / 22 : size >= 28 ? 32 : 24;
    uint32_t top = UI_NEON_CYAN, bot = UI_NEON_VIOLET;
    if (theme_get_mode() == THEME_HIGH_CONTRAST) top = bot = 0x00FFFF00;
    for (int pass = 0; pass < 2; pass++) {
        /* pass 0: the outline; pass 1: the inner echo line (large sizes) */
        if (pass == 1 && size < 48) break;
        for (int i = 0; i < 12; i++) {
            int j = (i + 1) % 12;
            int ax = pts[i][0], ay = pts[i][1], bx = pts[j][0], by = pts[j][1];
            if (pass == 1) {            /* pull the points towards the centre */
                ax = 52 + (ax - 52) * 78 / 100; ay = 50 + (ay - 50) * 82 / 100;
                bx = 52 + (bx - 52) * 78 / 100; by = 50 + (by - 50) * 82 / 100;
            }
            fb_stroke16(x * 16 + ax * size * 16 / 100, y * 16 + ay * size * 16 / 100,
                        x * 16 + bx * size * 16 / 100, y * 16 + by * size * 16 / 100,
                        pass ? w16 / 2 : w16, top, bot, y, y + size, pass ? 110 : 255);
        }
    }
}

/* The assistant orb: glowing rings around a bright core. level (0..100)
 * widens the glow while JARVIS is listening or speaking. */
void ui_orb(int cx, int cy, int r, int level)
{
    if (r < 4) r = 4;
    int glow = r / 3 + level * r / 260;
    fb_glow_circle(cx, cy, r, glow, UI_NEON_CYAN, 70);
    fb_fill_circle(cx, cy, r, 0x00081A2E);
    fb_draw_arc(cx, cy, r, r >= 20 ? 3 : 2, 0, 360, UI_NEON_CYAN, 255);
    if (r >= 14) {
        fb_draw_arc(cx, cy, r * 76 / 100, r >= 24 ? 2 : 1, 200, 250, 0x0049C8F0, 220);
        fb_draw_arc(cx, cy, r * 76 / 100, r >= 24 ? 2 : 1, 100, 80, UI_NEON_VIOLET, 220);
    }
    fb_glow_circle(cx, cy, r * 30 / 100, r * 30 / 100, 0x0040D8FF, 235);
    fb_fill_circle(cx, cy, r * 16 / 100 > 1 ? r * 16 / 100 : 2, 0x00E8FCFF);
}

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
        ui_orb(x + size / 2, y + size / 2, size / 2, 0);
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

    case ICON_DRIVE:
        ic_rr(&c, 2, 8, 20, 10, 2, 0x007A8892);
        ic_rr(&c, 3, 9, 18, 6, 1, 0x00B8C4CC);
        ic_rect(&c, 5, 16, 14, 1, 0x00505A60);
        ic_circle(&c, 18, 12, 1, 0x0040D080);
        break;

    case ICON_USB:
        ic_rr(&c, 6, 2, 12, 7, 1, 0x009AA7B0);
        ic_rect(&c, 8, 4, 2, 2, 0x00404850);
        ic_rect(&c, 14, 4, 2, 2, 0x00404850);
        ic_rr(&c, 4, 9, 16, 13, 3, 0x002F6FD0);
        ic_rect(&c, 9, 13, 6, 1, 0x00FFFFFF);
        ic_rect(&c, 11, 11, 2, 5, 0x00FFFFFF);
        break;

    case ICON_PACKAGE:
        ic_rr(&c, 3, 6, 18, 15, 2, 0x00C8904A);
        ic_rect(&c, 3, 6, 18, 4, 0x00A8743A);
        ic_rect(&c, 10, 6, 4, 15, 0x00E8C890);
        break;

    case ICON_PROGRAM:
        ic_rr(&c, 2, 3, 20, 18, 3, 0x003A7BD5);
        ic_rect(&c, 2, 3, 20, 4, 0x002A5BA5);
        ic_rr(&c, 6, 10, 12, 8, 1, 0x00E8F2FF);
        break;

    case ICON_STORE:
        ic_rr(&c, 3, 7, 18, 15, 3, 0x002F6FD0);
        ic_ring(&c, 12, 7, 5, 3, 0x002F6FD0);
        ic_rect(&c, 7, 11, 4, 4, 0x00F25022);
        ic_rect(&c, 13, 11, 4, 4, 0x007FBA00);
        ic_rect(&c, 7, 16, 4, 4, 0x0000A4EF);
        ic_rect(&c, 13, 16, 4, 4, 0x00FFB900);
        break;

    case ICON_DISKS:
        ic_rr(&c, 2, 9, 20, 11, 2, 0x007A8892);
        ic_rr(&c, 3, 10, 18, 7, 1, 0x00B8C4CC);
        ic_circle(&c, 18, 14, 1, 0x0040D080);
        ic_circle(&c, 9, 7, 6, 0x003A7BD5);
        ic_rect(&c, 9, 1, 6, 6, 0x0060CDFF);
        break;

    case ICON_DEVICES:
        ic_rr(&c, 5, 5, 14, 14, 2, 0x00384858);
        ic_rr(&c, 8, 8, 8, 8, 1, 0x0046D3C6);
        for (int k = 0; k < 3; k++) {
            ic_rect(&c, 7 + k * 4, 2, 2, 3, 0x009AA7B0);
            ic_rect(&c, 7 + k * 4, 19, 2, 3, 0x009AA7B0);
            ic_rect(&c, 2, 7 + k * 4, 3, 2, 0x009AA7B0);
            ic_rect(&c, 19, 7 + k * 4, 3, 2, 0x009AA7B0);
        }
        break;

    case ICON_PHOTOS:
        ic_rr(&c, 2, 4, 20, 16, 3, 0x003A7BD5);
        ic_rr(&c, 4, 6, 16, 12, 2, 0x0090D0FF);
        ic_circle(&c, 15, 9, 2, 0x00FFE070);
        ic_line(&c, 4, 17, 10, 10, 0x002E8B57);
        ic_line(&c, 10, 10, 14, 15, 0x002E8B57);
        ic_line(&c, 13, 14, 16, 12, 0x002E8B57);
        ic_line(&c, 16, 12, 20, 17, 0x002E8B57);
        ic_rect(&c, 4, 16, 16, 2, 0x002E8B57);
        break;

    case ICON_REGEDIT:                                  /* blue cubes, like regedit */
        ic_rr(&c, 3, 11, 9, 9, 1, 0x002F6FD0);
        ic_rr(&c, 12, 11, 9, 9, 1, 0x003A8EE8);
        ic_rr(&c, 7, 2, 9, 9, 1, 0x0060B0FF);
        ic_rect(&c, 5, 13, 5, 2, 0x00A0D0FF);
        ic_rect(&c, 14, 13, 5, 2, 0x00A0D0FF);
        ic_rect(&c, 9, 4, 5, 2, 0x00E0F0FF);
        break;

    case ICON_BROWSER:
        ic_circle(&c, 12, 12, 10, 0x001A73E8);
        ic_circle(&c, 12, 12, 8, 0x0034A0F0);
        ic_ring(&c, 12, 12, 8, 7, 0x00D0E8FF);
        ic_rect(&c, 4, 11, 16, 1, 0x00D0E8FF);
        ic_rect(&c, 11, 4, 2, 16, 0x00D0E8FF);
        ic_ring(&c, 12, 12, 5, 4, 0x00D0E8FF);
        break;

    case ICON_CLOCK:
        ic_circle(&c, 12, 12, 10, 0x003A7BD5);
        ic_circle(&c, 12, 12, 8, 0x00F4F7FA);
        ic_rect(&c, 11, 6, 2, 7, 0x00243442);
        ic_line(&c, 12, 12, 16, 15, 0x00243442);
        ic_circle(&c, 12, 12, 1, 0x00E04040);
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
        icon_kryzen_logo(x, y, size);
        break;
    case ICON_NEURAL:
        ic_line(&c, 9, 8, 15, 8, 0x008083FF);
        ic_line(&c, 9, 8, 7, 13, 0x008083FF);
        ic_line(&c, 15, 8, 17, 13, 0x008083FF);
        ic_line(&c, 7, 13, 12, 17, 0x004CD7F6);
        ic_line(&c, 17, 13, 12, 17, 0x004CD7F6);
        ic_line(&c, 9, 8, 12, 12, 0x00B76DFF);
        ic_line(&c, 15, 8, 12, 12, 0x00B76DFF);
        ic_line(&c, 12, 12, 12, 17, 0x004CD7F6);
        ic_circle(&c, 9, 8, 4, 0x00B76DFF);
        ic_circle(&c, 15, 8, 4, 0x00DDB7FF);
        ic_circle(&c, 7, 13, 3, 0x008083FF);
        ic_circle(&c, 17, 13, 3, 0x004CD7F6);
        ic_circle(&c, 12, 17, 3, 0x00C0C1FF);
        ic_circle(&c, 12, 12, 3, 0x00FFFFFF);
        break;

    case ICON_COPILOT:
        ic_line(&c, 12, 3, 12, 20, 0x00C0C1FF);
        ic_line(&c, 3, 11, 20, 11, 0x00C0C1FF);
        ic_rr(&c, 9, 8, 7, 7, 3, 0x004CD7F6);
        ic_circle(&c, 12, 11, 2, 0x00FFFFFF);
        ic_line(&c, 18, 3, 18, 8, 0x00DDB7FF);
        ic_line(&c, 15, 6, 21, 6, 0x00DDB7FF);
        ic_circle(&c, 18, 6, 1, 0x00FFFFFF);
        ic_circle(&c, 6, 17, 2, 0x008083FF);
        break;

    case ICON_CODE:
        ic_rr(&c, 2, 3, 20, 18, 3, 0x001C2026);
        ic_rr(&c, 3, 4, 18, 16, 2, 0x0010141A);
        ic_line(&c, 8, 8, 5, 12, 0x004CD7F6);
        ic_line(&c, 5, 12, 8, 16, 0x004CD7F6);
        ic_line(&c, 13, 7, 11, 17, 0x008083FF);
        ic_line(&c, 16, 8, 19, 12, 0x00C0C1FF);
        ic_line(&c, 19, 12, 16, 16, 0x00C0C1FF);
        break;

    default:
        ic_rr(&c, 3, 3, 18, 18, 3, T(HOVER));
        break;
    }
}

/* ---- Single-colour glyphs ------------------------------------------------ */
/* Stroke on the 24-unit grid; coordinates in grid units x 2 (so 0.5 steps). */
static void gl_line(const ic_t *c, int ax2, int ay2, int bx2, int by2, uint32_t col)
{
    int w16 = c->size >= 28 ? 34 : c->size >= 20 ? 28 : 22;
    fb_stroke16(c->x * 16 + ax2 * c->size * 16 / 48, c->y * 16 + ay2 * c->size * 16 / 48,
                c->x * 16 + bx2 * c->size * 16 / 48, c->y * 16 + by2 * c->size * 16 / 48,
                w16, col, col, 0, 0, 255);
}

static void gl_arc(const ic_t *c, int gx2, int gy2, int gr2, int start, int sweep, uint32_t col)
{
    int th = c->size >= 28 ? 2 : c->size >= 20 ? 2 : 1;
    fb_draw_arc(c->x + gx2 * c->size / 48, c->y + gy2 * c->size / 48, gr2 * c->size / 48 + th / 2, th, start, sweep, col, 255);
}

void glyph_draw(glyph_id_t glyph, int x, int y, int size, uint32_t fg, uint32_t bg)
{
    ic_t c = { x, y, size };

    switch (glyph) {
    case GLYPH_MENU:
        gl_line(&c, 8, 14, 40, 14, fg); gl_line(&c, 8, 24, 40, 24, fg); gl_line(&c, 8, 34, 40, 34, fg);
        return;
    case GLYPH_COG:
        for (int a = 0; a < 360; a += 45) {
            int cx = 24 + fb_cos1024(a) * 17 / 1024, cy = 24 + fb_sin1024(a) * 17 / 1024;
            int dx = 24 + fb_cos1024(a) * 13 / 1024, dy = 24 + fb_sin1024(a) * 13 / 1024;
            gl_line(&c, dx, dy, cx, cy, fg);
        }
        gl_arc(&c, 24, 24, 13, 0, 360, fg);
        gl_arc(&c, 24, 24, 5, 0, 360, fg);
        return;
    case GLYPH_ACCESS:
        fb_fill_circle(x + size / 2, y + size * 7 / 48, size * 5 / 48 > 1 ? size * 5 / 48 : 2, fg);
        gl_line(&c, 8, 17, 40, 17, fg);
        gl_line(&c, 24, 17, 24, 30, fg);
        gl_line(&c, 24, 30, 16, 44, fg); gl_line(&c, 24, 30, 32, 44, fg);
        return;
    case GLYPH_CHECK:
        gl_line(&c, 10, 25, 20, 35, fg); gl_line(&c, 20, 35, 38, 13, fg);
        return;
    case GLYPH_ARROW_RIGHT:
        gl_line(&c, 6, 24, 42, 24, fg); gl_line(&c, 30, 12, 42, 24, fg); gl_line(&c, 30, 36, 42, 24, fg);
        return;
    case GLYPH_CHEVRON_RIGHT:
        gl_line(&c, 18, 10, 32, 24, fg); gl_line(&c, 32, 24, 18, 38, fg);
        return;
    case GLYPH_CHEVRON_LEFT:
        gl_line(&c, 30, 10, 16, 24, fg); gl_line(&c, 16, 24, 30, 38, fg);
        return;
    case GLYPH_CHEVRON_UP:
        gl_line(&c, 10, 30, 24, 16, fg); gl_line(&c, 24, 16, 38, 30, fg);
        return;
    case GLYPH_WARNING:
        gl_line(&c, 24, 6, 44, 40, fg); gl_line(&c, 44, 40, 4, 40, fg); gl_line(&c, 4, 40, 24, 6, fg);
        gl_line(&c, 24, 18, 24, 28, fg);
        fb_fill_circle(x + size / 2, y + size * 34 / 48, size >= 24 ? 2 : 1, fg);
        return;
    case GLYPH_HISTORY:
        gl_arc(&c, 26, 24, 16, 200, 320, fg);
        gl_line(&c, 26, 14, 26, 25, fg); gl_line(&c, 26, 25, 33, 30, fg);
        gl_line(&c, 4, 18, 11, 27, fg); gl_line(&c, 11, 27, 19, 20, fg);
        return;
    case GLYPH_DISPLAY:
        gl_line(&c, 6, 9, 42, 9, fg); gl_line(&c, 42, 9, 42, 32, fg); gl_line(&c, 42, 32, 6, 32, fg); gl_line(&c, 6, 32, 6, 9, fg);
        gl_line(&c, 24, 32, 24, 40, fg); gl_line(&c, 15, 41, 33, 41, fg);
        return;
    case GLYPH_SQUARE:
        fb_neon_rounded_rect(x + size / 8, y + size / 8, size * 3 / 4, size * 3 / 4, size / 6, size >= 24 ? 2 : 1, 0, fg, fg, 255);
        return;
    case GLYPH_PADLOCK:
        fb_neon_rounded_rect(x + size * 9 / 48, y + size * 21 / 48, size * 30 / 48, size * 23 / 48, size / 12, size >= 24 ? 2 : 1, 0, fg, fg, 255);
        gl_arc(&c, 24, 18, 8, 180, 180, fg);
        gl_line(&c, 16, 18, 16, 22, fg); gl_line(&c, 32, 18, 32, 22, fg);
        fb_fill_circle(x + size / 2, y + size * 32 / 48, size >= 24 ? 2 : 1, fg);
        return;
    case GLYPH_PROFILE:
        gl_arc(&c, 24, 24, 19, 0, 360, fg);
        fb_fill_circle(x + size / 2, y + size * 19 / 48, size * 6 / 48 > 1 ? size * 6 / 48 : 2, fg);
        gl_arc(&c, 24, 40, 11, 200, 140, fg);
        return;
    case GLYPH_FOLDER_LINE:
        gl_line(&c, 5, 12, 18, 12, fg); gl_line(&c, 18, 12, 23, 17, fg); gl_line(&c, 23, 17, 43, 17, fg);
        gl_line(&c, 43, 17, 43, 39, fg); gl_line(&c, 43, 39, 5, 39, fg); gl_line(&c, 5, 39, 5, 12, fg);
        return;
    case GLYPH_CLOSE:
        gl_line(&c, 12, 12, 36, 36, fg); gl_line(&c, 36, 12, 12, 36, fg);
        return;
    case GLYPH_KEYBOARD:
        gl_line(&c, 4, 13, 44, 13, fg); gl_line(&c, 44, 13, 44, 36, fg); gl_line(&c, 44, 36, 4, 36, fg); gl_line(&c, 4, 36, 4, 13, fg);
        gl_line(&c, 12, 21, 14, 21, fg); gl_line(&c, 22, 21, 26, 21, fg); gl_line(&c, 34, 21, 36, 21, fg);
        gl_line(&c, 14, 29, 34, 29, fg);
        return;
    case GLYPH_DISK:
        gl_line(&c, 6, 28, 42, 28, fg); gl_line(&c, 42, 28, 42, 40, fg); gl_line(&c, 42, 40, 6, 40, fg); gl_line(&c, 6, 40, 6, 28, fg);
        gl_line(&c, 6, 28, 12, 10, fg); gl_line(&c, 12, 10, 36, 10, fg); gl_line(&c, 36, 10, 42, 28, fg);
        fb_fill_circle(x + size * 35 / 48, y + size * 34 / 48, size >= 24 ? 2 : 1, fg);
        return;
    default: break;
    }

    switch (glyph) {
    case GLYPH_SUN:
        ic_circle(&c, 12, 12, 5, fg);
        for (int k = 0; k < 8; k++) {
            static const int dx[8] = { 0, 6, 8, 6, 0, -6, -8, -6 }, dy[8] = { -8, -6, 0, 6, 8, 6, 0, -6 };
            ic_line(&c, 12 + dx[k] * 9 / 8, 12 + dy[k] * 9 / 8, 12 + dx[k] * 11 / 8, 12 + dy[k] * 11 / 8, fg);
        }
        break;

    case GLYPH_MOON:
        ic_circle(&c, 11, 12, 9, fg);
        ic_circle(&c, 16, 8, 8, bg);
        break;

    case GLYPH_PLANE:
        ic_line(&c, 12, 2, 12, 20, fg);
        ic_line(&c, 11, 2, 11, 20, fg);
        ic_line(&c, 3, 13, 12, 8, fg);
        ic_line(&c, 21, 13, 12, 8, fg);
        ic_line(&c, 7, 21, 12, 18, fg);
        ic_line(&c, 17, 21, 12, 18, fg);
        break;

    case GLYPH_CONTRAST:
        ic_ring(&c, 12, 12, 9, 7, fg);
        ic_rect(&c, 12, 4, 6, 16, fg);
        ic_circle(&c, 12, 12, 7, fg);
        ic_rect(&c, 5, 4, 7, 16, bg);
        ic_ring(&c, 12, 12, 9, 7, fg);
        break;

    case GLYPH_CAMERA:
        ic_rr(&c, 2, 7, 20, 13, 2, fg);
        ic_rect(&c, 8, 4, 8, 4, fg);
        ic_ring(&c, 12, 13, 5, 3, bg);
        break;

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

    case GLYPH_FORWARD:
        ic_line(&c, 4, 12, 20, 12, fg);
        ic_line(&c, 20, 12, 14, 6, fg);
        ic_line(&c, 20, 12, 14, 18, fg);
        break;

    case GLYPH_GRID:
        ic_rect(&c, 4, 4, 7, 7, fg);  ic_rect(&c, 13, 4, 7, 7, fg);
        ic_rect(&c, 4, 13, 7, 7, fg); ic_rect(&c, 13, 13, 7, 7, fg);
        break;

    case GLYPH_LIST:
        for (int r = 0; r < 4; r++) {
            ic_rect(&c, 4, 5 + r * 4, 3, 2, fg);
            ic_rect(&c, 9, 5 + r * 4, 11, 2, fg);
        }
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

    case GLYPH_BOLT:
        ic_line(&c, 14, 2, 8, 12, fg);
        ic_line(&c, 8, 12, 13, 12, fg);
        ic_line(&c, 11, 12, 9, 21, fg);
        ic_line(&c, 9, 21, 16, 10, fg);
        ic_line(&c, 16, 10, 11, 10, fg);
        ic_line(&c, 11, 10, 14, 2, fg);
        break;

    case GLYPH_BATTERY:
        ic_rr(&c, 3, 7, 16, 10, 2, fg);
        ic_rect(&c, 5, 9, 12, 6, bg);
        ic_rect(&c, 6, 10, 8, 4, fg);
        ic_rect(&c, 19, 10, 2, 4, fg);
        break;
    default: break;
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
    if (max_w <= 0 || !text) return;
    if (fb_text_width(text) <= max_w) { fb_draw_text(x, y, text, fg); return; }
    /* Longest prefix that still fits with an ellipsis after it. */
    int ell = fb_text_width(SYM_ELLIPSIS);
    int n = (int)strlen(text);
    while (n > 0 && fb_text_width_n(text, n) + ell > max_w) n--;
    int end = fb_text_n(x, y, text, n, fg, FONT_UI);
    fb_draw_text(end, y, SYM_ELLIPSIS, fg);
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

void ui_switch(int x, int y, int on, uint32_t accent_color)
{
    int w = 42, h = 22, r = 11;
    uint32_t track = on ? accent_color : T(HOVER);
    fb_fill_rounded_rect(x, y, w, h, r, track);
    fb_draw_rounded_rect(x, y, w, h, r, on ? accent_color : T(BORDER));
    int kx = on ? (x + w - r) : (x + r);
    int ky = y + r;
    fb_fill_circle(kx, ky, r - 3, on ? T(ACCENT_TEXT) : T(TEXT));
}

void ui_slider(int x, int y, int w, int h, int val, int min, int max, uint32_t track_col, uint32_t fill_col)
{
    if (max <= min) max = min + 1;
    if (val < min) val = min;
    if (val > max) val = max;
    int bar_h = 6;
    int bar_y = y + (h - bar_h) / 2;
    int knob_x = x + (val - min) * (w - 16) / (max - min) + 8;
    int filled = knob_x - x;

    fb_fill_rounded_rect(x, bar_y, w, bar_h, bar_h / 2, track_col);
    if (filled > 0)
        fb_fill_rounded_rect(x, bar_y, filled, bar_h, bar_h / 2, fill_col);
    fb_fill_circle(knob_x, y + h / 2 + 1, 8, 0x00000000);
    fb_fill_circle(knob_x, y + h / 2, 8, fill_col);
    fb_fill_circle(knob_x, y + h / 2, 4, 0x00FFFFFF);
}

void ui_badge(int x, int y, const char *text, uint32_t bg, uint32_t fg)
{
    if (!text) return;
    int tw = fb_text_width(text);
    int w = tw + 16, h = 20;
    fb_fill_rounded_rect(x, y, w, h, 10, bg);
    fb_draw_text(x + 8, y + 2, text, fg);
}

void ui_tooltip(int x, int y, const char *text)
{
    if (!text || !*text) return;
    int tw = fb_text_width(text);
    int w = tw + 20, h = 26;
    fb_blend_rounded_rect(x + 2, y + 3, w, h, 6, 0x00000000, 70);
    fb_fill_rounded_rect(x, y, w, h, 6, T(BORDER));
    fb_fill_rounded_rect(x + 1, y + 1, w - 2, h - 2, 5, T(MENU));
    fb_draw_text(x + 10, y + 5, text, T(TEXT));
}

void ui_pill_button(int x, int y, int w, int h, const char *label, int active, uint32_t bg, uint32_t fg)
{
    int r = h / 2;
    if (active) {
        fb_fill_rounded_rect(x, y, w, h, r, bg);
        ui_text_center(x, y + (h - 16) / 2, w, label, fg);
    } else {
        fb_fill_rounded_rect(x, y, w, h, r, T(SURFACE));
        fb_draw_rounded_rect(x, y, w, h, r, T(BORDER));
        ui_text_center(x, y + (h - 16) / 2, w, label, T(TEXT));
    }
}

void ui_sparkline(int x, int y, int w, int h, const uint32_t *values, int count, uint32_t line_col, uint32_t fill_col)
{
    if (!values || count < 2 || w <= 0 || h <= 0) return;
    fb_fill_rounded_rect(x, y, w, h, 4, T(SURFACE));
    fb_draw_rounded_rect(x, y, w, h, 4, T(BORDER));

    int inner_x = x + 2, inner_y = y + 2, inner_w = w - 4, inner_h = h - 4;
    int step = inner_w / (count - 1);
    if (step < 1) step = 1;

    for (int i = 0; i < count - 1; i++) {
        int v0 = values[i] > 100 ? 100 : (int)values[i];
        int v1 = values[i + 1] > 100 ? 100 : (int)values[i + 1];
        int px0 = inner_x + i * step;
        int py0 = inner_y + inner_h - (v0 * inner_h / 100);
        int px1 = inner_x + (i + 1) * step;
        int py1 = inner_y + inner_h - (v1 * inner_h / 100);

        for (int sx = px0; sx <= px1 && sx < inner_x + inner_w; sx++) {
            float t = (px1 == px0) ? 0.0f : (float)(sx - px0) / (float)(px1 - px0);
            int sy = (int)(py0 + t * (py1 - py0));
            if (inner_y + inner_h > sy)
                fb_blend_rect(sx, sy, 1, inner_y + inner_h - sy, fill_col, 50);
        }

        fb_draw_line(px0, py0, px1, py1, line_col);
        fb_draw_line(px0, py0 + 1, px1, py1 + 1, line_col);
    }
}
