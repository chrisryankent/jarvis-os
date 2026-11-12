/*
 * =============================================================================
 * JARVIS OS — Shared desktop UI definitions (desktop/ui.h)
 *
 * Theme palette, icon set, small widgets and system helpers used by the
 * window manager, the shell (taskbar / Start / notifications) and every app.
 * =============================================================================
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "../drivers/gpu.h"

/* ---- Theme --------------------------------------------------------------- */
typedef enum {
    THEME_DARK = 0,
    THEME_LIGHT = 1,
    THEME_HIGH_CONTRAST = 2,
} theme_mode_t;

typedef enum {
    TC_ACCENT = 0,      /* highlight colour (selection, focus, indicators)  */
    TC_ACCENT_TEXT,     /* text drawn on top of TC_ACCENT                    */
    TC_WINDOW,          /* window client background                          */
    TC_SURFACE,         /* cards, sidebars, list headers                     */
    TC_HOVER,           /* hovered / pressed control                         */
    TC_TEXT,
    TC_TEXT_DIM,
    TC_BORDER,
    TC_TITLE_ACTIVE,
    TC_TITLE_INACTIVE,
    TC_TITLE_TEXT,
    TC_TASKBAR,
    TC_MENU,            /* Start menu, context menu, flyouts                 */
    TC_WALL_TOP,
    TC_WALL_BOTTOM,
    TC_INPUT,           /* text field background                             */
    TC_OK,
    TC_WARN,
    TC_DANGER,
    TC_COUNT
} theme_color_t;

void         theme_init(void);
void         theme_set_mode(theme_mode_t mode);
theme_mode_t theme_get_mode(void);
const char  *theme_get_name(void);
uint32_t     theme_color(theme_color_t id);
#define T(id) theme_color(TC_##id)

/* ---- Icons --------------------------------------------------------------- */
typedef enum {
    ICON_NONE = 0,
    ICON_FOLDER,
    ICON_FILE,
    ICON_TERMINAL,
    ICON_JARVIS,        /* the assistant orb */
    ICON_SHIELD,
    ICON_GEAR,
    ICON_CALC,
    ICON_PC,
    ICON_TRASH,
    ICON_CHART,         /* system monitor */
    ICON_START,
} icon_id_t;

typedef enum {
    GLYPH_SEARCH = 0,
    GLYPH_POWER,
    GLYPH_LOCK,
    GLYPH_RESTART,
    GLYPH_BELL,
    GLYPH_NET,
    GLYPH_VOLUME,
    GLYPH_MIC,
    GLYPH_USER,
    GLYPH_BACK,
    GLYPH_UP,
    GLYPH_REFRESH,
} glyph_id_t;

/* Full-colour app icon in a size x size box. */
void icon_draw(icon_id_t icon, int x, int y, int size);
/* Single-colour UI glyph; bg is the colour it sits on (used for cut-outs). */
void glyph_draw(glyph_id_t glyph, int x, int y, int size, uint32_t fg, uint32_t bg);

/* ---- Widgets ------------------------------------------------------------- */
int  ui_hit(int px, int py, int x, int y, int w, int h);
void ui_button(int x, int y, int w, int h, const char *label,
               uint32_t bg, uint32_t fg);
void ui_card(int x, int y, int w, int h);
void ui_progress(int x, int y, int w, int h, int percent,
                 uint32_t track, uint32_t fill);
/* Draws at most max_w pixels of text, ending in ".." when truncated. */
void ui_text_fit(int x, int y, int max_w, const char *text, uint32_t fg);
void ui_text_center(int x, int y, int w, const char *text, uint32_t fg);

/* ---- System helpers ------------------------------------------------------ */
typedef struct {
    int year, month, day;       /* month 1-12, day 1-31 */
    int hour, minute, second;
    int weekday;                /* 0 = Sunday */
} sys_time_t;

void sys_get_time(sys_time_t *t);                   /* CMOS real-time clock  */
void sys_format_clock(char *buf, size_t n);         /* "14:05"               */
void sys_format_date(char *buf, size_t n);          /* "Thu 12 Nov 2026"     */
void sys_format_uptime(char *buf, size_t n);        /* "1h 04m 09s"          */
void sys_poweroff(void);
void sys_reboot(void);
