"""Writes the village: models, technical textures, materials, shader graphs, effects, the terrain table
for Lua and the scene. Run from the project folder after tools/import_art.py:

    python tools/make_world.py

The terrain is a 56 x 56 grid of voxel columns (1 unit per tile, heights 0..4, water at 0) made with
the same seeded noise as the original web prototype, so the plaza, terraces, ponds, houses, roads
and the scattered trees/flowers come out the same every time.

Outputs:
  assets/models/terrain.glb      visible faces of every column, one texture atlas, per-tile tint
  assets/models/houses.glb       four half-timbered houses (walls, gables, roofs, chimneys, doors)
  assets/models/windows.glb      their windows (glow at night through a shader graph uniform)
  assets/models/plants.glb       grass tufts and flowers as crossed quads (sway + hero push graph)
  assets/models/mountain_ring.glb  open cylinder seen from inside, for the mountain backdrops
  assets/textures/{terrain_atlas,house_atlas,glow,blob,shore,shaft,stars}.png
  materials/*.mat.json, materials/*.shader.json, effects/*.vfx.json
  scripts/lib/terrain.lua        heights and blocked tiles for movement
  scenes/main.scene.json
"""
import json
import math
import os
import struct

import numpy as np
from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
TEX = "assets/textures/"
N = 56
SHORE_PAD = 4  # deep-water texels around the map in shore.png
WATER, GRASS, SAND, STONE, PATH = 0, 1, 2, 3, 4


def path(*parts):
    return os.path.join(ROOT, *parts)


def write_json(rel, data):
    full = path(rel)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)
        f.write("\n")


def write_text(rel, text):
    full = path(rel)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def save_png(rel, rgba):
    full = path(rel)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    Image.fromarray(np.clip(np.asarray(rgba, np.float32) * 255 + 0.5, 0, 255).astype(np.uint8), "RGBA").save(full)


def load_png(name):
    return np.asarray(Image.open(path(TEX + name + ".png")).convert("RGBA")).astype(np.float32) / 255


# ----- Seeded randomness and noise (ports of the prototype's mulberry32 rng and value noise) ------------

def rng(seed):
    state = [seed & 0xFFFFFFFF]

    def imul(a, b):
        return (a * b) & 0xFFFFFFFF

    def next_value():
        state[0] = (state[0] + 0x6D2B79F5) & 0xFFFFFFFF
        t = state[0]
        t = imul(t ^ (t >> 15), t | 1)
        t ^= (t + imul(t ^ (t >> 7), t | 61)) & 0xFFFFFFFF
        return ((t ^ (t >> 14)) & 0xFFFFFFFF) / 4294967296

    return next_value


def make_noise(seed, cell):
    r = rng(seed)
    g = math.ceil(N / cell) + 2
    grid = [r() for _ in range(g * g)]

    def noise(x, y):
        gx, gy = x / cell, y / cell
        x0, y0 = math.floor(gx), math.floor(gy)
        fx, fy = gx - x0, gy - y0
        sx, sy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
        # out of the grid the prototype read `undefined` (NaN): comparisons with it are false
        v = lambda i, j: grid[j * g + i] if 0 <= j * g + i < len(grid) else math.nan
        top = v(x0, y0) * (1 - sx) + v(x0 + 1, y0) * sx
        bot = v(x0, y0 + 1) * (1 - sx) + v(x0 + 1, y0 + 1) * sx
        return top * (1 - sy) + bot * sy

    return noise


def smooth(a, b, x):
    t = min(1, max(0, (x - a) / (b - a)))
    return t * t * (3 - 2 * t)


def js_round(x):
    return math.floor(x + 0.5)


# ----- The terrain ------------------------------------------------------------------------------------

def build_terrain():
    heights = [0] * (N * N)
    types = [GRASS] * (N * N)
    blocked = [0] * (N * N)
    reserved = [0] * (N * N)
    at = lambda i, j: j * N + i
    inside = lambda i, j: 0 <= i < N and 0 <= j < N
    n1, n2, nw = make_noise(11, 13), make_noise(23, 6), make_noise(47, 14)
    cx = cz = N // 2
    for j in range(N):
        for i in range(N):
            d = math.hypot(i - cx, j - cz)
            raw = n1(i, j) * 0.82 + n2(i, j) * 0.18
            h = 1 + math.floor(raw * 4.4)
            plaza = 1 - smooth(9, 15, d)
            h = js_round(h * (1 - plaza) + 2 * plaza)
            h = min(4, max(1, h))
            edge = min(i, j, N - 1 - i, N - 1 - j)
            t = WATER if nw(i, j) < 0.3 and d > 14 and edge > 1 else GRASS
            heights[at(i, j)] = 0 if t == WATER else h
            types[at(i, j)] = t
            if t == WATER or edge == 0:
                blocked[at(i, j)] = 1
    for _ in range(2):  # remove lonely spikes and pits so plateaus read as terraces
        for j in range(1, N - 1):
            for i in range(1, N - 1):
                if types[at(i, j)] == WATER:
                    continue
                nb = [heights[at(i + 1, j)], heights[at(i - 1, j)], heights[at(i, j + 1)], heights[at(i, j - 1)]]
                counts = {}
                for v in nb:
                    if v > 0:
                        counts[v] = counts.get(v, 0) + 1
                for v, c in counts.items():
                    if c >= 3 and v != heights[at(i, j)]:
                        heights[at(i, j)] = v
    houses = [(cx - 8, cx - 6, cz - 6, cz - 5), (cx + 6, cx + 8, cz - 6, cz - 5),
              (cx - 2, cx + 1, cz - 10, cz - 9), (cx + 8, cx + 9, cz + 1, cz + 3)]
    for i0, i1, j0, j1 in houses:
        for j in range(j0 - 2, j1 + 3):
            for i in range(i0 - 2, i1 + 3):
                k = at(i, j)
                reserved[k] = 1
                heights[k] = 2
                types[k] = GRASS
                blocked[k] = 1 if i0 <= i <= i1 and j0 <= j <= j1 else 0
    for j in range(N):
        for i in range(N):
            k = at(i, j)
            if types[k] == WATER:
                continue
            if heights[k] >= 4:
                types[k] = STONE
            if math.hypot(i - cx, j - cz) < 5.2 and heights[k] == 2:
                types[k] = PATH

    def road_to(ti, tj):
        i, j = cx, cz
        while i != ti or j != tj:
            if i != ti and (j == tj or abs(i - ti) >= abs(j - tj)):
                i += (ti > i) - (ti < i)
            else:
                j += (tj > j) - (tj < j)
            if inside(i, j) and heights[at(i, j)] == 2 and types[at(i, j)] != WATER:
                types[at(i, j)] = PATH

    for i0, i1, _, j1 in houses:
        road_to(js_round((i0 + i1) / 2), j1 + 1)
    for j in range(N):
        for i in range(N):
            if types[at(i, j)] != GRASS:
                continue
            for di, dj in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                if inside(i + di, j + dj) and types[at(i + di, j + dj)] == WATER:
                    types[at(i, j)] = SAND
    return {"heights": heights, "types": types, "blocked": blocked, "reserved": reserved, "houses": houses,
            "at": at, "inside": inside, "center": (cx, cz)}


