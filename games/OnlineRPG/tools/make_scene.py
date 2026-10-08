"""Writes OnlineRPG's scene, materials, prefabs and scripts/lib/world.lua (python tools/make_scene.py,
from the project folder).

HD-2D look (see samples/HD2D): pixel-art sprites standing in a lit 3D world, seen through a long lens
with depth of field. 1 world unit = 32 texture pixels. The camera never turns, so sprites keep the
rotation written here instead of being turned by a billboard script.

The world is flat: players walk by script (scripts/player.lua) inside WORLD["bounds"] and are pushed
out of WORLD["circles"] / WORLD["boxes"], which this file writes to scripts/lib/world.lua next to the
entities they stand for. Server and predicting client run the same table, so they agree.
"""
import json
import math
import os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")

import hashlib


def vfx(settings):
    """Write one reusable emitter asset; preserve the owner id and index-zero RNG seed."""
    path = "effects/generated-" + hashlib.sha256(json.dumps(settings, sort_keys=True).encode()).hexdigest()[:12] + ".vfx.json"
    defaults = {'startColor': [1, 1, 1], 'endColor': [1, 1, 1], 'speed': 1, 'startSize': 0.100000001, 'playing': True}
    params = {}
    for field, default in defaults.items():
        value = settings.get(field, default)
        if field in ("startColor", "endColor") and isinstance(value, str):
            value = [int(value[i:i + 2], 16) / 255 for i in (1, 3, 5)]
        params[field] = {"default": value, "targets": [{"to": "emitter." + field}]}
    asset = {"format": "oe.vfx", "version": 1, "name": "Generated effect",
             "budget": {"maxParticles": settings.get("maxParticles", 256)},
             "emitters": [{"name": "emitter", "emitter": settings}], "params": params}
    full = os.path.join(ROOT, path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "w", encoding="utf-8", newline="\n") as file:
        json.dump(asset, file, indent=2)
        file.write("\n")
    return {"asset": path}

TEX = "assets/textures/"
FONT = "assets/fonts/Jua-Regular.ttf"
PPU = 32
MAX_PLAYERS = 8
ITEMS = ["potion", "herb", "coin", "gem"]  # keep in sync with scripts/lib/items.lua and tools/make_art.py

# Follow camera: offset from the player and the point above the feet it looks at.
CAMERA_OFFSET = (0, 6.2, 8.8)
LOOK_HEIGHT = 0.9
CAMERA_PITCH = -math.degrees(math.atan2(CAMERA_OFFSET[1] - LOOK_HEIGHT, CAMERA_OFFSET[2]))
CAMERA_DISTANCE = math.hypot(CAMERA_OFFSET[1] - LOOK_HEIGHT, CAMERA_OFFSET[2])
START = (0, 0, 6)  # where the camera looks before a player exists

entities = []
materials = {}
WORLD = {"bounds": [-15, 15, -2.6, 13.5], "circles": [], "boxes": [], "spawns": [], "items": []}


def write_json(path, data):
    full = os.path.join(ROOT, path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)
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


def ground(name, texture, x, y, z, width, depth):
    return add(name, {"Transform": {"position": [x, y, z], "scale": [width, 1, depth]},
                      "MeshRenderer": {"mesh": "plane", "material": material(texture, width, depth), "castShadows": False}})


def block(name, texture, x, y, z, width, height, depth):
    """Box whose front face shows `texture` at the shared texel size; y is its base."""
    return add(name, {"Transform": {"position": [x, y + height / 2, z], "scale": [width, height, depth]},
                      "MeshRenderer": {"mesh": "cube", "material": material(texture, width, height), "shading": "flat"}})


def sprite_values(texture, extra=None):
    values = {"texture": TEX + texture + ".png", "pixelsPerUnit": PPU, "pivotY": 0, "lit": True, "castShadows": True}
    values.update(extra or {})
    return values


def sprite(name, texture, x, y, z, radius=0.0, parent=None):
    """An upright sprite; radius > 0 also blocks walking around its foot."""
    if radius > 0:
        WORLD["circles"].append([x, z, radius])
    return add(name, {"Transform": {"position": [x, y, z]}, "Sprite": sprite_values(texture)}, parent)


# ----- Light and camera -------------------------------------------------------------------------
# The dusk sun shines away from the camera, so sprites are lit from the front and shadows fall behind them.
add("Sun", {"Transform": {"rotation": [-40, -48, 0]},
            "DirectionalLight": {"color": [1.0, 0.66, 0.42], "intensity": 1.25, "ambient": [0.17, 0.2, 0.36],
                                 "shadowStrength": 0.85}})
