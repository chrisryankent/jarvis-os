#!/usr/bin/env python3
"""
Generates kernel/font8x16.c — a complete, original 8x16 monospace bitmap
font covering printable ASCII 32-126, designed from scratch using simple
stroke primitives (not copied from any existing font/ROM).

Replaces the previous `const uint8_t font8x16[256][16] = {{0}};` placeholder
in kernel/platform.c, which meant fb_put_char() in drivers/gpu.c always fell
through to its ~50-character 5x7 fallback glyph set — every character on
every screen in the OS was being drawn by the low-resolution emergency path,
never the real font.

Grid: 8 columns (bit 7 = leftmost, matching fb_put_char's `0x80 >> col`
read order) x 16 rows. Body occupies rows 2-12 (11px tall) for most glyphs;
descenders (g, j, p, q, y, comma, semicolon) extend to row 14. This mirrors
the classic terminal-font proportions (a few rows of padding above/below,
descenders extending below the baseline) without reproducing any specific
existing font's bitmap data.
"""

W, H = 8, 16

def blank():
    return [[0]*W for _ in range(H)]

def px(g, x, y):
    if 0 <= x < W and 0 <= y < H:
        g[y][x] = 1

def hline(g, x0, x1, y):
    for x in range(min(x0,x1), max(x0,x1)+1):
        px(g, x, y)

def vline(g, y0, y1, x):
    for y in range(min(y0,y1), max(y0,y1)+1):
        px(g, x, y)

def line(g, x0, y0, x1, y1):
    """Simple stepping line (Bresenham-lite) for diagonals."""
    dx = x1 - x0
    dy = y1 - y0
    steps = max(abs(dx), abs(dy))
    if steps == 0:
        px(g, x0, y0)
        return
    for i in range(steps+1):
        t = i / steps
        x = round(x0 + dx*t)
        y = round(y0 + dy*t)
        px(g, x, y)

def rect(g, x0, y0, x1, y1):
    hline(g, x0, x1, y0)
    hline(g, x0, x1, y1)
    vline(g, y0, y1, x0)
    vline(g, y0, y1, x1)

def to_bytes(g):
    out = []
    for row in g:
        b = 0
        for col in range(W):
            if row[col]:
                b |= (0x80 >> col)
        out.append(b)
    return out

glyphs = {}

# ---- Digits (rows 2-12, 11px tall body) ------------------------------------
def digit_0():
    g = blank(); rect(g,2,2,5,12)
    line(g,2,12,5,2)  # diagonal slash through zero
    return g
def digit_1():
    g = blank(); vline(g,2,12,4); line(g,2,4,4,2); hline(g,2,6,12); return g
def digit_2():
    g = blank()
    hline(g,2,5,2); px(g,6,3); px(g,6,4); line(g,6,5,2,9); hline(g,2,6,12)
    return g
def digit_3():
    g = blank()
    hline(g,2,5,2); px(g,6,3); px(g,6,4); hline(g,3,6,7)
    px(g,6,8); px(g,6,9); px(g,6,10); hline(g,2,5,12); vline(g,11,11,6)
    return g
def digit_4():
    g = blank(); vline(g,2,7,5); line(g,5,2,2,8); hline(g,2,6,8); vline(g,2,12,5)
    return g
def digit_5():
    g = blank()
    hline(g,2,6,2); vline(g,2,7,2); hline(g,2,6,7)
    px(g,6,8); px(g,6,9); px(g,6,10); hline(g,2,5,12); vline(g,11,11,6)
    return g
def digit_6():
    g = blank()
    line(g,5,2,2,6); vline(g,6,12,2); hline(g,2,5,7)
    vline(g,8,11,5); hline(g,2,5,12); vline(g,8,11,2)
    return g
def digit_7():
    g = blank(); hline(g,2,6,2); line(g,6,3,3,12); return g
def digit_8():
    g = blank()
    rect(g,2,2,5,7); rect(g,2,7,5,12)
    return g
def digit_9():
    g = blank()
    rect(g,2,2,5,7); vline(g,2,10,5); line(g,5,10,2,12); hline(g,2,5,12)
    return g
glyphs['0']=digit_0(); glyphs['1']=digit_1(); glyphs['2']=digit_2()
glyphs['3']=digit_3(); glyphs['4']=digit_4(); glyphs['5']=digit_5()
glyphs['6']=digit_6(); glyphs['7']=digit_7(); glyphs['8']=digit_8()
glyphs['9']=digit_9()

# ---- Uppercase letters ------------------------------------------------------
def L_A():
    g=blank(); line(g,2,12,4,2); line(g,4,2,6,12); hline(g,3,5,8); return g
