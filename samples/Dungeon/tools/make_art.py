"""Generates the pixel art of the Dungeon sample (no dependencies).

    python tools/make_art.py      (from samples/Dungeon)

Output:
  assets/tiles/dungeon.png   16x16 tiles, 8 columns x 9 rows:
      0-46   wall, autotile "blob" (47 neighbour patterns, see BLOB_MASKS)
      47     (empty)
      48-51  floor variants
      52-67  water, autotile "sides" (16 patterns: N=1 E=2 S=4 W=8)
      68-70  mossy / cracked floor variants
      71     stairs down (exit)
  assets/sprites/*.png  hero, slime, coin, crate, bolt, pillar, rock, heart

The rock sprite is rasterized from ROCK_POLYGON, the same outline the scene
uses for its Collider2D polygon, so the collision matches the picture.
"""
import math
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


# ----- Canvas -----------------------------------------------------------------------
class Canvas:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = [[None] * w for _ in range(h)]

    def set(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y][x] = c

    def get(self, x, y):
        return self.px[y][x] if 0 <= x < self.w and 0 <= y < self.h else None

    def rect(self, x, y, w, h, c):
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                self.set(xx, yy, c)

    def blit(self, other, ox, oy):
        for y in range(other.h):
            for x in range(other.w):
                if other.px[y][x] is not None:
                    self.set(ox + x, oy + y, other.px[y][x])

    def outline(self, c=(24, 20, 34)):
        src = [row[:] for row in self.px]
        for y in range(self.h):
            for x in range(self.w):
                if src[y][x] is not None:
                    continue
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    nx, ny = x + dx, y + dy
                    if 0 <= nx < self.w and 0 <= ny < self.h and src[ny][nx] not in (None, c):
                        self.px[y][x] = c
                        break
        return self

    def flip_x(self):
        out = Canvas(self.w, self.h)
        for y in range(self.h):
            out.px[y] = self.px[y][::-1]
        return out

    def save(self, rel):
        raw = bytearray()
        for row in self.px:
            raw.append(0)
            for c in row:
                raw.extend(bytes(c + (255,)) if c else b"\x00\x00\x00\x00")
        def chunk(tag, data):
            return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", self.w, self.h, 8, 6, 0, 0, 0))
        png += chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
        path = os.path.join(ROOT, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(png)
        print("wrote", rel, f"{self.w}x{self.h}")


def sheet(frames, columns, w=16, h=16):
    rows = (len(frames) + columns - 1) // columns
    out = Canvas(w * columns, h * rows)
    for i, f in enumerate(frames):
        if f is not None:
            out.blit(f, (i % columns) * w, (i // columns) * h)
    return out


def rnd(seed):
    """Tiny deterministic generator (same art on every machine)."""
    state = [seed * 2654435761 % 4294967296 or 1]
    def next_int(n):
        state[0] = (state[0] * 1103515245 + 12345) % 2147483648
        return state[0] % n
    return next_int


def shade(c, k):
    return tuple(max(0, min(255, int(v * k))) for v in c)


OUTLINE = (24, 20, 34)
STONE = (104, 108, 132)
STONE_LIGHT = (148, 152, 176)
STONE_DARK = (62, 62, 84)
GROUT = (80, 82, 104)
FLOOR = (58, 54, 70)
FLOOR_LIGHT = (72, 68, 86)
FLOOR_DARK = (44, 40, 54)
MOSS = (74, 120, 70)
WATER = (40, 92, 170)
WATER_LIGHT = (90, 150, 220)
WATER_DARK = (28, 64, 128)

# ----- Autotile masks ------------------------------------------------------------------
N, NE, E, SE, S, SW, W, NW = 1, 2, 4, 8, 16, 32, 64, 128


def clean(m):
    if not (m & N and m & E): m &= ~NE
    if not (m & S and m & E): m &= ~SE
    if not (m & S and m & W): m &= ~SW
    if not (m & N and m & W): m &= ~NW
    return m


BLOB_MASKS = [m for m in range(256) if clean(m) == m]
assert len(BLOB_MASKS) == 47


def wall(mask):
    """Top-down wall block: brick top, lit north/west rims, a dark south face."""
    t = Canvas(16, 16)
    t.rect(0, 0, 16, 16, STONE)
    for y in range(16):                       # bricks
        off = 0 if (y // 4) % 2 == 0 else 4
        for x in range(16):
            if y % 4 == 3 or (x + off) % 8 == 7:
                t.set(x, y, GROUT)
    if not mask & N:
        t.rect(0, 0, 16, 1, OUTLINE); t.rect(0, 1, 16, 2, STONE_LIGHT)
    if not mask & W:
        t.rect(0, 0, 1, 16, OUTLINE); t.rect(1, 0, 1, 16, STONE_LIGHT)
    if not mask & E:
        t.rect(15, 0, 1, 16, OUTLINE); t.rect(14, 0, 1, 16, STONE_DARK)
    if not mask & S:
        t.rect(0, 11, 16, 4, STONE_DARK)       # face toward the viewer
        for x in range(0, 16, 4):
            t.rect(x, 12, 1, 3, OUTLINE)
        t.rect(0, 15, 16, 1, OUTLINE)
    # Rounded outer corners (both sides open).
    for (sx, sy, cx, cy) in ((N, W, 0, 0), (N, E, 15, 0), (S, W, 0, 15), (S, E, 15, 15)):
        if not mask & sx and not mask & sy:
            t.set(cx, cy, None)
            t.set(cx + (1 if cx == 0 else -1), cy, OUTLINE)
            t.set(cx, cy + (1 if cy == 0 else -1), OUTLINE)
    # Inner corners: both sides connected, the diagonal open.
    for (a, b, d, cx, cy) in ((N, E, NE, 14, 0), (N, W, NW, 0, 0), (S, E, SE, 14, 13), (S, W, SW, 0, 13)):
        if mask & a and mask & b and not mask & d:
            t.rect(cx, cy, 2, 3 if cy else 2, OUTLINE if cy == 0 else STONE_DARK)
    return t


def floor(seed, moss=False, cracked=False):
    t = Canvas(16, 16)
    t.rect(0, 0, 16, 16, FLOOR)
    r = rnd(seed)
    for y in range(16):
        for x in range(16):
            if x % 8 == 0 or y % 8 == 0:
                t.set(x, y, FLOOR_DARK)
            elif (x % 8 == 1 or y % 8 == 1) and r(3) == 0:
                t.set(x, y, FLOOR_LIGHT)
    for _ in range(5):
        t.set(r(16), r(16), FLOOR_LIGHT if r(2) else FLOOR_DARK)
    if moss:
        for _ in range(26):
            x, y = r(16), r(16)
            t.set(x, y, MOSS)
            t.set(x + 1, y, shade(MOSS, 0.8))
    if cracked:
        x, y = 3 + r(4), 2
        while y < 14:
            t.set(x, y, OUTLINE)
            x += r(3) - 1
            y += 1
    return t


def water(mask, frame_seed):
    t = Canvas(16, 16)
    t.rect(0, 0, 16, 16, WATER)
    r = rnd(frame_seed)
    for y in range(2, 16, 4):                  # ripples
        x0 = r(8)
        t.rect(x0, y, 4, 1, WATER_LIGHT)
        t.rect((x0 + 8) % 12, y + 2, 3, 1, WATER_DARK)
    rim = STONE_LIGHT
    if not mask & 1:
        t.rect(0, 0, 16, 2, rim); t.rect(0, 2, 16, 1, WATER_DARK)
    if not mask & 4:
        t.rect(0, 14, 16, 2, rim)
    if not mask & 8:
        t.rect(0, 0, 2, 16, rim)
    if not mask & 2:
        t.rect(14, 0, 2, 16, rim)
    return t


def stairs():
    t = Canvas(16, 16)
    t.rect(0, 0, 16, 16, FLOOR_DARK)
    for i in range(5):
        c = shade(STONE, 1.2 - i * 0.16)
        t.rect(1 + i, 1 + i * 3, 14 - 2 * i, 3, c)
        t.rect(1 + i, 3 + i * 3, 14 - 2 * i, 1, OUTLINE)
    t.rect(6, 13, 4, 3, (10, 8, 16))
    return t


tiles = [wall(m) for m in BLOB_MASKS]
tiles.append(None)
tiles += [floor(s) for s in (11, 23, 37, 51)]
tiles += [water(m, 70 + m) for m in range(16)]
tiles += [floor(91, moss=True), floor(97, moss=True), floor(103, cracked=True), stairs()]
assert len(tiles) == 72
sheet(tiles, 8).save("assets/tiles/dungeon.png")


# ----- Sprites -----------------------------------------------------------------------------
ROBE, ROBE_DARK = (120, 70, 190), (80, 44, 140)
SKIN, HAT = (247, 196, 145), (70, 50, 150)
GOLD = (255, 214, 64)


def hero(facing, step):
    """facing: down, up, side (looks right). step: 0 idle, 1 walk."""
    c = Canvas(16, 16)
    bob = 1 if step else 0
    c.rect(4, 7 + bob, 8, 7, ROBE)                          # robe
    c.rect(4, 12 + bob, 8, 2, ROBE_DARK)
    if step:
        c.rect(4, 14, 3, 1, OUTLINE); c.rect(9, 13, 3, 1, OUTLINE)
    else:
        c.rect(5, 14, 2, 1, OUTLINE); c.rect(9, 14, 2, 1, OUTLINE)
    c.rect(5, 3 + bob, 6, 5, SKIN)                          # head
    c.rect(3, 2 + bob, 10, 2, HAT)                          # hat brim
    c.rect(5, 0 + bob, 6, 2, HAT)
    c.set(7, bob, GOLD)
    if facing == "down":
        c.set(6, 5 + bob, OUTLINE); c.set(9, 5 + bob, OUTLINE)
        c.rect(7, 8 + bob, 2, 3, GOLD)                      # clasp
    elif facing == "up":
        c.rect(5, 3 + bob, 6, 4, HAT)                       # back of the hat / hair
    else:
        c.set(9, 5 + bob, OUTLINE)
        c.rect(11, 8 + bob, 3, 2, SKIN)                     # hand forward
        c.rect(13, 5 + bob, 1, 6, (150, 110, 60))           # staff
        c.set(13, 4 + bob, (120, 220, 255))
    return c.outline()


hero_frames = [hero(f, s) for f in ("down", "up", "side") for s in (0, 1)]
sheet(hero_frames, 6).save("assets/sprites/hero.png")

SLIME, SLIME_DARK, SLIME_LIGHT = (96, 200, 96), (48, 132, 64), (170, 240, 150)


def slime(squash, hurt=False):
    c = Canvas(16, 16)
    body = (230, 90, 90) if hurt else SLIME
    dark = (160, 50, 60) if hurt else SLIME_DARK
    top = 5 + squash
    for y in range(top, 15):
        half = int(6 * math.sqrt(max(0.0, 1 - ((y - 14) / (15 - top)) ** 2))) + (1 if y > 11 else 0)
        c.rect(8 - half, y, half * 2, 1, body)
    c.rect(3, 13, 10, 2, dark)
    c.rect(6, top + 1, 2, 1, SLIME_LIGHT)
    c.set(6, top + 4, OUTLINE); c.set(10, top + 4, OUTLINE)
    return c.outline()


sheet([slime(0), slime(2), slime(1, hurt=True)], 3).save("assets/sprites/slime.png")


def coin(width):
    c = Canvas(16, 16)
    for y in range(3, 13):
        dy = (y - 7.5) / 5.0
        half = int(round(width * math.sqrt(max(0.0, 1 - dy * dy))))
        if half > 0:
            c.rect(8 - half, y, half * 2, 1, GOLD)
    if width > 2:
        c.rect(7, 5, 2, 6, (222, 150, 30))
    return c.outline()


sheet([coin(w) for w in (5, 3, 1, 3)], 4).save("assets/sprites/coin.png")


def crate():
    c = Canvas(16, 16)
    wood, dark, light = (170, 116, 64), (116, 76, 42), (206, 156, 96)
    c.rect(1, 1, 14, 14, wood)
    c.rect(1, 1, 14, 2, light); c.rect(1, 13, 14, 2, dark)
    c.rect(1, 1, 2, 14, light); c.rect(13, 1, 2, 14, dark)
    for i in range(3, 13):
        c.set(i, i, dark); c.set(i, 15 - i, dark)
    return c.outline()


crate().save("assets/sprites/crate.png")


def bolt(phase):
    c = Canvas(8, 8)
    core, glow = (220, 250, 255), (90, 190, 255)
    r = 3 if phase else 2.5
    for y in range(8):
        for x in range(8):
            d = math.hypot(x - 3.5, y - 3.5)
            if d < r - 1.2:
                c.set(x, y, core)
            elif d < r:
                c.set(x, y, glow)
    return c


sheet([bolt(0), bolt(1)], 2, 8, 8).save("assets/sprites/bolt.png")


def pillar():
    c = Canvas(16, 16)
    for y in range(16):
        for x in range(16):
            d = math.hypot(x - 7.5, y - 7.5)
            if d < 7.5:
                k = 1.25 - 0.06 * math.hypot(x - 5, y - 5)
                c.set(x, y, shade(STONE, max(0.55, k)))
            if 5.5 < d < 6.5:
                c.set(x, y, STONE_DARK)
    return c.outline()


pillar().save("assets/sprites/pillar.png")

# Boulder outline in world units (1 unit = 16 px), centered on the entity. The
# scene's Collider2D {shape: "polygon", points} uses the same list (concave: the notch on top).
ROCK_POLYGON = [(-1.0, -0.5), (-0.4, -0.75), (0.6, -0.7), (1.0, -0.2), (0.8, 0.5),
                (0.25, 0.75), (-0.05, 0.3), (-0.55, 0.6), (-1.0, 0.1)]


def inside(px, py, poly):
    hit = False
    for i in range(len(poly)):
        (x1, y1), (x2, y2) = poly[i], poly[(i + 1) % len(poly)]
        if (y1 > py) != (y2 > py) and px < x1 + (py - y1) * (x2 - x1) / (y2 - y1):
            hit = not hit
    return hit


def rock():
    w, h = 32, 24
    c = Canvas(w, h)
    for y in range(h):
        for x in range(w):
            ux, uy = (x + 0.5 - w / 2) / 16, (h / 2 - y - 0.5) / 16
            if inside(ux, uy, ROCK_POLYGON):
                light = 1.15 + 0.35 * uy - 0.15 * ux
                c.set(x, y, shade((132, 118, 104), light))
    r = rnd(5)
    for _ in range(18):
        x, y = r(w), r(h)
        if c.get(x, y):
            c.set(x, y, shade(c.get(x, y), 0.75))
    return c


rock().save("assets/sprites/rock.png")


def heart(full):
    c = Canvas(9, 8)
    shape = ["011000110", "111101111", "111111111", "111111111", "011111110", "001111100", "000111000", "000010000"]
    for y, row in enumerate(shape):
        for x, v in enumerate(row):
            if v == "1":
                c.set(x, y, (230, 60, 80) if full else (70, 60, 80))
    if full:
        c.set(2, 1, (255, 170, 180))
    return c


sheet([heart(True), heart(False)], 2, 9, 8).save("assets/sprites/heart.png")
print("rock polygon:", [[x, y] for x, y in ROCK_POLYGON])