# Long lens from above: little perspective; the player's distance is the focus.
add("Camera", {"Transform": {"position": [START[0] + CAMERA_OFFSET[0], START[1] + CAMERA_OFFSET[1], START[2] + CAMERA_OFFSET[2]],
                             "rotation": [round(CAMERA_PITCH, 3), 0, 0]},
               "Camera": {"fov": 30, "nearPlane": 1, "farPlane": 120, "clearColor": [0.2, 0.17, 0.3]},
               "PostProcess": {"exposure": 1.7, "toneMapping": "reinhard", "bloom": 0.7, "bloomThreshold": 1.0,
                               "bloomRadius": 12, "vignette": 0.5, "vignetteRadius": 0.55, "vignetteSoftness": 0.7,
                               "dofRadius": 6, "dofFocus": round(CAMERA_DISTANCE, 2), "dofRange": 2.0, "dofFalloff": 6},
               "Script": {"path": "scripts/camera.lua",
                          "params": {"offset": list(CAMERA_OFFSET), "lookHeight": LOOK_HEIGHT, "smoothing": 6}}})
add("Game", {"Script": {"path": "scripts/game.lua"}})

# ----- Terrain ----------------------------------------------------------------------------------
ground("Meadow", "grass", 0, 0, 3, 44, 30)
ground("Plaza", "path", 0, 0.02, 6, 8, 6)
ground("South Road", "path", 0, 0.02, 12.5, 2, 7)
ground("North Road", "path", 0, 0.02, 0, 2, 6)
ground("East Road", "path", 7, 0.02, 6, 6, 2)
block("Terrace", "cliff", 0, 0, -10, 44, 2, 8)
ground("Terrace Top", "grass", 0, 2.01, -10, 44, 8)
add("Pond", {"Transform": {"position": [-9.5, 0.03, 8], "scale": [6, 1, 4]},
             "MeshRenderer": {"mesh": "plane", "material": "materials/water.mat.json", "castShadows": False}})
WORLD["boxes"].append([-9.5, 8, 2.8, 1.8])  # x, z, half width, half depth
# Ruins: a broken wall closing the north side (walking stops in front of it, see bounds).
for index, (x, height) in enumerate(((-13, 2), (-11, 4), (-9, 2), (-7, 4), (-5, 2), (5, 2), (7, 4), (9, 2), (11, 4), (13, 2))):
    block("Ruin %d" % (index + 1), "stone", x, 0, -3.6, 2, height, 1.2)
block("Gate Pillar L", "stone", -2, 0, -3.6, 1, 3, 1)
block("Gate Pillar R", "stone", 2, 0, -3.6, 1, 3, 1)
block("Gate Lintel", "stone", 0, 3, -3.6, 5, 1, 1)
block("Gate Door", "stone", 0, 0, -3.9, 3, 3, 0.4)

# ----- Sprites ----------------------------------------------------------------------------------
for index, (x, y, z) in enumerate(((-18, 2, -9.5), (-11, 2, -8.5), (-4.5, 2, -10), (5, 2, -8.5), (11.5, 2, -10), (18, 2, -8.8))):
    sprite("Terrace Tree %d" % (index + 1), "tree", x, y, z)
for index, (x, z) in enumerate(((-13.5, 2.5), (-6, 11.5), (8.5, 11), (12.5, 1.5), (13.5, 8), (-12.5, 12))):
    sprite("Tree %d" % (index + 1), "tree", x, 0, z, radius=0.55)
for index, (x, z) in enumerate(((-6, 0.5), (6.2, 9.4), (-4.5, 9.8), (9, 3.2), (-13, 5.5), (10.5, 12.4), (3.5, -1.6), (-9, -1.4))):
    sprite("Bush %d" % (index + 1), "bush", x, 0, z)
for index, (x, z) in enumerate(((-4.6, 3.4), (4.6, 3.4), (-4.6, 8.6), (4.6, 8.6), (-1.8, 12.5), (1.8, -0.6))):
    lamp = sprite("Lamp %d" % (index + 1), "lamp", x, 0, z, radius=0.3)
    # In front of the glass, so the sprite itself is lit past white and blooms.
    add("Lamp Light %d" % (index + 1), {"Transform": {"position": [0, 1.75, 0.45]},
                                        "PointLight": {"color": [1.0, 0.68, 0.32], "intensity": 3.2, "range": 8},
                                        "Script": {"path": "scripts/flicker.lua", "params": {"phase": index * 1.7}}}, lamp)
