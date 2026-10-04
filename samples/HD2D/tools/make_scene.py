"""Writes the HD2D sample's materials and scene (python tools/make_scene.py, from the sample folder).

HD-2D = pixel-art sprites standing as billboards in a lit 3D world, seen through a long lens with
depth of field. Units: 1 world unit = 32 texture pixels, for sprites (pixelsPerUnit) and for the
64 px block textures (one repeat per 2 units), so every texel has the same size on screen.
"""
import json
import os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
TEX = "assets/textures/"
PPU = 32

entities = []
materials = {}


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


def material(texture, width, height):
    """Material repeating a 64 px texture once per 2 units over a width x height surface."""
    tiling = [width / 2, height / 2]
    name = "materials/%s_%gx%g.mat.json" % (texture, tiling[0], tiling[1])
    materials[name] = {"baseTexture": TEX + texture + ".png", "pixelArt": True, "roughness": 1, "tiling": tiling}
    return name


def ground(name, texture, x, y, z, width, depth, collider=False):
    components = {
        "Transform": {"position": [x, y, z], "scale": [width, 1, depth]},
        "MeshRenderer": {"mesh": "plane", "material": material(texture, width, depth), "castShadows": False},
    }
    if collider:
        components["Collider"] = {"center": [0, -0.5, 0]}
    return add(name, components)


def block(name, texture, x, y, z, width, height, depth, collider=True):
    """Box whose front face shows `texture` at the shared texel size; y is its base."""
    components = {
        "Transform": {"position": [x, y + height / 2, z], "scale": [width, height, depth]},
        "MeshRenderer": {"mesh": "cube", "material": material(texture, width, height), "shading": "flat"},
    }
    if collider:
        components["Collider"] = {}
    return add(name, components)


CAMERA_PITCH = -35.2  # the follow camera looks down at the hero by this angle


def sprite(name, texture, x, y, z, extra=None, components=None, parent=None, facing=False):
    """A billboard sprite. scripts/billboards.lua turns it toward the camera while playing: upright
    around +Y, or fully facing the camera with facing=True. The rotation written here is the same pose
    for the camera's start, so the scene also looks right in the editor."""
    values = {"texture": TEX + texture + ".png", "pixelsPerUnit": PPU, "pivotY": 0, "lit": True, "castShadows": True}
    values.update(extra or {})
    transform = {"position": [x, y, z]}
    if facing:
        transform["rotation"] = [CAMERA_PITCH, 0, 0]
    all_components = {"Transform": transform, "Sprite": values}
    all_components.update(components or {})
    tags = all_components.get("Tag", {}).get("tags", "")
    all_components["Tag"] = {"tags": (tags + "," if tags else "") + ("billboard-camera" if facing else "billboard")}
    return add(name, all_components, parent)


def wall(name, x, y, z, width, height, depth):
    add(name, {"Transform": {"position": [x, y, z], "scale": [width, height, depth]}, "Collider": {}})


# ----- Light and camera -------------------------------------------------------------------------
# The dusk sun shines away from the camera, so sprites are lit from the front and shadows fall behind them.
add("Sun", {"Transform": {"rotation": [-40, -48, 0]},
            "DirectionalLight": {"color": [1.0, 0.66, 0.42], "intensity": 1.25, "ambient": [0.17, 0.2, 0.36],
                                 "shadowStrength": 0.85}})
hero = add("Hero", {"Transform": {"position": [0, 0.6, 7]},
                    "CharacterBody": {"radius": 0.3, "height": 1.2},
                    "Tag": {"tags": "hero"},
                    "Script": {"path": "scripts/hero.lua", "params": {"speed": 3.2}}})
sprite("HeroSprite", "hero", 0, -0.6, 0, {"columns": 4, "rows": 3},
       {"SpriteAnimation": {"clip": "idle_down", "clips": {
           "idle_down": {"frames": [0], "fps": 1}, "walk_down": {"frames": [0, 1, 2, 3], "fps": 8},
           "idle_side": {"frames": [4], "fps": 1}, "walk_side": {"frames": [4, 5, 6, 7], "fps": 8},
           "idle_up": {"frames": [8], "fps": 1}, "walk_up": {"frames": [8, 9, 10, 11], "fps": 8}}}},
       parent=hero)
# Long lens from above: little perspective, and the hero's distance (offset length 10.8) is the focus.
add("Camera", {"Transform": {"position": [0, 6.8, 15.8], "rotation": [CAMERA_PITCH, 0, 0]},
               "Camera": {"fov": 30, "nearPlane": 1, "farPlane": 120, "clearColor": [0.2, 0.17, 0.3]},
               "CameraFollow": {"target": hero, "offset": [0, 6.2, 8.8], "lookOffset": [0, 0.9, 0], "smoothing": 5},
               "PostProcess": {"exposure": 1.7, "toneMapping": "reinhard", "bloom": 0.7, "bloomThreshold": 1.0,
                               "bloomRadius": 12, "vignette": 0.5, "vignetteRadius": 0.55, "vignetteSoftness": 0.7,
                               "dofRadius": 6, "dofFocus": 10.8, "dofRange": 2.0, "dofFalloff": 6}})