def L_B():
    g=blank(); vline(g,2,12,2); hline(g,2,5,2); vline(g,3,6,6); hline(g,2,5,7)
    vline(g,8,11,6); hline(g,2,5,12); return g
def L_C():
    g=blank(); hline(g,3,6,2); vline(g,3,11,2); hline(g,3,6,12); line(g,3,2,2,3); line(g,2,11,3,12); return g
def L_D():
    g=blank(); vline(g,2,12,2); hline(g,2,4,2); vline(g,3,11,6); hline(g,2,4,12); line(g,4,2,6,4); line(g,6,10,4,12); vline(g,4,10,6); return g
def L_E():
    g=blank(); vline(g,2,12,2); hline(g,2,6,2); hline(g,2,5,7); hline(g,2,6,12); return g
def L_F():
    g=blank(); vline(g,2,12,2); hline(g,2,6,2); hline(g,2,5,7); return g
def L_G():
    g=blank(); hline(g,3,6,2); vline(g,3,11,2); hline(g,3,6,12); line(g,3,2,2,3); line(g,2,11,3,12)
    hline(g,4,6,7); vline(g,7,11,6); return g
def L_H():
    g=blank(); vline(g,2,12,2); vline(g,2,12,6); hline(g,2,6,7); return g
def L_I():
    g=blank(); hline(g,2,6,2); vline(g,2,12,4); hline(g,2,6,12); return g
def L_J():
    g=blank(); hline(g,3,6,2); vline(g,2,11,6); hline(g,2,5,12); line(g,2,12,3,11)
    return g
def L_K():
    g=blank(); vline(g,2,12,2); line(g,7,2,3,7); line(g,3,7,7,12); return g
def L_L():
    g=blank(); vline(g,2,12,2); hline(g,2,6,12); return g
def L_M():
    g=blank(); vline(g,2,12,2); vline(g,2,12,6); line(g,2,2,4,6); line(g,4,6,6,2); return g
def L_N():
    g=blank(); vline(g,2,12,2); vline(g,2,12,6); line(g,2,2,6,12); return g
def L_O():
    g=blank(); rect(g,2,2,6,12); return g
def L_P():
    g=blank(); vline(g,2,12,2); hline(g,2,5,2); vline(g,3,6,6); hline(g,2,5,7); return g
def L_Q():
    g=blank(); rect(g,2,2,6,11); line(g,4,9,7,13); return g
def L_R():
    g=blank(); vline(g,2,12,2); hline(g,2,5,2); vline(g,3,6,6); hline(g,2,5,7); line(g,3,7,7,12); return g
def L_S():
    g=blank(); hline(g,3,6,2); vline(g,2,2,3); hline(g,3,5,7); vline(g,6,6,9); hline(g,3,5,12); vline(g,2,2,10)
    px(g,2,3); px(g,6,9)
    return g
def L_T():
    g=blank(); hline(g,2,6,2); vline(g,2,12,4); return g
def L_U():
    g=blank(); vline(g,2,11,2); vline(g,2,11,6); hline(g,2,6,12); line(g,2,11,3,12); line(g,5,12,6,11); return g
def L_V():
    g=blank(); line(g,2,2,4,12); line(g,4,12,6,2); return g
def L_W():
    g=blank(); vline(g,2,12,1); vline(g,7,12,4); vline(g,2,12,7); line(g,1,12,4,8); line(g,4,8,7,12); return g
def L_X():
    g=blank(); line(g,2,2,6,12); line(g,6,2,2,12); return g
def L_Y():
    g=blank(); line(g,2,2,4,7); line(g,6,2,4,7); vline(g,7,12,4); return g
def L_Z():
    g=blank(); hline(g,2,6,2); line(g,6,2,2,12); hline(g,2,6,12); return g

for ch, fn in [('A',L_A),('B',L_B),('C',L_C),('D',L_D),('E',L_E),('F',L_F),
               ('G',L_G),('H',L_H),('I',L_I),('J',L_J),('K',L_K),('L',L_L),
               ('M',L_M),('N',L_N),('O',L_O),('P',L_P),('Q',L_Q),('R',L_R),
               ('S',L_S),('T',L_T),('U',L_U),('V',L_V),('W',L_W),('X',L_X),
               ('Y',L_Y),('Z',L_Z)]:
    glyphs[ch] = fn()

# ---- Lowercase letters (x-height rows 6-12, ascenders to row 2, descenders to row 14) --
def l_a():
    g=blank(); rect(g,3,6,6,12); vline(g,6,12,6); return g
def l_b():
    g=blank(); vline(g,2,12,2); hline(g,3,5,6); vline(g,7,11,6); hline(g,3,5,12); vline(g,7,11,2); return g
