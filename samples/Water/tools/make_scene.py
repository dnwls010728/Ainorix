"""Writes the Water sample's shader graph, material and scene (python tools/make_scene.py, from the sample folder).

The sea is one `plane64` mesh. Its shader graph sums four sine waves, one per register channel:
the `offset` output lifts every vertex, the `normal` output gives each pixel the matching slope
and the color follows the wave height. scripts/waves.lua holds the same wave table, so floating
objects ride the surface the renderer draws.
"""
import json
import math
import os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")

# direction (degrees, in the XZ plane), wavelength, amplitude at sea state 1. Keep in sync with scripts/waves.lua.
WAVES = [(20, 7.0, 0.16), (-35, 4.3, 0.09), (75, 2.3, 0.04), (-110, 1.4, 0.02)]
SPEED = 0.6  # of the deep-water speed sqrt(g * k)

entities = []


def write_json(path, data):
    full = os.path.join(ROOT, path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=2)
        f.write("\n")


def add(name, components, parent=None):
    entity = {"id": len(entities) + 1, "name": name, "components": components}
    if parent is not None:
        entity["parent"] = parent
    entities.append(entity)
    return entity["id"]


def shape(name, mesh, position, scale, color, parent=None, rotation=None, extra=None):
    transform = {"position": position, "scale": scale}
    if rotation:
        transform["rotation"] = rotation
    renderer = {"mesh": mesh, "color": color}
    renderer.update(extra or {})
    return add(name, {"Transform": transform, "MeshRenderer": renderer}, parent)


def floater(name, position, params):
    return add(name, {"Transform": {"position": position}, "Script": {"path": "scripts/float.lua", "params": params}})


# ----- Light and camera -------------------------------------------------------------------------
# A low sun ahead of the camera: the wave slopes facing it glitter.
add("Sun", {"Transform": {"rotation": [-24, 150, 0]},
            "DirectionalLight": {"color": [1.0, 0.9, 0.76], "intensity": 1.5, "ambient": [0.3, 0.4, 0.52],
                                 "shadowStrength": 0.6}})
focus = add("Focus", {"Transform": {"position": [0, 0.4, 0]}})
add("Camera", {"Transform": {"position": [0, 4.6, 13.5], "rotation": [-17, 0, 0]},
               "Camera": {"fov": 45, "nearPlane": 0.3, "farPlane": 200, "clearColor": [0.45, 0.7, 1.0]},
               "CameraFollow": {"target": focus, "offset": [0, 4.2, 13.5], "smoothing": 0},
               "PostProcess": {"exposure": 1.25, "toneMapping": "reinhard", "bloom": 0.35, "bloomThreshold": 1.1,
                               "bloomRadius": 8},
               "Script": {"path": "scripts/orbit.lua"}})

# ----- Sea --------------------------------------------------------------------------------------
add("Water", {"Transform": {"scale": [44, 1, 44]},
              "MeshRenderer": {"mesh": "plane64", "material": "materials/water.mat.json", "castShadows": False},
              "Script": {"path": "scripts/ocean.lua"}})

# ----- Island, lighthouse and pier --------------------------------------------------------------
sand, rock, wood = [0.84, 0.74, 0.52], [0.42, 0.43, 0.46], [0.5, 0.34, 0.2]
shape("Island", "sphere", [-5, -0.9, -4], [9, 3.2, 8], sand)
shape("Rock A", "sphere", [-8.2, 0.1, -1.4], [1.9, 1.5, 1.6], rock, extra={"shading": "flat"})
shape("Rock B", "sphere", [-1.2, 0.0, -7.6], [1.4, 1.2, 1.7], rock, extra={"shading": "flat"})
shape("Rock C", "sphere", [6.5, -0.1, -6.5], [2.4, 1.6, 2.0], rock, extra={"shading": "flat"})
shape("Rock D", "sphere", [8.3, -0.2, -5.2], [1.2, 0.9, 1.1], rock, extra={"shading": "flat"})
for i, (height, color) in enumerate([(1.1, [0.92, 0.92, 0.9]), (1.0, [0.82, 0.2, 0.18]), (0.9, [0.92, 0.92, 0.9])]):
    width = 1.1 - 0.15 * i
    shape("Lighthouse %d" % (i + 1), "cube", [-5.2, 0.7 + 0.55 + sum([1.1, 1.0, 0.9][:i]) - (1.1 - height) / 2, -4.2],
          [width, height, width], color)
