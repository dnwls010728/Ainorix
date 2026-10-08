"""Writes the VFXLab textures (python tools/make_art.py, from the sample folder). No dependencies.

glow.png   soft round falloff: fire, embers, sparks, the magic ring and the orb
smoke.png  2 x 2 sheet of four lumpy puffs; emitters pick one per particle (randomFrame)
streak.png bright core that fades toward both ends: the trail ribbon
"""
import math
import os
import struct
import zlib

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")


def write_png(path, width, height, pixel):
    """pixel(x, y) -> (r, g, b, a) floats in 0..1."""
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            rows.extend(max(0, min(255, int(c * 255 + 0.5))) for c in pixel(x, y))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    full = os.path.join(ROOT, path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + chunk(b"IEND", b""))


def lattice(x, y, seed):
    h = (x * 374761393 + y * 668265263 + seed * 2246822519) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0


def noise(x, y, seed):
    x0, y0 = math.floor(x), math.floor(y)
    tx, ty = x - x0, y - y0
    tx, ty = tx * tx * (3 - 2 * tx), ty * ty * (3 - 2 * ty)
    top = lattice(x0, y0, seed) * (1 - tx) + lattice(x0 + 1, y0, seed) * tx
    bottom = lattice(x0, y0 + 1, seed) * (1 - tx) + lattice(x0 + 1, y0 + 1, seed) * tx
    return top * (1 - ty) + bottom * ty


def glow(x, y):
    d = math.hypot((x + 0.5) / 64 * 2 - 1, (y + 0.5) / 64 * 2 - 1)
    a = max(0.0, 1 - d)
    return 1, 1, 1, a * a


def smoke(x, y):
    cell = (y // 64) * 2 + (x // 64)
    u, v = (x % 64 + 0.5) / 64 * 2 - 1, (y % 64 + 0.5) / 64 * 2 - 1
    lumps = noise(u * 2.2 + 9, v * 2.2 + 9, cell) * 0.6 + noise(u * 5 + 3, v * 5 + 3, cell + 17) * 0.4
    edge = max(0.0, 1 - math.hypot(u, v) / (0.62 + 0.3 * lumps))
    a = min(1.0, edge * 2.2) * (0.55 + 0.45 * lumps)
    shade = 0.75 + 0.25 * lumps
    return shade, shade, shade, a


def streak(x, y):
    across = 1 - abs((y + 0.5) / 16 * 2 - 1)
    return 1, 1, 1, across * across


write_png("assets/textures/glow.png", 64, 64, glow)
write_png("assets/textures/smoke.png", 128, 128, smoke)
write_png("assets/textures/streak.png", 64, 16, streak)
print("wrote glow.png, smoke.png, streak.png")
