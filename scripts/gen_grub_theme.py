#!/usr/bin/env python3
"""
Renders the Kryzen boot-menu theme into configs/grub-theme/:

  background.jpg     nebula backdrop with the K logo, the title, the frosted
                     glass panel, the countdown tab and the key hints
  select_*.png       neon box around the selected entry (9 slices)
  item_*.png         hairline under the other entries (9 slices)
  pb_*.png, pbh_*.png  countdown bar: track and fill (3 slices each)
  icons/<class>.png  one icon per menu-entry class
  kryzen-20.pf2, kryzen-16.pf2   Noto Sans for the menu (grub-mkfont)
  theme.txt

The layout is drawn for 1280x720; theme.txt positions everything in percent,
so GRUB can stretch it to another mode. Outputs are checked in: a normal
build needs neither Pillow nor grub-mkfont.
"""
import math
import os
import subprocess
import numpy as np
from PIL import Image, ImageDraw, ImageFilter
import kryzen_art as art

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "configs", "grub-theme")
W, H = 1280, 720

PANEL = (314, 144, 966, 556)            # glass panel
TAB = (512, 556, 768, 618)              # countdown tab under it
HINT = (382, 644, 898, 682)
MENU = (346, 164, 934, 540)             # where GRUB draws the entries
ITEM_H, ITEM_GAP = 52, 10


def notched_frame(d, box, colour, notch=26, width=2):
    """Sci-fi outline with stepped corners, like the mock-up's outer frame."""
    x0, y0, x1, y1 = box
    n = notch
    pts = [(x0 + n, y0), (x0 + 96, y0), (x0 + 108, y0 + 12), (x1 - 108, y0 + 12), (x1 - 96, y0), (x1 - n, y0),
           (x1, y0 + n), (x1, y0 + 120), (x1 - 12, y0 + 134), (x1 - 12, y1 - 134), (x1, y1 - 120), (x1, y1 - n),
           (x1 - n, y1), (x1 - 150, y1), (x1 - 162, y1 - 12), (x0 + 162, y1 - 12), (x0 + 150, y1), (x0 + n, y1),
           (x0, y1 - n), (x0, y1 - 120), (x0 + 12, y1 - 134), (x0 + 12, y0 + 134), (x0, y0 + 120), (x0, y0 + n)]
    d.line(pts + [pts[0]], fill=colour, width=width, joint="curve")