add("Lamp", {"Transform": {"position": [-5.2, 4.0, -4.2], "scale": [0.5, 0.5, 0.5]},
             "MeshRenderer": {"mesh": "sphere", "color": [1.0, 0.92, 0.6], "unlit": True, "castShadows": False},
             "PointLight": {"color": [1.0, 0.85, 0.5], "intensity": 2.2, "range": 9}})
shape("Lighthouse Roof", "pyramid", [-5.2, 4.55, -4.2], [1.0, 0.6, 1.0], [0.25, 0.27, 0.32])
for i in range(7):
    shape("Pier Plank %d" % (i + 1), "cube", [-1.6 + i * 0.62, 0.62, -2.3], [0.56, 0.08, 1.3], wood)
for i, x in enumerate([-1.5, 0.4, 2.2]):
    for side, z in enumerate([-2.85, -1.75]):
        shape("Pier Post %d%s" % (i + 1, "ab"[side]), "cube", [x, -0.1, z], [0.14, 1.5, 0.14], [0.36, 0.25, 0.16])

# ----- Things that ride the waves ---------------------------------------------------------------
for i, (x, z) in enumerate([(3.6, 2.4), (-3.2, 4.6), (7.2, -1.6)]):
    buoy = floater("Buoy %d" % (i + 1), [x, 0, z], {"lift": 0.12, "tilt": 0.6})
    shape("Buoy Body", "sphere", [0, 0, 0], [0.62, 0.62, 0.62], [0.9, 0.22, 0.14], buoy)
    shape("Buoy Band", "cube", [0, 0.22, 0], [0.5, 0.1, 0.5], [0.95, 0.95, 0.92], buoy)
    shape("Buoy Mast", "cube", [0, 0.62, 0], [0.06, 0.62, 0.06], [0.2, 0.2, 0.22], buoy)
    shape("Buoy Top", "pyramid", [0, 1.02, 0], [0.22, 0.24, 0.22], [1.0, 0.8, 0.2], buoy)

boat = floater("Boat", [0.6, 0, 3.2], {"lift": 0.2, "tilt": 0.55, "yaw": 28})
shape("Hull", "cube", [0, 0.02, 0], [2.3, 0.42, 0.95], [0.86, 0.86, 0.82], boat)
shape("Keel", "cube", [0, -0.2, 0], [1.9, 0.2, 0.6], [0.16, 0.3, 0.5], boat)
shape("Bow", "pyramid", [1.45, 0.02, 0], [0.95, 0.6, 0.42], [0.86, 0.86, 0.82], boat, rotation=[0, 0, -90])
shape("Cabin", "cube", [-0.45, 0.48, 0], [0.8, 0.5, 0.7], [0.2, 0.42, 0.62], boat)
shape("Mast", "cube", [0.35, 0.95, 0], [0.06, 1.5, 0.06], [0.45, 0.32, 0.2], boat)
shape("Flag", "cube", [0.6, 1.55, 0], [0.45, 0.26, 0.03], [0.95, 0.3, 0.25], boat)

for i, (x, z, yaw) in enumerate([(-2.4, 1.3, 15), (5.0, 5.2, 50)]):
    crate = floater("Crate %d" % (i + 1), [x, 0, z], {"lift": 0.1, "tilt": 1.0, "yaw": yaw})
    shape("Crate Box", "cube", [0, 0, 0], [0.6, 0.6, 0.6], [0.66, 0.48, 0.28], crate, extra={"shading": "flat"})

# ----- UI ---------------------------------------------------------------------------------------
outline = {"outlineWidth": 2, "outlineColor": [0.03, 0.1, 0.18]}
add("Title", {"UIText": dict({"text": "Sea state: Swell", "size": 26, "x": 28, "y": 22, "color": [0.92, 0.98, 1.0]}, **outline)})
add("Hint", {"UIText": dict({"text": "1 Calm   2 Swell   3 Storm      A / D orbit   W / S zoom   Q / E height",
                             "size": 18, "anchor": "bottom", "x": 0, "y": -16, "color": [0.9, 0.95, 1.0]}, **outline)})