def scatter(world):
    """Trees, bushes, rocks, grass tufts and flowers, in the prototype's order of random draws."""
    at, heights, types, blocked, reserved = world["at"], world["heights"], world["types"], world["blocked"], world["reserved"]
    cx, cz = world["center"]
    r = rng(99)
    n_for = make_noise(88, 10)
    pick = lambda a: a[math.floor(r() * len(a))]
    oaks, lime, autumn, pink = ["oak", "oak", "oak"], ["oak_lime", "oak_lime"], ["oak_autumn", "oak_autumn"], ["oak_blossom"]
    pines, birches = ["pine", "pine"], ["birch", "birch"]
    bushes, rocks = ["bush", "bush_berry", "bush_berry"], ["rock", "rock"]
    props, tufts, flowers = [], [], [[], [], [], [], []]
    for j in range(1, N - 1):
        for i in range(1, N - 1):
            k = at(i, j)
            t, y = types[k], heights[k]
            d = math.hypot(i - cx, j - cz)
            if reserved[k] or blocked[k] or t == WATER:
                continue
            zone = n_for(i, j)
            if t in (GRASS, STONE):
                if d > 8 and r() < (0.12 if zone > 0.5 else 0.055):
                    kind = r()
                    if zone > 0.62:
                        art = pick(pines) if kind < 0.7 else pick(birches)
                    elif zone < 0.3:
                        art = pick(autumn) if kind < 0.45 else pick(pink) if kind < 0.55 else pick(oaks)
                    else:
                        art = pick(oaks) if kind < 0.5 else pick(lime) if kind < 0.75 else pick(birches) if kind < 0.9 else pick(autumn)
                    props.append((art, i + (r() - 0.5) * 0.4, y, j + (r() - 0.5) * 0.4, 1.0))
                    blocked[k] = 1
                    continue
                if d > 7 and r() < 0.04:
                    props.append((pick(bushes), i + (r() - 0.5) * 0.5, y, j + (r() - 0.5) * 0.5, 0.5))
                    blocked[k] = 1
                    continue
                if r() < 0.018:
                    props.append((pick(rocks), i + (r() - 0.5) * 0.4, y, j + (r() - 0.5) * 0.4, 0.5))
                    blocked[k] = 1
                    continue
            if t == GRASS:
                for _ in range(3 + math.floor(r() * 3)):
                    tufts.append((i + (r() - 0.5) * 0.95, y, j + (r() - 0.5) * 0.95, 0.8 + r() * 0.6))
                patch = n_for(i + 60, j + 60)
                if r() < (0.9 if patch > 0.6 else 0.05):
                    for _ in range(2 + math.floor(r() * 3) if patch > 0.6 else 1):
                        flowers[math.floor(r() * 5)].append((i + (r() - 0.5) * 0.9, y, j + (r() - 0.5) * 0.9, 1.0))
    plants = []  # (cell, x, y, z, scale, rotation, shade); rotations/shades drawn like the prototype's instancing
    for cell, spots in [(0, tufts)] + [(k + 1, f) for k, f in enumerate(flowers)]:
        for x, y, z, s in spots:
            rot = r() * math.pi
            shade = 0.85 + r() * 0.3
            plants.append((cell, x, y, z, s, rot, shade))
    lamps = []
    for lx, lz in ((-4, -3), (4, -3), (-4, 3), (4, 3)):
        i, j = cx + lx, cz + lz
        blocked[at(i, j)] = 1
        lamps.append((i, heights[at(i, j)], j, r() * 10))
    return props, plants, lamps


# ----- glTF writer -------------------------------------------------------------------------------------

class MeshBuilder:
    def __init__(self):
        self.pos, self.nrm, self.uv, self.col, self.idx = [], [], [], [], []

    def quad(self, corners, uvs, normal, color=(1, 1, 1, 1)):
        """corners in counter-clockwise order seen from the front (the normal side)."""
        base = len(self.pos)
        a, b, c = (np.array(corners[k], float) for k in range(3))
        if np.dot(np.cross(b - a, c - a), normal) < 0:
            corners, uvs = corners[::-1], uvs[::-1]
        for p, t in zip(corners, uvs):
            self.pos.append(tuple(float(v) for v in p))
            self.nrm.append(tuple(float(v) for v in normal))
            self.uv.append(tuple(float(v) for v in t))
            self.col.append(tuple(float(v) for v in color))
        if len(corners) == 4:
            self.idx += [base, base + 1, base + 2, base, base + 2, base + 3]
        else:
            self.idx += [base, base + 1, base + 2]

    def save(self, rel, colors=False):
        data = bytearray()
        views, accessors = [], []

        def accessor(values, fmt, kind, component, target, bounds=False):
            while len(data) % 4:
                data.append(0)
            offset = len(data)
            for v in values:
                data.extend(struct.pack(fmt, *v) if isinstance(v, tuple) else struct.pack(fmt, v))
            views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(data) - offset, "target": target})
            entry = {"bufferView": len(views) - 1, "componentType": component, "count": len(values), "type": kind}
            if bounds:
                entry["min"] = [min(p[k] for p in values) for k in range(3)]
                entry["max"] = [max(p[k] for p in values) for k in range(3)]
            accessors.append(entry)
            return len(accessors) - 1

        attributes = {"POSITION": accessor(self.pos, "<3f", "VEC3", 5126, 34962, True),
                      "NORMAL": accessor(self.nrm, "<3f", "VEC3", 5126, 34962),
                      "TEXCOORD_0": accessor(self.uv, "<2f", "VEC2", 5126, 34962)}
        if colors:
            attributes["COLOR_0"] = accessor(self.col, "<4f", "VEC4", 5126, 34962)
        index = accessor(self.idx, "<I", "SCALAR", 5125, 34963)
        doc = {"asset": {"version": "2.0", "generator": "RPG tools/make_world.py"}, "scene": 0,
               "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
               "meshes": [{"primitives": [{"attributes": attributes, "indices": index}]}],
               "buffers": [{"byteLength": len(data)}], "bufferViews": views, "accessors": accessors}
        header = json.dumps(doc, separators=(",", ":")).encode()
        header += b" " * (-len(header) % 4)
        data += b"\0" * (-len(data) % 4)
        blob = struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(header) + 8 + len(data))
        blob += struct.pack("<II", len(header), 0x4E4F534A) + header
        blob += struct.pack("<II", len(data), 0x004E4942) + bytes(data)
        full = path(rel)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as f:
            f.write(blob)
        return len(self.pos)


# ----- Texture atlases ---------------------------------------------------------------------------------

class Atlas:
    """Cells of size x size with a wrapped gutter, so nearest/mip sampling never bleeds between cells."""

    def __init__(self, names, size=32, gutter=2):
        self.size, self.gutter, self.step = size, gutter, size + 2 * gutter
        self.names = names
        self.image = np.zeros((self.step, self.step * len(names), 4), np.float32)
        self.width, self.height = self.step * len(names), self.step

    def put(self, name, img):
        k = self.names.index(name)
        s, g = self.size, self.gutter
        cell = np.zeros((s, s, 4), np.float32)
        h, w = img.shape[:2]
        cell[:h, :w] = img[:s, :s]
        tiled = np.tile(cell, (3, 3, 1))[s - g:2 * s + g, s - g:2 * s + g]
        self.image[:, k * self.step:(k + 1) * self.step] = tiled

    def fill(self, name, color):
        self.put(name, np.tile(np.array(color + (1,), np.float32), (self.size, self.size, 1)))

    def uv(self, name, u, v):
        """Atlas uv of cell-relative (u, v) in 0..1 (v = 0 at the image top)."""
        k = self.names.index(name)
        x = k * self.step + self.gutter + u * self.size
        y = self.gutter + v * self.size
        return (x / self.width, y / self.height)


