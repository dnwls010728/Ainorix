"""Generate the NetChase player model, toon graph, materials and prefab.

Python standard library only; the geometry is the original WuwaToon mannequin scaled to
the 1.8 m player and moved so the entity origin (0.6 m above the feet) matches player.lua.
Run it after changing this file: python samples/NetChase/tools/make_art.py
"""
import json
import math
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCALE, ORIGIN, OUTLINE = .5, .6, .014  # model scale, origin height and hull width in metres
PARTS = ("skin", "hair", "coat", "boots", "trim", "eyes", "iris", "ink", "blush")
HULLS = ("skin", "hair", "coat", "boots")


def write_json(path, value):
    target = ROOT / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def add(a, b):
    return tuple(x + y for x, y in zip(a, b))


def mul(a, s):
    return tuple(x * s for x in a)


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def cross(a, b):
    return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])


def unit(v):
    return mul(v, 1 / max(1e-12, math.sqrt(dot(v, v))))


class Mesh:
    def __init__(self):
        self.positions, self.normals, self.uvs, self.indices = [], [], [], []

    def surface(self, point, normal, nu=32, nv=20, u_range=(0, 1), v_range=(0, 1)):
        start = len(self.positions)
        for j in range(nv + 1):
            v = v_range[0] + (v_range[1]-v_range[0]) * j / nv
            for i in range(nu + 1):
                u = u_range[0] + (u_range[1]-u_range[0]) * i / nu
                self.positions.append(point(u, v))
                self.normals.append(unit(normal(u, v)))
                self.uvs.append((i / nu, j / nv))
        for j in range(nv):
            for i in range(nu):
                a = start + j * (nu + 1) + i
                for tri in ((a, a+1, a+nu+1), (a+1, a+nu+2, a+nu+1)):
                    p, q, r = (self.positions[k] for k in tri)
                    face = cross(add(q, mul(p, -1)), add(r, mul(p, -1)))
                    n = add(add(self.normals[tri[0]], self.normals[tri[1]]), self.normals[tri[2]])
                    self.indices.extend(tri if dot(face, n) >= 0 else (tri[0], tri[2], tri[1]))

    def ellipsoid(self, center, scale, nu=32, nv=20, v_range=(0, 1), u_range=(0, 1)):
        def radial(u, v):
            theta, phi = v * math.pi, u * math.tau
            return (math.sin(theta)*math.cos(phi), math.cos(theta), math.sin(theta)*math.sin(phi))
        self.surface(lambda u, v: add(center, tuple(x*s for x, s in zip(radial(u, v), scale))),
                     lambda u, v: tuple(x/s for x, s in zip(radial(u, v), scale)), nu, nv, u_range, v_range)

    def strand(self, start, end, width, depth, bend=(0, 0, 0), taper=True):
        direction = unit(add(end, mul(start, -1)))
        side = unit(cross(direction, (0, 0, 1)))
        back = unit(cross(side, direction))
        def center(v):
            return add(add(mul(start, 1-v), mul(end, v)), mul(bend, math.sin(v*math.pi)))
        def radius(v):
            return max(.012, math.sin(v*math.pi) ** .5 * ((1-v*.7) if taper else 1))
        def radial(u):
            return add(mul(side, math.cos(u*math.tau)), mul(back, math.sin(u*math.tau)))
        def point(u, v):
            return add(center(v), add(mul(side, width*radius(v)*math.cos(u*math.tau)),
                                      mul(back, depth*radius(v)*math.sin(u*math.tau))))
        self.surface(point, lambda u, v: radial(u), 16, 16)

    def save(self, name, outline=False):
        placed = [(p[0]*SCALE, p[1]*SCALE - ORIGIN, p[2]*SCALE) for p in self.positions]
        positions = [add(p, mul(n, OUTLINE)) if outline else p for p, n in zip(placed, self.normals)]
        normals = [mul(n, -1) if outline else n for n in self.normals]
        indices = list(self.indices)
        if outline:
            for i in range(0, len(indices), 3):
                indices[i+1], indices[i+2] = indices[i+2], indices[i+1]
        data, views, accessors = bytearray(), [], []
        def accessor(values, fmt, kind, component, target, bounds=False):
            while len(data) % 4:
                data.append(0)
            offset = len(data)
            for value in values:
                data.extend(struct.pack(fmt, *value) if isinstance(value, tuple) else struct.pack(fmt, value))
            views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(data)-offset, "target": target})
            entry = {"bufferView": len(views)-1, "componentType": component, "count": len(values), "type": kind}
            if bounds:
                entry["min"] = [min(p[k] for p in values) for k in range(3)]
                entry["max"] = [max(p[k] for p in values) for k in range(3)]
            accessors.append(entry)
            return len(accessors)-1
        p = accessor(positions, "<3f", "VEC3", 5126, 34962, True)
        n = accessor(normals, "<3f", "VEC3", 5126, 34962)
        uv = accessor(self.uvs, "<2f", "VEC2", 5126, 34962)
        index = accessor(indices, "<I", "SCALAR", 5125, 34963)
        document = {"asset": {"version": "2.0", "generator": "OwnEngine NetChase original procedural geometry"},
                    "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
                    "meshes": [{"primitives": [{"attributes": {"POSITION": p, "NORMAL": n, "TEXCOORD_0": uv}, "indices": index}]}],
                    "buffers": [{"byteLength": len(data)}], "bufferViews": views, "accessors": accessors}
        header = json.dumps(document, separators=(",", ":")).encode()
        header += b" " * (-len(header) % 4)
        data += b"\0" * (-len(data) % 4)
        binary = struct.pack("<III", 0x46546C67, 2, 12+8+len(header)+8+len(data))
        binary += struct.pack("<II", len(header), 0x4E4F534A) + header
        binary += struct.pack("<II", len(data), 0x004E4942) + data
        target = ROOT / "assets/models" / (name + ".glb")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(binary)


