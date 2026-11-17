#!/usr/bin/env python3
"""
Renders the two built-in Kryzen backdrops into desktop/assets/:

  cosmos.jpg   desktop wallpaper: deep space with the particle sphere
  nebula.jpg   sign-in, first-run setup and boot screens: stars and nebula
               with a soft ring where the glass panel sits

The kernel embeds both (desktop/assets.S) and scales them to the screen.
Run once after changing the artwork:  python3 scripts/gen_wallpapers.py
"""
import os
import kryzen_art as art

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "desktop", "assets")
W, H = 1600, 900


def main():
    os.makedirs(OUT, exist_ok=True)

    img = art.cosmos(W, H, seed=7, nebula=0.75)
    art.particle_sphere(img)
    art.to_image(img).save(os.path.join(OUT, "cosmos.jpg"), quality=88, subsampling=0, progressive=False)

    img = art.cosmos(W, H, seed=21, nebula=1.0)
    art.ring_glow(img, W * 0.50, H * 0.53, 0.200, (70, 200, 235), 0.20, 0.007, squash=1.12)
    art.ring_glow(img, W * 0.50, H * 0.53, 0.208, (150, 90, 255), 0.16, 0.014, squash=1.12)
    art.ring_glow(img, W * 0.50, H * 0.53, 0.250, (120, 150, 255), 0.05, 0.003, squash=1.12)
    art.to_image(img).save(os.path.join(OUT, "nebula.jpg"), quality=88, subsampling=0, progressive=False)

    for name in ("cosmos.jpg", "nebula.jpg"):
        print(name, os.path.getsize(os.path.join(OUT, name)) // 1024, "KB")


if __name__ == "__main__":
    main()