# ----- Shader graphs -----------------------------------------------------------------------------------

class Graph:
    def __init__(self, uniforms):
        self.uniforms = uniforms
        self.nodes = []

    def add(self, op, *args, value=None, name=None):
        node = {"op": op}
        if args:
            node["args"] = list(args)
        if value is not None:
            node["value"] = value
        if name is not None:
            node["name"] = name
        self.nodes.append(node)
        if len(self.nodes) > 48:
            raise SystemExit("shader graph over 48 nodes")
        return len(self.nodes) - 1

    def c(self, value):
        return self.add("constant", value=value)

    def u(self, name):
        return self.add("uniform", name=name)

    def swz(self, a, channels):
        return self.add("swizzle", a, value=channels)

    def doc(self, **outputs):
        d = {"format": "ownengine.shader", "uniforms": self.uniforms, "nodes": self.nodes}
        d.update(outputs)
        return d


def cloud_shadow_graph():
    """Drifting cloud shadows over the ground (the prototype's sin pattern), keeping alpha."""
    g = Graph({})
    p, t = g.add("position"), g.add("time")
    s = g.add("sin", g.add("add", g.add("multiply", p, g.c([0.21, 0, 0.17, 0])), g.add("multiply", t, g.c([0.32, 0, -0.21, 0]))))
    wave = g.add("multiply", g.swz(s, [0, 0, 0, 0]), g.swz(s, [2, 2, 2, 2]))
    diag = g.add("sin", g.add("add", g.add("dot", p, g.c([0.11, 0, 0.11, 0])), g.add("multiply", t, g.c(0.14))))
    cl = g.add("add", wave, g.add("multiply", diag, g.c(0.6)))
    zero, one = g.c(0), g.c(1)
    x = g.add("clamp", g.add("multiply", g.add("subtract", cl, g.c(0.15)), g.c(1 / 0.95)), zero, one)
    smooth_x = g.add("multiply", g.add("multiply", x, x), g.add("subtract", g.c(3), g.add("multiply", g.c(2), x)))
    factor = g.add("subtract", one, g.add("multiply", smooth_x, g.c(0.17)))
    base = g.add("baseColor")
    shaded = g.add("multiply", base, factor)
    return g.doc(color=g.add("mix", base, shaded, g.c([1, 1, 1, 0])))


def plants_graph():
    """Grass sway plus the hero pushing blades aside (uniform `hero`, set by scripts/game.lua)."""
    g = Graph({"hero": [0, -100, 0, 0]})
    p, t, uv = g.add("position"), g.add("time"), g.add("uv")
    one, zero = g.c(1), g.c(0)
    k = g.add("subtract", one, g.swz(uv, [1, 1, 1, 1]))  # 1 at the top of a blade, 0 at the root
    phase = g.add("dot", p, g.c([1.7, 0, 1.3, 0]))
    sway = g.add("multiply", g.add("sin", g.add("add", g.add("multiply", t, g.c([2.2, 0, 1.7, 0])), phase)), g.c([0.07, 0, 0.04, 0]))
    d = g.add("multiply", g.add("subtract", p, g.u("hero")), g.c([1, 0, 1, 0]))
    push = g.add("multiply", g.add("clamp", g.add("multiply", g.add("subtract", g.c(1.96), g.add("dot", d, d)), g.c(0.55)), zero, one), g.c(0.6))
    bend = g.add("add", g.add("normalize", d), g.c([0, -0.18, 0, 0]))
    offset = g.add("multiply", g.add("add", sway, g.add("multiply", bend, push)), k)
    return g.doc(color=g.add("baseColor"), offset=offset)


def sky_graph():
    """Sky dome: horizon-to-zenith gradient, sun disc and halo (HDR, blooms), stars from the base texture."""
    g = Graph({"cameraPosition": [0, 0, 0, 1], "horizon": [0.8, 0.89, 0.95, 1], "zenith": [0.25, 0.51, 0.86, 1],
               "sunDir": [-0.5, 0.6, 0.4, 0], "sunColor": [1, 0.95, 0.88, 0]})
    p, cam = g.add("position"), g.u("cameraPosition")
    xyz = g.c([1, 1, 1, 0])
    d = g.add("normalize", g.add("multiply", g.add("subtract", p, cam), xyz))
    dy = g.swz(d, [1, 1, 1, 1])
    zero, one = g.c(0), g.c(1)
    h = g.add("clamp", dy, zero, one)
    t = g.add("clamp", g.add("multiply", h, g.c(1.43)), zero, one)
    col = g.add("mix", g.u("horizon"), g.u("zenith"), t)
    sun = g.u("sunDir")
    s = g.add("max", g.add("dot", d, g.add("multiply", sun, xyz)), zero)
    s2 = g.add("multiply", s, s)
    s4 = g.add("multiply", s2, s2)
    s8 = g.add("multiply", s4, s4)
    s16 = g.add("multiply", s8, s8)
    s32 = g.add("multiply", s16, s16)
    disc = g.add("multiply", g.add("step", g.c(0.9993), s), g.c(12))
    amount = g.add("add", g.add("add", disc, g.add("multiply", s32, g.c(0.45))), g.add("multiply", s8, g.c(0.12)))
    col = g.add("add", col, g.add("multiply", g.u("sunColor"), amount))
    star_uv = g.add("divide", g.swz(d, [0, 2, 0, 0]), g.add("add", dy, one))
    stars = g.add("multiply", g.swz(g.add("texture", star_uv), [1, 1, 1, 1]), g.swz(sun, [3, 3, 3, 3]))
    return g.doc(color=g.add("add", col, stars))


def clouds_graph():
    """Cloud layer on a slightly smaller dome: drifting coverage texture, lit toward the sun, faded at the horizon."""
    g = Graph({"cameraPosition": [0, 0, 0, 1], "sunDir": [-0.5, 0.6, 0.4, 0], "cloudLit": [1, 1, 1, 1],
               "cloudShade": [0.66, 0.72, 0.85, 1]})
    p, cam = g.add("position"), g.u("cameraPosition")
    d = g.add("normalize", g.add("multiply", g.add("subtract", p, cam), g.c([1, 1, 1, 0])))
    dy = g.swz(d, [1, 1, 1, 1])
    plane = g.add("divide", g.swz(d, [0, 2, 0, 0]), g.add("add", dy, g.c(0.14)))
    cuv = g.add("add", g.add("multiply", plane, g.c(0.12)), g.add("multiply", g.add("time"), g.c([0.004, 0.0015, 0, 0])))
    c = g.swz(g.add("texture", cuv), [0, 0, 0, 0])
    zero, one = g.c(0), g.c(1)
    cover = g.add("clamp", g.add("multiply", g.add("subtract", c, g.c(0.42)), g.c(3.2)), zero, one)
    toward = g.add("multiply", g.swz(g.u("sunDir"), [0, 2, 0, 0]), g.c(0.03))
    edge = g.swz(g.add("texture", g.add("add", cuv, toward)), [0, 0, 0, 0])
    lit = g.add("clamp", g.add("add", g.add("multiply", g.add("subtract", c, edge), g.c(3)), g.c(0.55)), zero, one)
    color = g.add("mix", g.u("cloudShade"), g.u("cloudLit"), lit)
    alpha = g.add("multiply", cover, g.add("clamp", g.add("multiply", dy, g.c(5)), zero, one))
    return g.doc(color=g.add("mix", color, alpha, g.c([0, 0, 0, 1])))