def character():
    parts = {name: Mesh() for name in ("skin", "hair", "coat", "boots", "trim", "eyes", "iris", "ink", "blush")}
    parts["skin"].ellipsoid((0, 3.08, 0), (.45, .55, .38), 48, 32)
    parts["skin"].ellipsoid((0, 2.59, 0), (.13, .2, .13))
    parts["skin"].ellipsoid((0, 3.0, .375), (.043, .062, .064), 16, 12)
    parts["coat"].ellipsoid((0, 2.1, 0), (.39, .54, .24))
    parts["coat"].ellipsoid((0, 1.61, 0), (.43, .29, .25))
    parts["boots"].ellipsoid((0, 1.48, 0), (.34, .24, .23))
    for side in (-1, 1):
        parts["skin"].ellipsoid((side*.44, 3.06, 0), (.065, .13, .075), 20, 16)
        parts["coat"].strand((side*.32, 2.4, 0), (side*.69, 1.69, .07), .14, .14, taper=False)
        parts["skin"].ellipsoid((side*.7, 1.63, .09), (.105, .17, .09))
        parts["boots"].ellipsoid((side*.21, .88, 0), (.16, .7, .15))
        parts["boots"].ellipsoid((side*.21, .16, .11), (.18, .15, .32))
        parts["trim"].strand((side*.27, 2.5, .17), (side*.19, 1.71, .24), .024, .024, taper=False)
        parts["trim"].strand((side*.14, 1.44, .16), (side*.17, .45, .145), .018, .018, taper=False)
        parts["eyes"].ellipsoid((side*.195, 3.13, .353), (.145, .085, .045), 24, 16)
        parts["iris"].ellipsoid((side*.185, 3.128, .399), (.057, .065, .009), 24, 16)
        parts["ink"].ellipsoid((side*.185, 3.128, .409), (.022, .055, .004), 16, 12)
        parts["eyes"].ellipsoid((side*.166, 3.155, .416), (.017, .021, .004), 12, 8)
        parts["ink"].strand((side*.08, 3.188, .37), (side*.32, 3.193, .35), .015, .012, (0, .022, .027), False)
        parts["blush"].ellipsoid((side*.29, 3.0, .318), (.065, .022, .012), 16, 10)
        parts["hair"].strand((side*.33, 3.46, .05), (side*.54, 2.13, .03), .16, .12, (side*.11, 0, .08))
        parts["hair"].strand((side*.39, 3.36, -.07), (side*.65, 1.94, -.15), .15, .1, (side*.06, 0, -.06))
    # Top cap and back sheet leave the front of the face visible.
    parts["hair"].ellipsoid((0, 3.12, -.015), (.48, .55, .415), 48, 24, (0, .40))
    parts["hair"].ellipsoid((0, 3.12, -.015), (.48, .55, .415), 32, 20, (.38, .92), (.5, 1))
    for i in range(7):
        x = (i-3)*.108
        parts["hair"].strand((x*.7, 3.56, .12), (x+.03, 3.22+abs(i-3)*.015, .397),
                             .095, .036, (-.015, .015, .16))
    for i in range(7):
        x = (i-3)*.115
        parts["hair"].strand((x, 3.18, -.36), (x*1.4, 1.9+abs(i-3)*.05, -.28), .12, .065, (0, 0, -.08))
    parts["ink"].strand((-.045, 2.911, .355), (.045, 2.911, .355), .009, .009, (0, -.005, .005), False)
    parts["trim"].ellipsoid((0, 2.42, .24), (.09, .11, .025), 20, 16)
    for name, mesh in parts.items():
        mesh.save(name)
        if name in HULLS:
            mesh.save(name + "-outline", True)


