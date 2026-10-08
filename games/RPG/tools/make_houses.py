"""Half-timbered village houses, one model per house (used by tools/make_world.py).

Each house is built in its own local frame (footprint centred on the origin, ground at y = 0, front door
facing +z) from real parts instead of painted textures:

- a stone plinth, plaster walls (optionally a second storey jettied out over the front and back),
- a timber frame of corner posts, sill/floor/top beams, studs flanking every opening and corner braces,
- a gable roof with thickness, eave and rake overhangs, wooden fascia/bargeboards, a soffit and a ridge cap;
  the ridge runs along the longer side, so narrow houses show their gable to the street,
- gable walls with a king post and collar beam,
- windows with frames, sills and coloured shutters (the glass is a separate model that glows at night),
- a framed door with a stone step and a small hood, a stone chimney with a cap.

Every surface is cut along a 1 m grid so the 32 px textures keep the same texel size everywhere; per-house
colour variations (plaster, shutters, roof) come from atlas cells and COLOR_0 tints.
"""
import math

import numpy as np

PLINTH = 0.35   # stone base height
BEAM = 0.13     # timber width
BEAM_OUT = 0.05  # how far timber stands proud of the plaster


def rot(axis, angle):
    c, s = math.cos(angle), math.sin(angle)
    x, y, z = axis
    return np.array([[c + x * x * (1 - c), x * y * (1 - c) - z * s, x * z * (1 - c) + y * s],
                     [y * x * (1 - c) + z * s, c + y * y * (1 - c), y * z * (1 - c) - x * s],
                     [z * x * (1 - c) - y * s, z * y * (1 - c) + x * s, c + z * z * (1 - c)]])


class Builder:
    """Adds textured planar polygons to a MeshBuilder, tiled on a 1 m grid in each face's own (u, v) frame."""

    def __init__(self, mesh, atlas):
        self.m, self.atlas = mesh, atlas

    def poly(self, cell, origin, ua, va, pts, normal, color=(1, 1, 1, 1), scale=1.0, stretch=None):
        """pts: (u, v) in metres on the plane origin + ua*u + va*v, counter-clockwise seen from `normal`.
        stretch=(width, height): map the whole polygon box onto the cell once (doors, signs)."""
        o, ua, va, n = (np.asarray(x, float) for x in (origin, ua, va, normal))
        if stretch:
            w, h = stretch
            uv = [(u / w, v / h) for u, v in pts]
            self._emit(cell, o, ua, va, pts, uv, n, color, 0, 0)
            return
        tp = [(u / scale, v / scale) for u, v in pts]
        us, vs = [p[0] for p in tp], [p[1] for p in tp]
        for i in range(math.floor(min(us) + 1e-6), math.ceil(max(us) - 1e-6)):
            for j in range(math.floor(min(vs) + 1e-6), math.ceil(max(vs) - 1e-6)):
                clipped = clip(tp, i, j)
                if len(clipped) < 3:
                    continue
                world = [(u * scale, v * scale) for u, v in clipped]
                uv = [(u - i, v - j) for u, v in clipped]
                self._emit(cell, o, ua, va, world, uv, n, color, 0, 0)

    def _emit(self, cell, o, ua, va, pts, uvs, n, color, _a, _b):
        corners = [tuple(o + ua * u + va * v) for u, v in pts]
        tex = [self.atlas.uv(cell, min(max(u, 0), 1), min(max(v, 0), 1)) for u, v in uvs]
        for k in range(1, len(corners) - 1):
            self.m.quad([corners[0], corners[k], corners[k + 1]], [tex[0], tex[k], tex[k + 1]], tuple(n), color)

    def rect(self, cell, origin, ua, va, w, h, normal, color=(1, 1, 1, 1), stretch=False):
        """Rectangle w x h starting at origin (its top-left seen from the front), v running down."""
        self.poly(cell, origin, ua, va, [(0, 0), (0, h), (w, h), (w, 0)], normal, color, stretch=(w, h) if stretch else None)

    def box(self, cell, size, center=(0, 0, 0), matrix=None, color=(1, 1, 1, 1), faces="xXyYzZ", cells=None):
        """Box of size (sx, sy, sz) around center, then rotated by `matrix` (3x3) and moved by its own center.
        faces: x/X = +x/-x, y/Y = +y/-y, z/Z = +z/-z. cells: optional per-face cell overrides."""
        sx, sy, sz = (s / 2 for s in size)
        R = np.eye(3) if matrix is None else matrix
        c = np.asarray(center, float)
        P = lambda p: tuple(c + R @ np.asarray(p, float))
        N = lambda n: tuple(R @ np.asarray(n, float))
        spec = {
            "x": ((sx, sy, sz), (0, 0, -1), (0, -1, 0), 2 * sz, 2 * sy, (1, 0, 0)),
            "X": ((-sx, sy, -sz), (0, 0, 1), (0, -1, 0), 2 * sz, 2 * sy, (-1, 0, 0)),
            "y": ((-sx, sy, -sz), (1, 0, 0), (0, 0, 1), 2 * sx, 2 * sz, (0, 1, 0)),
            "Y": ((-sx, -sy, sz), (1, 0, 0), (0, 0, -1), 2 * sx, 2 * sz, (0, -1, 0)),
            "z": ((-sx, sy, sz), (1, 0, 0), (0, -1, 0), 2 * sx, 2 * sy, (0, 0, 1)),
            "Z": ((sx, sy, -sz), (-1, 0, 0), (0, -1, 0), 2 * sx, 2 * sy, (0, 0, -1)),
        }
        for f in faces:
            o, ua, va, w, h, n = spec[f]
            self.rect((cells or {}).get(f, cell), P(o), N(ua), N(va), w, h, N(n), color)


