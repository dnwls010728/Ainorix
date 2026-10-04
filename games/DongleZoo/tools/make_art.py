"""Draws the game's art from code: the eleven animal balls, backgrounds, the jar, effect
textures and UI icons. Run from the project folder:

    python tools/make_art.py

Images in tools/art_src/ (hand-made or generated) replace the drawn animals when
tools/import_art.py is run afterwards; this script alone always gives a complete game.
"""
import math
import os

from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
SS = 4  # supersampling factor


def rgb(h, a=255):
    h = h.lstrip("#")
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16), a)


def shade(c, f):
    """Darker (f < 1) or lighter (f > 1) version of a colour."""
    if f <= 1:
        return (int(c[0] * f), int(c[1] * f), int(c[2] * f), c[3])
    t = f - 1
    return (int(c[0] + (255 - c[0]) * t), int(c[1] + (255 - c[1]) * t), int(c[2] + (255 - c[2]) * t), c[3])


def save(img, path, size=None):
    full = os.path.join(ROOT, path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    if size:
        img = img.resize(size, Image.LANCZOS)
    img.save(full)


class Canvas:
    """A supersampled RGBA canvas addressed in final pixels."""

    def __init__(self, w, h, bg=(0, 0, 0, 0)):
        self.w, self.h = w, h
        self.img = Image.new("RGBA", (w * SS, h * SS), bg)
        self.d = ImageDraw.Draw(self.img)

    def layer(self):
        """A transparent layer drawn with alpha, composited by merge()."""
        return Canvas(self.w, self.h)

    def merge(self, other):
        self.img = Image.alpha_composite(self.img, other.img)
        self.d = ImageDraw.Draw(self.img)

    def circle(self, x, y, r, fill, outline=None, width=0):
        self.ellipse(x, y, r, r, fill, outline, width)

    def ellipse(self, x, y, rx, ry, fill, outline=None, width=0):
        box = [(x - rx) * SS, (y - ry) * SS, (x + rx) * SS, (y + ry) * SS]
        self.d.ellipse(box, fill=fill, outline=outline, width=int(width * SS))

    def poly(self, pts, fill, outline=None, width=0):
        p = [(x * SS, y * SS) for x, y in pts]
        self.d.polygon(p, fill=fill)
        if outline:
            self.d.line(p + [p[0]], fill=outline, width=int(width * SS), joint="curve")

    def line(self, pts, fill, width):
        p = [(x * SS, y * SS) for x, y in pts]
        self.d.line(p, fill=fill, width=int(width * SS), joint="curve")
        r = width * SS / 2
        for x, y in (p[0], p[-1]):
            self.d.ellipse([x - r, y - r, x + r, y + r], fill=fill)

    def arc(self, x, y, rx, ry, a0, a1, fill, width):
        box = [(x - rx) * SS, (y - ry) * SS, (x + rx) * SS, (y + ry) * SS]
        self.d.arc(box, a0, a1, fill=fill, width=int(width * SS))

    def rrect(self, x0, y0, x1, y1, r, fill, outline=None, width=0):
        self.d.rounded_rectangle([x0 * SS, y0 * SS, x1 * SS, y1 * SS], radius=r * SS, fill=fill, outline=outline, width=int(width * SS))

    def out(self):
        return self.img.resize((self.w, self.h), Image.LANCZOS)


# ---------------------------------------------------------------- animals

A = 256          # texture size
C = A / 2        # centre
R = 100          # body radius: the physics circle (Sprite size = 2r * A / (2R))
LINE = rgb("#5a3d2b")
LW = 5


def ear_round(c, x, y, r, col, inner=None):
    c.circle(x, y, r, col, LINE, LW)
    if inner:
        c.circle(x, y + 2, r * 0.52, inner)


def ear_point(c, x, y, w, h, lean, col, inner=None):
    pts = [(x - w, y), (x + lean, y - h), (x + w, y)]
    c.poly(pts, col, LINE, LW)
    if inner:
        c.poly([(x - w * 0.5, y - 2), (x + lean * 0.8, y - h * 0.62), (x + w * 0.5, y - 2)], inner)


def face(c, eye_dx=30, eye_y=-4, eye_r=8.5, blush=rgb("#ff9fae", 150), mouth="w", mouth_y=22):
    """Eyes, blush and mouth shared by every animal."""
    for s in (-1, 1):
        ex = C + s * eye_dx
        c.ellipse(ex, C + eye_y, eye_r, eye_r * 1.12, rgb("#33241c"))
        c.circle(ex - eye_r * 0.3, C + eye_y - eye_r * 0.38, eye_r * 0.36, rgb("#ffffff"))
    if blush:
        lay = c.layer()
        for s in (-1, 1):
            lay.ellipse(C + s * (eye_dx + 22), C + eye_y + 20, 13, 8, blush)
        c.merge(lay)
    if mouth == "w":
        c.arc(C - 7, C + mouth_y - 6, 7, 7, 20, 160, LINE, 3.4)
        c.arc(C + 7, C + mouth_y - 6, 7, 7, 20, 160, LINE, 3.4)
    elif mouth == "smile":
        c.arc(C, C + mouth_y - 8, 12, 10, 20, 160, LINE, 3.6)


def body(c, col):
    c.circle(C, C, R, col, LINE, LW)
    lay = c.layer()  # soft shading: darker rim at the bottom right, highlight at the top left
    lay.circle(C, C, R - LW, shade(col, 0.86))
    cut = c.layer()
    cut.circle(C - 7, C - 9, R - LW - 2, (255, 255, 255, 255))
    mask = cut.img.split()[3]
    lay.img.paste((0, 0, 0, 0), (0, 0), mask)
    c.merge(lay)
    hl = c.layer()
    hl.ellipse(C - 44, C - 58, 26, 14, (255, 255, 255, 105))
    hl.img = hl.img.rotate(28, center=((C - 44) * SS, (C - 58) * SS))
    c.merge(hl)


def nose(c, y=10, col=None, w=7, h=5):
    c.ellipse(C, C + y, w, h, col or rgb("#5a3d2b"))


def chick(c):
    col = rgb("#ffd84d")
    c.line([(C - 6, C - R + 4), (C - 12, C - R - 16)], LINE, 5)
    c.line([(C + 4, C - R + 4), (C + 6, C - R - 20)], LINE, 5)
    body(c, col)
    face(c, eye_dx=32, eye_y=-6, mouth=None)
    c.poly([(C - 13, C + 8), (C + 13, C + 8), (C, C + 26)], rgb("#ff9a3c"), LINE, 3.5)
    for s in (-1, 1):
        c.arc(C + s * 70, C + 22, 22, 26, 110 if s < 0 else 310, 250 if s < 0 else 70, shade(col, 0.72), 4)


def hamster(c):
    col = rgb("#ffc27a")
    for s in (-1, 1):
        ear_round(c, C + s * 62, C - 78, 22, col, rgb("#ff9fae"))
    body(c, col)
    lay = c.layer()
    lay.ellipse(C, C + 34, 66, 52, rgb("#fff6e6"))
    cut = c.layer()
    cut.circle(C, C, R - LW, (255, 255, 255, 255))
    lay.img.putalpha(Image.composite(lay.img.split()[3], Image.new("L", lay.img.size, 0), cut.img.split()[3]))
    c.merge(lay)
    face(c, eye_dx=34, eye_y=-10, mouth="w", mouth_y=24)
    nose(c, 8, rgb("#ff8fa0"), 6, 4.5)


def rabbit(c):
    col = rgb("#fff3f3")
    for s in (-1, 1):
        lay = c.layer()
        lay.ellipse(C + s * 34, C - 92, 16, 34, col, LINE, LW)
        lay.ellipse(C + s * 34, C - 90, 7, 22, rgb("#ffb3c0"))
        lay.img = lay.img.rotate(-s * 12, center=((C + s * 34) * SS, (C - 70) * SS))
        c.merge(lay)
    body(c, col)
    face(c, eye_dx=30, eye_y=-2, mouth="w", mouth_y=26)
    nose(c, 10, rgb("#ff8fa0"), 6, 4.5)


def cat(c):
    col = rgb("#aab8d6")
    for s in (-1, 1):
        ear_point(c, C + s * 56, C - 66, 28, 52, s * 12, col, rgb("#ffb3c0"))
    body(c, col)
    face(c, eye_dx=32, eye_y=-4, mouth="w", mouth_y=24)
    nose(c, 9, rgb("#ff8fa0"), 6, 4.5)
    for s in (-1, 1):
        for dy in (-5, 7):
            c.line([(C + s * 52, C + 14 + dy * 0.6), (C + s * 82, C + 10 + dy * 1.6)], LINE, 2.6)
    for i in (-1, 0, 1):
        c.line([(C + i * 14, C - R + 12), (C + i * 14, C - R + 30)], shade(col, 0.78), 5)


def dog(c):
    col = rgb("#f2cf9a")
    ear = rgb("#a9744b")
    body(c, col)
    for s in (-1, 1):
        lay = c.layer()
        lay.ellipse(C + s * 74, C - 34, 22, 46, ear, LINE, LW)
        lay.img = lay.img.rotate(s * 18, center=((C + s * 74) * SS, (C - 60) * SS))
        c.merge(lay)
    c.ellipse(C + 34, C - 10, 24, 28, rgb("#c9955f"))
    c.ellipse(C, C + 26, 34, 26, rgb("#fff6e6"))
    face(c, eye_dx=34, eye_y=-8, mouth="w", mouth_y=36)
    nose(c, 14, None, 10, 7.5)


def fox(c):
    col = rgb("#ff9a4a")
    for s in (-1, 1):
        ear_point(c, C + s * 54, C - 64, 30, 60, s * 10, col, rgb("#5a3d2b"))
    body(c, col)
    lay = c.layer()
    lay.poly([(C - 92, C + 16), (C, C + 2), (C + 92, C + 16), (C + 80, C + 110), (C - 80, C + 110)], rgb("#fff6e6"))
    lay.circle(C, C + 44, 52, rgb("#fff6e6"))
    cut = c.layer()
    cut.circle(C, C, R - LW, (255, 255, 255, 255))
    lay.img.putalpha(Image.composite(lay.img.split()[3], Image.new("L", lay.img.size, 0), cut.img.split()[3]))
    c.merge(lay)
    face(c, eye_dx=34, eye_y=-10, mouth="w", mouth_y=30)
    nose(c, 12, None, 8, 6)


def panda(c):
    col = rgb("#ffffff")
    dark = rgb("#3b3038")
    for s in (-1, 1):
        ear_round(c, C + s * 66, C - 72, 26, dark)
    body(c, col)
    for s in (-1, 1):
        lay = c.layer()
        lay.ellipse(C + s * 36, C - 6, 22, 27, dark)
        lay.img = lay.img.rotate(s * 22, center=((C + s * 36) * SS, (C - 6) * SS))
        c.merge(lay)
    for s in (-1, 1):
        c.circle(C + s * 34, C - 6, 9, rgb("#ffffff"))
        c.circle(C + s * 34, C - 5, 6, rgb("#33241c"))
        c.circle(C + s * 34 - 2, C - 8, 2.2, rgb("#ffffff"))
    lay = c.layer()
    for s in (-1, 1):
        lay.ellipse(C + s * 62, C + 26, 13, 8, rgb("#ff9fae", 150))
    c.merge(lay)
    nose(c, 20, dark, 9, 6.5)
    c.arc(C - 7, C + 30, 7, 7, 20, 160, LINE, 3.4)
    c.arc(C + 7, C + 30, 7, 7, 20, 160, LINE, 3.4)


def bear(c):
    col = rgb("#b98358")
    for s in (-1, 1):
        ear_round(c, C + s * 64, C - 72, 27, col, rgb("#e8c39a"))
    body(c, col)
    c.ellipse(C, C + 26, 40, 31, rgb("#f0d6b3"))
    face(c, eye_dx=38, eye_y=-10, mouth="w", mouth_y=38, blush=rgb("#ff8f8f", 130))
    nose(c, 14, None, 11, 8)


def lion(c):
    # The mane stays inside the physics circle (R), so lions that look like they touch do touch.
    mane = rgb("#e8792c")
    for i in range(12):
        a = i / 12 * math.tau
        c.circle(C + math.cos(a) * 80, C + math.sin(a) * 80, 22, mane, LINE, LW)
    c.circle(C, C, 84, mane)
    col = rgb("#ffd27a")
    for s in (-1, 1):
        c.circle(C + s * 46, C - 50, 14, col, LINE, LW)
    c.circle(C, C, 66, col, LINE, LW)
    for s in (-1, 1):
        c.circle(C + s * 46, C - 50, 9.5, col)
    c.ellipse(C, C + 24, 27, 21, rgb("#fff6e6"))
    face(c, eye_dx=27, eye_y=-9, mouth="w", mouth_y=31, eye_r=7.5)
    nose(c, 12, None, 8.5, 6)


def elephant(c):
    col = rgb("#b9c7e0")
    for s in (-1, 1):
        c.ellipse(C + s * 70, C - 8, 33, 50, col, LINE, LW)
        c.ellipse(C + s * 73, C - 6, 19, 35, rgb("#ffc2cf"))
    body_r = 78
    c.circle(C, C, body_r, col, LINE, LW)
    lay = c.layer()
    lay.ellipse(C - 34, C - 46, 20, 11, (255, 255, 255, 105))
    lay.img = lay.img.rotate(28, center=((C - 34) * SS, (C - 46) * SS))
    c.merge(lay)
    face(c, eye_dx=37, eye_y=-14, mouth=None, eye_r=7.5)
    c.rrect(C - 14, C - 6, C + 14, C + 58, 14, col, LINE, LW)
    for y in (20, 33, 46):
        c.line([(C - 8, C + y), (C + 8, C + y)], shade(col, 0.8), 3)


def whale(c):
    col = rgb("#5aa9ff")
    c.line([(C, C - R + 6), (C, C - R - 12)], rgb("#8fd3ff"), 6)
    c.arc(C - 14, C - R - 12, 14, 14, 180, 360, rgb("#8fd3ff"), 6)
    c.arc(C + 14, C - R - 12, 14, 14, 180, 360, rgb("#8fd3ff"), 6)
    body(c, col)
    lay = c.layer()
    lay.ellipse(C, C + 78, 104, 64, rgb("#eaf6ff"))
    cut = c.layer()
    cut.circle(C, C, R - LW, (255, 255, 255, 255))
    lay.img.putalpha(Image.composite(lay.img.split()[3], Image.new("L", lay.img.size, 0), cut.img.split()[3]))
    c.merge(lay)
    for i in (-2, -1, 0, 1, 2):
        c.line([(C + i * 20, C + 44), (C + i * 21, C + 86 - abs(i) * 9)], shade(rgb("#eaf6ff"), 0.86), 3)
    face(c, eye_dx=46, eye_y=-12, mouth="smile", mouth_y=22, eye_r=9)


ANIMALS = [chick, hamster, rabbit, cat, dog, fox, panda, bear, lion, elephant, whale]


def animals():
    for i, fn in enumerate(ANIMALS):
        c = Canvas(A, A)
        fn(c)
        img = c.out()
        save(img, "assets/animals/%02d.png" % (i + 1))
        # locked look for the collection: a flat silhouette
        sil = Image.new("RGBA", img.size, (120, 104, 92, 255))
        sil.putalpha(img.split()[3])
        save(sil, "assets/animals/%02d_locked.png" % (i + 1))


# ---------------------------------------------------------------- backgrounds

def gradient(w, h, top, bottom):
    img = Image.new("RGBA", (w, h))
    px = img.load()
    for y in range(h):
        t = y / (h - 1)
        col = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(4))
        for x in range(w):
            px[x, y] = col
    return img


