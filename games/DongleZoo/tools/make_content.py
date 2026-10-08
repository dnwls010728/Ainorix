"""Writes the prefabs and the scene (scenes/main.scene.json).

They are plain engine files and can be edited in the editor, but rerunning this tool
overwrites them. It exists so the eleven animal prefabs and the many similar UI elements
stay consistent. Run from the project folder:

    python tools/make_content.py
"""
import json
import os
import re
import argparse

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

FONT = "assets/fonts/Jua-Regular.ttf"
UIA, FX, ANI = "assets/ui/", "assets/fx/", "assets/animals/"

# Must match scripts/lib/defs.lua.
PX = 96
JAR_HALF, FLOOR_Y, LINE_Y, DROP_Y = 3.3, -5.0, 3.4, 4.3
RADII = [0.26, 0.34, 0.45, 0.52, 0.66, 0.80, 0.92, 1.08, 1.25, 1.43, 1.62]
SPRITE_SCALE = 2 * 256 / 200
ITEMS = ["hammer", "shake", "wand"]
THEMES = ["meadow", "beach", "night"]
DAILY = [50, 60, 80, 100, 120, 150, 300]

# Sprite sorting layers
L_BG, L_JAR, L_GUIDE, L_LINE, L_PIECE, L_HOLDER, L_FX = 0, 1, 2, 3, 5, 6, 8

INK, INK_DIM, TITLE = "#6b4a2f", "#9a7a5c", "#ff8a3c"
CREAM, CREAM2, EDGE = "#fff8ea", "#fff0d2", "#ffdca0"

STYLES = {  # background, text, border
    "primary": ("#ffb443", "#ffffff", "#ffd98a"),
    "green": ("#6ecb5a", "#ffffff", "#b4eaa6"),
    "blue": ("#5fb4f5", "#ffffff", "#b5dcfb"),
    "pink": ("#ff8fb0", "#ffffff", "#ffc7d8"),
    "plain": ("#fff3dc", INK, "#ffdca0"),
}


def project_data(name):
    with open(os.path.join(ROOT, "config", name + ".json"), encoding="utf-8") as f:
        return json.load(f)


def lua_data(value):
    """Serialize the JSON data subset as UTF-8 Lua literals in source order."""
    if isinstance(value, str):
        return json.dumps(value, ensure_ascii=False)
    if isinstance(value, bool):
        return "true" if value else "false"
    if value is None:
        return "nil"
    if isinstance(value, (int, float)):
        return json.dumps(value, allow_nan=False)
    if isinstance(value, list):
        return "{ " + ", ".join(lua_data(v) for v in value) + " }"
    return "{ " + ", ".join("[" + lua_data(k) + "] = " + lua_data(v) for k, v in value.items()) + " }"


def shop_data():
    catalog = project_data("shop")
    products = catalog["products"]
    ids = set()
    for product in products:
        sku = product["id"]
        if not isinstance(sku, str) or not re.fullmatch(r"[a-z][a-z0-9_]*", sku) or sku in ids:
            raise ValueError("Product IDs must be unique lowercase names: " + repr(sku))
        ids.add(sku)
        for field in ("name", "desc"):
            for locale in ("en", "ko"):
                if not isinstance(product[field][locale], str):
                    raise ValueError("Product " + sku + " needs EN/KO " + field)
        if not isinstance(product["price"], str):
            raise ValueError("Product " + sku + " needs a display price")
        if sku != "no_ads" and (type(product.get("coins")) is not int or product["coins"] <= 0):
            raise ValueError("Coin packs need a positive integer coins reward: " + sku)
    path = os.path.join(ROOT, "scripts", "config", "shop.lua")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("-- Generated from config/shop.json by tools/make_content.py; do not hand edit.\nreturn { products = {\n")
        for product in products:
            f.write("  " + lua_data(product) + ",\n")
        f.write("} }\n")
    return products


def rgb(hexcolor):
    h = hexcolor.lstrip("#")
    return [round(int(h[i:i + 2], 16) / 255, 4) for i in (0, 2, 4)]


def english_strings():
    """key -> English text from scripts/lib/i18n.lua, so the scene reads well in the editor before any script runs."""
    text = open(os.path.join(ROOT, "scripts", "lib", "i18n.lua"), encoding="utf-8").read()
    found = re.findall(r'\["([\w.]+)"\]\s*=\s*\{\s*en\s*=\s*"((?:[^"\\]|\\.)*)"', text)
    return {k: v.replace("\\n", "\n") for k, v in found}