def water_graph():
    """Stylised water from a shore-distance map (base texture, 1 = deep): shallow-to-deep colours, ripples, foam."""
    g = Graph({"shallow": [0.34, 0.78, 0.76, 1], "deep": [0.07, 0.4, 0.6, 1], "light": [1, 1, 1, 1]})
    p, t = g.add("position"), g.add("time")
    zero, one = g.c(0), g.c(1)
    # the map sits 4 texels inside a deep-water border; clamping repeats that border beyond the map
    uv = g.add("clamp", g.add("multiply", g.add("add", g.swz(p, [0, 2, 0, 0]), g.c(0.5 + SHORE_PAD)), g.c(1 / (N + 2 * SHORE_PAD))), zero, one)
    sd = g.swz(g.add("texture", uv), [0, 0, 0, 0])
    waves = g.add("sin", g.add("add", g.add("multiply", p, g.c([1.3, 0, 2.6, 0])), g.add("multiply", t, g.c([0.9, 0, -0.6, 0]))))
    ripple = g.add("dot", waves, g.c([0.5, 0, 0.5, 0]))
    dd = g.add("clamp", g.add("subtract", g.add("multiply", sd, g.c(1.52)), g.c(0.15)), zero, one)
    col = g.add("mix", g.u("shallow"), g.u("deep"), dd)
    col = g.add("multiply", col, g.add("add", g.c(0.92), g.add("multiply", ripple, g.c(0.08))))
    foam = g.add("clamp", g.add("multiply", g.add("add", g.add("subtract", g.c(0.2), sd), g.add("multiply", ripple, g.c(0.03))), g.c(6)), zero, one)
    # unlit (a lit 700 m plane would stretch the shadow map over it): scripts/game.lua sets `light`
    return g.doc(color=g.add("multiply", g.add("mix", col, g.c([0.97, 1, 1, 1]), foam), g.u("light")))


def window_graph():
    """Windows glow at night: the yellow glass (red clearly above blue) emits `glow` times its colour."""
    g = Graph({"glow": 0})
    base = g.add("baseColor")
    glass = g.add("step", g.c(0.25), g.add("subtract", g.swz(base, [0, 0, 0, 0]), g.swz(base, [2, 2, 2, 2])))
    return g.doc(color=base, emissive=g.add("multiply", base, g.add("multiply", glass, g.u("glow"))))


# ----- Models ------------------------------------------------------------------------------------------

TERRAIN_CELLS = ["grass_top", "grass_side", "earth", "stone", "sand", "cobble"]
TOP = {GRASS: "grass_top", SAND: "sand", STONE: "stone", PATH: "cobble"}
SIDE = {GRASS: "grass_side", SAND: "earth", STONE: "stone", PATH: "earth"}


def terrain_model(world):
    atlas = Atlas(TERRAIN_CELLS)
    for name in TERRAIN_CELLS:
        atlas.put(name, load_png(name))
    save_png(TEX + "terrain_atlas.png", atlas.image)
    heights, types, at, inside = world["heights"], world["types"], world["at"], world["inside"]
    n_tint = make_noise(71, 7)
    tint_b, tint_c = np.array([1.1, 1.02, 0.78]), np.array([0.86, 1.0, 0.94])
    m = MeshBuilder()
    for j in range(N):
        for i in range(N):
            t = types[at(i, j)]
            if t == WATER:
                continue
            h = heights[at(i, j)]
            v = n_tint(i, j)
            jitter = 0.94 + ((i * 7919 + j * 104729) % 100) / 100 * 0.1
            col = np.ones(3)
            if t == GRASS:
                col = col + ((tint_b if v > 0.55 else tint_c) - col) * abs(v - 0.5) * 1.1
            col = tuple(col * jitter) + (1.0,)
            x0, x1, z0, z1 = i - 0.5, i + 0.5, j - 0.5, j + 0.5
            top = TOP[t]
            m.quad([(x0, h, z0), (x0, h, z1), (x1, h, z1), (x1, h, z0)],
                   [atlas.uv(top, 0, 0), atlas.uv(top, 0, 1), atlas.uv(top, 1, 1), atlas.uv(top, 1, 0)], (0, 1, 0), col)
            for di, dj in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nh = heights[at(i + di, j + dj)] if inside(i + di, j + dj) else 0
                if nh >= h:
                    continue
                normal = (di, 0, dj)
                # the face's left/right edge as seen from outside
                if di == 1:
                    a, b = (x1, z1), (x1, z0)
                elif di == -1:
                    a, b = (x0, z0), (x0, z1)
                elif dj == 1:
                    a, b = (x0, z1), (x1, z1)
                else:
                    a, b = (x1, z0), (x0, z0)
                for y in range(nh, h):
                    cell = SIDE[t] if y == h - 1 else ("stone" if t == STONE else "earth")
                    m.quad([(a[0], y + 1, a[1]), (a[0], y, a[1]), (b[0], y, b[1]), (b[0], y + 1, b[1])],
                           [atlas.uv(cell, 0, 0), atlas.uv(cell, 0, 1), atlas.uv(cell, 1, 1), atlas.uv(cell, 1, 0)], normal, col)
    return m.save("assets/models/terrain.glb", colors=True)


HOUSE_CELLS = ["plaster", "roof", "stone", "door", "ridge", "wall"]


def rot_x(p, angle):
    c, s = math.cos(angle), math.sin(angle)
    return (p[0], p[1] * c - p[2] * s, p[1] * s + p[2] * c)


def tiled_face(m, atlas, cell, origin, u_axis, v_axis, u_len, v_len, normal, transform):
    """A rectangle split into unit squares so a repeating texture can come from one atlas cell.
    origin is the top-left corner seen from the front; v runs down."""
    o, ua, va = np.array(origin, float), np.array(u_axis, float), np.array(v_axis, float)
    nu, nv = max(1, math.ceil(u_len - 1e-6)), max(1, math.ceil(v_len - 1e-6))
    for b in range(nv):
        for a in range(nu):
            u0, u1 = a, min(a + 1, u_len)
            v0, v1 = b, min(b + 1, v_len)
            corners = [o + ua * u0 + va * v0, o + ua * u0 + va * v1, o + ua * u1 + va * v1, o + ua * u1 + va * v0]
            uvs = [atlas.uv(cell, 0, 0), atlas.uv(cell, 0, v1 - v0), atlas.uv(cell, u1 - u0, v1 - v0), atlas.uv(cell, u1 - u0, 0)]
            m.quad([transform(tuple(c)) for c in corners], uvs, transform(normal, True))


