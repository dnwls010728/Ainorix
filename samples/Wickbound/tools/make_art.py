"""Generates the procedural effect textures in assets/fx (white shapes that sprites tint).

The character, enemy, brazier and ground art in assets/sprites comes from the original
Wickbound web build; assets/icons are Noto Emoji images. Run from the project folder:

    python tools/make_art.py
"""
import math
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "fx")
SS = 4  # supersampling for smooth edges


def save(name, rgba):
    os.makedirs(OUT, exist_ok=True)
    Image.fromarray(np.clip(rgba, 0, 255).astype(np.uint8), "RGBA").save(os.path.join(OUT, name + ".png"), optimize=True)


def white(alpha):
    h, w = alpha.shape
    out = np.full((h, w, 4), 255.0)
    out[..., 3] = alpha * 255.0
    return out


def radial(size):
    c = (size - 1) / 2.0
    y, x = np.mgrid[0:size, 0:size]
    return np.hypot(x - c, y - c) / (size / 2.0)


def lerp_stops(d, stops):
    xs = [s[0] for s in stops]
    ys = [s[1] for s in stops]
    return np.interp(d, xs, ys)


def shape(size, draw_fn, blur=0.0):
    """Draws a white-on-black mask supersampled, returns alpha 0..1 at `size`."""
    img = Image.new("L", (size * SS, size * SS), 0)
    draw_fn(ImageDraw.Draw(img), size * SS)
    if blur:
        img = img.filter(ImageFilter.GaussianBlur(blur * SS))
    img = img.resize((size, size), Image.LANCZOS)
    return np.asarray(img, dtype=np.float64) / 255.0