EN = english_strings()


class Doc:
    """An entity list with ids, for a scene or a prefab."""

    def __init__(self):
        self.entities = []
        self.names = set()

    def add(self, name, components, parent=None):
        if name is None:
            name = "e%d" % (len(self.entities) + 1)
        assert name not in self.names, name
        self.names.add(name)
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
    s = {"texture": tex, "width": w, "height": h, "alphaCutoff": 0, "pixelArt": False, "layer": layer}
    for k, v in kw.items():
        s[k] = rgb(v) if k == "color" else v
    return s


# ---------------------------------------------------------------- prefabs

def prefabs():
    for i, r in enumerate(RADII):
        d = Doc()
        root = d.add("Piece", {
            "RigidBody2D": {"mass": round(r * r * 10, 3), "gravityScale": 2.2, "linearDamping": 0.05, "angularDamping": 1.2},
            "Collider2D": {"shape": "circle", "radius": r, "friction": 0.35, "bounciness": 0.12},
            "Tag": {"tags": "piece"},
        })
        size = round(r * SPRITE_SCALE, 4)
        d.add("Face", {"Sprite": sprite("%s%02d.png" % (ANI, i + 1), size, size, L_PIECE)}, root)
        d.write("prefabs/piece_%02d.prefab.json" % (i + 1), "ownengine.prefab", "piece_%02d" % (i + 1))

    d = Doc()
    d.add("Burst", {"VFXSystem": vfx({
        "rate": 0, "burst": 0, "loop": False, "lifetime": 0.7, "lifetimeVariation": 0.5, "speed": 4, "speedVariation": 0.6,
        "drag": 3, "spread": 180, "gravity": [0, -5, 0], "startSize": 0.3, "endSize": 0.04, "sizeVariation": 0.5,
        "space": "world", "dimensions": 2, "texture": FX + "star.png", "layer": L_FX, "maxParticles": 32})})
    d.write("prefabs/fx_burst.prefab.json", "ownengine.prefab", "fx_burst")

    d = Doc()
    d.add("Ring", {"Sprite": sprite(FX + "ring.png", 1, 1, L_FX, opacity=0.9)})
    d.write("prefabs/fx_ring.prefab.json", "ownengine.prefab", "fx_ring")


# ---------------------------------------------------------------- UI builders