def clip(poly, i, j):
    """Clip a polygon (list of (u, v)) to the square [i, i+1] x [j, j+1] (Sutherland-Hodgman)."""
    def cut(points, inside, cross):
        out = []
        for k in range(len(points)):
            a, b = points[k - 1], points[k]
            ia, ib = inside(a), inside(b)
            if ib:
                if not ia:
                    out.append(cross(a, b))
                out.append(b)
            elif ia:
                out.append(cross(a, b))
        return out

    def at_u(x):
        return lambda a, b: (x, a[1] + (b[1] - a[1]) * (x - a[0]) / (b[0] - a[0]))

    def at_v(y):
        return lambda a, b: (a[0] + (b[0] - a[0]) * (y - a[1]) / (b[1] - a[1]), y)

    pts = list(poly)
    for inside, cross in ((lambda p: p[0] >= i, at_u(i)), (lambda p: p[0] <= i + 1, at_u(i + 1)),
                          (lambda p: p[1] >= j, at_v(j)), (lambda p: p[1] <= j + 1, at_v(j + 1))):
        if not pts:
            break
        pts = cut(pts, inside, cross)
    return pts


class WholeTexture:
    """Glass quads use the whole window texture (their own material)."""

    def uv(self, cell, u, v):
        return (u, v)


# ----- One wall face -----------------------------------------------------------------------------------

class Wall:
    """A rectangular wall face: `origin` is its bottom-left corner seen from outside, `along` runs to the right,
    `normal` points out. Openings are (kind, u_center, bottom, width, height)."""

    def __init__(self, origin, along, normal, length, bottom, top):
        self.o = np.asarray(origin, float)
        self.a = np.asarray(along, float)
        self.n = np.asarray(normal, float)
        self.length, self.bottom, self.top = length, bottom, top
        self.openings = []

    def point(self, u, y, out=0.0):
        return self.o + self.a * u + np.array([0, y, 0]) + self.n * out