def l_c():
    g=blank(); hline(g,3,6,6); vline(g,7,11,2); hline(g,3,6,12); return g
def l_d():
    g=blank()
    vline(g,2,12,6); vline(g,7,11,2); hline(g,2,5,6); hline(g,2,5,12)
    return g
def l_e():
    g=blank(); rect(g,2,6,6,9); hline(g,2,6,9); vline(g,9,11,2); hline(g,3,5,12); line(g,2,11,3,12)
    return g
def l_f():
    g=blank(); line(g,5,2,3,4); vline(g,4,12,3); hline(g,2,5,6); return g
def l_g():
    g=blank(); rect(g,2,6,6,11); vline(g,6,14,6); hline(g,3,5,14); line(g,3,14,2,13)
    return g
def l_h():
    g=blank(); vline(g,2,12,2); vline(g,7,12,6); hline(g,3,5,6); return g
def l_i():
    g=blank(); px(g,4,3); vline(g,6,12,4); return g
def l_j():
    g=blank(); px(g,5,3); vline(g,6,13,5); hline(g,2,4,14); line(g,2,14,3,13); return g
def l_k():
    g=blank(); vline(g,2,12,2); line(g,6,6,3,9); line(g,3,9,6,12); return g
def l_l():
    g=blank(); vline(g,2,11,3); hline(g,3,5,12); return g
def l_m():
    g=blank(); vline(g,6,12,1); vline(g,6,12,4); vline(g,6,12,7)
    hline(g,1,4,6); hline(g,4,7,6); return g
def l_n():
    g=blank(); vline(g,6,12,2); vline(g,6,12,6); hline(g,3,5,6); return g
def l_o():
    g=blank(); rect(g,2,6,6,12); return g
def l_p():
    g=blank(); vline(g,6,14,2); hline(g,3,5,6); vline(g,7,11,6); hline(g,3,5,12); return g
def l_q():
    g=blank()
    vline(g,6,14,6); vline(g,7,11,2); hline(g,2,5,6); hline(g,2,5,12)
    return g
def l_r():
    g=blank(); vline(g,6,12,2); hline(g,3,6,6); return g
def l_s():
    g=blank(); hline(g,3,6,6); vline(g,2,2,7); hline(g,3,5,9); vline(g,6,6,10); hline(g,3,5,12)
    return g
def l_t():
    g=blank(); vline(g,3,11,4); hline(g,2,6,6); line(g,4,11,5,12); return g
def l_u():
    g=blank(); vline(g,6,11,2); vline(g,6,12,6); hline(g,2,6,12); line(g,2,11,3,12); return g
def l_v():
    g=blank(); line(g,2,6,4,12); line(g,4,12,6,6); return g
def l_w():
    g=blank(); vline(g,6,12,1); vline(g,9,12,4); vline(g,6,12,7); line(g,1,12,4,9); line(g,4,9,7,12); return g
def l_x():
    g=blank(); line(g,2,6,6,12); line(g,6,6,2,12); return g
def l_y():
    g=blank(); line(g,2,6,4,11); line(g,6,6,3,14); line(g,3,14,2,13); return g
def l_z():
    g=blank(); hline(g,2,6,6); line(g,6,6,2,12); hline(g,2,6,12); return g

for ch, fn in [('a',l_a),('b',l_b),('c',l_c),('d',l_d),('e',l_e),('f',l_f),
               ('g',l_g),('h',l_h),('i',l_i),('j',l_j),('k',l_k),('l',l_l),
               ('m',l_m),('n',l_n),('o',l_o),('p',l_p),('q',l_q),('r',l_r),
               ('s',l_s),('t',l_t),('u',l_u),('v',l_v),('w',l_w),('x',l_x),
               ('y',l_y),('z',l_z)]:
    glyphs[ch] = fn()

# ---- Punctuation & symbols ---------------------------------------------------
def from_rows(rows, y_start=2):
    """Direct row-by-row bitmap specification, for glyphs (&, @) that are
    impractical to express cleanly as line/rect primitives."""
    g = blank()
    for i, r in enumerate(rows):
        y = y_start + i
        if y >= H: break
        for x, c in enumerate(r):
            if c == '1' and x < W:
                px(g, x, y)
    return g