class UI:
    """UI entity builders. x, y, w, h are reference pixels (720 x 1280 design) relative to the parent element."""

    def __init__(self, doc):
        self.d = doc
        self.order = 0

    def _o(self):
        self.order += 1
        return self.order

    def comp(self, ent, name, values):
        self.d.entities[ent - 1]["components"][name] = values
        return ent

    def motion(self, ent, enter="fade", duration=0.25, delay=0.0, distance=40):
        return self.comp(ent, "UIMotion", {"enter": enter, "duration": duration, "delay": delay, "distance": distance})

    def root(self, name, parent=None, visible=True, block=False, color="#000000", opacity=0.0):
        return self.d.add(name, {"UIPanel": {
            "anchor": "stretch", "x": 0, "y": 0, "width": 0, "height": 0, "color": rgb(color), "opacity": opacity,
            "blockInput": block, "visible": visible, "order": self._o()}}, parent)

    def panel(self, name, parent, x, y, w, h, color=CREAM, anchor="center", radius=0, border=0, border_color=EDGE,
              opacity=1.0, visible=True, layout=None, clip=False):
        comps = {"UIPanel": {"anchor": anchor, "x": x, "y": y, "width": w, "height": h, "color": rgb(color), "opacity": opacity,
                             "radius": radius, "borderWidth": border, "borderColor": rgb(border_color), "visible": visible,
                             "clip": clip, "order": self._o()}}
        if layout:
            comps["UILayout"] = layout
        return self.d.add(name, comps, parent)

    def text(self, name, parent, text, x, y, size, color=INK, anchor="center", w=0, h=0, align="auto", outline=0,
             outline_color="#6b4a2f", visible=True, key=None, opacity=1.0, wrap=False):
        comps = {"UIText": {"text": text, "font": FONT, "size": size, "color": rgb(color), "anchor": anchor, "x": x, "y": y,
                            "width": w, "height": h, "align": align, "verticalAlign": "middle", "outlineWidth": outline,
                            "outlineColor": rgb(outline_color), "richText": False, "visible": visible, "opacity": opacity,
                            "order": self._o()}}
        if wrap:
            comps["UIText"]["wrap"] = True
        if key:  # localized by scripts/lib/ui.lua; the English text is the edit-time content
            comps["Tag"] = {"tags": "t:" + key}
            comps["UIText"]["text"] = EN.get(key, text)
        return self.d.add(name, comps, parent)

    def image(self, name, parent, tex, x, y, w, h, anchor="center", color="#ffffff", opacity=1.0, visible=True, **kw):
        comps = {"UIImage": {"texture": tex, "anchor": anchor, "x": x, "y": y, "width": w, "height": h, "color": rgb(color),
                             "opacity": opacity, "preserveAspect": True, "visible": visible, "order": self._o()}}
        comps["UIImage"].update(kw)
        return self.d.add(name, comps, parent)

    def button(self, name, parent, label, x, y, w, h, style, action, arg=None, anchor="center", size=None, radius=None,
               key=None, visible=True):
        bg, fg, border = STYLES[style]
        params = {"a": action}
        if arg is not None:
            params["b"] = arg
        comps = {
            "UIButton": {"text": label, "font": FONT, "size": size or int(h * 0.42), "textColor": rgb(fg), "anchor": anchor,
                         "x": x, "y": y, "width": w, "height": h, "color": rgb(bg),
                         "radius": radius if radius is not None else min(h / 2, 30), "borderWidth": 4, "borderColor": rgb(border),
                         "hoverBrightness": 1.05, "pressedBrightness": 0.92, "hoverScale": 1.03, "pressedScale": 0.95,
                         "visible": visible, "order": self._o()},
            "Script": {"path": "scripts/ui/button.lua", "params": params},
        }
        if key:
            comps["Tag"] = {"tags": "t:" + key}
            comps["UIButton"]["text"] = EN.get(key, label)
        return self.d.add(name, comps, parent)

    def bar(self, name, parent, x, y, w, h, fill, anchor="center", track="#f0dfc0", value=0.0):
        return self.d.add(name, {"UISlider": {
            "value": value, "min": 0, "max": 1, "anchor": anchor, "x": x, "y": y, "width": w, "height": h, "color": rgb(track),
            "fillColor": rgb(fill), "handle": False, "interactable": False, "radius": h / 2, "order": self._o()}}, parent)


def dialog(ui, name, w, h, title_key=None, title_name=None, close=True):
    """Cream box centred on a blocking backdrop; returns the box."""
    back = ui.root("Dlg:" + name, None, visible=False, block=True, color="#2a1c3a", opacity=0.6)
    ui.motion(back, "fade", 0.15)
    box = ui.panel("Dlg:%s:box" % name, back, 0, 0, w, h, CREAM, radius=40, border=6, border_color=EDGE)
    ui.motion(box, "pop", 0.28)
    if title_key or title_name:
        ui.text(title_name, box, "", 0, 34, 48, TITLE, anchor="top", key=title_key)
    if close:
        ui.button("Dlg:%s:close" % name, box, "X", -18, 18, 64, 64, "pink", "close", anchor="top-right", size=32, radius=32)
    return box


# ---------------------------------------------------------------- scene

