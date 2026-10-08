"""Turns the Codex pictures in tools/art_src/codex/ (large images that imitate pixel art) into the
game's textures in assets/textures/ at their real pixel sizes:

- character sheets: the twelve figures are found by empty rows/columns of the alpha mask and put,
  with one shared scale and a shared foot line, into a 4x3 grid of 32x48 cells
  (rows: facing down, right, up; columns: stand, step, stand, step);
- sprites (trees, bushes, rock, lamp, plants): cropped to the figure, fitted bottom-centred;
- textures: the whole image reduced to 32x32 (door 16x24, window 16x16);
- mountains: the silhouette strip reduced to 1024 px wide, ends cross-faded so it wraps;
- clouds: greyscale 256x256.

Colours are averaged over each new pixel (weighted by coverage) and reduced to a small palette, which
gives back hard pixel edges. Run from the project folder after tools/gen_art.py:

    python tools/import_art.py
"""
import os

import numpy as np
from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
SRC = os.path.join(ROOT, "tools", "art_src", "codex")
DST = os.path.join(ROOT, "assets", "textures")
SOLID = 160 / 255  # source alpha that counts as part of the figure (the generator adds a faint haze)

CELL_W, CELL_H, FIGURE_H = 32, 48, 45
SHEETS = ["hero", "npc_villager", "npc_farmer", "npc_girl"]
SPRITES = {  # name: (width, height) of the texture; the figure is fitted inside, bottom-centred
    "oak": (72, 80), "oak_lime": (72, 80), "oak_autumn": (72, 80), "oak_blossom": (72, 80),
    "pine": (46, 90), "birch": (52, 80), "bush": (32, 20), "bush_berry": (32, 20), "rock": (26, 18), "lamp": (14, 36), "moon": (32, 32),
}
TEXTURES = {"grass_top": (32, 32), "grass_side": (32, 32), "earth": (32, 32), "stone": (32, 32), "sand": (32, 32),
            "cobble": (32, 32), "plaster": (32, 32), "roof": (32, 32), "door": (16, 24), "window": (16, 16)}


def load(name):
    return np.asarray(Image.open(os.path.join(SRC, name + ".png")).convert("RGBA")).astype(np.float32) / 255


def save(name, rgba):
    os.makedirs(DST, exist_ok=True)
    Image.fromarray(np.clip(rgba * 255 + 0.5, 0, 255).astype(np.uint8), "RGBA").save(os.path.join(DST, name + ".png"))


def runs(profile, min_gap):
    """[start, end) ranges where profile > 0, merging gaps shorter than min_gap."""
    on = np.flatnonzero(profile > 0)
    if not len(on):
        return []
    out = [[on[0], on[0] + 1]]
    for i in on[1:]:
        if i - out[-1][1] < min_gap:
            out[-1][1] = i + 1
        else:
            out.append([i, i + 1])
    return [tuple(r) for r in out]


def reduce(img, mask, w, h):
    """Coverage-weighted average of img over a w x h grid; alpha = coverage >= 0.5."""
    src_h, src_w = mask.shape
    ys = np.linspace(0, src_h, h + 1).astype(int)
    xs = np.linspace(0, src_w, w + 1).astype(int)
    out = np.zeros((h, w, 4), np.float32)
    for j in range(h):
        for i in range(w):
            m = mask[ys[j]:max(ys[j + 1], ys[j] + 1), xs[i]:max(xs[i + 1], xs[i] + 1)]
            if m.size == 0:
                continue
            cover = m.mean()
            if cover < 0.5:
                continue
            c = img[ys[j]:max(ys[j + 1], ys[j] + 1), xs[i]:max(xs[i + 1], xs[i] + 1), :3]
            out[j, i, :3] = (c * m[..., None]).sum((0, 1)) / m.sum()
            out[j, i, 3] = 1
    return out


def palette(rgba, colors):
    """Snap opaque pixels to an adaptive palette (hard pixel-art colour steps)."""
    a = rgba[..., 3] > 0
    if not a.any():
        return rgba
    rgb = Image.fromarray(np.clip(rgba[..., :3] * 255 + 0.5, 0, 255).astype(np.uint8), "RGB")
    q = np.asarray(rgb.quantize(colors=colors, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE).convert("RGB"))
    out = rgba.copy()
    out[..., :3] = np.where(a[..., None], q.astype(np.float32) / 255, 0)
    return out


def outline(rgba):
    """Selective outline like hand-made sprites: empty pixels next to the figure take a dark shade of
    the neighbouring colour."""
    a = rgba[..., 3] > 0
    out = rgba.copy()
    h, w = a.shape
    for y in range(h):
        for x in range(w):
            if a[y, x]:
                continue
            for dx, dy in ((0, 1), (1, 0), (-1, 0), (0, -1)):
                nx, ny = x + dx, y + dy
                if 0 <= nx < w and 0 <= ny < h and a[ny, nx]:
                    c = rgba[ny, nx, :3]
                    out[y, x] = (c[0] * 0.28 + 0.03, c[1] * 0.28 + 0.016, c[2] * 0.3 + 0.055, 1)
                    break
    return out


def figure(img, box):
    x0, y0, x1, y1 = box
    part = img[y0:y1, x0:x1]
    mask = (part[..., 3] >= SOLID).astype(np.float32)
    ys, xs = np.nonzero(mask)
    return part[ys.min():ys.max() + 1, xs.min():xs.max() + 1], mask[ys.min():ys.max() + 1, xs.min():xs.max() + 1]