def timber(b, wall, floors, wood, braces=True):
    """Frame of one wall face: sill, floor and top beams, studs flanking openings and at even spacing, braces."""
    L = wall.length
    up = np.array([0.0, 1.0, 0.0])
    m = np.column_stack([wall.a, up, wall.n])  # local x along the wall, y up, z out
    depth = 0.08
    out = BEAM_OUT - depth / 2 + 0.002

    def beam_h(y):
        c = wall.point(L / 2, y, out + depth / 2)
        b.box(wood, (L, BEAM, depth), c, m, faces="xXyYz")

    def beam_v(u, y0, y1):
        c = wall.point(u, (y0 + y1) / 2, out + depth / 2)
        b.box(wood, (BEAM, y1 - y0, depth), c, m, faces="xXyYz")

    def brace(u0, y0, u1, y1):
        du, dy = u1 - u0, y1 - y0
        length = math.hypot(du, dy)
        ang = math.atan2(dy, du)
        c = wall.point((u0 + u1) / 2, (y0 + y1) / 2, out + depth / 2 - 0.005)
        r = m @ rot((0, 0, 1), ang)
        b.box(wood, (length, BEAM * 0.9, depth * 0.9), c, r, faces="xXyYz")

    levels = [wall.bottom] + floors + [wall.top]
    for y in levels:
        beam_h(min(max(y, wall.bottom + BEAM / 2), wall.top - BEAM / 2))
    for k in range(len(levels) - 1):
        y0, y1 = levels[k] + BEAM / 2, levels[k + 1] - BEAM / 2
        posts = {BEAM / 2, L - BEAM / 2}
        story = [o for o in wall.openings if y0 - 0.01 <= o[2] < y1]
        for kind, uc, bottom, w, h in story:
            posts.add(uc - w / 2 - BEAM / 2 - 0.05)
            posts.add(uc + w / 2 + BEAM / 2 + 0.05)
        # fill wide blank panels with studs roughly every metre
        ordered = sorted(posts)
        extra = set()
        for p, q in zip(ordered, ordered[1:]):
            blank = not any(p < uc < q for _, uc, _, _, _ in story)
            if blank and q - p > 1.3:
                n = round((q - p) / 1.0)
                for s in range(1, n):
                    extra.add(p + (q - p) * s / n)
        for u in sorted(posts | extra):
            if 0 < u < L:
                beam_v(u, y0, y1)
        if braces and y1 - y0 > 1.2:
            # corner braces where the end panel has no opening
            first = min(p for p in posts | extra if p > BEAM)
            last = max(p for p in posts | extra if p < L - BEAM)
            if first > 0.5 and not any(o[1] - o[3] / 2 < first for o in story):
                brace(BEAM, y0 + 0.05, first, y1 - 0.05)
            if L - last > 0.5 and not any(o[1] + o[3] / 2 > last for o in story):
                brace(L - BEAM, y0 + 0.05, last, y1 - 0.05)
        # short horizontal rails under and over windows
        for kind, uc, bottom, w, h in story:
            if kind == "window":
                c = wall.point(uc, bottom - 0.12, out + depth / 2)
                b.box(wood, (w + 0.3, BEAM * 0.8, depth), c, m, faces="xXyYz")


def window(b, glass, wall, uc, bottom, w, h, shutter_tint, wood):
    """Frame, sill and shutters on the wall (body mesh) and the glass quad (window mesh)."""
    up = np.array([0.0, 1.0, 0.0])
    m = np.column_stack([wall.a, up, wall.n])
    t = 0.07
    for du, dy, sw, sh in ((0, h + t / 2, w + 2 * t, t), (-(w + t) / 2, h / 2, t, h), ((w + t) / 2, h / 2, t, h)):
        b.box(wood, (sw, sh, 0.1), wall.point(uc + du, bottom + dy, 0.05), m, faces="xXyYz")
    b.box("stone", (w + 0.28, 0.07, 0.18), wall.point(uc, bottom - 0.035, 0.09), m, faces="xXyYz")  # sill
    for side in (-1, 1):  # shutters, opened flat against the wall
        c = wall.point(uc + side * (w / 2 + t + 0.19), bottom + h / 2, 0.035)
        b.box("shutter", (0.36, h + 0.04, 0.04), c, m, color=shutter_tint, faces="xXyYz")
    left = wall.point(uc - w / 2, bottom + h, 0.012)
    glass.rect("glass", left, wall.a, -up, w, h, wall.n, stretch=True)