def graph():
    """Hard light band with a tinted shadow, a view-angle rim and a quantized highlight.

    cameraPosition and lightDirection are reserved uniform names: the renderer replaces
    them every frame with the view position and the first directional light.
    """
    nodes = []
    def node(op, args=None, **kwargs):
        value = {"op": op, **kwargs}
        if args is not None:
            value["args"] = args
        nodes.append(value)
        return len(nodes)-1
    normal = node("normal")
    light = node("uniform", name="lightDirection")
    band = node("step", [node("uniform", name="shadowCut"), node("dot", [normal, light])])
    white = node("constant", value=[1, 1, 1, 1])
    ramp = node("mix", [node("uniform", name="shadowTint"), white, band])
    color = node("multiply", [node("baseColor"), ramp])
    eye = node("uniform", name="cameraPosition")
    view = node("normalize", [node("subtract", [eye, node("position")])])
    edge = node("subtract", [white, node("dot", [normal, view])])
    rim = node("step", [node("uniform", name="rimStart"), edge])
    color = node("add", [color, node("multiply", [rim, node("uniform", name="rimColor")])])
    half = node("normalize", [node("add", [light, view])])
    spec = node("step", [node("uniform", name="specCut"), node("dot", [normal, half])])
    color = node("add", [color, node("multiply", [spec, node("uniform", name="specColor")])])
    assert len(nodes) <= 32
    return {"format": "ownengine.shader", "uniforms": {
            "lightDirection": [0, 1, 0, 0], "cameraPosition": [0, 3, 12, 1], "shadowCut": .12,
            "shadowTint": [.45, .37, .6, 1], "rimStart": .78, "rimColor": [.06, .15, .18, 0],
            "specCut": .965, "specColor": [.14, .14, .14, 0]}, "nodes": nodes, "color": color}


def materials():
    write_json("materials/toon.shader.json", graph())
    write_json("materials/outline.mat.json", {"format": "ownengine.material", "unlit": True, "baseColor": [.018, .022, .04]})
    # The coat is white: MeshRenderer.color on the player root tints it with the player colour.
    palettes = {"skin": [.96, .77, .65], "hair": [.14, .18, .24], "coat": [1, 1, 1],
                "boots": [.10, .12, .16], "trim": [.95, .96, 1], "eyes": [.96, .97, .94],
                "iris": [.12, .64, .67], "ink": [.035, .025, .045], "blush": [.76, .35, .37],
                "prop": [1, 1, 1]}
    for name, color in palettes.items():
        overrides = {"shadowTint": [.63, .43, .47, 1] if name == "skin" else [.36, .40, .58, 1]}
        if name in ("eyes", "ink", "blush"):
            overrides.update({"shadowTint": [.85, .85, .9, 1], "rimColor": [0, 0, 0, 0], "specColor": [0, 0, 0, 0]})
        if name == "hair":
            overrides.update({"specColor": [.20, .28, .30, 0], "rimColor": [.08, .2, .23, 0]})
        if name == "prop":  # arena pillars and the crystal: a wider band and a brighter highlight
            overrides.update({"shadowCut": .05, "specCut": .93, "specColor": [.3, .3, .26, 0]})
        write_json(f"materials/{name}-toon.mat.json", {"format": "ownengine.material", "baseColor": color,
                   "unlit": True, "shader": "materials/toon.shader.json", "shaderUniforms": overrides})


def prefab():
    def renderer(name, **extra):
        return {"mesh": f"assets/models/{name}.glb", "material": f"materials/{name}-toon.mat.json", **extra}
    entities = [{"id": 1, "name": "Player", "components": {
        "Transform": {"position": [0, ORIGIN, 9], "rotation": [0, 180, 0]},
        "MeshRenderer": renderer("coat", color=[.22, .65, 1]),
        "NetPlayer": {"player": 1},
        "NetSync": {"owner": 0, "predict": True, "fields": {
            "Transform.position": {"onChange": True}, "Transform.rotation": {"onChange": True},
            "MeshRenderer.color": {"onChange": True}}},
        "Script": {"path": "scripts/player.lua"}}}]
    def child(name, components):
        entities.append({"id": len(entities)+1, "name": name, "parent": 1, "components": components})
    for name in PARTS:
        if name != "coat":
            child("Part_"+name, {"Transform": {}, "MeshRenderer": renderer(name)})
    for name in HULLS:
        child("Outline_"+name, {"Transform": {}, "MeshRenderer": {
            "mesh": f"assets/models/{name}-outline.glb", "material": "materials/outline.mat.json", "castShadows": False}})
    write_json("prefabs/player.prefab.json", {"format": "ownengine.prefab", "version": 1, "name": "Player", "entities": entities})


if __name__ == "__main__":
    character()
    materials()
    prefab()
