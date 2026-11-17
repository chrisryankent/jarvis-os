#!/usr/bin/env python3
"""
Kryzen OS artwork, drawn by code (needs numpy, scipy and Pillow).

Used by scripts/gen_grub_theme.py (boot menu) and scripts/gen_wallpapers.py
(desktop, sign-in and setup backdrops). Everything is generated from fixed
seeds, so a rebuild gives the same pictures; the outputs are checked in and a
normal `make` does not need these packages.
"""
import math
import numpy as np
from scipy.ndimage import gaussian_filter, zoom
from PIL import Image, ImageDraw, ImageFilter, ImageFont

CYAN = (0x5C, 0xE6, 0xFF)
VIOLET = (0x9B, 0x7B, 0xFF)
MAGENTA = (0xD0, 0x5C, 0xE0)
NOTO = "/usr/share/fonts/truetype/noto/"


def font(size, weight="Regular"):
    return ImageFont.truetype(NOTO + "NotoSans-%s.ttf" % weight, size)


# ---- noise -------------------------------------------------------------------
def fbm(rng, w, h, base=6, octaves=5, stretch=1.0):
    """Smooth fractal noise in 0..1 (w x h)."""
    out = np.zeros((h, w), np.float32)
    amp, total = 1.0, 0.0
    for o in range(octaves):
        gw = max(2, int(base * (2 ** o) * stretch))
        gh = max(2, int(base * (2 ** o) * h / w))
        g = rng.random((gh + 2, gw + 2)).astype(np.float32)
        layer = zoom(g, ((h + 8) / (gh + 2), (w + 8) / (gw + 2)), order=3)[:h, :w]
        layer = gaussian_filter(layer, max(0.6, w / (gw * 5.0)))
        out += amp * layer
        total += amp
        amp *= 0.5
    out /= total
    lo, hi = out.min(), out.max()
    return (out - lo) / (hi - lo + 1e-6)


def add_rgb(img, mask, colour, strength=1.0):
    for c in range(3):
        img[..., c] += mask * (colour[c] / 255.0) * strength


