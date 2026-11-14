/*
 * JARVIS OS — anti-aliased font data (drivers/font_aa.h)
 *
 * Glyphs are pre-rendered by tools/gen_fonts.py into 8-bit coverage maps.
 * Characters 32..126 are stored first, then the extra symbols listed in
 * FONT_EXTRA_* (reachable through the byte codes 0x80 + n).
 */
#pragma once
#include <stdint.h>

typedef enum {
    FONT_UI = 0,        /* 13 px regular: almost all UI text            */
    FONT_UI_BOLD,       /* 13 px semibold: labels, headings in lists    */
    FONT_MONO,          /* 13 px monospace, 8 px advance: terminal etc. */
    FONT_MONO_BOLD,
    FONT_TITLE,         /* 22 px semibold: page titles (old scale 2)    */
    FONT_LARGE,         /* 32 px: big numbers (old scale 3)             */
    FONT_DISPLAY,       /* 58 px light: lock-screen clock (old scale 4) */
    FONT_SMALL,         /* 11 px: captions                               */
    FONT_COUNT
} font_id_t;

#define FONT_FIRST   32
#define FONT_ASCII   95                 /* 32..126 */
#define FONT_GLYPHS  (FONT_ASCII + 10)

/* Extra symbols: put "\x80" etc. in a string to draw them. */
#define SYM_BULLET   "\x80"
#define SYM_ELLIPSIS "\x81"
#define SYM_DEGREE   "\x82"
#define SYM_CHECK    "\x83"
#define SYM_LEFT     "\x84"
#define SYM_RIGHT    "\x85"
#define SYM_UP       "\x86"
#define SYM_DOWN     "\x87"
#define SYM_TIMES    "\x88"
#define SYM_COPY     "\x89"

typedef struct {
    uint8_t  w, h;          /* coverage map size                          */
    int8_t   bx, by;        /* offset from pen: left bearing, top - baseline */
    uint16_t adv;           /* advance in 1/64 px                         */
    uint32_t off;           /* start in the face's alpha array            */
} font_glyph_t;

typedef struct {
    uint8_t size, cell, baseline;   /* baseline: rows below the text top */
    const font_glyph_t *glyphs;
    const uint8_t *alpha;
} font_face_t;

extern const font_face_t g_font_faces[FONT_COUNT];