# ----- Water graph ------------------------------------------------------------------------------
# Channel i of every register belongs to wave i, so one instruction handles all four waves.
k = [2 * math.pi / length for _, length, _ in WAVES]
dir_x = [round(k[i] * math.cos(math.radians(WAVES[i][0])), 5) for i in range(4)]
dir_z = [round(k[i] * math.sin(math.radians(WAVES[i][0])), 5) for i in range(4)]
speed = [round(-SPEED * math.sqrt(9.8 * k[i]), 5) for i in range(4)]  # negative: the wave travels along its direction
amp = [a for _, _, a in WAVES]
nodes = [
    {"op": "position"},                                    # 0  world position (xz is not moved by the offset)
    {"op": "swizzle", "args": [0], "value": [0, 0, 0, 0]},  # 1  x for every wave
    {"op": "swizzle", "args": [0], "value": [2, 2, 2, 2]},  # 2  z for every wave
    {"op": "uniform", "name": "dirX"},                     # 3
    {"op": "uniform", "name": "dirZ"},                     # 4
    {"op": "multiply", "args": [1, 3]},                    # 5
    {"op": "multiply", "args": [2, 4]},                    # 6
    {"op": "add", "args": [5, 6]},                         # 7  k . xz
    {"op": "time"},                                        # 8
    {"op": "uniform", "name": "speed"},                    # 9
    {"op": "multiply", "args": [8, 9]},                    # 10
    {"op": "add", "args": [7, 10]},                        # 11 phase of each wave
    {"op": "sin", "args": [11]},                           # 12
    {"op": "uniform", "name": "amp"},                      # 13
    {"op": "multiply", "args": [12, 13]},                  # 14 height of each wave
    {"op": "constant", "value": 1},                        # 15
    {"op": "dot", "args": [14, 15]},                       # 16 surface height
    {"op": "constant", "value": [0, 1, 0, 0]},             # 17
    {"op": "multiply", "args": [16, 17]},                  # 18 OFFSET: lift the vertex
    {"op": "cos", "args": [11]},                           # 19
    {"op": "multiply", "args": [19, 13]},                  # 20
    {"op": "multiply", "args": [20, 3]},                   # 21
    {"op": "multiply", "args": [20, 4]},                   # 22
    {"op": "dot", "args": [21, 15]},                       # 23 d height / dx
    {"op": "dot", "args": [22, 15]},                       # 24 d height / dz
    {"op": "constant", "value": [-1, 0, 0, 0]},            # 25
    {"op": "multiply", "args": [23, 25]},                  # 26
    {"op": "constant", "value": [0, 0, -1, 0]},            # 27
    {"op": "multiply", "args": [24, 27]},                  # 28
    {"op": "add", "args": [26, 28]},                       # 29
    {"op": "add", "args": [29, 17]},                       # 30 NORMAL: (-dh/dx, 1, -dh/dz)
    {"op": "uniform", "name": "deep"},                     # 31 trough color
    {"op": "uniform", "name": "shallow"},                  # 32 crest color
    {"op": "constant", "value": 1.5},                      # 33
    {"op": "multiply", "args": [16, 33]},                  # 34
    {"op": "constant", "value": 0.5},                      # 35
    {"op": "add", "args": [34, 35]},                       # 36
    {"op": "constant", "value": 0},                        # 37
    {"op": "clamp", "args": [36, 37, 15]},                 # 38 0 in troughs .. 1 on crests
    {"op": "mix", "args": [31, 32, 38]},                   # 39
    {"op": "constant", "value": 0.3},                      # 40 foam starts at this height
    {"op": "subtract", "args": [16, 40]},                  # 41
    {"op": "constant", "value": 5},                        # 42
    {"op": "multiply", "args": [41, 42]},                  # 43
    {"op": "clamp", "args": [43, 37, 15]},                 # 44
    {"op": "constant", "value": [0.95, 0.98, 1, 1]},       # 45 foam
    {"op": "mix", "args": [39, 45, 44]},                   # 46 COLOR
]
write_json("materials/water.shader.json", {
    "format": "ownengine.shader",
    "uniforms": {"dirX": dir_x, "dirZ": dir_z, "speed": speed, "amp": amp,
                 "deep": [0.02, 0.2, 0.34, 1], "shallow": [0.1, 0.55, 0.62, 1]},
    "nodes": nodes, "color": 46, "normal": 30, "offset": 18,
})
write_json("materials/water.mat.json", {"shader": "materials/water.shader.json", "roughness": 0.14})
write_json("scenes/main.scene.json", {"format": "ownengine.scene", "version": 1, "name": "Sea", "entities": entities})
print("wrote %d entities; waves dirX %s dirZ %s speed %s" % (len(entities), dir_x, dir_z, speed))
