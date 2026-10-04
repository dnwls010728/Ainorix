"""Installs the illustrated animals from tools/art_src/codex/ (generated with Codex image
generation: one square PNG per animal with a transparent background, plus icon.png) as
the game's textures. Run from the project folder, after tools/make_art.py:

    python tools/import_art.py

Each image is fitted so the animal's round body becomes the physics circle: the body is
found by removing thin parts (ears, tufts, spouts) from the alpha mask, then the picture is
scaled and centred to put that circle at radius 200 in a 512 px texture (the same
proportions as the drawn animals, at twice the resolution for large phone screens). Animals whose features reach far out (elephant ears) are scaled down to fit.
"""
import os

from PIL import Image, ImageFilter

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
SRC = os.path.join(ROOT, "tools", "art_src", "codex")
NAMES = ["chick", "hamster", "rabbit", "cat", "dog", "fox", "panda", "bear", "lion", "elephant", "whale"]
SIZE, BODY, LIMIT = 512, 200, 252  # texture size, body radius, furthest pixel from the centre (twice the drawn set)
WHOLE = {"lion"}  # the mane is part of the ball: the whole silhouette is the body


def body_circle(alpha):
    """Centre and radius of the round body in the image's own pixels."""
    n = 160
    small = alpha.resize((n, n), Image.LANCZOS).point(lambda v: 255 if v > 128 else 0)
    k = 49  # opening: anything narrower than ~30 % of the image is not the body
    opened = small.filter(ImageFilter.MinFilter(k)).filter(ImageFilter.MaxFilter(k))
    box = opened.getbbox()
    if not box:
        box = small.getbbox()
    f = alpha.width / n
    cx, cy = (box[0] + box[2]) / 2 * f, (box[1] + box[3]) / 2 * f
    r = ((box[2] - box[0]) + (box[3] - box[1])) / 4 * f
    return cx, cy, r


def install(name, index):
    img = Image.open(os.path.join(SRC, name + ".png")).convert("RGBA")
    alpha = img.getchannel("A").point(lambda v: 0 if v < 24 else v)  # drop stray haze
    img.putalpha(alpha)
    cx, cy, r = body_circle(alpha)
    box = alpha.getbbox()
    if name in WHOLE:
        cx, cy, r = (box[0] + box[2]) / 2, (box[1] + box[3]) / 2, ((box[2] - box[0]) + (box[3] - box[1])) / 4
    extent = max(cx - box[0], box[2] - cx, cy - box[1], box[3] - cy)
    scale = min(BODY / r, LIMIT / extent)
    w, h = round(img.width * scale), round(img.height * scale)
    scaled = img.resize((w, h), Image.LANCZOS)
    out = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    out.alpha_composite(scaled, (round(SIZE / 2 - cx * scale), round(SIZE / 2 - cy * scale)))
    out.save(os.path.join(ROOT, "assets", "animals", "%02d.png" % index))
    locked = Image.new("RGBA", out.size, (120, 104, 92, 255))
    locked.putalpha(out.getchannel("A"))
    locked.save(os.path.join(ROOT, "assets", "animals", "%02d_locked.png" % index))
    return round(r * scale, 1)


if __name__ == "__main__":
    for i, name in enumerate(NAMES):
        print("%-9s body radius %s px" % (name, install(name, i + 1)))
    icon = os.path.join(SRC, "icon.png")
    if os.path.exists(icon):
        Image.open(icon).convert("RGB").resize((512, 512), Image.LANCZOS).save(os.path.join(ROOT, "assets", "icon.png"))
        print("icon installed")