def scene():
    products = shop_data()
    layout = project_data("layout")
    d = Doc()
    ui = UI(d)

    # ----- world
    d.add("Camera", {"Transform": {"position": [0, 0, 20]},
                     "Camera": {"projection": "orthographic", "orthoSize": 1280 / PX / 2, "clearColor": rgb("#8fd8ff")}})
    d.add("Canvas", {"UICanvas": {"referenceWidth": 720, "referenceHeight": 1280, "match": 0}})
    d.add("Background", {"Transform": {"position": [0, 0, -2]}, "Sprite": sprite("assets/bg/meadow.png", 14, 14, L_BG)})
    d.add("Jar", {"Transform": {"position": [0, FLOOR_Y + 9.9 / 2, -1]}, "Sprite": sprite(FX + "jar.png", 7.2, 10.5, L_JAR)})
    solid = {"friction": 0.35, "bounciness": 0.05}
    d.add("Floor", {"Transform": {"position": [0, FLOOR_Y - 0.5, 0]}, "Collider2D": {"shape": "box", "size": [12, 1, 1], **solid}})
    d.add("Wall L", {"Transform": {"position": [-JAR_HALF - 0.5, 5, 0]}, "Collider2D": {"shape": "box", "size": [1, 30, 1], **solid}})
    d.add("Wall R", {"Transform": {"position": [JAR_HALF + 0.5, 5, 0]}, "Collider2D": {"shape": "box", "size": [1, 30, 1], **solid}})
    d.add("Line", {"Transform": {"position": [0, LINE_Y, 0]},
                   "Sprite": sprite(FX + "dash.png", JAR_HALF * 2, JAR_HALF * 2 / 32, L_LINE, color="#ff6b6b", opacity=0.45)})
    d.add("Guide", {"Sprite": sprite(FX + "guide.png", 0.12, 9, L_GUIDE, opacity=0.6, visible=False)})
    d.add("Holder", {"Transform": {"position": [0, DROP_Y, 0]}, "Sprite": sprite(ANI + "01.png", 0.66, 0.66, L_HOLDER, visible=False)})
    d.add("App", {"Script": {"path": "scripts/app.lua"}})

    # ----- HUD
    hud = ui.root("HUD", visible=False)
    ui.text("HUD:score", hud, "0", 0, 16, 76, "#ffffff", anchor="top", outline=6)
    best = ui.panel(None, hud, 0, 100, 200, 40, anchor="top", opacity=0)
    ui.image(None, best, UIA + "crown.png", -52, 0, 34, 34)
    ui.text("HUD:bestv", best, "0", 10, 0, 28, "#ffffff", outline=4, w=120, h=40, align="left")
    pause = ui.button("HUD:pause", hud, "", -18, 18, 72, 72, "blue", "pause", anchor="top-right", radius=36)
    ui.image(None, pause, UIA + "pause.png", 0, 0, 40, 40)
    nxt = ui.panel(None, hud, -14, 100, 100, 104, CREAM, anchor="top-right", radius=24, border=4)
    ui.text(None, nxt, "", 0, 8, 22, INK_DIM, anchor="top", key="hud.next")
    ui.image("HUD:nextimg", nxt, ANI + "01.png", 0, 12, 62, 62)
    ui.text("HUD:combo", hud, "", 0, 150, 40, "#ffe066", anchor="top", outline=5)
    pick = ui.panel("HUD:pick", hud, 0, 150, 600, 60, "#fff8ea", anchor="top", radius=30, border=4, visible=False)
    ui.text("HUD:pick:text", pick, "", 0, 0, 28, INK)
    hint = ui.panel("HUD:hint", hud, 0, -60, 620, 130, "#fff8ea", radius=30, border=4, opacity=0.92, visible=False)
    ui.text(None, hint, "", 0, 0, 27, INK, w=580, h=110, align="center", key="hud.tutorial")
    for i, it in enumerate(ITEMS):
        b = ui.button("Item:" + it, hud, "", (i - 1) * 190, -16, 160, 112, "plain", "item", it, anchor="bottom", radius=30)
        ui.image(None, b, UIA + it + ".png", 0, 0, 78, 78)
        badge = ui.panel("Item:%s:badge" % it, b, 8, -8, 46, 46, "#ff6b6b", anchor="top-right", radius=23, border=3, border_color="#ffffff")
        ui.text("Item:%s:n" % it, badge, "2", 0, 0, 26, "#ffffff")

    # ----- home
    home = ui.root("Home", visible=True, color="#ffffff", opacity=0.18)
    ui.motion(home, "fade", 0.25)
    ui.image(None, home, UIA + "cloud.png", 0, 120, 640, 260, anchor="top")
    ui.text(None, home, "", 0, 190, 92, TITLE, anchor="top", outline=7, outline_color="#ffffff", key="home.title")
    ui.text(None, home, "", 0, 306, 30, INK, anchor="top", key="home.tagline")
    for x, y, tier, size in ((-190, -150, 7, 190), (10, -190, 11, 250), (200, -140, 6, 170), (-80, -60, 1, 90), (110, -50, 3, 120)):
        ui.image(None, home, "%s%02d.png" % (ANI, tier), x, y, size, size)
    ui.text("Home:best", home, "", 0, 60, 38, "#ffffff", outline=5)
    play = ui.button("Home:play", home, "", 0, 170, 400, 124, "primary", "play", size=58, radius=62, key="home.play")
    ui.comp(play, "UIMotion", {"enter": "pop", "duration": 0.35, "delay": 0.1, "distance": 40})
    menu = [("shop", "shop", "home.shop", "pink"), ("book", "book", "home.book", "blue"), ("missions", "tasks", "home.missions", "green"),
            ("daily", "gift", "home.daily", "primary"), ("settings", "gear", "home.settings", "blue")]
    for i, (dlg, icon, key, style) in enumerate(menu):
        x = (i - 2) * 134
        b = ui.button("Home:btn:" + dlg, home, "", x, 350, 112, 112, style, "open", dlg, radius=36)
        ui.image(None, b, UIA + icon + ".png", 0, 0, 62, 62)
        ui.text(None, home, "", x, 430, 26, "#ffffff", outline=4, key=key)
        if dlg in ("daily", "missions"):
            ui.panel("Home:dot:" + dlg, b, 6, -6, 30, 30, "#ff4d4d", anchor="top-right", radius=15, border=3, border_color="#ffffff", visible=False)

    # ----- wallet (always on top of the screens, under the dialogs)
    wallet = ui.panel("Wallet", None, 16, 22, 196, 64, CREAM, anchor="top-left", radius=32, border=4)
    ui.image(None, wallet, UIA + "coin.png", 8, 0, 48, 48, anchor="left")
    ui.text("Wallet:n", wallet, "0", -18, 0, 32, INK, anchor="right", w=120, h=40, align="right")

    # ----- floating score texts
    for i in range(1, 7):
        ui.text("Pop%d" % i, None, "", 0, 0, 34, "#ffffff", anchor="top", outline=5, opacity=0)

    # ----- pause
    box = dialog(ui, "pause", layout["pause"]["width"], 560, "pause.title", close=False)
    ui.button("Pause:resume", box, "", 0, -70, 380, 100, "primary", "resume", size=44, key="pause.resume")
    ui.button("Pause:restart", box, "", 0, 50, 380, 92, "green", "restart", size=40, key="pause.restart")
    ui.button("Pause:home", box, "", 0, 162, 380, 92, "blue", "home", size=40, key="pause.home")

    # ----- game over
    box = dialog(ui, "over", 580, 860, title_name="Over:title", close=False)
    ui.text(None, box, "", 0, 112, 30, INK_DIM, anchor="top", key="over.score")
    ui.text("Over:score", box, "0", 0, 140, 104, INK, anchor="top")
    ui.text("Over:best", box, "", 0, 262, 32, INK_DIM, anchor="top")
    coins = ui.panel(None, box, 0, 316, 240, 64, CREAM2, anchor="top", radius=32, border=4)
    ui.image(None, coins, UIA + "coin.png", 22, 0, 46, 46, anchor="left")
    ui.text("Over:coins", coins, "+0", 26, 0, 36, INK)
    col = ui.panel(None, box, 0, 404, 460, 0, anchor="top", opacity=0,
                   layout={"direction": "vertical", "spacing": 14, "crossAlign": "stretch", "fit": True})
    ui.button("Over:revive", col, "", 0, 0, 460, 92, "green", "revive", size=38, key="over.revive")
    ui.button("Over:double", col, "", 0, 0, 460, 92, "pink", "double", size=38, key="over.double")
    ui.button("Over:retry", col, "", 0, 0, 460, 92, "primary", "retry", size=40, key="over.retry")
    ui.button("Over:home", col, "", 0, 0, 460, 80, "blue", "home", size=34, key="over.home")

    # ----- shop
    box = dialog(ui, "shop", 660, 1010, "shop.title")
    ui.text(None, box, "", 0, 110, 30, INK_DIM, anchor="top", key="shop.themes")
    for i, th in enumerate(THEMES):
        card = ui.panel(None, box, (i - 1) * 206, 156, 194, 300, CREAM2, anchor="top", radius=26, border=4)
        ui.image(None, card, "assets/bg/%s_thumb.png" % th, 0, 14, 160, 160, anchor="top", preserveAspect=False, radius=20)
        ui.text("Theme:%s:name" % th, card, "", 0, 182, 23, INK, anchor="top")
        b = ui.button("Theme:%s:btn" % th, card, "", 0, -14, 164, 60, "primary", "theme", th, anchor="bottom", size=28)
        ui.image("Theme:%s:coin" % th, b, UIA + "coin.png", 12, 0, 34, 34, anchor="left")
    ui.text(None, box, "", 0, 486, 30, INK_DIM, anchor="top", key="shop.store")
    # Preserve the shipped three-row layout exactly; larger catalogs scroll inside it.
    product_parent, product_y = box, 532
    if len(products) > 3:
        product_parent = ui.panel("Shop:products", box, 0, 532, 600, 430,
                                  anchor="top", opacity=0, clip=True)
        ui.comp(product_parent, "UIScroll", {"direction": "vertical", "barColor": rgb(INK_DIM)})
        product_y = 0
    for i, product in enumerate(products):
        p = product["id"]
        row = ui.panel(None, product_parent, 0, product_y + i * 148, 600, 134, CREAM2, anchor="top", radius=26, border=4)
        ui.image(None, row, UIA + ("noads.png" if p == "no_ads" else "coin.png"), 20, 0, 80, 80, anchor="left")
        ui.text("Product:%s:name" % p, row, "", 118, -24, 32, INK, anchor="left", w=280, h=40, align="left")
        ui.text("Product:%s:desc" % p, row, "", 118, 24, 21, INK_DIM, anchor="left", w=290, h=52, align="left", wrap=True)
        ui.button("Product:%s:btn" % p, row, "", -18, 0, 160, 72, "green", "product", p, anchor="right", size=30)

    # ----- collection
    box = dialog(ui, "book", 660, 960, "book.title")
    ui.text("Book:count", box, "", 0, 104, 28, INK_DIM, anchor="top")
    grid = ui.panel(None, box, 0, 150, 616, 780, anchor="top", opacity=0,
                    layout={"direction": "grid", "columns": 3, "spacing": 10, "align": "center", "crossAlign": "center"})
    for tier in range(1, 12):
        cell = ui.panel(None, grid, 0, 0, 196, 186, CREAM2, radius=24, border=4)
        ui.image("Book:img%d" % tier, cell, "%s%02d.png" % (ANI, tier), 0, -18, 128, 128)
        ui.text("Book:name%d" % tier, cell, "", 0, -10, 24, INK, anchor="bottom")

    # ----- missions
    box = dialog(ui, "missions", 660, 720, "missions.title")
    for i in range(1, 4):
        row = ui.panel(None, box, 0, 116 + (i - 1) * 164, 600, 150, CREAM2, anchor="top", radius=26, border=4)
        ui.text("Mission%d:text" % i, row, "", 24, 18, 28, INK, anchor="top-left", w=400, h=36, align="left")
        ui.bar("Mission%d:bar" % i, row, 24, -36, 330, 22, "#6ecb5a", anchor="bottom-left")
        ui.text("Mission%d:val" % i, row, "", 24, -68, 22, INK_DIM, anchor="bottom-left", w=330, h=26, align="left")
        ui.image(None, row, UIA + "coin.png", -128, 18, 34, 34, anchor="top-right")
        ui.text("Mission%d:reward" % i, row, "", -22, 18, 28, INK, anchor="top-right", w=100, h=34, align="left")
        ui.button("Mission%d:btn" % i, row, "", -18, -16, 170, 66, "primary", "claimMission", i, anchor="bottom-right", size=28)
    ui.text(None, box, "", 0, -34, 24, INK_DIM, anchor="bottom", key="missions.reset")

    # ----- daily gift
    box = dialog(ui, "daily", 660, 800, "daily.title")
    ui.text(None, box, "", 0, 106, 25, INK_DIM, anchor="top", key="daily.desc")
    grid = ui.panel(None, box, 0, 156, 616, 320, anchor="top", opacity=0,
                    layout={"direction": "grid", "columns": 4, "spacing": 10, "align": "center", "crossAlign": "center"})
    for i in range(1, 8):
        cell = ui.panel("Day%d" % i, grid, 0, 0, 144, 150, "#fff3dc", radius=22, border=4)
        ui.text("Day%d:label" % i, cell, "", 0, 10, 22, INK_DIM, anchor="top")
        ui.image(None, cell, UIA + ("gift.png" if i == 7 else "coin.png"), 0, -2, 56, 56, color="#ff8fb0" if i == 7 else "#ffffff")
        ui.text("Day%d:n" % i, cell, "", 0, -10, 26, INK, anchor="bottom")
        chk = ui.panel("Day%d:check" % i, cell, 0, 0, 144, 150, "#6ecb5a", radius=22, opacity=0.78, visible=False)
        ui.image(None, chk, UIA + "check.png", 0, 0, 72, 72)
    ui.button("Daily:claim", box, "", 0, -150, 440, 96, "primary", "dailyClaim", anchor="bottom", size=42, key="daily.claim")
    ui.button("Daily:claimx2", box, "", 0, -44, 440, 88, "green", "dailyClaim2", anchor="bottom", size=36, key="daily.claimx2")
    ui.text("Daily:tomorrow", box, "", 0, -110, 36, INK, anchor="bottom", key="daily.tomorrow", visible=False)

    # ----- settings
    box = dialog(ui, "settings", 580, 600, "settings.title")
    rows = [("sound", "settings.sound", "Set:sfx", "toggleSfx"), ("music", "settings.music", "Set:music", "toggleMusic"),
            ("gear", "settings.language", "Set:lang", "toggleLang")]
    for i, (icon, key, name, action) in enumerate(rows):
        row = ui.panel(None, box, 0, 126 + i * 124, 500, 108, CREAM2, anchor="top", radius=26, border=4)
        ui.image(None, row, UIA + icon + ".png", 22, 0, 54, 54, anchor="left", color="#ffb443")
        ui.text(None, row, "", 96, 0, 32, INK, anchor="left", w=200, h=40, align="left", key=key)
        ui.button(name, row, "", -16, 0, 170, 72, "blue", action, anchor="right", size=30)
    ui.text(None, box, "", 0, -30, 22, INK_DIM, anchor="bottom", key="settings.version")

    # ----- out of an item
    box = dialog(ui, "item", 560, 620, close=True)
    ui.text("ItemDlg:title", box, "", 0, 92, 38, TITLE, anchor="top")
    ui.image("ItemDlg:icon", box, UIA + "hammer.png", 0, -110, 150, 150)
    ui.text("ItemDlg:desc", box, "", 0, 4, 27, INK_DIM, w=460, h=80, align="center", wrap=True)
    buy = ui.button("ItemDlg:buy", box, "", 0, 110, 420, 92, "primary", "itemBuy", size=38)
    ui.image(None, buy, UIA + "coin.png", 30, 0, 46, 46, anchor="left")
    ui.button("ItemDlg:free", box, "", 0, 218, 420, 92, "green", "itemFree", size=38, key="item.free")

    # ----- toasts
    stack = ui.panel("Toasts", None, 0, -170, 700, 0, anchor="bottom", opacity=0,
                     layout={"direction": "vertical", "spacing": 8, "crossAlign": "center", "fit": True})
    for i in range(1, 4):
        pill = ui.panel("Toast%d" % i, stack, 0, 0, 500, 60, CREAM, radius=30, border=4, visible=False)
        ui.motion(pill, "slide-up", 0.25, distance=24)
        ui.text("Toast%d:text" % i, pill, "", 0, 0, 30, INK)

    # ----- ad / store overlay (demo provider, scripts/lib/ads.lua)
    ad = ui.root("Ad", None, visible=False, block=True, color="#12101c", opacity=0.94)
    ui.text("Ad:title", ad, "", 0, -120, 44, "#fff3dc")
    ui.text("Ad:sub", ad, "", 0, -40, 27, "#d8ceb8", w=600, h=90, align="center", wrap=True)
    ui.bar("Ad:bar", ad, 0, 50, 420, 16, "#ffb443", track="#3a3040")
    ui.button("Ad:skip", ad, "", 0, 150, 220, 72, "plain", "adSkip", size=30, key="ad.skip")
    ui.button("Ad:buy", ad, "", -130, 150, 220, 80, "green", "adBuy", size=34, key="ad.buy")
    ui.button("Ad:cancel", ad, "", 130, 150, 220, 80, "plain", "adCancel", size=34, key="common.cancel")

    d.write("scenes/main.scene.json", "ownengine.scene", "Main")
    return len(d.entities)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scene-only", action="store_true", help="Regenerate scene and shop Lua, preserving prefabs.")
    args = parser.parse_args()
    if not args.scene_only:
        prefabs()
    print("entities:", scene())