def box(m, atlas, size, center, cells, transform, repeat=None):
    """Axis-aligned box (before `transform`); cells/repeat per face: +x, -x, +y, -y, +z, -z."""
    sx, sy, sz = size
    cx, cy, cz = center
    hx, hy, hz = sx / 2, sy / 2, sz / 2
    faces = [  # origin (top-left from outside), u axis, v axis, u length, v length, normal
        ((cx + hx, cy + hy, cz + hz), (0, 0, -1), (0, -1, 0), sz, sy, (1, 0, 0)),
        ((cx - hx, cy + hy, cz - hz), (0, 0, 1), (0, -1, 0), sz, sy, (-1, 0, 0)),
        ((cx - hx, cy + hy, cz - hz), (1, 0, 0), (0, 0, 1), sx, sz, (0, 1, 0)),
        ((cx - hx, cy - hy, cz + hz), (1, 0, 0), (0, 0, -1), sx, sz, (0, -1, 0)),
        ((cx - hx, cy + hy, cz + hz), (1, 0, 0), (0, -1, 0), sx, sy, (0, 0, 1)),
        ((cx + hx, cy + hy, cz - hz), (-1, 0, 0), (0, -1, 0), sx, sy, (0, 0, -1)),
    ]
    for k, (o, ua, va, ul, vl, n) in enumerate(faces):
        sc = repeat[k] if repeat else (1, 1)
        if cells[k] is None:
            continue
        o, ua, va = np.array(o), np.array(ua, float) * sc[0], np.array(va, float) * sc[1]
        tiled_face(m, atlas, cells[k], o, ua, va, ul / sc[0], vl / sc[1], n, transform)


def houses_model(world):
    atlas = Atlas(HOUSE_CELLS)
    for name in ("plaster", "roof", "stone"):
        atlas.put(name, load_png(name))
    door = load_png("door")
    atlas.put("door", np.asarray(Image.fromarray((door * 255).astype(np.uint8)).resize((32, 32), Image.NEAREST)).astype(np.float32) / 255)
    atlas.fill("ridge", (0.35, 0.16, 0.13))
    plaster = load_png("plaster")
    atlas.fill("wall", tuple(float(v) for v in np.median(plaster[8:24, 8:24, :3].reshape(-1, 3), 0)))
    save_png(TEX + "house_atlas.png", atlas.image)
    m, win = MeshBuilder(), MeshBuilder()
    chimneys = []
    for i0, i1, j0, j1 in world["houses"]:
        W, D = i1 - i0 + 1, j1 - j0 + 1
        gx, gz, gy = (i0 + i1) / 2, (j0 + j1) / 2, 2.0
        wall_h, roof_h, fh = 2.7, 1.6, 0.22

        def at_house(p, direction=False):
            return p if direction else (p[0] + gx, p[1] + gy, p[2] + gz)

        box(m, atlas, (W + 0.08, fh, D + 0.08), (0, fh / 2, 0), ["stone"] * 6, at_house)
        box(m, atlas, (W, wall_h, D), (0, fh + wall_h / 2, 0), ["plaster", "plaster", None, None, "plaster", "plaster"], at_house)
        y0 = fh + wall_h
        for s in (-1, 1):  # gables, plain plaster
            z = s * D / 2 + s * 0.005
            tri = [(-W / 2, y0, z), (W / 2, y0, z), (0, y0 + roof_h, z)]
            m.quad([at_house(p) for p in tri], [atlas.uv("wall", 0.1, 0.9), atlas.uv("wall", 0.9, 0.9), atlas.uv("wall", 0.5, 0.1)], (0, 0, s))
        slope = math.atan2(roof_h, D / 2)
        slab_l = math.hypot(D / 2, roof_h) + 0.35
        for s in (-1, 1):
            cy, cz = y0 + roof_h / 2 + 0.06, s * D / 4 + s * 0.12

            def slab(p, direction=False, s=s, cy=cy, cz=cz):
                q = rot_x(p, s * slope)
                return q if direction else at_house((q[0], q[1] + cy, q[2] + cz))

            box(m, atlas, (W + 0.6, 0.12, slab_l), (0, 0, 0), ["ridge", "ridge", "roof", "ridge", "ridge", "ridge"], slab)
        box(m, atlas, (W + 0.66, 0.1, 0.16), (0, y0 + roof_h + 0.1, 0), ["ridge"] * 6, at_house)
        chim = (W / 2 - 0.55, y0 + roof_h * 0.8, -D / 4)
        box(m, atlas, (0.45, 1.3, 0.45), chim, ["stone"] * 6, at_house)
        chimneys.append((gx + chim[0], gy + chim[1] + 0.7, gz + chim[2]))
        dz = D / 2 + 0.012
        m.quad([at_house(p) for p in [(-0.375, fh + 1.55, dz), (-0.375, fh, dz), (0.375, fh, dz), (0.375, fh + 1.55, dz)]],
               [atlas.uv("door", 0, 0), atlas.uv("door", 0, 1), atlas.uv("door", 1, 1), atlas.uv("door", 1, 0)], (0, 0, 1))

        def window(x, z, normal):
            n = np.array(normal, float)
            right = np.cross((0, 1, 0), n)
            c = np.array((x, fh + 1.7, z))
            corners = [c - right * 0.36 + (0, 0.36, 0), c - right * 0.36 - (0, 0.36, 0), c + right * 0.36 - (0, 0.36, 0), c + right * 0.36 + (0, 0.36, 0)]
            win.quad([at_house(tuple(p)) for p in corners], [(0, 0), (0, 1), (1, 1), (1, 0)], normal)

        if W >= 3:
            window(-W / 2 + 0.75, dz, (0, 0, 1))
            window(W / 2 - 0.75, dz, (0, 0, 1))
        else:
            window(W / 2 - 0.65, dz, (0, 0, 1))
        window(0, -dz, (0, 0, -1))
        window(W / 2 + 0.012, 0, (1, 0, 0))
        window(-W / 2 - 0.012, 0, (-1, 0, 0))
    m.save("assets/models/houses.glb")
    win.save("assets/models/windows.glb")
    return chimneys


def plants_model(plants):
    sizes = {0: (0.44, 0.44)}
    m = MeshBuilder()
    for cell, x, y, z, s, rot, shade in plants:
        w, h = sizes.get(cell, (0.38, 0.38))
        w, h = w * s, h * s
        u0, u1 = cell / 6, (cell + 1) / 6
        for extra in (0, math.pi / 2):
            a = rot + extra
            dx, dz = math.cos(a) * w / 2, -math.sin(a) * w / 2
            n = (math.sin(a), 0, math.cos(a))
            m.quad([(x - dx, y + h, z - dz), (x - dx, y, z - dz), (x + dx, y, z + dz), (x + dx, y + h, z + dz)],
                   [(u0, 0), (u0, 1), (u1, 1), (u1, 0)], n, (shade, shade, shade, 1))
    return m.save("assets/models/plants.glb", colors=True)