def door(b, wall, uc, w, h, wood, roof_cell):
    up = np.array([0.0, 1.0, 0.0])
    m = np.column_stack([wall.a, up, wall.n])
    bottom = wall.bottom
    b.rect("door", wall.point(uc - w / 2, bottom + h, 0.02), wall.a, -up, w, h, wall.n, stretch=True)
    t = 0.09
    for du, dy, sw, sh in ((0, h + t / 2, w + 2 * t, t), (-(w + t) / 2, h / 2, t, h), ((w + t) / 2, h / 2, t, h)):
        b.box(wood, (sw, sh, 0.12), wall.point(uc + du, bottom + dy, 0.06), m, faces="xXyYz")
    # stone step down to the ground
    b.box("stone", (w + 0.5, PLINTH / 2, 0.42), wall.point(uc, PLINTH / 4, 0.21), m, faces="xXyz")
    b.box("stone", (w + 0.3, PLINTH, 0.2), wall.point(uc, PLINTH / 2, 0.1), m, faces="xXyz")
    # small hood on two brackets
    hood_y = bottom + h + 0.28
    depth, width = 0.55, w + 0.6
    pitch = math.radians(28)
    r = m @ rot((1, 0, 0), pitch)
    centre = wall.point(uc, hood_y - math.sin(pitch) * depth / 2, math.cos(pitch) * depth / 2)
    b.box(roof_cell, (width, 0.07, depth), centre, r, cells={"Y": wood, "x": wood, "X": wood, "z": wood, "Z": wood})
    for side in (-1, 1):
        c = wall.point(uc + side * (width / 2 - 0.12), hood_y - 0.2, 0.2)
        b.box(wood, (0.08, 0.08, 0.42), c, m @ rot((1, 0, 0), -math.radians(40)), faces="xXyYz")


# ----- Roof --------------------------------------------------------------------------------------------

def roof(b, along, span_half, length_half, top, rise, roof_cell, wood, ridge_tint, eave=0.38, rake=0.3, thick=0.13):
    """Gable roof. `along` = 'x' or 'z' is the ridge direction. The underside touches the wall top at the wall plane."""
    A = np.array([1.0, 0, 0]) if along == "x" else np.array([0, 0, 1.0])
    B = np.array([0, 0, 1.0]) if along == "x" else np.array([1.0, 0, 0])
    up = np.array([0, 1.0, 0])
    k = rise / span_half
    tv = thick * math.sqrt(1 + k * k)
    run = span_half + eave
    L = length_half + rake
    apex_top = top + rise + tv
    for s in (1, -1):
        Bs = B * s
        down = Bs * run + up * (-k * run)
        slope_len = float(np.linalg.norm(down))
        dn = down / slope_len  # down the slope
        # top surface: u along the ridge, v down the slope
        start = up * apex_top + A * (-L if s == 1 else L)
        ua = A if s == 1 else -A
        normal = np.cross(dn, ua) * -1 if s == 1 else np.cross(dn, ua) * -1
        normal = normal if normal[1] > 0 else -normal
        b.rect(roof_cell, start, ua, dn, 2 * L, slope_len, normal)
        # underside (soffit) - same plane lowered by the vertical thickness
        under = start - up * tv
        b.rect(wood, under + ua * 2 * L, -ua, dn, 2 * L, slope_len, -normal)
        # eave fascia: the end face between top and underside at the eave
        eave_top = start + dn * slope_len
        b.rect(wood, eave_top + ua * 0, ua, -up, 2 * L, tv, Bs)
        # rake bargeboards at both gable ends
        for e in (-1, 1):
            edge = A * e * L
            n = A * e
            pts_world = [up * apex_top + edge, up * apex_top + edge + down, up * (apex_top - tv) + edge + down, up * (apex_top - tv) + edge]
            o = pts_world[0]
            uax = Bs
            vax = -up
            uv = [(float(np.dot(p - o, uax)), float(np.dot(p - o, vax))) for p in pts_world]
            if e * s * (1 if along == "x" else -1) < 0:
                uv = uv[::-1]
            b.poly(wood, o, uax, vax, uv, n)
    # ridge cap along the apex
    cap = up * (apex_top + 0.04)
    b.box(roof_cell, (2 * L + 0.06, 0.12, 0.24) if along == "x" else (0.24, 0.12, 2 * L + 0.06), tuple(cap), color=ridge_tint)


