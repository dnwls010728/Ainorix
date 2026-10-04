"""Writes the prefabs and the two scenes (scenes/menu.scene.json, scenes/run.scene.json).

They are plain engine files: open and edit them in the editor like any other. This script
exists so the many similar entities (ten enemy prefabs, card grids, dialogs) stay
consistent; rerun it after changing a layout here. Run from the project folder:

    python tools/make_content.py
"""
import json
import os
import re

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
U = 1 / 40  # world units per design pixel (same as scripts/lib/balance.lua)
FX, SPR, ICON = "assets/fx/", "assets/sprites/", "assets/icons/"
FONT = "assets/fonts/Jua-Regular.ttf"
with open(os.path.join(ROOT, "assets/sprites/animated/animations.json"), encoding="utf-8") as f:
    ANIMATIONS = json.load(f)

# 2D sorting layers (Sprite.layer): the darkness sits at DARK, everything above it stays visible.
L_POOL, L_RING, L_BRAZIER, L_GLOW, L_PICKUP, L_SHADOW, L_ENEMY, L_PLAYER, L_LANTERN, L_PROJ, L_FX, L_PART = range(1, 13)
L_DARK, L_EYES, L_ARROW = 20, 21, 22

# Collision layers (Collider2D.layer)
C_WALL, C_PLAYER, C_ENEMY, C_SHOT, C_EBULLET, C_PICKUP, C_ZONE, C_HURT, C_MAGNET = range(9)
ALL = list(range(9))


def without(*keep):
    return [l for l in ALL if l not in keep]


def rgb(hexcolor):
    h = hexcolor.lstrip("#")
    return [round(int(h[i:i + 2], 16) / 255, 4) for i in (0, 2, 4)]


def english_strings():
    """key -> English text from scripts/lib/i18n.lua, so the scenes read well in the editor before any script runs."""
    text = open(os.path.join(ROOT, "scripts", "lib", "i18n.lua"), encoding="utf-8").read()
    return dict(re.findall(r'\["([\w.]+)"\]\s*=\s*\{\s*en\s*=\s*"((?:[^"\\]|\\.)*)"', text))


EN = english_strings()


def describe(components):
    """A readable name for an entity that scripts never look up."""
    key = components.get("Tag", {}).get("tags", "")
    key = key[2:] if key.startswith("t:") else ""
    if "UIButton" in components:
        return "Button " + (key or components["UIButton"].get("text") or "card")
    if "UIText" in components:
        return "Text " + (key or components["UIText"].get("text") or "label")
    if "UIImage" in components:
        return "Image " + os.path.splitext(os.path.basename(components["UIImage"].get("texture", "")))[0]
    if "UISlider" in components:
        return "Bar"
    if "UIPanel" in components:
        layout = components.get("UILayout", {}).get("direction")
        return {"vertical": "Column", "horizontal": "Row", "grid": "Grid"}.get(layout, "Panel")
    return "Entity"