def mountain_ring_model():
    m = MeshBuilder()
    seg = 96
    for k in range(seg):
        a0, a1 = k / seg * math.tau, (k + 1) / seg * math.tau
        p0, p1 = (math.cos(a0), math.sin(a0)), (math.cos(a1), math.sin(a1))
        mid = (a0 + a1) / 2
        inward = (-math.cos(mid), 0, -math.sin(mid))
        m.quad([(p0[0], 0.5, p0[1]), (p0[0], -0.5, p0[1]), (p1[0], -0.5, p1[1]), (p1[0], 0.5, p1[1])],
               [(k / seg * 2, 0), (k / seg * 2, 1), ((k + 1) / seg * 2, 1), ((k + 1) / seg * 2, 0)], inward)
    m.save("assets/models/mountain_ring.glb")


# ----- Technical textures ------------------------------------------------------------------------------

def technical_textures(world):
    y, x = np.mgrid[0:64, 0:64]
    r = np.hypot(x - 31.5, y - 31.5) / 32
    glow = np.clip(np.interp(r, [0, 0.25, 1], [1, 0.55, 0]), 0, 1)
    save_png(TEX + "glow.png", np.dstack([np.ones_like(r)] * 3 + [glow]))
    blob = np.clip(np.interp(r, [0, 0.6, 1], [0.55, 0.3, 0]), 0, 1)
    save_png(TEX + "blob.png", np.dstack([np.full_like(r, 20 / 255), np.full_like(r, 12 / 255), np.full_like(r, 30 / 255), blob]))
    # shore distance (in tiles, capped at 5) for the water graph
    types, at, inside = world["types"], world["at"], world["inside"]
    dist = [0 if types[k] != WATER else 99 for k in range(N * N)]
    queue = [k for k in range(N * N) if dist[k] == 0]
    for k in queue:
        i, j = k % N, k // N
        for di, dj in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            if inside(i + di, j + dj) and dist[at(i + di, j + dj)] > dist[k] + 1:
                dist[at(i + di, j + dj)] = dist[k] + 1
                queue.append(at(i + di, j + dj))
    shore = np.ones((N + 2 * SHORE_PAD, N + 2 * SHORE_PAD), np.float32)
    shore[SHORE_PAD:SHORE_PAD + N, SHORE_PAD:SHORE_PAD + N] = np.array([min(5, d) / 5 for d in dist], np.float32).reshape(N, N)
    save_png(TEX + "shore.png", np.dstack([shore, shore, shore, np.ones_like(shore)]))
    # light shaft: soft across, fading at both ends
    sy, sx = np.mgrid[0:64, 0:16]
    across = 1 - np.abs((sx + 0.5) / 16 * 2 - 1)
    across = across * across * (3 - 2 * across)
    v = (sy + 0.5) / 64
    along = np.clip(v / 0.22, 0, 1) * np.clip((1 - v) / 0.55, 0, 1)
    save_png(TEX + "shaft.png", np.dstack([np.ones_like(v)] * 3 + [across * along]))
    # stars in the green channel
    g = np.zeros((512, 512), np.float32)
    r_ = rng(5)
    for _ in range(900):
        px, py = int(r_() * 512), int(r_() * 512)
        g[py, px] = 0.5 + r_() * 0.5
        if r_() < 0.15:
            for qx, qy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                g[(py + qy) % 512, (px + qx) % 512] = max(g[(py + qy) % 512, (px + qx) % 512], 0.35)
    save_png(TEX + "stars.png", np.dstack([np.zeros_like(g), g, np.zeros_like(g), np.ones_like(g)]))


# ----- Effects -----------------------------------------------------------------------------------------

def effects():
    motes = {"texture": TEX + "glow.png", "pixelArt": False, "space": "world", "shape": "box", "shapeSize": [15, 2.4, 15],
             "rate": 34, "lifetime": 5, "lifetimeVariation": 0.4, "speed": 0.25, "spread": 180, "gravity": [0.05, 0.03, 0],
             "turbulence": 0.6, "turbulenceScale": 0.35, "blend": "add", "maxParticles": 200, "startSize": 0.16,
             "endSize": 0.16, "sizeVariation": 0.5, "startColor": [1, 0.86, 0.45], "endColor": [1, 0.86, 0.45],
             "opacityOverLife": [[0, 0], [0.2, 0.9], [0.5, 0.35], [0.75, 0.95], [1, 0]]}
    write_json("effects/motes.vfx.json", {
        "format": "oe.vfx", "version": 1, "name": "Motes",
        "description": "Pollen drifting in the sun by day, glowing fireflies at night. Follows the camera focus.",
        "emitters": [{"name": "motes", "emitter": motes}],
        "params": {"color": {"default": [1, 0.86, 0.45], "doc": "Mote colour (warm pollen, green fireflies).",
                             "targets": [{"to": "motes.startColor"}, {"to": "motes.endColor"}]},
                   "size": {"default": 0.16, "min": 0, "max": 1, "doc": "Mote size.",
                            "targets": [{"to": "motes.startSize"}, {"to": "motes.endSize"}]}},
        "budget": {"maxParticles": 200}})
    smoke = {"texture": TEX + "glow.png", "pixelArt": False, "space": "world", "shape": "sphere", "shapeSize": [0.08, 0, 0],
             "rate": 2, "lifetime": 8, "speed": 0.45, "spread": 10, "direction": [0, 1, 0], "gravity": [0.22, 0, 0.07],
             "drag": 0.05, "turbulence": 0.25, "turbulenceScale": 0.6, "maxParticles": 24, "startSize": 0.5, "endSize": 2.1,
             "startColor": [0.85, 0.82, 0.8], "endColor": [0.85, 0.82, 0.8],
             "opacityOverLife": [[0, 0], [0.15, 0.22], [0.5, 0.3], [1, 0]]}
    write_json("effects/smoke.vfx.json", {
        "format": "oe.vfx", "version": 1, "name": "Chimney smoke", "description": "Slow smoke drifting with the wind.",
        "emitters": [{"name": "smoke", "emitter": smoke}],
        "params": {"color": {"default": [0.85, 0.82, 0.8], "doc": "Smoke tint (follows the sky colour).",
                             "targets": [{"to": "smoke.startColor"}, {"to": "smoke.endColor"}]}},
        "budget": {"maxParticles": 24}})
    dust = {"texture": TEX + "glow.png", "pixelArt": False, "space": "world", "shape": "sphere", "shapeSize": [0.12, 0, 0],
            "rate": 0, "lifetime": 0.8, "speed": 0.5, "spread": 60, "direction": [0, 1, 0], "gravity": [0, -0.2, 0], "drag": 1.5,
            "maxParticles": 40, "startSize": 0.18, "endSize": 0.63, "startColor": [0.8, 0.68, 0.5], "endColor": [0.8, 0.68, 0.5],
            "opacityOverLife": [[0, 0.38], [1, 0]]}
    write_json("effects/dust.vfx.json", {
        "format": "oe.vfx", "version": 1, "name": "Footstep dust", "description": "Small puffs kicked up by the hero's steps (bursts from scripts/hero.lua).",
        "emitters": [{"name": "dust", "emitter": dust}],
        "params": {"color": {"default": [0.8, 0.68, 0.5], "doc": "Dust tint.",
                             "targets": [{"to": "dust.startColor"}, {"to": "dust.endColor"}]}},
        "budget": {"maxParticles": 40}})