def background():
    src = Image.open(os.path.join(HERE, "..", "desktop", "assets", "nebula.jpg")).convert("RGB")
    bg = src.resize((W, H), Image.LANCZOS)

    # outer neon frame with glow
    S = 2
    layer = Image.new("L", (W * S, H * S), 0)
    box = tuple(v * S for v in (PANEL[0] - 12, PANEL[1] - 12, PANEL[2] + 12, PANEL[3] + 12))
    notched_frame(ImageDraw.Draw(layer), box, 255, notch=22 * S, width=2 * S)
    layer = layer.resize((W, H), Image.LANCZOS)
    grad = art.gradient(W, H, art.CYAN, art.VIOLET, horizontal=True).convert("RGBA")
    glow = grad.copy(); glow.putalpha(layer.filter(ImageFilter.GaussianBlur(5)).point(lambda a: min(255, a * 2)))
    line = grad.copy(); line.putalpha(layer.point(lambda a: int(a * 0.9)))
    bg = bg.convert("RGBA"); bg.alpha_composite(glow); bg.alpha_composite(line); bg = bg.convert("RGB")

    # countdown tab, then the glass panel on top of it
    art.glass(bg, (TAB[0], TAB[1] - 22, TAB[2], TAB[3]), 14, tint=(26, 30, 52), alpha=165, blur=10)
    art.glass(bg, PANEL, 26, tint=(52, 56, 84), alpha=150, blur=16)

    # logo + title
    logo = art.k_logo(84)
    bg.paste(logo, (W // 2 - 42, 14), logo)
    d = ImageDraw.Draw(bg)
    f = art.font(27, "SemiBold")
    parts = [("Kryzen ", (255, 255, 255)), ("Hybrid Boot Manager ", (150, 225, 255)), ("(GRUB2)", (190, 225, 245))]
    total = sum(d.textlength(t, font=f) for t, _ in parts)
    x = (W - total) / 2
    for t, col in parts:
        d.text((x + 1, 105), t, font=f, fill=(0, 0, 0))
        d.text((x, 104), t, font=f, fill=col)
        x += d.textlength(t, font=f)

    # key hints
    art.glass(bg, HINT, 12, tint=(30, 34, 58), alpha=170, blur=8, frame=False)
    d = ImageDraw.Draw(bg)
    fb, fr = art.font(16, "SemiBold"), art.font(16)
    hints = [("Enter:", " Boot"), ("E:", " Edit Kernel Params"), ("C:", " GRUB Console")]
    widths = [d.textlength(a, font=fb) + d.textlength(b, font=fr) for a, b in hints]
    gap = 44
    x = (W - (sum(widths) + gap * 2)) / 2
    for i, (a, b) in enumerate(hints):
        d.text((x, HINT[1] + 8), a, font=fb, fill=(215, 225, 245))
        d.text((x + d.textlength(a, font=fb), HINT[1] + 8), b, font=fr, fill=(150, 160, 190))
        x += widths[i]
        if i < 2:
            d.text((x + gap / 2 - 3, HINT[1] + 7), "|", font=fr, fill=(110, 118, 150))
        x += gap
    bg.save(os.path.join(OUT, "background.jpg"), quality=90, subsampling=0, progressive=False)


def nine_slice(name, img, b):
    w, h = img.size
    cuts = {"nw": (0, 0, b, b), "n": (b, 0, w - b, b), "ne": (w - b, 0, w, b),
            "w": (0, b, b, h - b), "c": (b, b, w - b, h - b), "e": (w - b, b, w, h - b),
            "sw": (0, h - b, b, h), "s": (b, h - b, w - b, h), "se": (w - b, h - b, w, h)}
    for k, box in cuts.items():
        img.crop(box).save(os.path.join(OUT, "%s_%s.png" % (name, k)))


def selection():
    b = 14
    w, h = 160, ITEM_H + 2 * b
    frame = art.neon_frame(w, h, 14, width=2, glow=7)
    fill = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    m = art.rounded_mask(w - 14, h - 14, 12)
    tint = Image.new("RGBA", (w - 14, h - 14), (120, 170, 255, 44))
    fill.paste(tint, (7, 7), m)
    fill.alpha_composite(frame)
    nine_slice("select", fill, b)

    item = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(item).line([(b, h - b + 4), (w - b, h - b + 4)], fill=(170, 185, 230, 46), width=1)
    nine_slice("item", item, b)


def progress():
    hgt = 8
    for name, col, a in (("pb", (150, 165, 205), 70), ("pbh", None, 255)):
        img = Image.new("RGBA", (40, hgt), (0, 0, 0, 0))
        m = art.rounded_mask(40, hgt, 4)
        if col:
            body = Image.new("RGBA", (40, hgt), col + (a,))
        else:
            body = art.gradient(40, hgt, (120, 235, 255), (150, 215, 255), horizontal=True).convert("RGBA")
        img.paste(body, (0, 0), m)
        img.crop((0, 0, 4, hgt)).save(os.path.join(OUT, name + "_w.png"))
        img.crop((4, 0, 36, hgt)).save(os.path.join(OUT, name + "_c.png"))
        img.crop((36, 0, 40, hgt)).save(os.path.join(OUT, name + "_e.png"))


# ---- icons (line art, drawn 4x and scaled down) -------------------------------
def icon(draw_fn, size=40):
    S = 4
    n = size * S
    m = Image.new("L", (n, n), 0)
    draw_fn(ImageDraw.Draw(m), n, S)
    m = m.resize((size, size), Image.LANCZOS)
    g = art.gradient(size, size, (130, 235, 255), (165, 140, 255)).convert("RGBA")
    out = g.copy()
    out.putalpha(m.filter(ImageFilter.GaussianBlur(1.6)).point(lambda a: int(a * 0.55)))
    top = g.copy(); top.putalpha(m)
    out.alpha_composite(top)
    return out


def ic_kryzen(d, n, S):
    lw = 3 * S
    d.rounded_rectangle([n * .16, n * .16, n * .84, n * .84], n * .12, outline=255, width=lw)
    for i in range(4):                              # chip pins
        t = n * (.28 + .147 * i)
        for a, b in ((.04, .13), (.87, .96)):
            d.line([(t, n * a), (t, n * b)], fill=255, width=2 * S)
            d.line([(n * a, t), (n * b, t)], fill=255, width=2 * S)
    k = [(.34, .30), (.34, .70)], [(.36, .52), (.64, .30)], [(.44, .46), (.66, .70)]
    for a, b in k:
        d.line([(a[0] * n, a[1] * n), (b[0] * n, b[1] * n)], fill=255, width=4 * S)


def gear(d, cx, cy, r, S, teeth=8):
    for i in range(teeth):
        a = i * 2 * math.pi / teeth
        d.line([(cx + math.cos(a) * r * .78, cy + math.sin(a) * r * .78),
                (cx + math.cos(a) * r * 1.12, cy + math.sin(a) * r * 1.12)], fill=255, width=int(r * .34))
    d.ellipse([cx - r * .82, cy - r * .82, cx + r * .82, cy + r * .82], outline=255, width=3 * S)
    d.ellipse([cx - r * .30, cy - r * .30, cx + r * .30, cy + r * .30], outline=255, width=3 * S)


def ic_recovery(d, n, S):
    gear(d, n * .42, n * .42, n * .27, S)
    tri = [(n * .70, n * .50), (n * .96, n * .93), (n * .44, n * .93)]
    d.polygon(tri, fill=0)
    d.line(tri + [tri[0]], fill=255, width=3 * S, joint="curve")
    d.line([(n * .70, n * .64), (n * .70, n * .79)], fill=255, width=3 * S)
    d.ellipse([n * .68, n * .83, n * .72, n * .87], fill=255)


def ic_windows(d, n, S):
    pts = lambda x0, x1, ya, yb, yc, yd: [(n * x0, n * ya), (n * x1, n * yb), (n * x1, n * yc), (n * x0, n * yd)]
    d.polygon(pts(.10, .46, .20, .14, .47, .47), fill=255)
    d.polygon(pts(.52, .92, .13, .07, .47, .47), fill=255)
    d.polygon(pts(.10, .46, .53, .53, .86, .80), fill=255)
    d.polygon(pts(.52, .92, .53, .53, .93, .87), fill=255)


def ic_tty(d, n, S):
    d.rounded_rectangle([n * .08, n * .16, n * .92, n * .84], n * .08, outline=255, width=3 * S)
    d.line([(n * .08, n * .30), (n * .92, n * .30)], fill=255, width=2 * S)
    d.line([(n * .24, n * .44), (n * .40, n * .56), (n * .24, n * .68)], fill=255, width=3 * S, joint="curve")
    d.line([(n * .48, n * .68), (n * .70, n * .68)], fill=255, width=3 * S)


def ic_firmware(d, n, S):
    d.line([(n * .16, n * .84), (n * .56, n * .44)], fill=255, width=5 * S)       # wrench shaft
    d.arc([n * .44, n * .12, n * .80, n * .48], 120, 30, fill=255, width=4 * S)   # wrench jaw
    d.line([(n * .84, n * .84), (n * .46, n * .46)], fill=255, width=3 * S)       # screwdriver
    d.line([(n * .46, n * .46), (n * .36, n * .30)], fill=255, width=2 * S)
    d.rounded_rectangle([n * .62, n * .58, n * .90, n * .86], n * .04, fill=0, outline=255, width=3 * S)
    d.rectangle([n * .71, n * .67, n * .81, n * .77], outline=255, width=2 * S)


def ic_install(d, n, S):
    d.line([(n * .50, n * .10), (n * .50, n * .58)], fill=255, width=4 * S)
    d.line([(n * .30, n * .40), (n * .50, n * .60), (n * .70, n * .40)], fill=255, width=4 * S, joint="curve")
    d.rounded_rectangle([n * .12, n * .66, n * .88, n * .90], n * .06, outline=255, width=3 * S)
    d.ellipse([n * .70, n * .74, n * .78, n * .82], fill=255)


def theme_txt():
    def px(v, total):
        return "%d%%" % round(v * 100 / total)
    ml, mt, mr, mb = MENU
    lines = f'''# Kryzen OS boot menu theme (generated by scripts/gen_grub_theme.py)

title-text: ""
desktop-image: "background.jpg"
desktop-image-scale-method: "stretch"
desktop-color: "#05060f"
terminal-font: "Noto Sans Regular 16"
terminal-left: "10%"
terminal-top: "12%"
terminal-width: "80%"
terminal-height: "76%"

+ boot_menu {{
    left = 27%
    top = 23%
    width = 46%
    height = 52%
    item_font = "Noto Sans Regular 20"
    item_color = "#dfe6f5"
    selected_item_color = "#ffffff"
    item_height = {ITEM_H}
    item_padding = 0
    item_spacing = {ITEM_GAP}
    item_icon_space = 20
    icon_width = 40
    icon_height = 40
    item_pixmap_style = "item_*.png"
    selected_item_pixmap_style = "select_*.png"
    scrollbar = false
}}

+ label {{
    id = "__timeout__"
    left = 40%
    top = 78%
    width = 20%
    align = "center"
    text = "Auto-booting in %ds..."
    color = "#dfe6f5"
    font = "Noto Sans Regular 16"
}}

+ progress_bar {{
    id = "__timeout__"
    left = 41%
    top = 82%
    width = 18%
    height = 8
    show_text = false
    bar_style = "pb_*.png"
    highlight_style = "pbh_*.png"
}}
'''
    open(os.path.join(OUT, "theme.txt"), "w").write(lines)


def fonts():
    for size in (20, 16):
        subprocess.check_call(["grub-mkfont", "-s", str(size), "-o", os.path.join(OUT, "kryzen-%d.pf2" % size),
                               art.NOTO + "NotoSans-Regular.ttf"])


def main():
    os.makedirs(os.path.join(OUT, "icons"), exist_ok=True)
    for old in ("background.png", "select_c.png"):
        p = os.path.join(OUT, old)
        if os.path.exists(p):
            os.remove(p)
    background()
    selection()
    progress()
    for name, fn in (("kryzen", ic_kryzen), ("recovery", ic_recovery), ("windows", ic_windows),
                     ("tty", ic_tty), ("firmware", ic_firmware), ("install", ic_install)):
        icon(fn).save(os.path.join(OUT, "icons", name + ".png"))
    fonts()
    theme_txt()
    print("wrote", os.path.abspath(OUT))


if __name__ == "__main__":
    main()
