# In-game UI

Screen-space UI built from components, modelled on Unity's uGUI (RectTransform anchors, Image, Slider,
layout groups, CanvasScaler) and Godot's Control nodes (anchor presets, containers, `clip_contents`).
Both renderers draw it from the same list of quads (`BuildUIQuads` in `engine/render/UI.cpp`), so the
software screenshot an agent looks at shows exactly what the game window shows.

| Component | What it is |
|---|---|
| `UIText` | Text in a TrueType font: alignment, word wrap, line/letter spacing, bold, outline, shadow, rich text, typewriter reveal |
| `UIPanel` | Rectangle with rounded corners and a border; container for other elements; can clip its children |
| `UIButton` | Clickable button with a label, hover/pressed/disabled looks, `onClick`, `onPointerEnter/Exit` |
| `UIImage` | Picture: sprite sheet frame, 9-slice, preserve aspect, fill amount (health bars, cooldowns), rounded corners |
| `UISlider` | Draggable slider (`onValueChanged`), or a progress/health bar with `interactable: false` |
| `UILayout` | Arranges the element's children in a column, row or grid; `fit` sizes the element to its content |
| `UICanvas` | Optional, once per scene: reference resolution and scale mode |

## Placement

Every element has `anchor`, `x`, `y`, `width`, `height`, `opacity`, `visible` and `order`.

- The **parent rectangle** is the nearest ancestor entity with a UI element, or the whole screen. Use
  `entity.create {parent: "Window", ...}` to put a button inside a panel.
- `anchor` picks the point of the parent it hangs from **and** the element's pivot:
  `anchor: "bottom-right", x: -20, y: -20` puts the element's bottom-right corner 20 px from the parent's
  bottom-right corner. Presets: `top-left, top, top-right, left, center, right, bottom-left, bottom, bottom-right`.