# ----- Scene -------------------------------------------------------------------------------------------

entities = []


def add(name, components, parent=None):
    e = {"id": len(entities) + 1, "name": name, "components": components}
    if parent is not None:
        e["parent"] = parent
    entities.append(e)
    return e["id"]


def blob_shadow(parent, radius):
    add("Shadow", {"Transform": {"position": [0, 0.03, 0], "rotation": [-90, 0, 0]},
                   "Sprite": {"texture": TEX + "blob.png", "width": radius * 2, "height": radius * 2, "alphaCutoff": 0,
                              "pixelArt": False, "layer": 0}}, parent)


SPRITE_PPU = {"oak": 20, "oak_lime": 20, "oak_autumn": 20, "oak_blossom": 20, "pine": 20, "birch": 20,
              "bush": 20, "bush_berry": 20, "rock": 20}


def terrain_lua(world):
    heights, blocked, at = world["heights"], world["blocked"], world["at"]
    rows = []
    for j in range(N):
        rows.append('  "' + "".join("#" if blocked[at(i, j)] else str(heights[at(i, j)]) for i in range(N)) + '",')
    write_text("scripts/lib/terrain.lua",
               "-- Generated by tools/make_world.py: terrain rows (z = row - 1, x = column - 1). A digit is the ground\n"
               "-- height of that tile, '#' a tile nobody can stand on (water, houses, trees, lamps, the map edge).\n"
               "local rows = {\n" + "\n".join(rows) + "\n}\n\n"
               "local Terrain = {size = " + str(N) + "}\n\n"
               "-- Ground height at tile (i, j), or -1 where blocked or outside.\n"
               "function Terrain.height(i, j)\n"
               "  local row = rows[j + 1]\n"
               "  if not row or i < 0 or i >= Terrain.size then return -1 end\n"
               "  local c = row:sub(i + 1, i + 1)\n"
               "  if c == \"#\" then return -1 end\n"
               "  return tonumber(c)\n"
               "end\n\n"
               "return Terrain\n")


def scene(world, props, lamps, chimneys):
    cx, cz = world["center"]
    heights, at = world["heights"], world["at"]
    add("Game", {"Script": {"path": "scripts/game.lua", "params": {"startTime": 0.46, "daySeconds": 320, "skipIntro": False}}})
    add("Camera", {"Transform": {"position": [cx, 12, cz + 22], "rotation": [-30, 0, 0]},
                   "Camera": {"fov": 24, "nearPlane": 0.5, "farPlane": 600, "clearColor": [0.8, 0.86, 0.92]},
                   "PostProcess": {"exposure": 1.1, "toneMapping": "aces", "bloom": 0.5, "bloomThreshold": 0.85, "bloomRadius": 10,
                                   "vignette": 0.45, "vignetteRadius": 0.55, "vignetteSoftness": 0.9,
                                   "dofRadius": 6, "dofFocus": 25, "dofRange": 6, "dofFalloff": 18, "fog": 1,
                                   "fogColor": [0.94, 0.86, 0.75], "fogStart": 26, "fogEnd": 70, "saturation": 1.16, "contrast": 1.06,
                                   "shadowTint": [0.86, 0.92, 1.14], "highlightTint": [1.1, 1.02, 0.84]}})
    add("Sun", {"Transform": {"rotation": [-40, -60, 0]},
                "DirectionalLight": {"color": [1, 0.83, 0.6], "intensity": 1.2, "ambient": [0.35, 0.42, 0.55], "hemisphere": True,
                                     "groundAmbient": [0.3, 0.24, 0.17], "shadowStrength": 0.7}})
    add("Sky", {"Transform": {"position": [cx, 0, cz], "scale": [360, 360, 360]},
                "MeshRenderer": {"mesh": "sphere", "material": "materials/sky.mat.json", "castShadows": False, "unlit": True}})
    add("Clouds", {"Transform": {"position": [cx, 0, cz], "scale": [340, 340, 340]},
                   "MeshRenderer": {"mesh": "sphere", "material": "materials/clouds.mat.json", "castShadows": False, "unlit": True}})
    for k, (radius, height, tex) in enumerate([(150, 46, "far"), (128, 36, "far"), (108, 26, "near")]):
        add("Mountains%d" % (k + 1), {"Transform": {"position": [cx, height / 2 - 3, cz], "rotation": [0, math.degrees(k * 1.7), 0],
                                                     "scale": [radius, height, radius]},
                                       "MeshRenderer": {"mesh": "assets/models/mountain_ring.glb", "material": "materials/mountains_%s.mat.json" % tex,
                                                        "castShadows": False, "unlit": True},
                                       "Tag": {"tags": "mountains"}})
    add("Terrain", {"MeshRenderer": {"mesh": "assets/models/terrain.glb", "material": "materials/terrain.mat.json"}})
    add("Water", {"Transform": {"position": [N / 2 - 0.5, 0.78, N / 2 - 0.5], "scale": [700, 1, 700]},
                  "MeshRenderer": {"mesh": "plane", "material": "materials/water.mat.json", "castShadows": False, "unlit": True}})
    add("Houses", {"MeshRenderer": {"mesh": "assets/models/houses.glb", "material": "materials/houses.mat.json"}})
    add("Windows", {"MeshRenderer": {"mesh": "assets/models/windows.glb", "material": "materials/windows.mat.json", "castShadows": False,
                                     "shaderUniforms": {"glow": 0.5}}})
    add("Plants", {"MeshRenderer": {"mesh": "assets/models/plants.glb", "material": "materials/plants.mat.json", "castShadows": False}})
    for k, c in enumerate(chimneys):
        add("Smoke%d" % (k + 1), {"Transform": {"position": [round(v, 3) for v in c]}, "VFXSystem": {"asset": "effects/smoke.vfx.json"},
                                  "Tag": {"tags": "smoke"}})
    add("Motes", {"Transform": {"position": [cx, 3, cz]}, "VFXSystem": {"asset": "effects/motes.vfx.json"}})
    add("Dust", {"Transform": {"position": [cx, 2, cz + 1]}, "VFXSystem": {"asset": "effects/dust.vfx.json"}})
    for k in range(7):
        add("Shaft%d" % (k + 1), {"Transform": {"position": [cx, 10, cz]},
                                  "Sprite": {"texture": TEX + "shaft.png", "width": 1.2 + (k * 0.618 % 1) * 2.2, "height": 26,
                                             "alphaCutoff": 0, "pixelArt": False, "blend": "add", "layer": 3, "opacity": 0.5,
                                             "color": [1, 0.8, 0.5]},
                                  "Tag": {"tags": "shaft"}})
    for k, (i, y, j, phase) in enumerate(lamps):
        lamp = add("Lamp%d" % (k + 1), {"Transform": {"position": [i, y, j]},
                                        "Sprite": {"texture": TEX + "lamp.png", "pixelsPerUnit": 17, "pivotY": 0, "lit": True, "emission": 0.22,
                                                   "castShadows": True},
                                        "Tag": {"tags": "billboard"}})
        blob_shadow(lamp, 0.3)
        add("Halo", {"Transform": {"position": [0, 1.78, 0.05]},
                     "Sprite": {"texture": TEX + "glow.png", "width": 1.7, "height": 1.7, "alphaCutoff": 0, "pixelArt": False,
                                "blend": "add", "layer": 2, "color": [1.5, 0.95, 0.4], "opacity": 0.5},
                     "Tag": {"tags": "halo"}}, lamp)
        add("Light", {"Transform": {"position": [0, 1.9, 0.3]}, "PointLight": {"color": [1, 0.69, 0.35], "intensity": 1.5, "range": 9},
                      "Tag": {"tags": "lamplight,phase%d" % int(phase * 100)}}, lamp)
    for k, (art, x, y, z, blob) in enumerate(props):
        e = add(art.capitalize() + str(k + 1), {
            "Transform": {"position": [round(x, 3), y, round(z, 3)]},
            "Sprite": {"texture": TEX + art + ".png", "pixelsPerUnit": SPRITE_PPU[art], "pivotY": 0, "lit": True, "emission": 0.22, "castShadows": True,
                       "flipX": (k * 7919) % 3 == 0},
            "Tag": {"tags": "billboard"}})
        blob_shadow(e, blob)
    characters = [("Hero", "hero", cx, cz + 1, 0), ("Villager", "npc_villager", cx + 2, cz - 1, 0),
                  ("Farmer", "npc_farmer", cx - 2.5, cz + 1.5, 1), ("Girl", "npc_girl", cx + 4.5, cz - 3.2, 2)]
    for name, sheet, x, z, facing in characters:
        ground = heights[at(round(x), round(z))]
        comps = {"Transform": {"position": [x, ground, z]},
                 "Sprite": {"texture": TEX + sheet + ".png", "columns": 4, "rows": 3, "frame": [0, 4, 4][facing],
                            "flipX": facing == 1, "pixelsPerUnit": 28, "pivotY": 0, "lit": True, "emission": 0.22, "castShadows": True},
                 "Tag": {"tags": "billboard,character"}}
        if name == "Hero":
            comps["Script"] = {"path": "scripts/hero.lua", "params": {"walk": 3.6, "run": 6.2}}
        e = add(name, comps)
        blob_shadow(e, 0.5)
    canvas = add("HUD", {"UICanvas": {}})
    add("Banner", {"UIPanel": {"anchor": "top", "y": 112, "width": 520, "height": 110, "opacity": 0}}, canvas)
    banner = len(entities)
    add("BannerSub", {"UIText": {"text": "C H A P T E R   I", "font": "assets/fonts/NanumMyeongjo-Bold.ttf", "size": 15, "anchor": "top",
                                 "y": 0, "color": [1, 0.96, 0.88], "opacity": 0, "shadowDistance": 2, "shadowColor": [0.16, 0.08, 0]}}, banner)
    add("BannerTitle", {"UIText": {"text": "윈더미어 광장", "font": "assets/fonts/NanumMyeongjo-Bold.ttf", "size": 44, "anchor": "top",
                                   "y": 26, "letterSpacing": 8, "color": [1, 0.96, 0.88], "opacity": 0, "shadowDistance": 3,
                                   "shadowColor": [0.16, 0.08, 0]}}, banner)
    add("BannerRule", {"UIPanel": {"anchor": "top", "y": 92, "width": 340, "height": 1, "color": [1, 0.91, 0.69], "opacity": 0}}, banner)
    add("Clock", {"UIText": {"text": "", "font": "assets/fonts/NanumMyeongjo-Bold.ttf", "size": 15, "anchor": "top-right", "x": -18, "y": 14,
                             "letterSpacing": 2, "color": [1, 1, 1], "opacity": 0, "shadowDistance": 1, "shadowColor": [0, 0, 0]}}, canvas)
    add("Help", {"UIText": {"text": "WASD / 방향키 이동 · Shift 달리기 · Q/E 회전 · 휠 줌 · R/F 각도 · T 시간 정지 · [ ] 시간 조절",
                            "font": "assets/fonts/NanumMyeongjo-Bold.ttf", "size": 13, "anchor": "bottom-left", "x": 16, "y": -14,
                            "color": [1, 1, 1], "opacity": 0, "shadowDistance": 1, "shadowColor": [0, 0, 0]}}, canvas)
    add("Fade", {"UIPanel": {"anchor": "stretch", "color": [0, 0, 0], "opacity": 1, "order": 100}}, canvas)
    write_json("scenes/main.scene.json", {"format": "ownengine.scene", "version": 1, "entities": entities})