for name, x, z in (("Fireflies Pond", -9.5, 8), ("Fireflies Meadow", 9, 6)):
    add(name, {"Transform": {"position": [x, 1.2, z]},
               "VFXSystem": vfx({"rate": 5, "lifetime": 5, "speed": 0.35, "spread": 180, "speedVariation": 0.6,
                                   "gravity": [0, 0.02, 0], "startSize": 0.1, "endSize": 0.03,
                                   "startColor": [1.6, 1.5, 0.5], "endColor": [0.9, 1.3, 0.3], "startOpacity": 0.9,
                                   "space": "world", "maxParticles": 40, "blend": "add", "seed": len(entities)})})

# Where players appear (by player number) and what lies on the ground when the world starts.
for index in range(MAX_PLAYERS):
    angle = index * 2 * math.pi / MAX_PLAYERS
    WORLD["spawns"].append([round(1.8 * math.sin(angle), 2), round(6 + 1.8 * math.cos(angle), 2)])
WORLD["items"] = [["potion", -2.5, 3.5], ["herb", 3, 4], ["coin", 0, 9.5], ["gem", 6.5, 6], ["coin", -3, 11],
                  ["herb", -6.5, 5], ["potion", 9, 9], ["gem", -11, 3], ["coin", 4, 0.5], ["herb", 11, 4.5]]

# ----- UI ---------------------------------------------------------------------------------------
outline = {"outlineWidth": 2, "outlineColor": [0.05, 0.04, 0.1]}
dark, gold = [0.07, 0.06, 0.16], [0.85, 0.72, 0.45]
add("Title", {"UIText": dict({"text": "OnlineRPG", "size": 26, "x": 24, "y": 18, "color": [1.0, 0.85, 0.55]}, **outline)})
add("Hint", {"UIText": dict({"text": "WASD / arrows: walk   Shift: run   E: pick up   I: bag   Enter: chat   click a bag slot: drop",
                             "size": 16, "anchor": "top", "x": 0, "y": 14, "color": [0.9, 0.88, 0.8]}, **outline)})
add("Prompt", {"UIText": dict({"text": "E  Pick up", "size": 22, "anchor": "center", "x": 0, "y": 150, "visible": False,
                               "color": [1.0, 0.9, 0.6]}, **outline)})

# Name plates and speech bubbles, one per player number; hud.lua moves them over the heads.
# Their order keeps them under the HUD panels (default order -1).
for number in range(1, MAX_PLAYERS + 1):
    add("Name %d" % number, {"UIText": dict({"text": "P%d" % number, "font": FONT, "size": 16, "x": 0, "y": 0, "width": 160,
                                             "align": "center", "visible": False, "order": -4, "color": [0.8, 0.95, 1.0]}, **outline)})
for number in range(1, MAX_PLAYERS + 1):
    bubble = add("Bubble %d" % number, {"UIPanel": {"x": 0, "y": 0, "width": 240, "height": 40, "radius": 12, "color": [0.97, 0.96, 0.9],
                                                    "opacity": 0.94, "borderWidth": 2, "borderColor": [0.2, 0.17, 0.3],
                                                    "visible": False, "order": -3}})
    add("Bubble Text %d" % number, {"UIText": {"text": "", "font": FONT, "size": 18, "x": 12, "y": 9, "width": 216, "align": "center",
                                               "richText": False, "color": [0.12, 0.1, 0.2], "order": -2}}, bubble)

chat = add("Chat", {"UIPanel": {"anchor": "bottom-left", "x": 16, "y": -68, "width": 440, "height": 176, "radius": 10,
                                "color": dark, "opacity": 0.55, "clip": True}})
add("Chat Text", {"UIText": {"text": "", "font": FONT, "size": 17, "x": 12, "y": 8, "width": 416, "height": 160,
                             "verticalAlign": "bottom", "richText": False, "lineSpacing": 1.1, "color": [0.95, 0.94, 0.9]}}, chat)
add("Chat Input", {"UIInput": {"anchor": "bottom-left", "x": 16, "y": -16, "width": 440, "height": 44, "font": FONT, "size": 18,
                               "placeholder": "Enter: chat", "maxLength": 80, "color": dark, "borderColor": gold, "borderWidth": 1},
                   "Script": {"path": "scripts/chatbox.lua"}})