def backgrounds():
    W, H = 1024, 1024

    def finish(img, name):
        save(img, "assets/bg/%s.png" % name)
        save(img.resize((256, 256), Image.LANCZOS), "assets/bg/%s_thumb.png" % name)

    # meadow: sky, clouds, rolling hills
    img = gradient(W, H, rgb("#8fd8ff"), rgb("#e9fbff"))
    d = ImageDraw.Draw(img, "RGBA")
    for cx, cy, s in ((150, 170, 1.0), (520, 110, 0.8), (860, 220, 1.1), (330, 330, 0.7), (760, 420, 0.6)):
        for dx, dy, r in ((-50, 10, 34), (-15, -12, 46), (30, -4, 40), (62, 14, 28)):
            d.ellipse([cx + (dx - r) * s, cy + (dy - r) * s, cx + (dx + r) * s, cy + (dy + r) * s], fill=(255, 255, 255, 215))
    d.ellipse([-300, 640, 620, 1500], fill=rgb("#a6e37c"))
    d.ellipse([380, 700, 1400, 1600], fill=rgb("#8fd66a"))
    d.ellipse([-200, 800, 1300, 1900], fill=rgb("#7cc95c"))
    for i in range(40):
        x, y = (i * 137) % W, 840 + (i * 71) % 170
        d.ellipse([x - 4, y - 4, x + 4, y + 4], fill=rgb("#fff2a8") if i % 3 else rgb("#ffffff"))
    finish(img, "meadow")

    # beach at sunset
    img = gradient(W, H, rgb("#ffb37a"), rgb("#ffe9c7"))
    d = ImageDraw.Draw(img, "RGBA")
    d.ellipse([362, 330, 662, 630], fill=rgb("#fff3c4"))
    d.rectangle([0, 560, W, H], fill=rgb("#6ec6d8"))
    for i in range(9):
        y = 590 + i * 28
        d.line([(60 + (i * 97) % 300, y), (360 + (i * 151) % 500, y)], fill=(255, 255, 255, 120), width=5)
    d.ellipse([-200, 780, 1300, 1500], fill=rgb("#ffe3a6"))
    for i in range(30):
        x, y = (i * 173) % W, 860 + (i * 59) % 150
        d.ellipse([x - 3, y - 3, x + 3, y + 3], fill=rgb("#f2c97a"))
    finish(img, "beach")

    # starry night
    img = gradient(W, H, rgb("#1d2350"), rgb("#56408a"))
    d = ImageDraw.Draw(img, "RGBA")
    for i in range(130):
        x, y = (i * 211 + 37) % W, (i * 127 + 11) % 760
        r = 1.5 + (i * 7 % 5) * 0.6
        d.ellipse([x - r, y - r, x + r, y + r], fill=(255, 250, 220, 150 + (i * 13) % 100))
    d.ellipse([720, 90, 880, 250], fill=rgb("#fff3c4"))
    d.ellipse([690, 70, 830, 210], fill=rgb("#232a5c"))
    d.ellipse([-300, 700, 620, 1500], fill=rgb("#2f2a66"))
    d.ellipse([380, 760, 1400, 1600], fill=rgb("#28235a"))
    d.ellipse([-200, 860, 1300, 1900], fill=rgb("#211d4e"))
    finish(img, "night")


