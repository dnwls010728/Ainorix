"""Writes the item art of OnlineRPG (python tools/make_art.py, from the project folder).

Each item is a 16 x 16 pixel drawing: assets/textures/items/<key>.png is the world sprite (32 px per
unit, so half a unit wide) and assets/ui/<key>.png the same drawing four times larger for inventory
buttons. Standard library only. The world textures (grass, hero, tree, ...) are copies of samples/HD2D.
"""
import os
import struct
import zlib

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")

PALETTE = {
    ".": (0, 0, 0, 0),
    "k": (28, 22, 40, 255),      # outline
    "w": (250, 250, 255, 255),   # highlight
    "r": (214, 58, 72, 255), "R": (150, 30, 54, 255),      # potion
    "g": (96, 190, 84, 255), "G": (46, 122, 62, 255),      # herb
    "y": (255, 212, 84, 255), "Y": (206, 140, 38, 255),    # coin
    "b": (96, 204, 255, 255), "B": (44, 118, 214, 255),    # gem
    "c": (196, 176, 150, 255), "C": (128, 104, 86, 255),   # cork, glass rim
}

ITEMS = {
    "potion": [
        "................",
        "......kkkk......",
        "......kcCk......",
        ".....kkkkkk.....",
        "......kCCk......",
        "......kwCk......",
        ".....kwrrRk.....",
        "....kwrrrrRk....",
        "...kwrrrrrrRk...",
        "...krwrrrrrRk...",
        "...krrrrrrrRk...",
        "...krrrrrrRRk...",
        "...kRrrrrRRRk...",
        "....kRRRRRRk....",
        ".....kkkkkk.....",
        "................",
    ],
    "herb": [
        "................",
        ".........kk.....",
        "........kggk....",
        ".......kgwgk....",
        "..kk...kggGk....",
        ".kggk..kgGk.....",
        ".kgwgk.kGk..kk..",
        ".kggGkkGk..kggk.",
        "..kgGGkGk.kgwgk.",
        "...kkGGGkkggGk..",
        ".....kGGGGGGk...",
        "......kGGkkk....",
        "......kGk.......",
        "......kGk.......",
        ".......k........",
        "................",
    ],
    "coin": [
        "................",
        ".....kkkkkk.....",
        "...kkyyyyyykk...",
        "..kyywwyyyyYYk..",
        "..kywyyyyyyyYk..",
        ".kyywyykkyyyYYk.",
        ".kyyyykyykyyYYk.",
        ".kyyyykyyyyyYYk.",
        ".kyyyykyyyyyYYk.",
        ".kyyyykyykyyYYk.",
        ".kyyyyykkyyYYYk.",
        "..kyyyyyyyYYYk..",
        "..kYyyyyyYYYYk..",
        "...kkYYYYYYkk...",
        ".....kkkkkk.....",
        "................",
    ],
    "gem": [
        "................",
        "................",
        "....kkkkkkkk....",
        "...kbwwbbbBBk...",
        "..kbwbbbbbbBBk..",
        ".kbwbbbbbbbbBBk.",
        ".kkkkkkkkkkkkkk.",
        ".kbbbkbbbbkBBBk.",
        "..kbbkbbbbkBBk..",
        "...kbbkbbkBBk...",
        "....kbkbbkBk....",
        ".....kbkkBk.....",
        "......kbBk......",
        ".......kk.......",
        "................",
        "................",
    ],
}


def write_png(path, width, height, pixels):
    """pixels: rows of (r, g, b, a) tuples."""
    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + b"".join(bytes(p) for p in row) for row in pixels)
    data = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    full = os.path.join(ROOT, path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "wb") as f:
        f.write(data)


def scaled(rows, factor):
    out = []
    for row in rows:
        wide = [p for p in row for _ in range(factor)]
        out.extend([wide] * factor)
    return out


for key, art in ITEMS.items():
    assert len(art) == 16 and all(len(line) == 16 for line in art), key
    rows = [[PALETTE[ch] for ch in line] for line in art]
    write_png("assets/textures/items/%s.png" % key, 16, 16, rows)
    write_png("assets/ui/%s.png" % key, 64, 64, scaled(rows, 4))
print("wrote %d items" % len(ITEMS))