def place(canvas, sprite, x_center, bottom):
    h, w = sprite.shape[:2]
    x0 = int(round(x_center - w / 2))
    y0 = bottom - h
    for j in range(h):
        for i in range(w):
            if sprite[j, i, 3] > 0 and 0 <= y0 + j < canvas.shape[0] and 0 <= x0 + i < canvas.shape[1]:
                canvas[y0 + j, x0 + i] = sprite[j, i]


def sheet(name):
    img = load(name)
    solid = img[..., 3] >= SOLID
    rows = [r for r in runs(solid.sum(1), 12) if r[1] - r[0] > 40]
    if len(rows) != 3:
        raise SystemExit("%s: expected 3 rows of figures, found %d" % (name, len(rows)))
    figures = []
    for y0, y1 in rows:
        cols = [c for c in runs(solid[y0:y1].sum(0), 8) if c[1] - c[0] > 20]
        if len(cols) != 4:
            raise SystemExit("%s: expected 4 figures in a row, found %d" % (name, len(cols)))
        figures.append([figure(img, (x0, y0, x1, y1)) for x0, x1 in cols])
    scale = FIGURE_H / max(m.shape[0] for row in figures for _, m in row)
    out = np.zeros((CELL_H * 3, CELL_W * 4, 4), np.float32)
    for r, row in enumerate(figures):
        for c, (part, mask) in enumerate(row):
            w = max(1, min(CELL_W, int(round(mask.shape[1] * scale))))
            h = max(1, int(round(mask.shape[0] * scale)))
            small = reduce(part, mask, w, h)
            cell = np.zeros((CELL_H, CELL_W, 4), np.float32)
            place(cell, small, CELL_W / 2, CELL_H - 1)
            out[r * CELL_H:(r + 1) * CELL_H, c * CELL_W:(c + 1) * CELL_W] = cell
    save(name, outline(palette(out, 40)))


NO_OUTLINE = {"moon"}  # soft-edged sky disc


def sprite(name, size):
    img = load(name)
    part, mask = figure(img, (0, 0, img.shape[1], img.shape[0]))
    w, h = size
    scale = min(w / mask.shape[1], h / mask.shape[0])
    small = reduce(part, mask, max(1, int(round(mask.shape[1] * scale))), max(1, int(round(mask.shape[0] * scale))))
    canvas = np.zeros((h, w, 4), np.float32)
    place(canvas, small, w / 2, h)
    canvas = palette(canvas, 28)
    save(name, canvas if name in NO_OUTLINE else outline(canvas))


def plants():
    img = load("plants")
    solid = img[..., 3] >= SOLID
    cols = [c for c in runs(solid.sum(0), 10) if c[1] - c[0] > 10]
    if len(cols) != 6:
        raise SystemExit("plants: expected 6 plants, found %d" % len(cols))
    parts = [figure(img, (x0, 0, x1, img.shape[0])) for x0, x1 in cols]
    scale = 15 / max(max(m.shape) for _, m in parts)
    out = np.zeros((16, 96, 4), np.float32)
    for k, (part, mask) in enumerate(parts):
        small = reduce(part, mask, max(1, int(round(mask.shape[1] * scale))), max(1, int(round(mask.shape[0] * scale))))
        cell = np.zeros((16, 16, 4), np.float32)
        place(cell, small, 8, 16)
        out[:, k * 16:(k + 1) * 16] = cell
    save("plants", outline(palette(out, 24)))


def texture(name, size):
    img = load(name)
    w, h = size
    out = reduce(img, np.ones(img.shape[:2], np.float32), w, h)
    save(name, palette(out, 16))


def mountains(name):
    img = load(name)
    solid = img[..., 3] >= 96 / 255
    ys = np.nonzero(solid.any(1))[0]
    part = img[ys.min():ys.max() + 1]
    w = 1024
    h = 120  # stretched to the strip height: the ring sets the on-screen proportions
    small = np.asarray(Image.fromarray((part * 255).astype(np.uint8), "RGBA").resize((w, h), Image.BOX)).astype(np.float32) / 255
    small[..., 3] = np.where(small[..., 3] > 0.4, 1.0, 0.0)
    fade = 96  # cross-fade the ends so the ring wraps around without a seam
    for i in range(fade):
        t = i / fade
        left, right = small[:, i].copy(), small[:, w - fade + i].copy()
        small[:, w - fade + i] = right * (1 - t) + left * t
    out = np.zeros((128, w, 4), np.float32)
    out[128 - h:] = small
    # light grey that brightens toward the ridge (the game tints it per time of day), fading out at the
    # bottom so the backdrop melts into the haze above the water
    rows = np.arange(128, dtype=np.float32)[:, None]
    top = 128 - h
    shade = 1.0 - 0.22 * np.clip((rows - top) / h, 0, 1)
    fade = np.clip((127 - rows) / (h * 0.35), 0, 1)
    out[..., :3] = (0.75 + 0.25 * out[..., :3].mean(-1, keepdims=True)) * shade[..., None]
    out[..., 3] = out[..., 3] * fade
    save(name, out)


def clouds():
    img = load("clouds")
    grey = img[..., :3].mean(-1)
    small = np.asarray(Image.fromarray((grey * 255).astype(np.uint8), "L").resize((256, 256), Image.BOX)).astype(np.float32) / 255
    out = np.zeros((256, 256, 4), np.float32)
    out[..., 0] = out[..., 1] = out[..., 2] = small
    out[..., 3] = 1
    save("clouds", out)


def main():
    for name in SHEETS:
        sheet(name)
    for name, size in SPRITES.items():
        sprite(name, size)
    plants()
    for name, size in TEXTURES.items():
        texture(name, size)
    for name in ("mountains_far", "mountains_near"):
        mountains(name)
    clouds()
    print("wrote", len(os.listdir(DST)), "textures to assets/textures")


if __name__ == "__main__":
    main()