def gable(b, along, span_half, length_half, top, rise, wood):
    """Triangular plaster gable walls with a king post and a collar beam, at both ends of the ridge."""
    A = np.array([1.0, 0, 0]) if along == "x" else np.array([0, 0, 1.0])
    B = np.array([0, 0, 1.0]) if along == "x" else np.array([1.0, 0, 0])
    up = np.array([0, 1.0, 0])
    for e in (-1, 1):
        n = A * e
        right = np.cross(up, n)  # seen from outside: right-hand direction
        o = A * e * length_half + up * top - right * span_half
        b.poly("plaster", o, right, up, [(0, 0), (2 * span_half, 0), (span_half, rise)], n)
        m = np.column_stack([right, up, n])
        c = A * e * length_half + up * (top + rise * 0.45) + n * (BEAM_OUT - 0.02)
        b.box(wood, (BEAM, rise * 0.9, 0.08), tuple(c), m, faces="xXyYz")
        c = A * e * length_half + up * (top + rise * 0.42) + n * (BEAM_OUT - 0.015)
        b.box(wood, (span_half * 1.1, BEAM, 0.08), tuple(c), m, faces="xXyYz")
        c = A * e * length_half + up * (top + BEAM / 2) + n * (BEAM_OUT - 0.02)
        b.box(wood, (2 * span_half, BEAM, 0.08), tuple(c), m, faces="xXyYz")


# ----- The house ---------------------------------------------------------------------------------------