def sym(name):
    g = blank()
    if name=='space': pass
    elif name=='!': vline(g,2,9,4); px(g,4,12)
    elif name=='"': vline(g,2,4,3); vline(g,2,4,5)
    elif name=='#': vline(g,2,12,3); vline(g,2,12,5); hline(g,1,6,5); hline(g,1,6,9)
    elif name=='$': vline(g,1,13,4); hline(g,2,6,3); vline(g,2,2,4); hline(g,2,6,7); vline(g,6,6,10); hline(g,2,6,11)
    elif name=='%': px(g,2,2); px(g,3,3); px(g,2,4); line(g,2,11,6,2); px(g,5,9); px(g,6,10); px(g,5,11)
    elif name=='&':
        g = from_rows([
            "01110000","10001000","10001000","01110000","01100100",
            "10001000","10010000","10100000","11000100","10001000","01110100",
        ])
    elif name=="'": vline(g,2,4,4)
    elif name=='(': line(g,5,2,3,5); vline(g,5,9,3); line(g,3,9,5,12)
    elif name==')': line(g,3,2,5,5); vline(g,5,9,5); line(g,5,9,3,12)
    elif name=='*': px(g,4,4); line(g,2,2,6,6); line(g,6,2,2,6); vline(g,2,6,4); hline(g,2,6,4)
    elif name=='+': hline(g,2,6,7); vline(g,4,10,4)
    elif name==',': px(g,4,12); px(g,4,13); px(g,3,14)
    elif name=='-': hline(g,2,6,7)
    elif name=='.': px(g,4,12)
    elif name=='/': line(g,2,12,6,2)
    elif name==':': px(g,4,5); px(g,4,9)
    elif name==';': px(g,4,5); px(g,4,9); px(g,3,12)
    elif name=='<': line(g,6,3,2,7); line(g,2,7,6,11)
    elif name=='=': hline(g,2,6,5); hline(g,2,6,9)
    elif name=='>': line(g,2,3,6,7); line(g,6,7,2,11)
    elif name=='?': hline(g,2,5,2); px(g,6,3); px(g,5,5); px(g,4,7); px(g,4,12)
    elif name=='@':
        g = from_rows([
            "00111100","01000010","10011001","10100101","10100101",
            "10100110","10100000","10010010","01001100","00100000","00011110",
        ])
    elif name=='[': vline(g,2,12,3); hline(g,3,5,2); hline(g,3,5,12)
    elif name=='\\': line(g,2,2,6,12)
    elif name==']': vline(g,2,12,5); hline(g,3,5,2); hline(g,3,5,12)
    elif name=='^': line(g,4,2,2,5); line(g,4,2,6,5)
    elif name=='_': hline(g,1,7,13)
    elif name=='`': line(g,3,2,5,4)
    elif name=='{':
        vline(g,3,5,4); line(g,4,5,3,6); vline(g,6,8,3); line(g,3,8,4,9); vline(g,9,11,4)
    elif name=='|': vline(g,1,13,4)
    elif name=='}':
        vline(g,3,5,4); line(g,5,5,4,6); vline(g,6,8,5); line(g,4,8,5,9); vline(g,9,11,4)
    elif name=='~': line(g,1,9,3,6); line(g,3,6,5,9); line(g,5,9,7,6)
    return g

for s in list(" !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"):
    name = s if s != ' ' else 'space'
    glyphs[s] = sym(name)

# ---- Emit C source -----------------------------------------------------------
lines = []
lines.append("/*")
lines.append(" * kernel/font8x16.c — original 8x16 monospace bitmap font.")
lines.append(" *")
lines.append(" * Covers printable ASCII 32-126. Generated by scripts/gen_font8x16.py")
lines.append(" * from hand-authored stroke primitives (lines/rects per glyph) — not")
lines.append(" * derived from any existing font or character ROM. Replaces the previous")
lines.append(" * `const uint8_t font8x16[256][16] = {{0}};` placeholder in platform.c,")
lines.append(" * which left fb_put_char() in drivers/gpu.c always falling through to its")
lines.append(" * ~50-character emergency fallback glyph set.")
lines.append(" *")
lines.append(" * Format: font8x16[char][row], one byte per row, bit 7 = leftmost pixel")
lines.append(" * (matches the `0x80 >> col` read order in fb_put_char).")
lines.append(" */")
lines.append("")
lines.append("#include <stdint.h>")
lines.append("")
lines.append("const uint8_t font8x16[256][16] = {")
for code in range(256):
    ch = chr(code) if 32 <= code <= 126 else None
    g = glyphs.get(ch, blank())
    bs = to_bytes(g)
    row = ",".join(str(b) for b in bs)
    comment = f"  /* {ch!r} ({code}) */" if ch else f"  /* 0x{code:02x} */"
    lines.append(f"    {{{row}}},{comment}")
lines.append("};")
lines.append("")

import os
out_path = os.path.join(os.path.dirname(__file__), "..", "kernel", "font8x16.c")
with open(out_path, "w") as f:
    f.write("\n".join(lines))

print("Wrote kernel/font8x16.c —", len(glyphs), "glyphs defined")