def main():
    # Soft glow: the original's 64 px radial gradient (1 -> 0.45 at 35% -> 0).
    save("glow", white(lerp_stops(radial(64), [(0, 1), (0.35, 0.45), (1, 0)])))

    # Solid disc with a soft edge, and a ground shadow.
    save("disc", white(shape(64, lambda d, s: d.ellipse([s * 0.04, s * 0.04, s * 0.96, s * 0.96], fill=255))))
    save("shadow", np.concatenate([np.zeros((64, 64, 3)), (lerp_stops(radial(64), [(0, 1), (0.6, 0.9), (1, 0)]) * 255)[..., None]], axis=2))

    # Rings: thin outline, filled (pulse) and dashed (unlit brazier).
    def ring(d, s, width):
        m = s * 0.02 + width / 2
        d.ellipse([m, m, s - m, s - m], outline=255, width=int(width))

    save("ring", white(shape(256, lambda d, s: ring(d, s, s * 0.016))))
    save("ring_fill", white(lerp_stops(radial(256), [(0, 0.55), (0.9, 0.75), (0.96, 1), (1, 0)])))

    def dashed(d, s):
        m = s * 0.03
        n = 28
        for i in range(n):
            a0 = 360.0 * i / n
            d.arc([m, m, s - m, s - m], a0, a0 + 180.0 / n, fill=255, width=int(s * 0.012))

    save("ring_dash", white(shape(256, dashed)))

    # Beam: a horizontal bar, opaque core fading to the top and bottom edges and rounded ends.
    h, w = 32, 128
    y, x = np.mgrid[0:h, 0:w]
    across = 1 - np.abs((y - (h - 1) / 2) / (h / 2))
    ends = np.clip(np.minimum(x, w - 1 - x) / 10.0, 0, 1)
    save("beam", white(np.clip(across * 1.6, 0, 1) * ends))

    # Oil pool: warm centre to yellow-green rim (the original gradient), premixed colours.
    d = radial(128)
    pool = np.zeros((128, 128, 4))
    t = np.clip((d - 0.2) / 0.5, 0, 1)[..., None]
    pool[..., :3] = np.array([255, 190, 70]) * (1 - t) + np.array([210, 255, 90]) * t
    pool[..., 3] = lerp_stops(d, [(0, 1), (0.2, 1), (0.7, 0.6), (1, 0)]) * 255
    save("pool", pool)

    # Pickups.
    save("gem", white(shape(48, lambda d, s: d.polygon([(s * 0.5, s * 0.04), (s * 0.87, s * 0.5), (s * 0.5, s * 0.96), (s * 0.13, s * 0.5)], fill=255))))

    def drop(d, s):
        d.ellipse([s * 0.22, s * 0.42, s * 0.78, s * 0.98], fill=255)
        d.polygon([(s * 0.3, s * 0.58), (s * 0.5, s * 0.04), (s * 0.7, s * 0.58)], fill=255)

    save("drop", white(shape(48, drop)))

    def cross(d, s):
        d.rounded_rectangle([s * 0.36, s * 0.08, s * 0.64, s * 0.92], radius=s * 0.06, fill=255)
        d.rounded_rectangle([s * 0.08, s * 0.36, s * 0.92, s * 0.64], radius=s * 0.06, fill=255)

    save("cross", white(shape(48, cross)))

    coin = np.zeros((48, 48, 4))
    outer = shape(48, lambda d, s: d.ellipse([s * 0.06, s * 0.06, s * 0.94, s * 0.94], fill=255))
    inner = shape(48, lambda d, s: d.ellipse([s * 0.3, s * 0.3, s * 0.7, s * 0.7], fill=255))[..., None]
    coin[..., :3] = np.array([255, 226, 122]) * (1 - inner) + np.array([184, 134, 43]) * inner
    coin[..., 3] = outer * 255
    save("coin", coin)

    chest = np.zeros((64, 64, 4))
    body = shape(64, lambda d, s: d.rounded_rectangle([s * 0.03, s * 0.18, s * 0.97, s * 0.82], radius=s * 0.06, fill=255))
    band = shape(64, lambda d, s: (d.rectangle([s * 0.03, s * 0.37, s * 0.97, s * 0.5], fill=255), d.rectangle([s * 0.4, s * 0.18, s * 0.6, s * 0.82], fill=255)))[..., None]
    chest[..., :3] = np.array([122, 74, 30]) * (1 - band) + np.array([255, 210, 74]) * band
    chest[..., 3] = body * 255
    save("chest", chest)

    # Projectiles.
    save("shard", white(shape(48, lambda d, s: d.polygon([(s * 0.98, s * 0.5), (s * 0.1, s * 0.72), (s * 0.1, s * 0.28)], fill=255))))

    def moth(d, s):
        for cx, tilt in ((0.3, -1), (0.7, 1)):
            box = [s * (cx - 0.26), s * 0.3, s * (cx + 0.26), s * 0.7]
            d.ellipse(box, fill=255)

    save("moth", white(shape(48, moth)))

    # A pair of eyes: what shows of a critter in the dark.
    eyes = Image.new("L", (96 * SS, 24 * SS), 0)
    pen = ImageDraw.Draw(eyes)
    radius = 24 * SS * 0.46
    for cx in (96 * SS * 0.25, 96 * SS * 0.75):
        pen.ellipse([cx - radius, 12 * SS - radius, cx + radius, 12 * SS + radius], fill=255)
    save("eyes", white(np.asarray(eyes.resize((96, 24), Image.LANCZOS), dtype=np.float64) / 255.0))

    # Off-screen indicator arrow (points +X).
    save("arrow", white(shape(64, lambda d, s: d.polygon([(s * 0.98, s * 0.5), (s * 0.12, s * 0.9), (s * 0.34, s * 0.5), (s * 0.12, s * 0.1)], fill=255))))

    # Four-point star: the Glim currency mark (the original's ✦ glyph).
    def star(d, s):
        c, r, k = s / 2, s * 0.48, s * 0.13
        pts = []
        for i in range(8):
            a = math.pi / 4 * i - math.pi / 2
            rad = r if i % 2 == 0 else k
            pts.append((c + math.cos(a) * rad, c + math.sin(a) * rad))
        d.polygon(pts, fill=255)

    save("star", white(shape(64, star)))

    # Hurt / low-health vignette: transparent centre, pink edges (stretched over the screen).
    h, w = 90, 160
    y, x = np.mgrid[0:h, 0:w]
    dv = np.hypot((x - (w - 1) / 2) / (w / 2), (y - (h - 1) / 2) / (h / 2)) / math.sqrt(2)
    vig = np.zeros((h, w, 4))
    vig[..., :3] = [255, 130, 170]
    vig[..., 3] = np.clip((dv - 0.45) / 0.55, 0, 1) * 255
    save("vignette", vig)

    print("wrote", len(os.listdir(OUT)), "files to", os.path.normpath(OUT))


if __name__ == "__main__":
    main()