# ---------------------------------------------------------------- jar and effects

def jar():
    # Interior 6.6 x 9.9 world units at 100 px per unit, plus a 30 px rim all around.
    w, h, pad = 660, 990, 30
    c = Canvas(w + pad * 2, h + pad * 2)
    c.rrect(pad - 16, -60, pad + w + 16, pad + h + 16, 44, (255, 255, 255, 70), (255, 255, 255, 235), 14)
    lay = c.layer()
    lay.rrect(pad + 14, 40, pad + 40, pad + h - 60, 13, (255, 255, 255, 70))
    c.merge(lay)
    img = c.out()
    img.paste((255, 255, 255, 0), (0, 0, img.width, 3))  # open top: nothing for bilinear sampling to wrap to the bottom edge
    save(img, "assets/fx/jar.png")


def effects():
    c = Canvas(64, 64)
    c.circle(32, 32, 30, (255, 255, 255, 255))
    save(c.out(), "assets/fx/disc.png")

    n = 128
    img = Image.new("RGBA", (n, n))
    px = img.load()
    for y in range(n):
        for x in range(n):
            dist = math.hypot(x - n / 2 + 0.5, y - n / 2 + 0.5) / (n / 2)
            a = max(0.0, 1 - dist)
            px[x, y] = (255, 255, 255, int(255 * a * a))
    save(img, "assets/fx/glow.png")

    c = Canvas(128, 128)
    c.circle(64, 64, 58, None, (255, 255, 255, 255), 9)
    save(c.out(), "assets/fx/ring.png")

    c = Canvas(96, 96)
    pts = []
    for i in range(10):
        a = -math.pi / 2 + i * math.pi / 5
        r = 44 if i % 2 == 0 else 19
        pts.append((48 + math.cos(a) * r, 48 + math.sin(a) * r))
    c.poly(pts, (255, 255, 255, 255))
    save(c.out(), "assets/fx/star.png")

    # dashed danger line, one dash per 64 px, tiled by the sprite width
    c = Canvas(512, 16)
    for i in range(8):
        c.rrect(i * 64 + 8, 3, i * 64 + 48, 13, 5, (255, 255, 255, 255))
    save(c.out(), "assets/fx/dash.png")

    # aiming guide: a vertical dotted line
    c = Canvas(16, 512)
    for i in range(16):
        c.circle(8, i * 32 + 16, 4.5, (255, 255, 255, 255))
    save(c.out(), "assets/fx/guide.png")

    c = Canvas(160, 40)
    c.ellipse(80, 20, 76, 16, (0, 0, 0, 90))
    save(c.out().filter(ImageFilter.GaussianBlur(3)), "assets/fx/shadow.png")