def materials():
    write_json("materials/terrain.mat.json", {"baseTexture": TEX + "terrain_atlas.png", "pixelArt": True, "roughness": 1,
                                              "shader": "materials/ground.shader.json"})
    write_json("materials/houses.mat.json", {"baseTexture": TEX + "house_atlas.png", "pixelArt": True, "roughness": 0.95,
                                             "doubleSided": True, "shader": "materials/ground.shader.json"})
    write_json("materials/windows.mat.json", {"baseTexture": TEX + "window.png", "pixelArt": True, "roughness": 0.6,
                                              "shader": "materials/window.shader.json"})
    write_json("materials/plants.mat.json", {"baseTexture": TEX + "plants.png", "pixelArt": True, "roughness": 1, "alphaMode": "mask",
                                             "emissive": [0.18, 0.18, 0.18], "emissiveTexture": TEX + "plants.png",
                                             "alphaCutoff": 0.5, "doubleSided": True, "shader": "materials/plants.shader.json"})
    write_json("materials/water.mat.json", {"baseTexture": TEX + "shore.png", "unlit": True,
                                            "shader": "materials/water.shader.json"})
    write_json("materials/sky.mat.json", {"baseTexture": TEX + "stars.png", "unlit": True, "doubleSided": True, "fog": False,
                                          "shader": "materials/sky.shader.json"})
    write_json("materials/clouds.mat.json", {"baseTexture": TEX + "clouds.png", "unlit": True, "doubleSided": True, "fog": False,
                                             "alphaMode": "blend", "shader": "materials/clouds.shader.json"})
    for tex in ("far", "near"):
        write_json("materials/mountains_%s.mat.json" % tex, {"baseTexture": TEX + "mountains_%s.png" % tex, "unlit": True,
                                                              "doubleSided": True, "fog": False, "alphaMode": "blend"})
    write_json("materials/ground.shader.json", cloud_shadow_graph())
    write_json("materials/plants.shader.json", plants_graph())
    write_json("materials/sky.shader.json", sky_graph())
    write_json("materials/clouds.shader.json", clouds_graph())
    write_json("materials/water.shader.json", water_graph())
    write_json("materials/window.shader.json", window_graph())


def main():
    world = build_terrain()
    props, plants, lamps = scatter(world)
    verts = terrain_model(world)
    chimneys = houses_model(world)
    plant_verts = plants_model(plants)
    mountain_ring_model()
    technical_textures(world)
    materials()
    effects()
    terrain_lua(world)
    scene(world, props, lamps, chimneys)
    print("terrain %d vertices, %d props, %d plants (%d vertices), %d entities" % (verts, len(props), len(plants), plant_verts, len(entities)))


if __name__ == "__main__":
    main()