# ---- deep space --------------------------------------------------------------
def cosmos(w, h, seed=7, nebula=1.0, stars=1.0):
    """Starfield with violet / magenta / teal nebula clouds. float RGB 0..1."""
    rng = np.random.default_rng(seed)
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    u, v = xx / w, yy / h

    img = np.zeros((h, w, 3), np.float32)
    # base: near-black navy, a little lighter towards the middle
    d = np.sqrt((u - 0.5) ** 2 + ((v - 0.5) * 0.8) ** 2)
    base = np.clip(1 - d * 1.35, 0, 1)
    img[..., 0] = 0.012 + 0.030 * base
    img[..., 1] = 0.016 + 0.036 * base
    img[..., 2] = 0.045 + 0.095 * base

    n1 = fbm(rng, w, h, 5, 6)
    n2 = fbm(rng, w, h, 7, 6)
    n3 = fbm(rng, w, h, 4, 5)
    wisps = fbm(rng, w, h, 14, 5)

    def cloud(cx, cy, rx, ry, noise, power=2.2):
        e = np.exp(-(((u - cx) / rx) ** 2 + ((v - cy) / ry) ** 2))
        return e * np.clip(noise * 1.5 - 0.28, 0, 1) ** power

    # left and right nebula bands (pink cores inside violet), like the mock-ups
    left = cloud(0.17, 0.44, 0.20, 0.17, n1)
    right = cloud(0.84, 0.56, 0.19, 0.18, n2)
    add_rgb(img, left, (120, 60, 220), 0.85 * nebula)
    add_rgb(img, right, (110, 55, 215), 0.85 * nebula)
    add_rgb(img, cloud(0.15, 0.42, 0.10, 0.07, n2, 3.0), (255, 120, 150), 0.75 * nebula)
    add_rgb(img, cloud(0.83, 0.55, 0.09, 0.07, n1, 3.0), (255, 130, 140), 0.70 * nebula)
    # blue-violet haze through the middle and a teal breath bottom-left
    add_rgb(img, cloud(0.50, 0.50, 0.55, 0.30, n3, 1.8), (50, 60, 190), 0.30 * nebula)
    add_rgb(img, cloud(0.28, 0.80, 0.25, 0.18, n1, 2.0), (30, 150, 190), 0.22 * nebula)
    add_rgb(img, cloud(0.74, 0.20, 0.24, 0.18, n3, 2.0), (90, 50, 200), 0.28 * nebula)
    # fine wisps modulate everything a little
    img *= (0.80 + 0.40 * wisps)[..., None]

    # ---- stars: many faint, few bright ----
    count = int(w * h / 420 * stars)
    sx = rng.integers(0, w, count)
    sy = rng.integers(0, h, count)
    mag = rng.random(count) ** 7                    # mostly dim
    tint = rng.random(count)
    layer = np.zeros((h, w, 3), np.float32)
    cols = np.stack([
        np.where(tint < 0.25, 0.75, np.where(tint > 0.85, 1.0, 0.95)),
        np.where(tint < 0.25, 0.85, np.where(tint > 0.85, 0.80, 0.96)),
        np.where(tint < 0.25, 1.0, np.where(tint > 0.85, 0.95, 1.0)),
    ], axis=1).astype(np.float32)
    np.add.at(layer, (sy, sx), cols * (0.10 + 0.90 * mag)[:, None])
    img += layer
    bright = mag > 0.45
    glow = np.zeros((h, w, 3), np.float32)
    np.add.at(glow, (sy[bright], sx[bright]), cols[bright] * mag[bright][:, None])
    s = max(1.0, w / 900)
    img += gaussian_filter(glow, (1.2 * s, 1.2 * s, 0)) * 5.0
    img += gaussian_filter(glow, (4.0 * s, 4.0 * s, 0)) * 9.0

    # a handful of big coloured stars with a soft halo
    big = np.zeros((h, w, 3), np.float32)
    for _ in range(int(16 * stars)):
        x, y = int(rng.integers(0, w)), int(rng.integers(0, h))
        c = [(0.55, 0.80, 1.0), (1.0, 0.70, 1.0), (0.80, 0.70, 1.0), (1, 1, 1)][int(rng.integers(0, 4))]
        big[y, x] = np.array(c) * (0.5 + rng.random())
    img += gaussian_filter(big, (2.2 * s, 2.2 * s, 0)) * 34.0
    img += gaussian_filter(big, (9.0 * s, 9.0 * s, 0)) * 60.0
    return img


def ring_glow(img, cx, cy, r, colour, strength=0.5, width=0.012, arc=None, squash=1.0):
    """Soft luminous ring (or arc) behind a glass panel."""
    h, w, _ = img.shape
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    dx, dy = (xx - cx) / w, (yy - cy) / w / squash
    d = np.sqrt(dx * dx + dy * dy)
    band = np.exp(-((d - r) / width) ** 2)
    if arc:
        ang = np.arctan2(dy, dx)
        a0, a1 = arc
        band *= np.clip(np.cos((ang - (a0 + a1) / 2)) * 1.4, 0, 1) ** 2
    add_rgb(img, band, colour, strength)