add("Game", {"Script": {"path": "scripts/game.lua"}})
add("Billboards", {"Script": {"path": "scripts/billboards.lua"}})

# ----- Terrain ----------------------------------------------------------------------------------
ground("Meadow", "grass", 0, 0, 0, 44, 32, collider=True)
ground("Plaza", "path", 0, 0.02, 0, 8, 6)
ground("South Road", "path", 0, 0.02, 9.5, 2, 13)
ground("North Road", "path", 0, 0.02, -6, 2, 6)
ground("East Road", "path", 7, 0.02, 0, 6, 2)
block("Terrace", "cliff", 0, 0, -13, 44, 2, 8)
ground("Terrace Top", "grass", 0, 2.01, -13, 44, 8)
block("Ledge", "cliff", -16, 0, -6, 8, 2, 6)
ground("Ledge Top", "grass", -16, 2.01, -6, 8, 6)
pond = add("Pond", {"Transform": {"position": [13.5, 0.03, 1], "scale": [8, 1, 6]},
                    "MeshRenderer": {"mesh": "plane", "material": "materials/water.mat.json", "castShadows": False}})
wall("Pond Edge", 13.5, 1, 1, 7.4, 2, 5.4)
for name, x, z, width, depth in (("Wall South", 0, 16.5, 46, 1), ("Wall West", -22.5, 0, 1, 34), ("Wall East", 22.5, 0, 1, 34)):
    wall(name, x, 1.5, z, width, 3, depth)

# ----- Ruins: a broken wall across the north road with a gate ---------------------------------------
for index, (x, height) in enumerate(((-9, 2), (-7, 4), (-5, 2), (5, 2), (7, 4), (9, 2), (11, 2))):
    block("Ruin %d" % (index + 1), "stone", x, 0, -3.6, 2, height, 1.2)
block("Gate Pillar L", "stone", -2, 0, -3.6, 1, 3, 1)
block("Gate Pillar R", "stone", 2, 0, -3.6, 1, 3, 1)
block("Gate Lintel", "stone", 0, 3, -3.6, 5, 1, 1, collider=False)

# ----- Sprites ----------------------------------------------------------------------------------
tree_collider = {"Collider": {"size": [0.9, 3, 0.6], "center": [0, 1.5, 0]}}
for index, (x, y, z) in enumerate(((-18, 2, -12.5), (-11, 2, -11.5), (-4.5, 2, -13), (5, 2, -11.5), (11.5, 2, -13),
                                   (18, 2, -11.8), (-17, 2, -5.5), (-13.5, 0, 3), (-17.5, 0, 9), (-9, 0, 12.5),
                                   (8.5, 0, 11), (17.5, 0, -5), (19, 0, 8.5), (6.5, 0, -7.5))):
    sprite("Tree %d" % (index + 1), "tree", x, y, z, components=tree_collider)
for index, (x, y, z) in enumerate(((-6, 0, -2.5), (6.2, 0, 3.4), (-10, 0, 6), (9, 0, 4.6), (17.8, 0, 2), (10, 0, -2.4),
                                   (-3.5, 0, 12), (4, 0, 14), (-13, 2, -4.2), (2.5, 2, -10.2), (-8, 2, -10.4))):
    sprite("Bush %d" % (index + 1), "bush", x, y, z)
lamp_collider = {"Collider": {"size": [0.3, 2, 0.3], "center": [0, 1, 0]}}
for index, (x, z) in enumerate(((-4.6, -2.6), (4.6, -2.6), (-4.6, 3.4), (4.6, 3.4), (-1.8, 11))):
    lamp = sprite("Lamp %d" % (index + 1), "lamp", x, 0, z, components=lamp_collider)
    # In front of the glass, so the camera-facing sprite itself is lit past white and blooms.
    add("Lamp Light %d" % (index + 1), {"Transform": {"position": [0, 1.75, 0.45]},
                                        "PointLight": {"color": [1.0, 0.68, 0.32], "intensity": 3.2, "range": 8},
                                        "Script": {"path": "scripts/flicker.lua", "params": {"phase": index * 1.7}}}, lamp)
sprite("Elder", "npc", 2.4, 0, -1.2, components={"Collider": {"shape": "capsule", "radius": 0.35, "height": 1.4, "center": [0, 0.7, 0]},
                                                    "Tag": {"tags": "elder"}})