- Stretch presets fill the parent along an axis: `stretch` (both), `stretch-top/middle/bottom` (horizontal band),
  `stretch-left/center/right` (vertical band). On a stretched axis `width`/`height` is **added** to the parent's
  size (Unity's sizeDelta): `anchor: "stretch", width: -40, height: -40` leaves a 20 px margin all around.
- `x`, `y`, sizes and font sizes are **reference pixels**. The reference canvas is 1280×720 scaled with the
  screen height, or what a `UICanvas {referenceWidth, referenceHeight, match}` says (`match` 0 = follow the
  width, 1 = the height, in between = blend).
- `UIText` and `UIImage` with `width`/`height` 0 take their natural size (the text, the image's pixel size).
- `visible: false` hides the element **and its children**. `opacity` applies to the element only; a
  `UIPanel` with `opacity: 0` is an invisible container.
- Draw order: siblings by `order`, then id; children always draw on top of their parent.

## Layout containers

`UILayout {direction, spacing, padding, align, crossAlign, columns, fit}` on an entity that has a UI element
places its UI children one after another in **hierarchy (id) order**; their `anchor`/`x`/`y` are ignored.

| Field | |
|---|---|
| `direction` | `vertical` (column), `horizontal` (row), `grid` (`columns` equal cells per row) |
| `align` | `start` / `center` / `end` along the direction |
| `crossAlign` | `start` / `center` / `end` / `stretch` across it (stretch = full width of a column) |
| `fit` | resize the element to wrap its children (menus, dialogs) |

```json
{"name": "Menu", "components": {
  "UIPanel":  {"anchor": "center", "x": 0, "y": 0, "width": 320, "height": 0, "radius": 12, "opacity": 0.9},
  "UILayout": {"padding": 20, "spacing": 12, "crossAlign": "stretch", "fit": true}}}
{"name": "Play",    "parent": "Menu", "components": {"UIButton": {"text": "Play", "height": 56}}}
{"name": "Options", "parent": "Menu", "components": {"UIButton": {"text": "Options", "height": 56}}}
```

## Text and fonts

`UIText {text, font, size, color, align, verticalAlign, wrap, lineSpacing, letterSpacing, bold, richText,
outlineWidth, outlineColor, shadowDistance, shadowColor, visibleCharacters}`

- `font`: `"default"` (Roboto, built into the engine), `"pixel"` (the 5×7 pixel font, ASCII only) or a font
  file in the project: put `.ttf` / `.otf` / `.ttc` files in `assets/fonts/` and use the path. Any script the font
  covers works (Korean, Japanese, Chinese, Cyrillic...); characters the font lacks fall back to the default font.
  `asset.info {path}` reports the font family. Font files reload when they change.
- `size` is the font size (em height) in reference pixels; for `"pixel"` it is the line height.
- A `width` (or a stretched anchor) gives the text a box: lines wrap at spaces (and between CJK characters) and
  `align` (`left/center/right`, `auto` = follow the anchor) positions them; with a `height`, `verticalAlign`
  (`top/middle/bottom`) places the block.
- Rich text (on by default): `<color=#ff8800>`, `<color=#f80>`, `<color=red>` … `</color>`, `<b>` … `</b>`.
  Other `<...>` text is shown as is; `richText: false` shows everything literally.
- `visibleCharacters: n` shows only the first n characters without reflowing (typewriter dialogue): increase it
  from a script every few frames.
- Glyphs are rasterized (stb_truetype) at the exact pixel size on screen and cached in atlas pages, so text is
  sharp at every resolution and identical in both renderers.

## Buttons, sliders and pointer events

Pointer input is processed once per simulated frame, after scripts' `onUpdate`, from the game view's mouse
position (disabled while the mouse is locked for mouse look):

| Callback (Script on the same entity) | When |
|---|---|
| `onPointerEnter(self)` / `onPointerExit(self)` | the pointer moves onto / off an interactable button or slider |
| `onClick(self)` | left button pressed on an interactable `UIButton` |
| `onValueChanged(self, value)` | a dragged `UISlider` changed value (the slider follows the pointer until release) |

`UIButton`: `color`, `textColor`, `font`, `size`, `radius`, `borderWidth`, `borderColor`, `hoverBrightness`,
`pressedBrightness`, `interactable` (false = faded, no events).
`UISlider`: `value`, `min`, `max`, `step`, `direction` (`left-to-right`, `right-to-left`, `bottom-to-top`,
`top-to-bottom`), `color` (track), `fillColor`, `handle`, `handleColor`, `radius`, `interactable`.
Panels, images and text never block clicks. A `UIPanel {clip: true}` cuts its children off at its edges; clipped
parts cannot be clicked.

### On-screen controls (touch screens)

`UIButton {key: "Left"}` turns a button into a virtual key: while the mouse or **any finger** is on it,
the key is down (`input.down`, one `input.pressed` edge, built-in `CharacterBody` controls), and the
button shows its pressed look. Several fingers hold several buttons at once, so a phone game can be
steered with the same code as the keyboard version:

```json
{"name": "BtnLeft",  "components": {"UIButton": {"text": "<", "anchor": "bottom-left",  "x": 24,  "y": -24, "width": 120, "height": 120, "key": "Left"}}}
{"name": "BtnRight", "components": {"UIButton": {"text": ">", "anchor": "bottom-left",  "x": 168, "y": -24, "width": 120, "height": 120, "key": "Right"}}}
{"name": "BtnJump",  "components": {"UIButton": {"text": "A", "anchor": "bottom-right", "x": -24, "y": -24, "width": 140, "height": 140, "key": "Space"}}}
```

Raw fingers are in `input.touches()` (Lua); tools put fingers down with `input.touch {id, x, y}` and lift
them with `input.touch {id, down: false}`. Hide the buttons on desktop builds from a script if needed.

Health bar: a `UISlider {interactable: false, handle: false}` or a `UIImage {fill, fillOrigin}` (with a
texture, e.g. a gradient). Set the value from Lua: `scene.set(scene.find("HP"), "UISlider", {value = hp})`.

## Images

`UIImage {texture, color, frame, columns, rows, slice, sliceScale, preserveAspect, pixelArt, fill, fillOrigin, radius}`

- `texture` is any project image; empty = a solid `color` rectangle (useful with `radius`).
- `columns`/`rows`/`frame` pick one cell of a sprite sheet (same numbering as `Sprite`).
- `slice: 8` = 9-slice: an 8-image-pixel border keeps its size (times `sliceScale`) while the edges and center
  stretch — frames and speech bubbles of any size from one small image.
- `fill` 0..1 shows only part of the image, growing from `fillOrigin` (`left/right/top/bottom`).

## For agents and tools

- `ui.layout {width, height, interactable?}` lists every visible element in draw order with its pixel `rect`
  and `center` on a screen of that size. Use the center with `input.click {x, y, width, height}`.
- `render.screenshot` includes UI in the game view (scene camera); free cameras skip it unless `{ui: true}`.
- `input.mouse {x, y}` moves the pointer (hover), `input.mouse {button: "MouseLeft", down}` presses/releases
  (slider drags), `input.click {x, y}` clicks.
- In the editor, the Add menu has Text, Button, Panel, Image, Slider, Progress Bar and Menu (vertical layout);
  with a UI element selected, new UI goes inside it.