# ---- the particle sphere of the desktop wallpaper ----------------------------
def particle_sphere(img, seed=11):
    h, w, _ = img.shape
    rng = np.random.default_rng(seed)
    acc = np.zeros((h, w, 3), np.float32)
    cx, cy, R = w * 0.50, h * 0.50, h * 0.27

    def splat(px, py, col, weight):
        ok = (px >= 0) & (px < w) & (py >= 0) & (py < h)
        np.add.at(acc, (py[ok].astype(int), px[ok].astype(int)), col[ok] * weight[ok][:, None])

    # points on a sphere; density follows a noise field so it looks like a
    # swirl of data rather than a uniform ball
    n = 150000
    z = rng.uniform(-1, 1, n)
    t = rng.uniform(0, 2 * math.pi, n)
    r = np.sqrt(1 - z * z)
    x, y = r * np.cos(t), r * np.sin(t)
    dens = (np.sin(t * 3 + z * 5) * 0.5 + 0.5) * (np.sin(z * 9 - t * 2) * 0.5 + 0.5)
    keep = rng.random(n) < (0.10 + 0.90 * dens ** 1.5)
    x, y, z = x[keep], y[keep], z[keep]
    # tilt
    a = 0.45
    y2 = y * math.cos(a) - z * math.sin(a)
    z2 = y * math.sin(a) + z * math.cos(a)
    depth = (z2 + 1) / 2
    mix = np.clip((x + 1) / 2 + rng.normal(0, 0.12, x.size), 0, 1)
    col = np.stack([0.20 + 0.55 * mix, 0.95 - 0.60 * mix, np.ones_like(mix)], axis=1).astype(np.float32)
    splat(cx + x * R, cy + y2 * R, col, 0.12 + 0.50 * depth)

    # orbital rings and long streams
    for k in range(6):
        m = 15000
        ph = rng.uniform(0, 2 * math.pi, m)
        rad = R * (1.30 + 0.42 * k + rng.normal(0, 0.006 + 0.004 * k, m))
        tilt = 0.24 + 0.035 * k
        rot = -0.22 + 0.05 * k
        ex, ey = rad * np.cos(ph), rad * np.sin(ph) * tilt
        px = cx + ex * math.cos(rot) - ey * math.sin(rot)
        py = cy + ex * math.sin(rot) + ey * math.cos(rot)
        bright = (np.sin(ph * 2 + k) * 0.5 + 0.5) ** 2
        c = np.tile(np.array([[0.25, 0.75, 1.0]] if k % 3 else [[0.60, 0.45, 1.0]], np.float32), (m, 1))
        splat(px, py, c, 0.04 + 0.26 * bright / (1 + 0.35 * k))

    for k in range(26):                 # streams leaving left and right
        side = -1 if k % 2 else 1
        m = 3600
        tpar = rng.random(m) ** 0.8
        y0 = rng.normal(0, 0.10)
        bend = rng.normal(0, 0.22)
        px = cx + side * (R * 0.9 + tpar * w * 0.62)
        py = cy + h * (y0 + bend * tpar * tpar) + rng.normal(0, 1.0 + 5 * tpar, m)
        c = np.tile(np.array([[0.30, 0.65, 1.0]] if k % 4 else [[0.70, 0.45, 1.0]], np.float32), (m, 1))
        splat(px, py, c, (1 - tpar) * 0.22 + 0.02)

    s = max(1.0, w / 1100)
    img += acc * 0.55
    img += gaussian_filter(acc, (1.2 * s, 1.2 * s, 0)) * 1.0
    img += gaussian_filter(acc, (6.0 * s, 6.0 * s, 0)) * 1.1
    # the glowing shell and core
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    d = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2) / R
    add_rgb(img, np.exp(-((d - 1.0) / 0.05) ** 2), (70, 150, 255), 0.12)
    add_rgb(img, np.exp(-(d / 0.75) ** 2), (60, 36, 170), 0.20)
    add_rgb(img, np.exp(-(((xx - cx * 0.86) / (R * 0.5)) ** 2 + ((yy - cy * 0.96) / (R * 0.4)) ** 2)), (40, 200, 220), 0.10)


def to_image(img):
    out = np.clip(img, 0, 1) ** (1 / 1.12)
    return Image.fromarray((out * 255 + 0.5).astype(np.uint8), "RGB")


# ---- vector pieces -----------------------------------------------------------
def gradient(w, h, top=CYAN, bottom=VIOLET, horizontal=False):
    g = np.zeros((h, w, 3), np.float32)
    t = (np.linspace(0, 1, w)[None, :] if horizontal else np.linspace(0, 1, h)[:, None])
    for c in range(3):
        g[..., c] = top[c] + (bottom[c] - top[c]) * t
    return Image.fromarray(g.astype(np.uint8), "RGB")