# ---------------------------------------------------------------- UI icons

W_ = (255, 255, 255, 255)


def icon(name, draw, size=96):
    c = Canvas(size, size)
    draw(c)
    save(c.out(), "assets/ui/%s.png" % name)


def icons():
    def coin(c):
        c.circle(48, 48, 42, rgb("#ffc93c"), rgb("#b9791a"), 5)
        c.circle(48, 48, 30, rgb("#ffdd6e"), rgb("#e0a52c"), 3)
        pts = []
        for i in range(10):
            a = -math.pi / 2 + i * math.pi / 5
            r = 19 if i % 2 == 0 else 8.5
            pts.append((48 + math.cos(a) * r, 49 + math.sin(a) * r))
        c.poly(pts, rgb("#fff6c9"))

    def hammer(c):
        lay = c.layer()
        lay.rrect(42, 34, 54, 90, 6, rgb("#c98a4b"), LINE, 4)
        lay.rrect(14, 12, 82, 46, 12, rgb("#ff6b6b"), LINE, 4)
        lay.rrect(14, 12, 30, 46, 8, rgb("#ffd84d"), LINE, 4)
        lay.rrect(66, 12, 82, 46, 8, rgb("#ffd84d"), LINE, 4)
        lay.img = lay.img.rotate(28, center=(48 * SS, 48 * SS), resample=Image.BICUBIC)
        c.merge(lay)

    def shake(c):
        c.rrect(28, 26, 68, 84, 12, rgb("#8fd3ff"), LINE, 4)
        c.rrect(34, 14, 62, 28, 6, rgb("#ffd84d"), LINE, 4)
        for s in (-1, 1):
            c.arc(48 + s * 30, 54, 10, 22, 300 if s > 0 else 120, 60 if s > 0 else 240, rgb("#ff9a3c"), 5)
            c.arc(48 + s * 40, 54, 10, 30, 300 if s > 0 else 120, 60 if s > 0 else 240, rgb("#ff9a3c"), 5)

    def wand(c):
        c.line([(22, 78), (58, 42)], LINE, 13)
        c.line([(22, 78), (58, 42)], rgb("#b98358"), 7)
        pts = []
        for i in range(10):
            a = -math.pi / 2 + i * math.pi / 5
            r = 24 if i % 2 == 0 else 10.5
            pts.append((64 + math.cos(a) * r, 34 + math.sin(a) * r))
        c.poly(pts, rgb("#ffd84d"), LINE, 4)
        c.circle(26, 30, 5, rgb("#fff3a8"))
        c.circle(82, 70, 4, rgb("#fff3a8"))

    def pause(c):
        c.rrect(26, 22, 42, 74, 6, W_)
        c.rrect(54, 22, 70, 74, 6, W_)

    def gear(c):
        for i in range(8):
            a = i * math.pi / 4
            c.line([(48 + math.cos(a) * 24, 48 + math.sin(a) * 24), (48 + math.cos(a) * 36, 48 + math.sin(a) * 36)], W_, 13)
        c.circle(48, 48, 28, W_)
        c.circle(48, 48, 11, (0, 0, 0, 0))
        ImageDraw.Draw(c.img).ellipse([37 * SS, 37 * SS, 59 * SS, 59 * SS], fill=(0, 0, 0, 0))

    def shop(c):
        c.rrect(18, 40, 78, 80, 8, W_)
        c.poly([(12, 40), (22, 18), (74, 18), (84, 40)], W_)
        for i in range(3):
            c.line([(30 + i * 18, 20), (27 + i * 21, 40)], (0, 0, 0, 40), 3)
        ImageDraw.Draw(c.img).rounded_rectangle([40 * SS, 54 * SS, 56 * SS, 80 * SS], radius=5 * SS, fill=(0, 0, 0, 0))

    def book(c):
        c.rrect(16, 20, 46, 78, 6, W_)
        c.rrect(50, 20, 80, 78, 6, W_)
        for y in (34, 46, 58):
            ImageDraw.Draw(c.img).line([(24 * SS, y * SS), (38 * SS, y * SS)], fill=(0, 0, 0, 0), width=4 * SS)
            ImageDraw.Draw(c.img).line([(58 * SS, y * SS), (72 * SS, y * SS)], fill=(0, 0, 0, 0), width=4 * SS)

    def gift(c):
        c.rrect(18, 44, 78, 82, 7, W_)
        c.rrect(12, 28, 84, 44, 6, W_)
        ImageDraw.Draw(c.img).rectangle([43 * SS, 28 * SS, 53 * SS, 82 * SS], fill=(0, 0, 0, 0))
        c.arc(36, 22, 12, 9, 0, 360, W_, 6)
        c.arc(60, 22, 12, 9, 0, 360, W_, 6)

    def tasks(c):
        c.rrect(18, 14, 78, 84, 9, W_)
        d = ImageDraw.Draw(c.img)
        for y in (32, 50, 68):
            d.line([(28 * SS, y * SS), (33 * SS, (y + 5) * SS), (41 * SS, (y - 5) * SS)], fill=(0, 0, 0, 0), width=4 * SS)
            d.line([(48 * SS, y * SS), (68 * SS, y * SS)], fill=(0, 0, 0, 0), width=4 * SS)

    def play(c):
        c.poly([(32, 20), (78, 48), (32, 76)], W_, W_, 8)

    def home(c):
        c.poly([(48, 14), (86, 48), (10, 48)], W_, W_, 6)
        c.rrect(22, 44, 74, 82, 6, W_)
        ImageDraw.Draw(c.img).rounded_rectangle([40 * SS, 58 * SS, 56 * SS, 82 * SS], radius=4 * SS, fill=(0, 0, 0, 0))

    def retry(c):
        c.arc(48, 50, 28, 28, 300, 230, W_, 10)
        c.poly([(52, 8), (80, 24), (52, 40)], W_)

    def lock(c):
        c.arc(48, 38, 18, 20, 180, 360, W_, 9)
        c.rrect(22, 40, 74, 84, 9, W_)
        ImageDraw.Draw(c.img).ellipse([42 * SS, 54 * SS, 54 * SS, 66 * SS], fill=(0, 0, 0, 0))

    def ad(c):
        c.rrect(10, 22, 86, 74, 12, W_)
        d = ImageDraw.Draw(c.img)
        d.polygon([(40 * SS, 34 * SS), (62 * SS, 48 * SS), (40 * SS, 62 * SS)], fill=(0, 0, 0, 0))

    def crown(c):
        c.poly([(14, 72), (10, 30), (32, 50), (48, 20), (64, 50), (86, 30), (82, 72)], rgb("#ffd84d"), rgb("#b9791a"), 4)
        c.rrect(14, 68, 82, 82, 5, rgb("#ffc93c"), rgb("#b9791a"), 4)

    def check(c):
        c.line([(20, 50), (40, 70), (76, 28)], W_, 13)

    def sound(c):
        c.poly([(14, 38), (32, 38), (52, 20), (52, 76), (32, 58), (14, 58)], W_)
        c.arc(54, 48, 14, 16, 300, 60, W_, 6)
        c.arc(56, 48, 26, 28, 300, 60, W_, 6)

    def music(c):
        c.line([(36, 70), (36, 22), (74, 14), (74, 62)], W_, 7)
        c.ellipse(27, 70, 13, 10, W_)
        c.ellipse(65, 62, 13, 10, W_)

    def noads(c):
        c.rrect(14, 26, 82, 70, 10, W_)
        c.circle(48, 48, 40, None, rgb("#ff5a5a"), 8)
        c.line([(20, 76), (76, 20)], rgb("#ff5a5a"), 8)

    for name, fn in (("coin", coin), ("hammer", hammer), ("shake", shake), ("wand", wand), ("pause", pause), ("gear", gear),
                     ("shop", shop), ("book", book), ("gift", gift), ("tasks", tasks), ("play", play), ("home", home),
                     ("retry", retry), ("lock", lock), ("ad", ad), ("crown", crown), ("check", check), ("sound", sound),
                     ("music", music), ("noads", noads)):
        icon(name, fn)

    # logo plate: a soft cloud shape behind the title text
    c = Canvas(640, 260)
    for x, y, r in ((120, 150, 90), (230, 110, 105), (360, 100, 100), (480, 120, 95), (540, 160, 80), (320, 170, 85)):
        c.circle(x, y, r, (255, 255, 255, 235))
    save(c.out(), "assets/ui/cloud.png")

    # app icon (512): panda and chick on a sky gradient; tools/import_art.py may replace it
    bg = gradient(512, 512, rgb("#8fd8ff"), rgb("#d9fbe8"))
    panda_img = Image.open(os.path.join(ROOT, "assets/animals/07.png")).resize((400, 400), Image.LANCZOS)
    chick_img = Image.open(os.path.join(ROOT, "assets/animals/01.png")).resize((230, 230), Image.LANCZOS)
    bg.alpha_composite(panda_img, (20, 40))
    bg.alpha_composite(chick_img, (270, 260))
    save(bg, "assets/icon.png")


if __name__ == "__main__":
    animals()
    backgrounds()
    jar()
    effects()
    icons()
    print("art written")