class Doc:
    """An entity list with ids, for a scene or a prefab."""

    def __init__(self):
        self.entities = []
        self.auto = 0

    def add(self, name, components, parent=None):
        if name is None:
            name = describe(components)
        ent = {"id": len(self.entities) + 1, "name": name}
        if parent:
            ent["parent"] = parent
        comps = dict(components)
        comps.setdefault("Transform", {})
        ent["components"] = comps
        self.entities.append(ent)
        return ent["id"]

    def write(self, path, fmt, name):
        full = os.path.join(ROOT, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w", encoding="utf-8", newline="\n") as f:
            json.dump({"format": fmt, "version": 1, "name": name, "entities": self.entities}, f, indent=2, ensure_ascii=False)
            f.write("\n")


def sprite(tex, w, h, layer, **kw):
    s = {"width": w, "height": h, "alphaCutoff": 0, "pixelArt": False, "layer": layer}
    if tex:
        s["texture"] = tex
    for k, v in kw.items():
        s[k] = rgb(v) if k == "color" else v
    return s


def glow(r, color, opacity, layer, **kw):
    return sprite(FX + "glow.png", r * 2, r * 2, layer, color=color, opacity=opacity, blend="add", **kw)


def animated_sprite(name, w, h, layer, clip="idle"):
    data = ANIMATIONS[name]
    scale = data.get("sizeScale", 1)
    components = {"Sprite": sprite(data["texture"], w * scale, h * scale, layer, columns=data["columns"], rows=data["rows"])}
    if data["clips"]:
        components["SpriteAnimation"] = {"clip": clip, "clips": data["clips"]}
    return components


def script(path, **params):
    s = {"path": "scripts/" + path}
    if params:
        s["params"] = params
    return s


def prefab(name, build):
    d = Doc()
    build(d)
    d.write("prefabs/%s.prefab.json" % name, "ownengine.prefab", name)


# ---------------------------------------------------------------- world prefabs

ENEMIES = {  # kind: (radius px, sprite, eye colour, boss)
    "shade": (13, "enemy_shade", "#ffffff", False), "skitter": (9, "enemy_skitter", "#c9fbff", False),
    "brute": (22, "enemy_brute", "#ffc4ec", False), "wisp": (12, "enemy_wisp", "#ffffff", False),
    "gloom": (17, "enemy_gloom", "#fff0a8", False), "gloomlet": (10, "enemy_gloom", "#fff0a8", False),
    "stalker": (13, "enemy_stalker", "#ffb0c0", False), "leech": (12, "enemy_leech", "#f0ffb0", False),
    "king": (44, "boss_king", "#ffe08a", True), "eclipse": (58, "boss_eclipse", "#ffb060", True),
}


def enemy_prefab(kind):
    rpx, tex, eye, boss = ENEMIES[kind]
    r = rpx * U
    size = r * (3.3 if boss else 3.5)

    def build(d):
        comps = {
            "RigidBody2D": {"gravityScale": 0, "fixedRotation": True, "mass": 8 if boss else 1},
            "Collider2D": {"shape": "circle", "radius": r, "friction": 0, "layer": C_ENEMY,
                           "ignoreLayers": [C_PLAYER, C_EBULLET, C_PICKUP, C_ZONE, C_MAGNET] + ([C_ENEMY] if boss else [])},
            "Script": script("run/enemy.lua", kind=kind),
            "Tag": {"tags": "enemy"},
        }
        if boss:  # a boss is dimly visible in the dark
            comps["Light2D"] = {"radius": r * 2.4, "strength": 0.5}
        root = d.add("Enemy " + kind, comps)
        d.add("Shadow", {"Transform": {"position": [0, -r * 0.8, 0]}, "Sprite": sprite(FX + "shadow.png", r * 1.8, r * 0.7, L_SHADOW, opacity=0.35)}, root)
        d.add("Aura", {"Sprite": glow(r * 2.5, eye if boss else "#ff9ac0", 0.35, L_SHADOW, order=1, visible=boss)}, root)
        d.add("Body", {"Transform": {"position": [0, r * 0.25, 0]}, **animated_sprite(tex, size, size, L_ENEMY, "move")}, root)
        d.add("Flash", {"Transform": {"position": [0, r * 0.25, 0]}, "Sprite": glow(r * 1.5, "#ffffff", 0.7, L_FX, visible=False)}, root)
        if kind == "eclipse":
            d.add("Eyes", {"Sprite": sprite(FX + "disc.png", r * 0.64, r * 0.64, L_EYES, color=eye, visible=False)}, root)
        else:
            d.add("Eyes", {"Transform": {"position": [0, r * 0.2, 0]},
                           "Sprite": sprite(FX + "eyes.png", r * 1.6, r * 0.4, L_EYES, color=eye, visible=False)}, root)

    prefab("enemy_" + kind, build)


def shot(kind, radius_px, extra):
    def build(d):
        comps = {
            "RigidBody2D": {"type": "kinematic", "gravityScale": 0, "bullet": True},
            "Collider2D": {"shape": "circle", "radius": radius_px * U, "isTrigger": True, "layer": C_SHOT, "ignoreLayers": without(C_ENEMY)},
            "Script": script("run/projectile.lua", kind=kind),
        }
        root = d.add("Shot " + kind, comps)
        extra(d, root)

    prefab("shot_" + kind, build)


def bolt_parts(d, root):
    d.add("Glow", {"Sprite": glow(20 * U, "#ff9a3c", 0.9, L_PROJ)}, root)
    d.add("Core", {"Sprite": glow(9 * U, "#fff6d6", 1, L_PROJ, order=1)}, root)
    d.add("Trail", {"ParticleEmitter": {
        "rate": 60, "lifetime": 0.18, "speed": 0, "gravity": [0, 0, 0], "startSize": 4.5 * U, "endSize": 1.5 * U,
        "startColor": rgb("#ff9a3c"), "endColor": rgb("#ff9a3c"), "space": "world", "dimensions": 2,
        "texture": FX + "disc.png", "blend": "add", "layer": L_PART, "maxParticles": 16}}, root)


def shard_parts(d, root):
    d.add("Glow", {"Sprite": glow(12 * U, "#9ff3ff", 0.5, L_PROJ)}, root)
    d.add("Body", {"Sprite": sprite(FX + "shard.png", 19 * U, 19 * U, L_PROJ, order=1, color="#c9f7ff")}, root)


def moth_parts(d, root):
    d.add("Glow", {"Sprite": glow(18 * U, "#e6c7ff", 0.6, L_PROJ)}, root)
    d.add("Body", {"Sprite": sprite(FX + "moth.png", 18 * U, 15 * U, L_PROJ, order=1, color="#f0dcff")}, root)


def ebullet_prefab():
    def build(d):
        root = d.add("Enemy bullet", {
            "RigidBody2D": {"type": "kinematic", "gravityScale": 0},
            # a solid shape nothing collides with: the player's hurtbox sensor still sees it
            "Collider2D": {"shape": "circle", "radius": 7 * U, "layer": C_EBULLET, "ignoreLayers": without(C_HURT)},
            "Script": script("run/projectile.lua", kind="ebullet"),
            "Tag": {"tags": "ebullet"},
        })
        d.add("Glow", {"Sprite": glow(20 * U, "#c9a0ff", 0.8, L_PROJ)}, root)
        d.add("Core", {"Sprite": sprite(FX + "disc.png", 11 * U, 11 * U, L_PROJ, order=1, color="#f3e6ff")}, root)

    prefab("ebullet", build)


def pickup_prefab(kind, parts, light=None):
    def build(d):
        comps = {
            "RigidBody2D": {"type": "kinematic", "gravityScale": 0},
            "Collider2D": {"shape": "circle", "radius": 6 * U, "layer": C_PICKUP, "ignoreLayers": without(C_HURT, C_MAGNET)},
            "Script": script("run/pickup.lua", kind=kind),
            "Tag": {"tags": "chest" if kind == "chest" else "pickup"},
        }
        if light:
            comps["Light2D"] = light
        root = d.add("Pickup " + kind, comps)
        parts(d, root)

    prefab("pickup_" + kind, build)


def brazier_prefab():
    def build(d):
        root = d.add("Brazier", {
            "Collider2D": {"shape": "circle", "radius": 90 * U, "isTrigger": True, "layer": C_ZONE, "ignoreLayers": without(C_PLAYER)},
            "Light2D": {"radius": 70 * U, "strength": 0.55},
            "Script": script("run/brazier.lua"),
            "Tag": {"tags": "brazier"},
        })
        d.add("Ring", {"Sprite": sprite(FX + "ring_dash.png", 185 * U, 185 * U, L_RING, color="#ffb347", opacity=0.45),
                       "Script": script("fx/spin.lua", speed=23)}, root)
        d.add("Progress", {"Transform": {"scale": [0.001, 0.001, 1]},
                           "Sprite": sprite(FX + "disc.png", 180 * U, 180 * U, L_RING, order=1, color="#ffb347", opacity=0.3)}, root)
        d.add("Body", {"Transform": {"position": [0, 10 * U, 0]}, **animated_sprite("brazier", 72 * U, 72 * U, L_BRAZIER, "unlit")}, root)
        d.add("Ember", {"Transform": {"position": [0, 6 * U, 0]}, "Sprite": glow(14 * U, "#ff7a3c", 0.5, L_GLOW)}, root)
        d.add("Beacon", {"Transform": {"position": [0, 6 * U, 0]}, "Sprite": glow(34 * U, "#ff9a3c", 0.3, L_EYES)}, root)
        d.add("Fire", {"Transform": {"position": [0, 10 * U, 0]}, "Sprite": glow(46 * U, "#ff9a3c", 0.9, L_GLOW, visible=False)}, root)
        d.add("FireCore", {"Transform": {"position": [0, 14 * U, 0]}, "Sprite": glow(22 * U, "#fff2b0", 1, L_GLOW, order=1, visible=False)}, root)
        d.add("Sparks", {"Transform": {"position": [0, 12 * U, 0]}, "ParticleEmitter": {
            "rate": 15, "lifetime": 0.7, "lifetimeVariation": 0.4, "speed": 80 * U, "speedVariation": 0.4, "spread": 12,
            "gravity": [0, 0, 0], "startSize": 4 * U, "endSize": 1 * U, "startColor": rgb("#ffb347"), "endColor": rgb("#ff7a3c"),
            "space": "world", "dimensions": 2, "texture": FX + "disc.png", "blend": "add", "layer": L_PART, "maxParticles": 24,
            "playing": False}}, root)

    prefab("brazier", build)


def misc_prefabs():
    def weapon(d):
        d.add("Weapon", {"Script": script("run/weapon.lua", id="ember_bolt")})

    prefab("weapon", weapon)

    def orb(d):
        root = d.add("Orb", {"Sprite": glow(15 * 2.4 * U, "#ff6a2a", 0.9, L_PROJ)})
        d.add("Core", {"Sprite": glow(15 * 1.1 * U, "#fff2b0", 1, L_PROJ, order=1)}, root)
        d.add("Trail", {"ParticleEmitter": {
            "rate": 24, "lifetime": 0.3, "speed": 0, "gravity": [0, 0, 0], "startSize": 6 * U, "endSize": 2 * U,
            "startColor": rgb("#ff9a3c"), "endColor": rgb("#ff9a3c"), "space": "world", "dimensions": 2,
            "texture": FX + "disc.png", "blend": "add", "layer": L_PART, "maxParticles": 12}}, root)

    prefab("orb", orb)

    def flask(d):
        root = d.add("Flask", {"Script": script("run/flask.lua"), "Sprite": sprite(FX + "shadow.png", 14 * U, 6 * U, L_SHADOW, opacity=0.3)})
        body = d.add("Body", {"Sprite": sprite(FX + "disc.png", 13 * U, 13 * U, L_PROJ, order=1, color="#d8ff8a")}, root)
        d.add("Glow", {"Sprite": glow(16 * U, "#c8ff5a", 0.7, L_PROJ)}, body)

    prefab("flask", flask)

    def pool(d):
        root = d.add("Pool", {"Script": script("run/pool.lua"), "Sprite": sprite(FX + "pool.png", 1, 1, L_POOL, opacity=0.55)})
        d.add("Bubbles", {"ParticleEmitter": {
            "rate": 18, "lifetime": 0.5, "speed": 50 * U, "spread": 180, "direction": [0, 1, 0], "gravity": [0, 1.2, 0],
            "startSize": 4 * U, "endSize": 1 * U, "startColor": rgb("#ffc04a"), "endColor": rgb("#ffc04a"), "speedVariation": 1,
            "space": "world", "dimensions": 2, "texture": FX + "disc.png", "blend": "add", "layer": L_PART, "maxParticles": 16}}, root)

    prefab("pool", pool)

    def burst(d):
        d.add("Burst", {"Script": script("fx/burst.lua"), "ParticleEmitter": {
            "rate": 0, "burst": 0, "loop": False, "lifetime": 0.8, "lifetimeVariation": 0.6, "speed": 4, "speedVariation": 0.7,
            "drag": 4, "spread": 180, "gravity": [0, 0, 0], "startSize": 5 * U, "endSize": 2 * U, "sizeVariation": 0.6,
            "space": "world", "dimensions": 2, "texture": FX + "disc.png", "blend": "add", "layer": L_PART, "maxParticles": 64}})

    prefab("fx_burst", burst)

    def ring(d):
        root = d.add("Ring fx", {"Script": script("fx/ring.lua"), "Sprite": sprite(FX + "ring.png", 1, 1, L_FX, blend="add")})
        d.add("Fill", {"Sprite": sprite(FX + "ring_fill.png", 1, 1, L_FX, blend="add", opacity=0.35, visible=False)}, root)

    prefab("fx_ring", ring)

    def beam(d):
        root = d.add("Beam fx", {"Script": script("fx/beam.lua"), "Sprite": sprite(FX + "beam.png", 1, 1, L_FX, blend="add", opacity=0.45)})
        d.add("Core", {"Sprite": sprite(FX + "beam.png", 1, 0.3, L_FX, order=1, blend="add")}, root)

    prefab("fx_beam", beam)


def pickups():
    def xp(d, root):
        d.add("Glow", {"Sprite": glow(4.5 * 2.6 * U, "#ffc857", 0.45, L_PICKUP)}, root)
        d.add("Body", {"Sprite": sprite(FX + "gem.png", 9 * U, 9 * U, L_PICKUP, order=1, color="#ffc857")}, root)

    def oil(d, root):
        d.add("Glow", {"Sprite": glow(22 * U, "#c8ff5a", 0.5, L_PICKUP)}, root)
        d.add("Body", {"Sprite": sprite(FX + "drop.png", 20 * U, 20 * U, L_PICKUP, order=1, color="#c8ff5a")}, root)

    def heal(d, root):
        d.add("Glow", {"Sprite": glow(22 * U, "#ff6b6b", 0.5, L_PICKUP)}, root)
        d.add("Body", {"Sprite": sprite(FX + "cross.png", 18 * U, 18 * U, L_PICKUP, order=1, color="#ff6b6b")}, root)

    def glim(d, root):
        d.add("Glow", {"Sprite": glow(18 * U, "#ffe27a", 0.5, L_PICKUP)}, root)
        d.add("Body", {"Sprite": sprite(FX + "coin.png", 12 * U, 12 * U, L_PICKUP, order=1)}, root)

    def chest(d, root):
        d.add("Glow", {"Sprite": glow(60 * U, "#ffe27a", 0.6, L_PICKUP)}, root)
        d.add("Body", {"Sprite": sprite(FX + "chest.png", 32 * U, 32 * U, L_EYES, order=1)}, root)

    pickup_prefab("xp", xp)
    pickup_prefab("oil", oil)
    pickup_prefab("heal", heal)
    pickup_prefab("glim", glim)
    pickup_prefab("chest", chest, light={"radius": 80 * U, "strength": 0.8})


# ---------------------------------------------------------------- UI helpers

INK, INK_DIM, TITLE, BROWN = "#5a4636", "#7d6450", "#c2621a", "#8a4b1c"
CREAM, CREAM2, HONEY, LINE, NIGHT = "#fff8ec", "#ffefd2", "#ffcf5c", "#e6d6bd", "#23275a"

STYLES = {
    "primary": ("#ffd56b", "#5a3a12", "#fffaf0"), "normal": ("#ece4ff", "#4a3a78", "#ffffff"),
    "gold": ("#ffb59c", "#5a2a1a", "#ffffff"), "danger": ("#ff8fa8", "#5a1a2a", "#ffffff"),
    "ghost": ("#4a4f85", "#ffffff", "#9aa0d0"), "poor": ("#e9e2d2", "#8d7a66", "#ffffff"),
    "mint": ("#8fe3c4", "#1f5a46", "#ffffff"),
}


class UI:
    """UI entity builders. x, y, w, h are reference pixels relative to the parent element."""

    def __init__(self, doc):
        self.d = doc
        self.order = 0

    def _o(self):
        self.order += 1
        return self.order

    def motion(self, ent, enter="fade", duration=0.25, delay=0.0, distance=40):
        """Entrance animation (UIMotion) played whenever the element becomes visible."""
        self.d.entities[ent - 1]["components"]["UIMotion"] = {"enter": enter, "duration": duration, "delay": delay, "distance": distance}
        return ent

    def root(self, name, parent=None, visible=True, block=False, color=None, opacity=0.0):
        """A full-screen container; block = modal backdrop."""
        return self.d.add(name, {"UIPanel": {
            "anchor": "stretch", "x": 0, "y": 0, "width": 0, "height": 0, "color": rgb(color or "#000000"), "opacity": opacity,
            "blockInput": block, "visible": visible, "order": self._o()}}, parent)

    def panel(self, name, parent, x, y, w, h, color=CREAM, anchor="center", radius=0, border=0, border_color="#ffffff",
              opacity=1.0, visible=True, layout=None, block=False, clip=False):
        comps = {"UIPanel": {"anchor": anchor, "x": x, "y": y, "width": w, "height": h, "color": rgb(color), "opacity": opacity,
                             "radius": radius, "borderWidth": border, "borderColor": rgb(border_color), "visible": visible,
                             "blockInput": block, "clip": clip, "order": self._o()}}
        if layout:
            comps["UILayout"] = layout
        return self.d.add(name, comps, parent)

    def text(self, name, parent, text, x, y, size, color=INK, anchor="center", w=0, h=0, align="auto", outline=0,
             outline_color="#000000", shadow=0, visible=True, key=None):
        comps = {"UIText": {"text": text, "font": FONT, "size": size, "color": rgb(color), "anchor": anchor, "x": x, "y": y,
                            "width": w, "height": h, "align": align, "verticalAlign": "middle", "outlineWidth": outline,
                            "outlineColor": rgb(outline_color), "shadowDistance": shadow, "shadowColor": rgb("#141440"),
                            "richText": False, "visible": visible, "order": self._o()}}
        if key:  # localized by scripts/lib/ui.lua; the English text is the edit-time content
            comps["Tag"] = {"tags": "t:" + key}
            comps["UIText"]["text"] = EN.get(key, text)
        return self.d.add(name, comps, parent)

    def image(self, name, parent, tex, x, y, w, h, anchor="center", color="#ffffff", opacity=1.0, visible=True, radius=0, **kw):
        comps = {"UIImage": {"texture": tex, "anchor": anchor, "x": x, "y": y, "width": w, "height": h, "color": rgb(color),
                             "opacity": opacity, "preserveAspect": True, "radius": radius, "visible": visible, "order": self._o()}}
        comps["UIImage"].update(kw)
        return self.d.add(name, comps, parent)

    def button(self, name, parent, label, x, y, w, h, style, target, action, arg=None, anchor="center", size=None, radius=None,
               key=None, visible=True, hotkey=None):
        bg, fg, border = STYLES[style]
        params = {"target": target, "a": action}
        if arg is not None:
            params["b"] = arg
        comps = {
            "UIButton": {"text": label, "font": FONT, "size": size or int(h * 0.42), "textColor": rgb(fg), "anchor": anchor, "x": x, "y": y,
                         "width": w, "height": h, "color": rgb(bg), "radius": radius if radius is not None else min(h / 2, 26),
                         "borderWidth": 3, "borderColor": rgb(border), "hoverBrightness": 1.06, "pressedBrightness": 0.92,
                         "hoverScale": 1.04, "pressedScale": 0.96, "visible": visible, "order": self._o()},
            "Script": script("ui/button.lua", **params),
        }
        if key:
            comps["Tag"] = {"tags": "t:" + key}
            comps["UIButton"]["text"] = EN.get(key, label)
        return self.d.add(name, comps, parent)

    def card(self, name, parent, x, y, w, h, target=None, action=None, arg=None, anchor="center", bg=CREAM, border="#ffe9c0", visible=True):
        """Rounded card; clickable when it has an action. Children are placed inside it."""
        if action:
            return self.d.add(name, {
                "UIButton": {"text": "", "anchor": anchor, "x": x, "y": y, "width": w, "height": h, "color": rgb(bg), "radius": 22,
                             "borderWidth": 4, "borderColor": rgb(border), "hoverBrightness": 1.03, "pressedBrightness": 0.97,
                             "hoverScale": 1.03, "pressedScale": 0.98, "visible": visible, "order": self._o()},
                "Script": script("ui/button.lua", target=target, a=action, **({"b": arg} if arg is not None else {})),
            }, parent)
        return self.panel(name, parent, x, y, w, h, bg, anchor=anchor, radius=22, border=4, border_color=border, visible=visible)

    def bar(self, name, parent, x, y, w, h, fill, anchor="center", track="#2b2f5c", visible=True, value=1.0):
        return self.d.add(name, {"UISlider": {
            "value": value, "min": 0, "max": 1, "anchor": anchor, "x": x, "y": y, "width": w, "height": h, "color": rgb(track),
            "fillColor": rgb(fill), "handle": False, "interactable": False, "radius": h / 2, "visible": visible, "order": self._o()}}, parent)

    def slider(self, name, parent, x, y, w, target, action, value=0.5):
        return self.d.add(name, {
            "UISlider": {"value": value, "min": 0, "max": 1, "step": 0.01, "anchor": "center", "x": x, "y": y, "width": w, "height": 18,
                         "color": rgb(LINE), "fillColor": rgb(HONEY), "handleColor": rgb("#ffffff"), "radius": 9, "order": self._o()},
            "Script": script("ui/button.lua", target=target, a=action)}, parent)

    def pips(self, prefix, parent, x, y, count, size=8, gap=4, anchor="center"):
        x0 = x - ((count - 1) * (size + gap)) / 2
        for i in range(count):
            self.panel("%s%d" % (prefix, i + 1), parent, x0 + i * (size + gap), y, size, size, LINE, anchor=anchor, radius=size / 2)

    def chip(self, parent):
        box = self.panel(None, parent, -16, 16, 108, 36, CREAM, anchor="top-right", radius=18, border=3, border_color=HONEY)
        self.image(None, box, FX + "star.png", 10, 0, 20, 20, anchor="left", color="#e0a020")
        self.text("Glims", box, "0", -12, 0, 18, BROWN, anchor="right", w=64, h=20, align="right")

    def title3d(self, name, parent, text, y, size, color=HONEY, key=None):
        return self.text(name, parent, text, 0, y, size, color, outline=5, outline_color=CREAM, shadow=4, key=key)


def modal_panel(ui, name, parent, w, h, title_key):
    """Cream dialog box centred on a blocking backdrop; returns the box."""
    back = ui.root(name, parent, visible=False, block=True, color="#14163a", opacity=0.62)
    ui.motion(back, "fade", 0.15)
    box = ui.panel(name + ":box", back, 0, 0, w, h, CREAM, radius=28, border=5, border_color="#ffe9c0")
    ui.motion(box, "pop", 0.3)
    if title_key:
        ui.text(None, box, "", 0, 42, 32, TITLE, anchor="top", key=title_key)
    return back, box


def ad_overlay(ui, target):
    """Mock rewarded / interstitial / purchase overlay driven by scripts/lib/ads.lua."""
    ad = ui.root("Ad", None, visible=False, block=True, color="#08060c", opacity=0.92)
    ui.text("Ad:title", ad, "", 0, -70, 26, "#f3e9d2")
    ui.text("Ad:sub", ad, "", 0, -30, 17, "#d8ceb8", w=520, h=44, align="center")
    ui.bar("Ad:bar", ad, 0, 20, 260, 10, "#ffb347", track="#3a3040", value=0)
    ui.button("Ad:skip", ad, "", 0, 76, 130, 40, "ghost", target, "adSkip", size=16, key="ad.skip")
    ui.button("Ad:buy", ad, "", -80, 76, 130, 42, "primary", target, "adBuy", size=17, key="ad.buy")
    ui.button("Ad:cancel", ad, "", 80, 76, 130, 42, "ghost", target, "adCancel", size=17, key="common.cancel")


def toasts(ui):
    stack = ui.panel("Toasts", None, 0, -30, 900, 0, anchor="bottom", opacity=0,
                     layout={"direction": "vertical", "spacing": 6, "crossAlign": "center", "fit": True})
    for i in range(1, 6):
        pill = ui.panel("Toast%d" % i, stack, 0, 0, 500, 40, CREAM, radius=20, border=3, border_color=HONEY, visible=False)
        ui.motion(pill, "slide-up", 0.25, distance=24)
        ui.text("Toast%d:text" % i, pill, "", 0, 0, 20, INK)


def settings_rows(ui, parent, target, top):
    """Volume sliders, toggles, language and reset; shared by the menu screen and the pause dialog."""
    y = top
    for key, name in (("settings.music", "Music"), ("settings.sfx", "Sfx")):
        ui.text(None, parent, "", -150, y, 19, INK, w=220, h=26, align="left", key=key)
        ui.slider("Set:%s" % name, parent, 60, y, 220, target, "set" + name)
        ui.text("Set:%s:label" % name, parent, "", 222, y, 18, BROWN, w=60, h=24, align="right")
        y += 56
    for key, name in (("settings.shake", "Shake"), ("settings.damage", "Damage")):
        ui.text(None, parent, "", -150, y, 19, INK, w=220, h=26, align="left", key=key)
        ui.button("Set:%s" % name, parent, "", 196, y, 110, 36, "mint", target, "toggle" + name, size=16)
        y += 56
    ui.text(None, parent, "", -150, y, 19, INK, w=220, h=26, align="left", key="settings.language")
    ui.button("Set:en", parent, "English", 78, y, 110, 36, "poor", target, "lang", "en", size=16)
    ui.button("Set:ko", parent, "한국어", 196, y, 110, 36, "poor", target, "lang", "ko", size=16)
    y += 62
    ui.button(None, parent, "", 166, y, 170, 36, "danger", target, "resetAsk", size=15, key="settings.reset")
    return y


# ---------------------------------------------------------------- run scene

def run_scene():
    d = Doc()
    half = 2400 * U / 2
    cam = d.add("Camera", {
        "Transform": {"position": [0, 0, 20]},
        "Camera": {"projection": "orthographic", "orthoSize": 290 * U, "clearColor": rgb("#14163a")},
        "CameraFollow": {"target": 0, "offset": [0, 0, 20], "smoothing": 8},
        "Darkness2D": {"color": rgb("#1b1a4e"), "opacity": 0.94, "layer": L_DARK},
        "Script": script("run/camera.lua"),
    })
    d.add("Canvas", {"UICanvas": {"referenceWidth": 1280, "referenceHeight": 720, "match": 1}})
    d.add("Game", {"Script": script("run/game.lua")})
    # one 512 px ground tile is 12.8 units; five rows of five cover the arena
    d.add("Ground", {
        "Transform": {"position": [-32, 32, -0.1]},
        "Tilemap": {"tileset": SPR + "ground_moor.png", "columns": 1, "rows": 1, "tileSize": 12.8, "pixelArt": False,
                    "map": ["ggggg"] * 5, "legend": {"g": 0}},
    })
    # the arena's edge: enemies and the keeper stay inside this loop
    d.add("Walls", {"Collider2D": {"shape": "edge", "loop": True, "layer": C_WALL,
                                   "points": [[-half, -half], [half, -half], [half, half], [-half, half]]}})
    for name, x, y, w, h in (("Wall N", 0, half + 10, 100, 20), ("Wall S", 0, -half - 10, 100, 20),
                             ("Wall W", -half - 10, 0, 20, 100), ("Wall E", half + 10, 0, 20, 100)):
        d.add(name, {"Transform": {"position": [x, y, 0]}, "Sprite": sprite(None, w, h, L_POOL, color="#14163a", alphaCutoff=0.5)})

    player = d.add("Player", {
        "CharacterBody2D": {"mode": "topdown", "shape": "circle", "radius": 12 * U, "layer": C_PLAYER,
                            "ignoreLayers": [C_ENEMY, C_SHOT, C_EBULLET, C_PICKUP], "pushStrength": 0},
        "Script": script("run/player.lua"),
        "Tag": {"tags": "player"},
    })
    d.entities[cam - 1]["components"]["CameraFollow"]["target"] = player
    d.add("Shadow", {"Transform": {"position": [0, -14 * U, 0]}, "Sprite": sprite(FX + "shadow.png", 28 * U, 10 * U, L_SHADOW, opacity=0.4)}, player)
    body = d.add("Body", {"Transform": {"position": [0, 12 * U, 0]}, **animated_sprite("keeper_ada", 68 * U, 68 * U, L_PLAYER)}, player)
    lamp = d.add("Lamp", {"Transform": {"position": [22 * U, -10 * U, 0]}, "Sprite": glow(32 * U, "#ff9a3c", 0.9, L_LANTERN)}, body)
    d.add("LampCore", {"Sprite": glow(8 * U, "#fff2b0", 1, L_LANTERN, order=1)}, lamp)
    d.add("Lantern", {"Light2D": {"radius": 235 * U * 1.18}}, player)
    d.add("Warmth", {"Sprite": glow(235 * U, "#ffaa50", 0.2, L_EYES)}, player)
    d.add("Hurtbox", {"RigidBody2D": {"type": "kinematic", "gravityScale": 0},
                      "Collider2D": {"shape": "circle", "radius": 20 * U, "isTrigger": True, "layer": C_HURT,
                                     "ignoreLayers": without(C_ENEMY, C_EBULLET, C_PICKUP)},
                      "Script": script("run/hurtbox.lua")}, player)
    d.add("Magnet", {"RigidBody2D": {"type": "kinematic", "gravityScale": 0},
                     "Collider2D": {"shape": "circle", "radius": 70 * U, "isTrigger": True, "layer": C_MAGNET, "ignoreLayers": without(C_PICKUP)}}, player)
    d.add("Director", {"Script": script("run/director.lua")})

    # off-screen indicators, drawn over the darkness
    ind = d.add("Indicators", {})
    for name, color, size in (("Arrow brazier", "#ffb347", 26), ("Arrow chest 1", "#ffe27a", 30), ("Arrow chest 2", "#ffe27a", 30),
                              ("Arrow boss 1", "#ff9ac0", 32), ("Arrow boss 2", "#ff9ac0", 32)):
        d.add(name, {"Sprite": sprite(FX + "arrow.png", size * U / 1.24, size * U / 1.24, L_ARROW, color=color, visible=False)}, ind)

    ui = UI(d)
    hud = ui.root("HUD")
    d.entities[hud - 1]["components"]["Script"] = script("run/hud.lua")
    ui.image("Vignette", hud, FX + "vignette.png", 0, 0, 0, 0, anchor="stretch", opacity=0, visible=False, preserveAspect=False)
    ui.root("Numbers", hud)
    ui.image("Stick", hud, FX + "ring.png", 0, 0, 140, 140, anchor="top-left", opacity=0.35, visible=False)
    ui.image("Stick knob", hud, FX + "disc.png", 0, 0, 56, 56, anchor="top-left", color="#ffb347", opacity=0.35, visible=False)
    ui.bar("XP", hud, 0, 0, 0, 18, "#8fe3c4", anchor="stretch-top", track="#1d2147", value=0)
    lv = ui.panel(None, hud, 6, 1, 52, 16, CREAM, anchor="top-left", radius=8, border=2, border_color="#4fb08e")
    ui.text("Lv", lv, "", 0, 0, 12, "#2f6a55")
    ui.text("Timer", hud, "0:00", 0, 26, 34, "#ffffff", anchor="top", shadow=2)
    stats = ui.panel("Stats", hud, 8, 60, 244, 0, anchor="top-left", opacity=0,
                     layout={"direction": "vertical", "spacing": 6, "fit": True})
    for name, fill, icon, outline in (("HP", "#ff8fa8", "2764", "#5a2a3a"), ("Oil", HONEY, "1f6e2", "#5a4a1a")):
        bar = ui.bar(name, stats, 0, 0, 240, 22, fill, track="#3a2f55")
        ui.image(None, bar, ICON + icon + ".png", 6, 0, 16, 16, anchor="left")
        ui.text(name + ":text", bar, "", 0, 0, 14, "#ffffff", outline=2, outline_color=outline)
    counters = ui.panel(None, stats, 0, 0, 240, 22, opacity=0, layout={"direction": "horizontal", "spacing": 6, "crossAlign": "center"})
    for name, icon, color, tint in (("Kills", ICON + "2620.png", "#ffffff", "#ffffff"), ("Run glims", FX + "star.png", HONEY, HONEY),
                                    ("Braziers", ICON + "1f525.png", "#ffffff", "#ffffff")):
        ui.image(None, counters, icon, 0, 0, 18, 18, color=tint)
        ui.text(name, counters, "0", 0, 0, 17, color, w=48, h=20, align="left", shadow=1)
    for row in ("Weapons", "Passives"):
        line = ui.panel(row, stats, 0, 0, 300, 40, opacity=0, layout={"direction": "horizontal", "spacing": 6})
        for i in range(1, 7):
            slot = ui.panel("%s%d" % (row, i), line, 0, 0, 44, 40, CREAM, radius=10, border=2, border_color="#ffe9c0", opacity=0.95, visible=False)
            ui.image("%s%d:icon" % (row, i), slot, ICON + "1f525.png", 0, 3, 24, 24, anchor="top")
            ui.pips("%s%d:pip" % (row, i), slot, 0, -5, 5, size=5, gap=2, anchor="bottom")
    ui.text("Boss name", hud, "", 0, 66, 20, "#ffffff", anchor="top", shadow=2, visible=False)
    ui.bar("Boss bar", hud, 0, 94, 420, 16, "#ff8fa8", anchor="top", track="#3a2f55", visible=False)
    ui.button("Pause button", hud, "II", -32, 26, 48, 48, "normal", "Dialogs", "pause", anchor="top-right", size=18, radius=24)
    banner = ui.panel("Banner", hud, 0, -70, 560, 132, HONEY, radius=30, border=5, opacity=0.96, visible=False)
    ui.motion(banner, "pop", 0.4)
    ui.text("Banner:kicker", banner, "", 0, -40, 20, "#7a3e1a")
    ui.text("Banner:name", banner, "", 0, 12, 64, "#ffffff", outline=5, outline_color="#b0502a")
    toasts(ui)

    dlg = ui.root("Dialogs")
    d.entities[dlg - 1]["components"]["Script"] = script("run/dialogs.lua")

    # level up: three choice cards (a fourth appears when only fallbacks are left)
    lvl = ui.root("Dlg:levelup", dlg, visible=False, block=True, color="#14163a", opacity=0.7)
    ui.motion(lvl, "fade", 0.15)
    ui.motion(ui.title3d(None, lvl, "", -150, 58, key="levelup.title"), "pop", 0.4)
    ui.text(None, lvl, "", 0, -104, 20, CREAM, shadow=2, key="levelup.sub")
    row = ui.panel(None, lvl, 0, 20, 0, 222, opacity=0, layout={"direction": "horizontal", "spacing": 16, "fit": True})
    for i in range(1, 4):
        c = ui.card("Choice%d" % i, row, 0, 0, 256, 212, "Dialogs", "pick", i)
        ui.motion(c, "slide-up", 0.3, delay=0.08 * i, distance=60)
        key = ui.panel(None, c, 12, 10, 24, 24, CREAM2, anchor="top-left", radius=8)
        ui.text(None, key, str(i), 0, 0, 14, INK_DIM)
        ui.image("Choice%d:icon" % i, c, ICON + "2728.png", 0, 28, 64, 64, anchor="top")
        ui.text("Choice%d:name" % i, c, "", 0, 104, 21, BROWN, anchor="top")
        badge = ui.panel("Choice%d:badge" % i, c, 0, 132, 70, 22, "#c9b6ff", anchor="top", radius=11, border=2)
        ui.text("Choice%d:badge:text" % i, badge, "", 0, 0, 14, "#3e2f78")
        ui.text("Choice%d:desc" % i, c, "", 0, -14, 14, INK_DIM, anchor="bottom", w=228, h=40, align="center")
    ui.button("Reroll", lvl, "", 0, 172, 220, 44, "normal", "Dialogs", "reroll", size=17)
    ui.image(None, lvl, ICON + "1f3b2.png", -88, 172, 20, 20)

    chest = ui.root("Dlg:chest", dlg, visible=False, block=True, color="#14163a", opacity=0.62)
    ui.motion(chest, "fade", 0.15)
    ui.motion(ui.image(None, chest, ICON + "1f381.png", 0, -206, 72, 72), "pop", 0.5)
    ui.motion(ui.title3d(None, chest, "", -142, 50, key="chest.title"), "pop", 0.4)
    ui.text(None, chest, "", 0, -100, 20, CREAM, shadow=2, key="chest.sub")
    row = ui.panel(None, chest, 0, 16, 0, 206, opacity=0, layout={"direction": "horizontal", "spacing": 14, "fit": True})
    for i in range(1, 5):
        c = ui.card("Reward%d" % i, row, 0, 0, 218, 196)
        ui.motion(c, "pop", 0.35, delay=0.25 + 0.3 * i)
        ui.image("Reward%d:icon" % i, c, ICON + "2728.png", 0, 22, 56, 56, anchor="top")
        ui.text("Reward%d:name" % i, c, "", 0, 92, 19, BROWN, anchor="top")
        ui.text("Reward%d:desc" % i, c, "", 0, 118, 13, INK_DIM, anchor="top", w=196, h=36, align="center")
        badge = ui.panel("Reward%d:badge" % i, c, 0, -12, 70, 22, "#c9b6ff", anchor="bottom", radius=11, border=2)
        ui.text("Reward%d:badge:text" % i, badge, "", 0, 0, 14, "#3e2f78")
    ui.button(None, chest, "", 0, 176, 240, 52, "primary", "Dialogs", "chestClose", size=22, key="chest.continue")

    _, box = modal_panel(ui, "Dlg:pause", dlg, 520, 480, "pause.title")
    ui.button(None, box, "", 0, -136, 420, 52, "primary", "Dialogs", "resume", size=22, key="pause.resume")
    ui.button(None, box, "", -108, -74, 204, 44, "normal", "Dialogs", "pauseSettings", size=18, key="menu.settings")
    ui.button(None, box, "", 108, -74, 204, 44, "danger", "Dialogs", "quitAsk", size=18, key="pause.quit")
    ui.text(None, box, "", 0, -24, 19, TITLE, key="pause.build")
    for col, x, key in (("Weapons", -116, "pause.weapons"), ("Passives", 116, "pause.passives")):
        ui.text(None, box, "", x, 4, 14, INK_DIM, key=key)
        ui.text("Build:%s:empty" % col, box, "", x, 34, 14, INK_DIM, key="pause.empty")
        column = ui.panel(None, box, x, 20, 212, 0, anchor="center", opacity=0, layout={"direction": "vertical", "spacing": 4})
        d.entities[column - 1]["components"]["UIPanel"]["anchor"] = "top"
        d.entities[column - 1]["components"]["UIPanel"]["y"] = 262
        for i in range(1, 7):
            r = ui.panel("Build:%s%d" % (col, i), column, 0, 0, 212, 26, CREAM2, radius=10, visible=False)
            ui.image("Build:%s%d:icon" % (col, i), r, ICON + "1f525.png", 6, 0, 20, 20, anchor="left")
            ui.text("Build:%s%d:name" % (col, i), r, "", 32, 0, 14, INK, anchor="left", w=120, h=20, align="left")
            ui.pips("Build:%s%d:pip" % (col, i), r, -30, 0, 5, size=6, gap=3, anchor="right")

    _, box = modal_panel(ui, "Dlg:settings", dlg, 580, 520, "settings.title")
    y = settings_rows(ui, box, "Dialogs", -170)
    ui.button(None, box, "", 0, y + 62, 240, 46, "primary", "Dialogs", "closeTop", size=19, key="common.back")

    _, box = modal_panel(ui, "Dlg:quit", dlg, 520, 280, "pause.quit")
    ui.text(None, box, "", 0, -40, 17, INK, w=440, h=50, align="center", key="pause.quitAsk")
    ui.button(None, box, "", 0, 30, 380, 50, "danger", "Dialogs", "quitYes", size=20, key="pause.quit")
    ui.button(None, box, "", 0, 92, 380, 40, "ghost", "Dialogs", "closeTop", size=17, key="common.cancel")

    _, box = modal_panel(ui, "Dlg:reset", dlg, 520, 280, "settings.reset")
    ui.text(None, box, "", 0, -40, 17, INK, w=440, h=50, align="center", key="settings.resetAsk")
    ui.button(None, box, "", 0, 30, 380, 50, "danger", "Dialogs", "resetYes", size=20, key="settings.resetYes")
    ui.button(None, box, "", 0, 92, 380, 40, "ghost", "Dialogs", "closeTop", size=17, key="common.cancel")

    _, box = modal_panel(ui, "Dlg:dying", dlg, 520, 300, "dying.title")
    ui.text(None, box, "", 0, -64, 18, INK_DIM, key="dying.sub")
    col = ui.panel(None, box, 0, 40, 380, 0, opacity=0, layout={"direction": "vertical", "spacing": 16, "crossAlign": "stretch", "fit": True})
    ui.button("Dying:revive", col, "", 0, 0, 380, 52, "primary", "Dialogs", "revive", size=22, key="dying.revive")
    ui.button("Dying:reviveAd", col, "", 0, 0, 380, 52, "gold", "Dialogs", "reviveAd", size=22, key="dying.reviveAd")
    ui.button(None, col, "", 0, 0, 380, 44, "ghost", "Dialogs", "giveUp", size=18, key="dying.giveUp")

    _, box = modal_panel(ui, "Dlg:victory", dlg, 560, 330, None)
    for i, c in enumerate(["2b50", "2728", "1f338", "1f49b", "2b50", "2728", "1f33c", "1f49c", "2b50", "2728"]):
        ui.image(None, box, ICON + c + ".png", -250 + i * 55, -128 + ((i + 1) * 37 % 5) * 6, 26, 26)
    ui.title3d(None, box, "", -70, 50, key="victory.title")
    ui.text(None, box, "", 0, -14, 17, INK_DIM, w=480, h=44, align="center", key="victory.sub")
    ui.button(None, box, "", 0, 50, 380, 52, "primary", "Dialogs", "endless", size=22, key="victory.endless")
    ui.button(None, box, "", 0, 116, 380, 46, "normal", "Dialogs", "finish", size=19, key="victory.finish")

    ad_overlay(ui, "Dialogs")
    d.write("scenes/run.scene.json", "ownengine.scene", "Run")


# ---------------------------------------------------------------- menu scene

KEEPERS = ["ada", "bram", "suri"]
STAGES = ["moor", "chapel", "frost"]
META = ["vitality", "might", "swiftness", "radiance", "thrift", "greed", "wisdom", "second_wind", "reroll"]
META_MAX = [10, 10, 5, 5, 5, 10, 5, 1, 3]
META_ICON = ["2764", "2694", "1f3c3", "1f31f", "1f6e2", "1f4b0", "1f4d8", "1f54a", "1f3b2"]
IAP = [("no_ads", "1f6ab"), ("starter", "1f381"), ("glims_s", "1f45d"), ("glims_l", "1f9f0")]
ACH = 13
STATS = ["runs", "wins", "kills", "bossKills", "bestTime", "braziersLit", "glimsEarned", "maxLevel", "evolutions"]


def topbar(ui, screen, key):
    ui.button(None, screen, "", 68, 38, 104, 40, "ghost", "Menu", "nav", "title", anchor="top-left", size=16, key="common.back")
    ui.text(None, screen, "", 0, 24, 30, "#ffffff", anchor="top", shadow=2, key=key)
    ui.chip(screen)


def menu_scene():
    d = Doc()
    d.add("Camera", {"Transform": {"position": [0, 0, 20]},
                     "Camera": {"projection": "orthographic", "orthoSize": 5, "clearColor": rgb(NIGHT)}})
    d.add("Canvas", {"UICanvas": {"referenceWidth": 1280, "referenceHeight": 720, "match": 1}})
    d.add("Menu", {"Script": script("menu/menu.lua")})
    ui = UI(d)

    # title
    s = ui.root("Screen:title", visible=False)  # menu.lua shows the right screen on start
    ui.motion(s, "fade", 0.25)
    ui.image(None, s, "assets/ui/keyart.jpg", 0, 0, 0, 0, anchor="stretch", preserveAspect=False)
    ui.chip(s)
    ui.motion(ui.text(None, s, "WICKBOUND", 0, -182, 104, "#ffd56b", outline=7, outline_color=CREAM, shadow=6), "slide-down", 0.6, distance=50)
    ui.text(None, s, "", 0, -102, 20, "#ffffff", shadow=2, key="title.tagline")
    col = ui.panel(None, s, 0, 118, 336, 0, opacity=0, layout={"direction": "vertical", "spacing": 13, "crossAlign": "stretch", "fit": True})
    ui.motion(ui.button(None, col, "", 0, 0, 336, 62, "primary", "Menu", "nav", "play", size=26, key="menu.play"), "slide-up", 0.35, delay=0.15)
    for n, (key, screen) in enumerate((("menu.upgrades", "upgrades"), ("menu.achievements", "ach"), ("menu.shop", "shop"), ("menu.settings", "settings"))):
        ui.motion(ui.button(None, col, "", 0, 0, 336, 52, "normal", "Menu", "nav", screen, size=20, key=key), "slide-up", 0.35, delay=0.22 + 0.07 * n)
    daily = ui.button("Daily badge", s, "", 100, -38, 176, 46, "gold", "Menu", "dailyOpen", anchor="bottom-left", size=17, key="menu.daily", visible=False)
    ui.image(None, daily, ICON + "1f381.png", 14, 0, 22, 22, anchor="left")

    # play setup
    s = ui.root("Screen:play", visible=False, color=NIGHT, opacity=1)
    ui.motion(s, "fade", 0.18)
    topbar(ui, s, "play.title")
    ui.text(None, s, "", 0, -252, 20, HONEY, key="play.keeper")
    row = ui.panel(None, s, 0, -110, 0, 254, opacity=0, layout={"direction": "horizontal", "spacing": 16, "fit": True})
    for n, k in enumerate(KEEPERS):
        c = ui.card("Keeper:" + k, row, 0, 0, 256, 244, "Menu", "selKeeper", k)
        ui.motion(c, "slide-up", 0.3, delay=0.05 * n)
        ui.image("Keeper:%s:img" % k, c, SPR + "keeper_%s.png" % k, 0, 14, 92, 92, anchor="top")
        ui.text("Keeper:%s:name" % k, c, "", 0, 114, 22, BROWN, anchor="top")
        ui.text("Keeper:%s:desc" % k, c, "", 0, 146, 15, INK_DIM, anchor="top", w=224, h=40, align="center")
        ui.text("Keeper:%s:tag" % k, c, "", 0, -16, 15, "#9a5a00", anchor="bottom", key="play.selected")
        ui.button("Keeper:%s:unlock" % k, c, "", 0, -14, 220, 40, "gold", "Menu", "unlockKeeper", k, anchor="bottom", size=16, visible=False)
    ui.text(None, s, "", 0, 48, 20, HONEY, key="play.stage")
    row = ui.panel(None, s, 0, 158, 0, 182, opacity=0, layout={"direction": "horizontal", "spacing": 16, "fit": True})
    for n, st in enumerate(STAGES):
        c = ui.card("Stage:" + st, row, 0, 0, 256, 172, "Menu", "selStage", st)
        ui.motion(c, "slide-up", 0.3, delay=0.15 + 0.05 * n)
        frame = ui.panel(None, c, 0, 8, 224, 54, "#ffffff", anchor="top", radius=14)
        ui.image("Stage:%s:img" % st, frame, SPR + "ground_%s.png" % st, 0, 0, 216, 46, radius=11, preserveAspect=False, rows=4, frame=1)
        ui.text("Stage:%s:name" % st, c, "", 0, 74, 20, BROWN, anchor="top")
        ui.text("Stage:%s:desc" % st, c, "", 0, 100, 14, INK_DIM, anchor="top", w=228, h=36, align="center")
        ui.text("Stage:%s:tag" % st, c, "", 0, -10, 14, "#9a5a00", anchor="bottom")
    ui.button("Start", s, "", 0, 306, 320, 62, "primary", "Menu", "start", size=26, key="play.start")

    # upgrades
    s = ui.root("Screen:upgrades", visible=False, color=NIGHT, opacity=1)
    ui.motion(s, "fade", 0.18)
    topbar(ui, s, "upgrades.title")
    grid = ui.panel(None, s, 0, 26, 1180, 0, opacity=0, layout={"direction": "grid", "columns": 5, "spacing": 14, "fit": True})
    for n, (m, mx, icon) in enumerate(zip(META, META_MAX, META_ICON)):
        c = ui.card("Meta:" + m, grid, 0, 0, 222, 262)
        ui.motion(c, "slide-up", 0.3, delay=0.04 * n)
        ui.image(None, c, ICON + icon + ".png", 0, 16, 60, 60, anchor="top")
        ui.text("Meta:%s:name" % m, c, "", 0, 86, 21, BROWN, anchor="top")
        ui.text("Meta:%s:desc" % m, c, "", 0, 116, 15, INK_DIM, anchor="top", w=200, h=24, align="center")
        ui.pips("Meta:%s:pip" % m, c, 0, 30, mx)
        ui.button("Meta:%s:buy" % m, c, "", 0, -26, 180, 40, "gold", "Menu", "buyMeta", m, anchor="bottom", size=16)
        ui.text("Meta:%s:max" % m, c, "", 0, -36, 20, "#9a5a00", anchor="bottom", key="common.max", visible=False)

    # achievements and lifetime stats
    s = ui.root("Screen:ach", visible=False, color=NIGHT, opacity=1)
    ui.motion(s, "fade", 0.18)
    topbar(ui, s, "ach.title")
    # a scroll view: wheel, drag or the bar moves the list
    lst = ui.panel("Ach list", s, -230, 26, 800, 560, "#1b1f4d", radius=22, opacity=0.6,
                   layout={"direction": "vertical", "spacing": 10, "padding": 14, "crossAlign": "stretch"})
    d.entities[lst - 1]["components"]["UIScroll"] = {"direction": "vertical", "wheelStep": 80, "barColor": rgb(HONEY)}
    for i in range(1, ACH + 1):
        r = ui.panel("Ach%d" % i, lst, 0, 0, 760, 76, CREAM, radius=18, border=3, border_color="#ffe9c0")
        ui.image("Ach%d:icon" % i, r, ICON + "1f512.png", 18, 0, 40, 40, anchor="left")
        ui.text("Ach%d:name" % i, r, "", 74, -14, 20, BROWN, anchor="left", w=480, h=24, align="left")
        ui.text("Ach%d:desc" % i, r, "", 74, 14, 15, INK_DIM, anchor="left", w=480, h=22, align="left")
        ui.text("Ach%d:reward" % i, r, "", -22, 0, 16, "#9a5a00", anchor="right", w=140, h=40, align="right")
    box = ui.panel(None, s, 420, 26, 330, 560, CREAM, radius=22, border=4, border_color="#ffe9c0")
    ui.text(None, box, "", 0, 22, 20, TITLE, anchor="top", key="ach.stats")
    col = ui.panel(None, box, 0, 60, 290, 0, anchor="top", opacity=0, layout={"direction": "vertical", "spacing": 0, "fit": True})
    for st in STATS:
        r = ui.panel(None, col, 0, 0, 290, 48, CREAM, opacity=0)
        ui.text(None, r, "", 0, 0, 17, INK_DIM, anchor="left", w=190, h=24, align="left", key="stat." + st)
        ui.text("Stat:" + st, r, "", 0, 0, 19, BROWN, anchor="right", w=100, h=24, align="right")
        ui.panel(None, r, 0, 0, 290, 2, LINE, anchor="bottom")

    # shop
    s = ui.root("Screen:shop", visible=False, color=NIGHT, opacity=1)
    ui.motion(s, "fade", 0.18)
    topbar(ui, s, "shop.title")
    row = ui.panel(None, s, 0, -30, 0, 310, opacity=0, layout={"direction": "horizontal", "spacing": 16, "fit": True})
    for n, (pid, icon) in enumerate(IAP):
        c = ui.card("Iap:" + pid, row, 0, 0, 248, 300)
        ui.motion(c, "slide-up", 0.3, delay=0.06 * n)
        ui.image(None, c, ICON + icon + ".png", 0, 26, 76, 76, anchor="top")
        ui.text("Iap:%s:name" % pid, c, "", 0, 116, 22, BROWN, anchor="top")
        ui.text("Iap:%s:desc" % pid, c, "", 0, 150, 15, INK_DIM, anchor="top", w=216, h=56, align="center")
        ui.button("Iap:%s:buy" % pid, c, "", 0, -30, 170, 42, "gold", "Menu", "buyIap", pid, anchor="bottom", size=18)
        ui.text("Iap:%s:owned" % pid, c, "", 0, -40, 18, "#9a5a00", anchor="bottom", key="shop.owned", visible=False)
    ui.text(None, s, "", 0, 300, 16, "#c9cdf5", key="shop.demo")

    # settings
    s = ui.root("Screen:settings", visible=False, color=NIGHT, opacity=1)
    ui.motion(s, "fade", 0.18)
    topbar(ui, s, "settings.title")
    ui.panel(None, s, 0, 6, 580, 420, "#cdb98f", radius=28)
    box = ui.panel(None, s, 0, 0, 580, 420, CREAM, radius=28, border=5, border_color="#ffe9c0")
    settings_rows(ui, box, "Menu", -160)

    # result
    s = ui.root("Screen:result", visible=False, color=NIGHT, opacity=1)
    ui.motion(s, "fade", 0.18)
    ui.text("Result:title", s, "", 0, -306, 50, HONEY, outline=5, outline_color=CREAM)
    ui.text("Result:sub", s, "", 0, -256, 20, CREAM, shadow=2)
    row = ui.panel(None, s, 0, -186, 0, 78, opacity=0, layout={"direction": "horizontal", "spacing": 12, "fit": True})
    for name in ("time", "kills", "level", "braziers", "glims"):
        gold = name == "glims"
        b = ui.panel(None, row, 0, 0, 160, 78, "#fff3c4" if gold else CREAM, radius=18, border=3, border_color=HONEY if gold else "#ffe9c0")
        ui.text("Result:" + name, b, "", 0, -12, 28, BROWN)
        ui.text(None, b, "", 0, 20, 14, INK_DIM, key="result." + name)
    ui.button("Result:double", s, "", 0, -112, 300, 46, "gold", "Menu", "double", size=19, key="result.double")
    ui.text("Result:doubled", s, "", 0, -112, 22, HONEY, shadow=2, key="result.doubled", visible=False)
    dmg = ui.panel("Result:damage", s, -280, 84, 520, 310, CREAM, radius=22, border=4, border_color="#ffe9c0")
    ui.text(None, dmg, "", 0, 14, 19, TITLE, anchor="top", key="result.damage")
    col = ui.panel(None, dmg, 0, 48, 480, 0, anchor="top", opacity=0, layout={"direction": "vertical", "spacing": 4, "fit": True})
    for i in range(1, 10):
        r = ui.panel("Dmg%d" % i, col, 0, 0, 480, 24, CREAM, opacity=0, visible=False)
        ui.image("Dmg%d:icon" % i, r, ICON + "1f525.png", 0, 0, 20, 20, anchor="left")
        ui.text("Dmg%d:name" % i, r, "", 28, 0, 15, INK, anchor="left", w=140, h=20, align="left")
        ui.bar("Dmg%d:bar" % i, r, 176, 0, 220, 12, "#ffb347", anchor="left", track=LINE)
        ui.text("Dmg%d:value" % i, r, "", 0, 0, 15, BROWN, anchor="right", w=70, h=20, align="right")
    ach = ui.panel("Result:ach", s, 280, 84, 520, 310, CREAM, radius=22, border=4, border_color="#ffe9c0")
    ui.text(None, ach, "", 0, 14, 19, TITLE, anchor="top", key="result.newAch")
    col = ui.panel(None, ach, 0, 52, 480, 0, anchor="top", opacity=0, layout={"direction": "vertical", "spacing": 6, "fit": True})
    for i in range(1, 6):
        r = ui.panel("NewAch%d" % i, col, 0, 0, 480, 44, CREAM, opacity=0, visible=False)
        ui.image(None, r, ICON + "1f3c6.png", 0, 0, 30, 30, anchor="left")
        ui.text("NewAch%d:name" % i, r, "", 44, -10, 17, BROWN, anchor="left", w=300, h=20, align="left")
        ui.text("NewAch%d:desc" % i, r, "", 44, 12, 13, INK_DIM, anchor="left", w=300, h=18, align="left")
        ui.text("NewAch%d:reward" % i, r, "", 0, 0, 14, "#9a5a00", anchor="right", w=100, h=20, align="right")
    ui.button(None, s, "", -250, 300, 230, 54, "primary", "Menu", "again", size=22, key="result.again")
    ui.button(None, s, "", 0, 300, 230, 54, "normal", "Menu", "resultUpgrades", size=20, key="result.upgrades")
    ui.button(None, s, "", 250, 300, 230, 54, "ghost", "Menu", "resultMenu", size=20, key="result.menu")

    toasts(ui)

    # dialogs
    _, box = modal_panel(ui, "Dlg:daily", None, 480, 380, "daily.title")
    ui.image(None, box, FX + "star.png", -70, -74, 34, 34, color="#e0a020")
    ui.text("Daily:amount", box, "", 16, -74, 44, TITLE)
    ui.text(None, box, "", 0, -26, 16, INK_DIM, key="daily.desc")
    ui.button(None, box, "", 0, 26, 340, 48, "primary", "Menu", "dailyClaim", size=20, key="daily.claim")
    ui.button(None, box, "", 0, 84, 340, 44, "gold", "Menu", "dailyClaim2", size=18, key="daily.claimx2")
    ui.button(None, box, "", 0, 142, 200, 38, "ghost", "Menu", "closeTop", size=16, key="common.close")

    _, box = modal_panel(ui, "Dlg:reset", None, 520, 280, "settings.reset")
    ui.text(None, box, "", 0, -40, 17, INK, w=440, h=50, align="center", key="settings.resetAsk")
    ui.button(None, box, "", 0, 30, 380, 50, "danger", "Menu", "resetYes", size=20, key="settings.resetYes")
    ui.button(None, box, "", 0, 92, 380, 40, "ghost", "Menu", "closeTop", size=17, key="common.cancel")

    ad_overlay(ui, "Menu")
    d.write("scenes/menu.scene.json", "ownengine.scene", "Menu")


def main():
    for kind in ENEMIES:
        enemy_prefab(kind)
    shot("bolt", 7, bolt_parts)
    shot("shard", 6, shard_parts)
    shot("moth", 8, moth_parts)
    ebullet_prefab()
    pickups()
    brazier_prefab()
    misc_prefabs()
    run_scene()
    menu_scene()
    print("wrote prefabs and scenes")


if __name__ == "__main__":
    main()