def build(mesh, glass_mesh, atlas, spec):
    """spec: W, D (footprint), floors (1 or 2), roof cell, plaster tint, shutter tint, chimney side.
    Returns the chimney top in local coordinates."""
    b, g = Builder(mesh, atlas), Builder(glass_mesh, WholeTexture())
    W, D = spec["W"], spec["D"]
    wood = "wood"
    floors = spec.get("floors", 1)
    story = 2.3
    jetty = 0.16 if floors == 2 else 0.0
    plaster = spec["plaster"]
    hw, hd = W / 2, D / 2
    top1 = PLINTH + story
    top = top1 + (story - 0.2 if floors == 2 else 0)
    # plinth
    b.box("stone", (W + 0.12, PLINTH, D + 0.12), (0, PLINTH / 2, 0), faces="xXyzZ")
    # walls: ground floor, and an upper floor jettied out front and back
    b.box("plaster", (W, story, D), (0, PLINTH + story / 2, 0), faces="xXzZ", color=plaster)
    upper_d = D + 2 * jetty
    if floors == 2:
        b.box("plaster", (W, top - top1, upper_d), (0, (top1 + top) / 2, 0), faces="xXzZY", color=plaster,
              cells={"Y": wood})
    walls = []
    def face(side, d_half, y0, y1):
        if side == "front":
            return Wall((-hw, 0, d_half), (1, 0, 0), (0, 0, 1), W, y0, y1)
        if side == "back":
            return Wall((hw, 0, -d_half), (-1, 0, 0), (0, 0, -1), W, y0, y1)
        if side == "right":
            return Wall((hw, 0, d_half), (0, 0, -1), (1, 0, 0), D if d_half == hd else upper_d, y0, y1)
        return Wall((-hw, 0, -d_half), (0, 0, 1), (-1, 0, 0), D if d_half == hd else upper_d, y0, y1)

    ridge_x = W >= D
    lower = {s: face(s, hd, PLINTH, top1) for s in ("front", "back", "right", "left")}
    upper = {s: face(s, upper_d / 2, top1, top) for s in ("front", "back", "right", "left")} if floors == 2 else {}
    if floors == 2:  # side walls of the jettied storey start at the outer face
        upper["right"] = Wall((hw, 0, upper_d / 2), (0, 0, -1), (1, 0, 0), upper_d, top1, top)
        upper["left"] = Wall((-hw, 0, -upper_d / 2), (0, 0, 1), (-1, 0, 0), upper_d, top1, top)

    win_w, win_h, sill = 0.62, 0.72, 0.95
    front = lower["front"]
    door_u = W / 2 + spec.get("door_offset", 0.0)
    front.openings.append(("door", door_u, PLINTH, 0.82, 1.62))
    if W >= 3:
        for u in (0.62, W - 0.62):
            if abs(u - door_u) > 1.0:
                front.openings.append(("window", u, PLINTH + sill, win_w, win_h))
    lower["back"].openings.append(("window", W / 2, PLINTH + sill, win_w, win_h))
    for s in ("right", "left"):
        lower[s].openings.append(("window", lower[s].length / 2, PLINTH + sill, win_w, win_h))
    for s, wl in upper.items():
        count = max(1, round(wl.length / 1.6))
        for k in range(count):
            wl.openings.append(("window", wl.length * (k + 0.5) / count, top1 + 0.65, win_w, win_h))

    for wl in list(lower.values()) + list(upper.values()):
        timber(b, wl, [], wood)
        for kind, uc, bottom, w, h in wl.openings:
            if kind == "window":
                window(b, g, wl, uc, bottom, w, h, spec["shutter"], wood)
            else:
                door(b, wl, uc, w, h, wood, spec["roof"])
    # corner posts
    for x in (-hw, hw):
        for z in (-hd, hd):
            b.box(wood, (0.16, top1 - PLINTH, 0.16), (x, (PLINTH + top1) / 2, z), faces="xXzZ")
        if floors == 2:
            for z in (-upper_d / 2, upper_d / 2):
                b.box(wood, (0.16, top - top1, 0.16), (x, (top1 + top) / 2, z), faces="xXzZY")
    if floors == 2:  # joist ends under the jetty
        for z in (-1, 1):
            for k in range(int(W / 0.5) + 1):
                x = -hw + 0.1 + k * (W - 0.2) / int(W / 0.5)
                b.box(wood, (0.1, 0.1, jetty + 0.06), (x, top1 - 0.05, z * (hd + jetty / 2)), faces="xXYzZ")

    span_half = (upper_d if floors == 2 else D) / 2 if ridge_x else W / 2
    length_half = W / 2 if ridge_x else (upper_d if floors == 2 else D) / 2
    rise = span_half * spec.get("pitch", 1.05)
    along = "x" if ridge_x else "z"
    gable(b, along, span_half, length_half, top, rise, wood)
    roof(b, along, span_half, length_half, top, rise, spec["roof"], wood, spec.get("ridge", (0.75, 0.75, 0.75, 1)))
    # chimney through the back slope
    cx = (hw - 0.6) * spec.get("chimney", 1) if ridge_x else 0.35 * spec.get("chimney", 1)
    cz = -span_half * 0.45 if ridge_x else -(length_half - 0.6)
    roof_y = top + rise * (1 - (abs(cz) if ridge_x else abs(cx)) / span_half)
    ch_top = top + rise + 0.75
    b.box("stone", (0.5, ch_top - top + 0.2, 0.5), (cx, (ch_top + top - 0.2) / 2, cz), faces="xXzZ")
    b.box("stone", (0.64, 0.09, 0.64), (cx, ch_top + 0.045, cz), color=(0.7, 0.7, 0.72, 1))
    b.box("stone", (0.36, 0.08, 0.36), (cx, ch_top + 0.13, cz), color=(0.35, 0.33, 0.33, 1), faces="xXyzZ")
    return (cx, ch_top + 0.25, cz), roof_y
