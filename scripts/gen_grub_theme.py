#!/usr/bin/env python3
"""
Renders the images for the GRUB boot-menu theme into configs/grub-theme/:
  background.png   gradient + logo + "JARVIS OS" title (1024x768)
  select_c.png     highlight colour of the selected menu entry

Run once (needs Pillow and the DejaVu fonts); the outputs are checked in so a
normal build does not depend on Pillow.
"""
from PIL import Image, ImageDraw, ImageFont
import os

OUT = os.path.join(os.path.dirname(__file__), "..", "configs", "grub-theme")
W, H = 1024, 768
TOP, BOTTOM = (0x07, 0x14, 0x26), (0x0F, 0x4C, 0x81)
ACCENT = (0x60, 0xCD, 0xFF)

img = Image.new("RGB", (W, H))
d = ImageDraw.Draw(img)
for y in range(H):
    t = y / (H - 1)
    d.line([(0, y), (W, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(TOP, BOTTOM)))

# soft glow rising from the bottom, matching the desktop wallpaper
glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
g = ImageDraw.Draw(glow)
cx, cy = W // 2, H + H // 6
for pct in (95, 72, 52, 34):
    r = H * pct // 100
    g.ellipse([cx - r, cy - r, cx + r, cy + r], fill=ACCENT + (16,))
    img.paste(Image.alpha_composite(img.convert("RGBA"), glow).convert("RGB"))
    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0)); g = ImageDraw.Draw(glow)

# logo: the assistant orb (supersampled for smooth edges)
S = 4
orb = Image.new("RGBA", (96 * S, 96 * S), (0, 0, 0, 0))
o = ImageDraw.Draw(orb)
c = 48 * S
def ring(r_out, r_in, col):
    o.ellipse([c - r_out * S, c - r_out * S, c + r_out * S, c + r_out * S], fill=col)
    if r_in: o.ellipse([c - r_in * S, c - r_in * S, c + r_in * S, c + r_in * S], fill=(0x0B, 0x3A, 0x5C, 255))
o.ellipse([c - 44 * S, c - 44 * S, c + 44 * S, c + 44 * S], fill=(0x0B, 0x3A, 0x5C, 255))
ring(40, 32, (0x46, 0xD3, 0xC6, 255))
ring(24, 20, ACCENT + (255,))
ring(12, 0, (255, 255, 255, 255))
orb = orb.resize((96, 96), Image.LANCZOS)
img.paste(orb, (W // 2 - 48, 120), orb)

d = ImageDraw.Draw(img)
bold = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 40)
small = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 14)
def centered(text, y, font, fill):
    w = d.textlength(text, font=font)
    d.text(((W - w) / 2, y), text, font=font, fill=fill)
centered("JARVIS OS", 236, bold, (255, 255, 255))
centered("Use the arrow keys to choose, Enter to start", H - 60, small, (0xA6, 0xB8, 0xCC))

img.save(os.path.join(OUT, "background.png"), optimize=True)
Image.new("RGBA", (8, 8), (0x60, 0xCD, 0xFF, 70)).save(os.path.join(OUT, "select_c.png"))
print("wrote", os.path.abspath(OUT))