add("Bag Button", {"UIButton": {"text": "Bag (I)", "anchor": "top-right", "x": -16, "y": 16, "width": 110, "height": 40, "size": 18,
                                "color": [0.2, 0.3, 0.5]},
                   "Script": {"path": "scripts/slot.lua", "params": {"index": 0}}})
bag = add("Bag", {"UIPanel": {"anchor": "top-right", "x": -16, "y": 64, "width": 304, "height": 196, "radius": 12, "color": dark,
                              "opacity": 0.88, "borderWidth": 2, "borderColor": gold}})
add("Bag Title", {"UIText": {"text": "Bag", "size": 18, "x": 14, "y": 10, "bold": True, "color": [1.0, 0.82, 0.5]}}, bag)
for index in range(8):
    add("Slot %d" % (index + 1), {"UIButton": {"text": "", "anchor": "top-left", "x": 14 + (index % 4) * 70, "y": 42 + (index // 4) * 74,
                                               "width": 64, "height": 68, "size": 15, "radius": 8, "color": [0.16, 0.15, 0.28],
                                               "borderWidth": 1, "borderColor": [0.4, 0.36, 0.5], "iconSize": 36, "iconSide": "top",
                                               "padding": 2, "pixelArt": True},
                                  "Script": {"path": "scripts/slot.lua", "params": {"index": index + 1}}}, bag)
add("HUD", {"Script": {"path": "scripts/hud.lua"}})

# The way in (scripts/connect.lua): a title over the dimmed world, and a modal popup for bad news.
# blockInput keeps clicks away from the HUD under them; their children stay clickable.
screen = {"anchor": "center", "x": 0, "y": 0, "width": 4000, "height": 4000, "color": [0.03, 0.02, 0.08], "blockInput": True}
add("Net", {"Script": {"path": "scripts/connect.lua"}})
title = add("Title Panel", {"UIPanel": dict(screen, opacity=0.62, order=40)})
add("Title Name", {"UIText": dict({"text": "OnlineRPG", "size": 64, "anchor": "center", "x": 0, "y": -110, "bold": True,
                                   "color": [1.0, 0.85, 0.55], "order": 41}, **outline)}, title)
for name, method, y, color in (("Login", "login", 10, [0.25, 0.45, 0.9]), ("Offline", "offline", 76, [0.2, 0.3, 0.5])):
    add(name, {"UIButton": {"text": name, "font": FONT, "anchor": "center", "x": 0, "y": y, "width": 260, "height": 54, "size": 24,
                            "color": color, "order": 42},
               "Script": {"path": "scripts/button.lua", "params": {"target": "Net", "method": method}}}, title)
add("Title Status", {"UIText": dict({"text": "", "font": FONT, "size": 20, "anchor": "center", "x": 0, "y": 140,
                                     "color": [0.9, 0.9, 0.95], "order": 41}, **outline)}, title)
popup = add("Popup", {"UIPanel": dict(screen, opacity=0.7, order=50, visible=False)})
box = add("Popup Box", {"UIPanel": {"anchor": "center", "x": 0, "y": 0, "width": 480, "height": 190, "radius": 14, "color": dark,
                                    "opacity": 0.96, "borderWidth": 2, "borderColor": gold, "order": 51}}, popup)
add("Popup Text", {"UIText": {"text": "", "font": FONT, "size": 22, "anchor": "top", "x": 0, "y": 38, "width": 420, "align": "center",
                              "color": [0.95, 0.94, 0.9], "order": 52}}, box)
add("Popup OK", {"UIButton": {"text": "OK", "font": FONT, "anchor": "bottom", "x": 0, "y": -24, "width": 160, "height": 48, "size": 22,
                              "color": [0.25, 0.45, 0.9], "order": 52},
                 "Script": {"path": "scripts/button.lua", "params": {"target": "Net", "method": "confirm"}}}, box)

# ----- Prefabs ----------------------------------------------------------------------------------
position_only = {"Transform.position": {"onChange": True}}
write_json("prefabs/player.prefab.json", {"format": "ownengine.prefab", "version": 1, "name": "Player", "entities": [
    {"id": 1, "name": "Player", "components": {
        "Transform": {"position": [START[0], 0, START[2]]},
        "NetPlayer": {"player": 1},
        # player.lua writes what the avatar is doing and the player's name ("walk_left,Guest0001") into the
        # tag; it is replicated so every peer shows the same walk cycle and name plate (hud.lua).
        "Tag": {"tags": "idle_down,"},
        "NetSync": {"owner": 0, "predict": True, "fields": dict(position_only, **{"Tag.tags": {"onChange": True}})},
        "Script": {"path": "scripts/player.lua", "params": {"speed": 3.2}}}},
    {"id": 2, "name": "Art", "parent": 1, "components": {
        "Transform": {},
        "Sprite": sprite_values("hero", {"columns": 4, "rows": 3}),
        "SpriteAnimation": {"clip": "idle_down", "clips": {
            "idle_down": {"frames": [0], "fps": 1}, "walk_down": {"frames": [0, 1, 2, 3], "fps": 8},
            "idle_side": {"frames": [4], "fps": 1}, "walk_side": {"frames": [4, 5, 6, 7], "fps": 8},
            "idle_up": {"frames": [8], "fps": 1}, "walk_up": {"frames": [8, 9, 10, 11], "fps": 8}}}}}]})
for key in ITEMS:
    path = "prefabs/items/%s.prefab.json" % key
    write_json(path, {"format": "ownengine.prefab", "version": 1, "name": key, "entities": [
        {"id": 1, "name": key, "components": {
            "Transform": {},
            "Tag": {"tags": "item"},
            "NetSync": {"owner": 0, "prefab": path, "fields": position_only}}},
        # Unlit and brighter than white, so the bloom makes it glow; it faces the camera.
        {"id": 2, "name": "Art", "parent": 1, "components": {
            "Transform": {"position": [0, 0.45, 0], "rotation": [round(CAMERA_PITCH, 3), 0, 0]},
            "Sprite": {"texture": TEX + "items/" + key + ".png", "pixelsPerUnit": 30, "color": [1.35, 1.35, 1.35]}}}]})

# ----- Files ------------------------------------------------------------------------------------
for path, values in materials.items():
    write_json(path, values)
# Two layers of the water texture slide over each other (uv repeats 3 x 2 over the pond).
write_json("materials/water.shader.json", {
    "format": "ownengine.shader",
    "uniforms": {"repeat": [3, 2, 0, 0], "flowA": [0.035, 0.02, 0, 0], "flowB": [-0.025, 0.03, 0, 0]},
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
write_json("scenes/main.scene.json", {"format": "ownengine.scene", "version": 1, "name": "OnlineRPG", "entities": entities})


def lua(value):
    if isinstance(value, list):
        return "{" + ", ".join(lua(v) for v in value) + "}"
    return json.dumps(value)


with open(os.path.join(ROOT, "scripts/lib/world.lua"), "w", encoding="utf-8", newline="\n") as f:
    f.write("-- Written by tools/make_scene.py: where players may walk and what the world starts with.\n")
    f.write("-- bounds = {minX, maxX, minZ, maxZ}; circles = {x, z, radius}; boxes = {x, z, halfWidth, halfDepth};\n")
    f.write("-- spawns = {x, z} by player number; items = {key, x, z}.\n")
    f.write("local World = {\n")
    for key in ("bounds", "circles", "boxes", "spawns", "items"):
        f.write("  %s = %s,\n" % (key, lua(WORLD[key])))
    f.write("}\n\n")
    f.write("""-- Moves a walker of `radius` from (x, z) by (dx, dz) and returns where it ends up.
function World.move(x, z, dx, dz, radius)
  x, z = x + dx, z + dz
  for _, c in ipairs(World.circles) do
    local ox, oz = x - c[1], z - c[2]
    local d, reach = math.sqrt(ox * ox + oz * oz), c[3] + radius
    if d < reach and d > 1e-4 then x, z = c[1] + ox / d * reach, c[2] + oz / d * reach end
  end
  for _, b in ipairs(World.boxes) do
    local ox, oz = x - b[1], z - b[2]
    local px, pz = b[3] + radius - math.abs(ox), b[4] + radius - math.abs(oz)
    if px > 0 and pz > 0 then -- inside: leave through the nearer side
      if px < pz then x = b[1] + (ox < 0 and -1 or 1) * (b[3] + radius)
      else z = b[2] + (oz < 0 and -1 or 1) * (b[4] + radius) end
    end
  end
  local m = World.bounds
  return math.max(m[1], math.min(m[2], x)), math.max(m[3], math.min(m[4], z))
end

return World
""")
print("wrote %d entities, %d materials" % (len(entities), len(materials) + 1))