def k_logo(size, stroke=None, glow=True):
    """The Kryzen K: a double outline in a cyan-to-violet gradient. RGBA."""
    S = 4
    n = size * S
    pts = [(.12, .06), (.34, .06), (.34, .40), (.66, .06), (.93, .06), (.53, .49),
           (.95, .94), (.68, .94), (.40, .62), (.34, .69), (.34, .94), (.12, .94)]
    pts = [(x * n, y * n) for x, y in pts]
    lw = int((stroke or max(2, size // 22)) * S)
    mask = Image.new("L", (n, n), 0)
    d = ImageDraw.Draw(mask)
    d.line(pts + [pts[0]], fill=255, width=lw, joint="curve")
    # inner echo line
    cx = sum(p[0] for p in pts) / len(pts)
    cy = sum(p[1] for p in pts) / len(pts)
    inner_mask = Image.new("L", (n, n), 0)
    ImageDraw.Draw(inner_mask).polygon(pts, fill=255)
    inner = inner_mask.filter(ImageFilter.MinFilter(2 * int(lw * 1.6) + 1))
    edge = Image.fromarray((np.asarray(inner, np.int16) - np.asarray(inner.filter(ImageFilter.MinFilter(2 * (lw // 2) + 1)), np.int16)).clip(0, 255).astype(np.uint8))
    mask = Image.fromarray(np.maximum(np.asarray(mask), (np.asarray(edge) * 0.85).astype(np.uint8)))
    mask = mask.resize((size, size), Image.LANCZOS)
    out = gradient(size, size).convert("RGBA")
    out.putalpha(mask)
    if glow:
        halo = gradient(size, size).convert("RGBA")
        halo.putalpha(mask.filter(ImageFilter.GaussianBlur(size / 18)).point(lambda a: min(255, int(a * 0.9))))
        halo.alpha_composite(out)
        out = halo
    return out


def rounded_mask(w, h, r, S=3):
    m = Image.new("L", (w * S, h * S), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, w * S - 1, h * S - 1], r * S, fill=255)
    return m.resize((w, h), Image.LANCZOS)


def neon_frame(w, h, r, width=2, top=CYAN, bottom=VIOLET, glow=10, horizontal=True, S=3):
    """Rounded outline in a gradient with a soft glow. RGBA (w x h)."""
    m = Image.new("L", (w * S, h * S), 0)
    pad = glow * S
    ImageDraw.Draw(m).rounded_rectangle([pad, pad, w * S - 1 - pad, h * S - 1 - pad], r * S,
                                        outline=255, width=width * S)
    m = m.resize((w, h), Image.LANCZOS)
    g = gradient(w, h, top, bottom, horizontal=horizontal).convert("RGBA")
    halo = g.copy()
    halo.putalpha(m.filter(ImageFilter.GaussianBlur(glow / 2.2)).point(lambda a: min(255, int(a * 1.5))))
    line = g.copy()
    line.putalpha(m)
    halo.alpha_composite(line)
    return halo


def glass(backdrop, box, r, tint=(40, 44, 70), alpha=150, blur=14, frame=True):
    """Frosted-glass panel pasted onto `backdrop` (RGB image) at box."""
    x0, y0, x1, y1 = box
    w, h = x1 - x0, y1 - y0
    region = backdrop.crop(box).filter(ImageFilter.GaussianBlur(blur))
    region = Image.blend(region, Image.new("RGB", (w, h), tint), alpha / 255.0)
    # light sheen from the top-left
    sheen = np.zeros((h, w), np.float32)
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    sheen = np.clip(1 - (xx / w * 0.6 + yy / h), 0, 1) * 26
    region = Image.fromarray(np.clip(np.asarray(region, np.float32) + sheen[..., None], 0, 255).astype(np.uint8))
    backdrop.paste(region, (x0, y0), rounded_mask(w, h, r))
    if frame:
        ImageDraw.Draw(backdrop, "RGBA").rounded_rectangle([x0, y0, x1 - 1, y1 - 1], r, outline=(190, 220, 255, 70), width=1)
    return backdrop