for index, (x, y, z) in enumerate(((-6.5, 0, 1.5), (8.5, 0, -1.2), (0, 0, -8.2), (-11.5, 0, 9.5), (16.2, 0, 5.4))):
    crystal = sprite("Crystal %d" % (index + 1), "crystal", x, y + 0.5, z,
                     {"lit": False, "castShadows": False, "pivotY": 0.5, "pixelsPerUnit": 44, "color": [1.5, 1.5, 1.5]},
                     {"Tag": {"tags": "crystal"}, "Script": {"path": "scripts/crystal.lua", "params": {"phase": index * 1.3}}},
                     facing=True)
    add("Crystal Light %d" % (index + 1), {"PointLight": {"color": [0.35, 0.9, 1.0], "intensity": 1.6, "range": 3.5}}, crystal)
# Fireflies: additive particles drifting over the pond and the meadow.
for name, x, z in (("Fireflies Pond", 13.5, 1), ("Fireflies Meadow", -9, 5)):
    add(name, {"Transform": {"position": [x, 1.2, z]},
               "ParticleEmitter": {"rate": 5, "lifetime": 5, "speed": 0.35, "spread": 180, "speedVariation": 0.6,
                                   "gravity": [0, 0.02, 0], "startSize": 0.1, "endSize": 0.03,
                                   "startColor": [1.6, 1.5, 0.5], "endColor": [0.9, 1.3, 0.3], "startOpacity": 0.9,
                                   "space": "world", "maxParticles": 40, "blend": "add", "seed": len(entities)}})

# ----- UI ---------------------------------------------------------------------------------------
outline = {"outlineWidth": 2, "outlineColor": [0.05, 0.04, 0.1]}
add("Quest", {"UIText": dict({"text": "Crystals 0 / 5", "size": 26, "x": 28, "y": 22, "color": [0.75, 0.95, 1.0]}, **outline)})
add("Hint", {"UIText": dict({"text": "WASD / arrows: walk   Shift: run   E: talk   1: effects   2: depth of field",
                             "size": 18, "anchor": "bottom", "x": 0, "y": -16, "color": [0.9, 0.88, 0.8]}, **outline)})
add("Prompt", {"UIText": dict({"text": "E  Talk", "size": 24, "anchor": "center", "x": 0, "y": 120, "visible": False,
                               "color": [1.0, 0.9, 0.6]}, **outline)})
dialog = add("Dialog", {"UIPanel": {"anchor": "bottom", "x": 0, "y": -56, "width": 860, "height": 150, "radius": 14,
                                    "color": [0.07, 0.06, 0.16], "opacity": 0.88, "borderWidth": 2,
                                    "borderColor": [0.85, 0.72, 0.45], "visible": False}})
add("Dialog Name", {"UIText": {"text": "Elder", "size": 22, "x": 26, "y": 16, "bold": True, "color": [1.0, 0.82, 0.5]}}, dialog)
add("Dialog Text", {"UIText": {"text": "", "size": 22, "x": 26, "y": 52, "width": 808, "lineSpacing": 1.2,
                               "color": [0.95, 0.94, 0.9]}}, dialog)

# ----- Files ------------------------------------------------------------------------------------
for path, values in materials.items():
    write_json(path, values)
# Two layers of the water texture slide over each other (uv repeats 4 x 3 over the pond).
write_json("materials/water.shader.json", {
    "format": "ownengine.shader",
    "uniforms": {"repeat": [4, 3, 0, 0], "flowA": [0.035, 0.02, 0, 0], "flowB": [-0.025, 0.03, 0, 0]},
    "nodes": [
        {"op": "uv"},
        {"op": "uniform", "name": "repeat"},
        {"op": "multiply", "args": [0, 1]},
        {"op": "time"},
        {"op": "uniform", "name": "flowA"},
        {"op": "multiply", "args": [3, 4]},
        {"op": "add", "args": [2, 5]},
        {"op": "texture", "args": [6]},
        {"op": "uniform", "name": "flowB"},
        {"op": "multiply", "args": [3, 8]},
        {"op": "add", "args": [2, 9]},
        {"op": "texture", "args": [10]},
        {"op": "constant", "value": 0.5},
        {"op": "mix", "args": [7, 11, 12]},
    ],
    "color": 13,
})
write_json("materials/water.mat.json", {"shader": "materials/water.shader.json", "baseTexture": TEX + "water.png",
                                        "pixelArt": True, "roughness": 0.25})
write_json("scenes/main.scene.json", {"format": "ownengine.scene", "version": 1, "name": "Lantern Road", "entities": entities})
print("wrote %d entities, %d materials" % (len(entities), len(materials) + 1))
