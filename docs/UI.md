# In-game UI

Screen-space UI built from components, modelled on Unity's uGUI (RectTransform anchors, Image, Slider,
layout groups, CanvasScaler) and Godot's Control nodes (anchor presets, containers, `clip_contents`).
Both renderers draw it from the same list of quads (`BuildUIQuads` in `engine/render/UI.cpp`), so the
software screenshot an agent looks at shows exactly what the game window shows.

| Component | What it is |
|---|---|
| `UIText` | Text in a TrueType font: alignment, word wrap, line/letter spacing, bold, outline, shadow, rich text, typewriter reveal |
| `UIPanel` | Rectangle (solid color or 9-slice image) with rounded corners and a border; container for other elements; can clip its children |
| `UIButton` | Clickable button: solid or textured (9-slice) background, an image per state, label + icon, toggles and radio groups, `onClick`, `onToggle`, `onPointerEnter/Exit` |
| `UIImage` | Picture: sprite sheet frame, 9-slice, preserve aspect, fill amount (health bars, cooldowns), rounded corners |
| `UISlider` | Draggable slider (`onValueChanged`), or a progress/health bar with `interactable: false` |
| `UIInput` | Text field, one line or several: caret, selection, clipboard, `maxLength`, `password`, `onTextChanged`, `onSubmit` |
| `UIDropdown` | Drop-down list of `options`; the open list draws on top of everything and scrolls when long; `onValueChanged` |
| `UIJoystick` | On-screen stick for touch screens that drives two gamepad axes |
| `UILayout` | Arranges the element's children in a column, row or grid; `fit` sizes the element to its content |
| `UIScroll` | Makes a panel a scroll view: clips its children and moves them with the wheel, a drag or the scroll bar |
| `UIMotion` | Entrance animation (fade, pop, slide) that plays whenever the element becomes visible |
| `UICanvas` | Optional, once per scene: reference resolution and scale mode, keyboard/gamepad `navigation` |

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

## Images instead of colors (skins)

Every element with a background takes an image for it, the way Unity uses sliced sprites, Unreal box brushes and Godot
`StyleBoxTexture`s. The fields are the same everywhere:

| Field | |
|---|---|
| `texture` | Background image. The element's `color` then **tints** it: set `color` to white (and a `UIPanel`'s `opacity` to 1) to show the image unchanged |
| `slice` | 9-slice border in image pixels: corners keep their size, edges and center stretch, so one small frame fits every size |
| `sliceScale` | Size of that border on screen, in reference pixels per image pixel (3 for a 16-pixel frame on a 720p canvas) |
| `pixelArt` | Nearest-neighbour sampling |

`UIPanel`, `UIButton`, `UISlider`, `UIInput`, `UIDropdown` have these four (`UIImage` has them already); `slice`,
`sliceScale` and `pixelArt` also apply to the element's other images:

| Element | Other images |
|---|---|
| `UIButton` | `hoverTexture`, `pressedTexture`, `disabledTexture`, `onTexture` (replace `texture` in that state), `icon` |
| `UISlider` | `fillTexture` (laid over the whole bar and cut at the value, tinted by `fillColor`), `handleTexture` (drawn at its own proportions, tinted by `handleColor`) |
| `UIDropdown` | the open list uses `texture` too |
| `UIJoystick` | `texture` (base), `handleTexture`; `pixelArt` |

`radius` and the border still apply on top of an image (use `radius: 0, borderWidth: 0` for a frame that has its own).
Text is styled by its font (`font`, `size`, `textColor`); put a child `UIText` on a `UIButton` with an empty `text` for
outlined or shadowed labels.

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

## Scroll views

`UIScroll {direction, scroll, wheelStep, bar, barColor}` on an entity with a `UIPanel` turns it into a scroll view: the
children are clipped to the panel and shifted by `scroll` (reference pixels from the start of the content). Give the panel a
`UILayout` for a list:

```json
{"name": "List", "components": {
  "UIPanel":  {"anchor": "center", "width": 600, "height": 400, "radius": 16},
  "UILayout": {"direction": "vertical", "spacing": 8, "padding": 12, "crossAlign": "stretch"},
  "UIScroll": {"wheelStep": 80}}}
{"name": "Row 1", "parent": "List", "components": {"UIButton": {"text": "First", "height": 64}}}
```

While simulating, the mouse wheel over the view scrolls it by `wheelStep` per notch, pressing its empty area (between or
beside the rows — also a finger) and dragging moves the content with the pointer, and the engine keeps `scroll` inside the
content. Rows scrolled out of view cannot be clicked. The bar on the edge can be grabbed and dragged (clicking its track jumps there; neither clicks the row underneath). The thumb shows the position when the content is larger than
the panel (`bar: false` hides it). Scripts read or set `scroll` like any field; `ui.layout` reports the panel. Tools turn the
wheel with `input.mouse {x, y, wheel}` and scripts read it with `input.wheel()`.

## Motion

- `UIMotion {enter, duration, delay, distance}` plays each time its element becomes visible (itself or through a parent that
  was hidden): `fade`, `pop` (grows from 85 % with a small overshoot) or `slide-up/down/left/right` (moves in from `distance`
  reference pixels away), always with a fade. The element's children move and fade with it. Increasing `delay` values along a
  row stagger the entrance of cards. It runs on real frames, so dialogs animate while the game is paused (`game.pause`); the state advances at the end of the frame, after scripts and click handlers, so an element shown this frame is drawn at the start of its entrance, never at its final look first; and
  outside a play session the element simply shows its final state. Set `time` to 0 to replay.
- `UIButton {hoverScale, pressedScale}` (1 = off) eases the button's size while the pointer is over it or holds it.
- Anything else is a tween from Lua: `tween.to(id, "UIPanel", {x = 0, opacity = 1}, 0.3, {ease = "outBack", delay = 0.1})`
  animates numeric, vector and colour fields of any component (see [SCRIPTING.md](SCRIPTING.md)); tweens are unscaled by
  default, like `UIMotion`.

## Text and fonts

`UIText {text, font, size, color, align, verticalAlign, wrap, lineSpacing, letterSpacing, bold, richText,
outlineWidth, outlineColor, shadowDistance, shadowColor, minSize, visibleCharacters}`

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
- `minSize: 14` lets the font shrink (one reference pixel at a time, down to that size) until the text fits its
  `width`/`height` box — for labels whose translations are longer. 0 = never shrink.
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
| `onClick(self)` | left button pressed on an interactable `UIButton` (released over it with `clickOn: "release"`) |
| `onToggle(self, on)` | a click changed `on` of a `UIButton {toggle: true}` (also on the button a radio group turned off) |
| `onValueChanged(self, value)` | a dragged `UISlider` changed value (the slider follows the pointer until release); a `UIDropdown` got another option (`value` = its index, 0 = the first) |
| `onTextChanged(self, text)` / `onSubmit(self, text)` | the text of a `UIInput` changed / Enter was pressed in it |

`UIButton`: see [Button looks and behaviour](#button-looks-and-behaviour) below.
`UISlider`: `value`, `min`, `max`, `step`, `direction` (`left-to-right`, `right-to-left`, `bottom-to-top`,
`top-to-bottom`), `color` (track), `fillColor`, `handle`, `handleColor`, `radius`, `interactable`.
Panels, images and text never block clicks. A `UIPanel {clip: true}` cuts its children off at its edges; clipped
parts cannot be clicked. A `UIPanel {blockInput: true}` is a modal backdrop: buttons and sliders drawn under it cannot be
clicked or hovered inside its rectangle, while its own children (drawn on top of it) can — show a full-screen blocking panel
with the dialog as its child and hide it again to close the dialog.

### Button looks and behaviour

Modelled on Unity's Button transitions and Toggle/ToggleGroup, Unreal's `FButtonStyle` brushes and Godot's
`Button` (icon, alignment, `toggle_mode`, `ButtonGroup`, action mode). Every field has a default that keeps
an older button looking and behaving as before.

| Group | Fields |
|---|---|
| Background | `color`, `texture` (+ `slice`, `sliceScale`, `pixelArt` like `UIImage`), `radius`, `borderWidth`, `borderColor` |
| State look | `hoverBrightness`, `pressedBrightness`; `hoverTexture`, `pressedTexture`, `disabledTexture`; `hoverScale`, `pressedScale` |
| Label | `text` (rich text: `<b>`, `<color=..>`), `font`, `size`, `textColor`, `align` (`left/center/right`), `padding` |
| Icon | `icon`, `iconSize` (height; 0 = the image's pixels), `iconSide` (`left/right/top/bottom`) |
| Behaviour | `interactable`, `clickOn` (`press` / `release`), `toggle`, `on`, `group`, `onColor`, `onTexture`, `key` |

- **Textured buttons**: `texture` is the background and `color` tints it, so set `color` to white. `slice: 8` keeps an
  8-image-pixel border of the image at its size, so one small frame fits every button size. A state texture replaces
  `texture` while that state lasts; empty means "the same". Priority: disabled, then pressed, then on, then hover.
- **State look**: the background is multiplied by `hoverBrightness` / `pressedBrightness` (1 = unchanged; a
  textured button usually sets both to 1 and swaps textures instead). A disabled button is drawn at half opacity.
  Other colours per state are a script's job (`onPointerEnter` + `tween.to`).
- **Icon and label** form one group placed by `align` inside the button, `padding` away from the left and right edge
  and from each other. An empty `text` leaves only the icon (round icon buttons: `radius` = half the size).
- **Toggles**: `toggle: true` makes a click flip `on` and call `onToggle(self, on)`; the button then shows `onColor`
  (and `onTexture`). Toggles with the same `group` name form a radio group across the scene (tabs, difficulty
  pickers): turning one on turns the others off (each gets `onToggle(self, false)`), and a click cannot turn the
  chosen one off. Scripts may write `on` directly; the group rule is applied on clicks only.
- **`clickOn: "release"`** clicks when the pointer comes up while still over the button it went down on, so the
  player can cancel by sliding off (the usual behaviour of menus on touch screens). `press` (default) clicks as the
  pointer goes down. `input.click` clicks either kind on the next frame.

```json
{"name": "Buy", "components": {"UIButton": {"text": "Buy", "icon": "assets/ui/coin.png", "iconSize": 32,
  "color": [1, 1, 1], "texture": "assets/ui/button.png", "pressedTexture": "assets/ui/button_down.png", "slice": 12,
  "pressedScale": 0.96, "clickOn": "release"}}}
{"name": "TabItems", "components": {"UIButton": {"text": "Items", "toggle": true, "on": true, "group": "tabs"}}}
{"name": "TabSkins", "components": {"UIButton": {"text": "Skins", "toggle": true, "group": "tabs"}}}
```

Sounds stay in scripts (`audio.play` in `onClick` / `onPointerEnter`).

### Keyboard and gamepad navigation

`UICanvas {navigation: true}` lets the player use menus without a pointer (Unity's Selectable navigation, Godot's
focus neighbours, worked out automatically from where the elements are):

- **Arrows / D-pad** move the focus to the nearest button or slider in that direction. The first press focuses the
  top-left element; give a menu a starting focus with `ui.focus(scene.find("Play"))`.
- **Enter / GamepadA** clicks the focused button (`onClick`, toggles flip). On a focused slider the keys along its
  direction change the value by `step` (or 1/20 of the range) and call `onValueChanged`; the other two move on.
- The focused element shows its hover look (`hoverBrightness`, `hoverTexture`, `hoverScale`) and a frame in
  `UICanvas.focusColor`. A press with the pointer moves the focus to what it pressed.
- Only what the pointer could reach can be focused: disabled or hidden elements, elements under a
  `UIPanel {blockInput}` and rows scrolled out of a scroll view are skipped (the view does not scroll by itself), so
  a modal dialog keeps the focus inside it once something in it is focused. A focus that becomes unreachable is dropped.
- Lua: `ui.focus(id)` / `ui.focus()` (clear) / `ui.focused()`. Tools: `ui.focus {id}`, `ui.layout` marks the element
  with `focused` (and toggles with `on`), keys with `input.key {key: "Down"}`.

Without `navigation` nothing reads these keys, so a game that uses the arrows for movement is unaffected.

### Text fields

`UIInput {text, placeholder, font, size, textColor, color, texture.., radius, borderWidth, borderColor, maxLength, password, multiline, interactable}`

A click (or tap) gives the field the focus and puts the caret where it was clicked; it does not need
`UICanvas.navigation`. Every change calls `onTextChanged(self, text)`.

| Input | Effect |
|---|---|
| typing | inserts at the caret, replacing the selection (any language the keyboard or IME produces; `maxLength` counts characters) |
| Left / Right, Home / End | move the caret; with **Shift** they select |
| Up / Down | `multiline`: move between lines |
| drag, Shift+click | select |
| Backspace / Delete | remove the selection, else the character before / after the caret (they repeat while held, like the arrows) |
| Ctrl+A, Ctrl+C, Ctrl+X, Ctrl+V | select all, copy, cut, paste (a `password` field never copies) |
| Enter | `onSubmit(self, text)`; in a `multiline` field a new line, and Ctrl+Enter submits |
| Escape, a click elsewhere | leave the field |

```lua
function Name:onSubmit(text) save.set("name", text) end
```

- `multiline: true` wraps the text at the field's width and scrolls so the caret's line stays in view; a single-line
  field scrolls sideways. `password: true` draws `*` for each character.
- **While a field has the focus, scripts do not see the keyboard** (`input.down("A")` is false while the player types
  "a"); mouse and gamepad input still reach them. `ui.focused()` tells which element has the focus.
- The clipboard is the system's in a game window and the editor (Windows); web and Android keep it inside the game.
  Tools read or set it with `ui.clipboard {text}`; headless sessions have their own.
- Tools type with `input.text {text}` after focusing the field (`input.click` on it, or `ui.focus {id}`); the editing
  keys go through `input.key` (`Backspace`, `Delete`, `Home`, `End`, `Left`, `Shift`, `Control` + `A`...).
- Typing is local input: do not edit a `UIInput` during a lockstep/rollback match (use it in the lobby, send the result
  with an RPC). In an **authoritative** match it works on every peer (chat fields): the text stays on the device, the
  keyboard is kept out of the network input stream while the field has the focus, and what the player submits goes to
  the server by RPC.
- Phones: Android shows the on-screen keyboard while a field has the focus (`Window::SetTextInput`) and takes the
  characters the keyboard sends as key presses — Latin letters, digits, punctuation. Composed text (Korean, Japanese,
  swipe typing) needs a Java `InputConnection` and does not arrive yet. The web player reports key presses (no IME
  composition, no on-screen keyboard on phones). See Status below.

### Drop-downs

`UIDropdown {options, selected, font, size, textColor, color, texture.., radius, borderWidth, borderColor, interactable}`

Shows `options[selected]`. A click opens the list under the element (above it when there is no room below): one row per
option, each as tall as the element, drawn after all other UI so no panel clips or covers it. A click on a row selects it
and calls `onValueChanged(self, index)` (0 = the first option, like the `selected` field); a click anywhere else only closes
the list — it does not also click what was under the pointer. A list with more options than fit between the element and
the screen edge shows as many rows as fit (on the side with more room) and scrolls with the mouse wheel or by dragging
it; a thumb shows the position. Scripts fill the list with
`scene.set(id, "UIDropdown", {options = {"English", "한국어"}, selected = 0})`.

`ui.layout` lists the rows of an open list as extra elements of the same entity with `item` (their row), so a tool
selects an option with two `input.click`s (scroll a long list with `input.mouse {x, y, wheel}` first).

### Joystick

`UIJoystick {axisX, axisY, color, handleColor, texture, handleTexture, pixelArt}` (defaults: bottom-left, 200 × 200, `LeftX` / `LeftY`) is the stick
counterpart of `UIButton.key`: while a finger (or the mouse) that **went down on it** is held, the two gamepad axes follow
the finger, clamped to the circle, +X right and +Y up; lifting the finger returns them to 0. Scripts read
`input.axis("LeftX")` and work the same with a real gamepad. A second stick with `axisX: "RightX", axisY: "RightY"` gives
twin-stick controls; fingers on other buttons do not disturb it. Hide it on desktop builds from a script if needed.

### Which component for which widget

Reviewed against what Unity (uGUI), Unreal (UMG) and Godot (Control) ship. The engine provides the widgets a script cannot
build (text entry, a list that escapes clipping, an on-screen stick, hit testing, layout); the rest are short
combinations of them:

| Widget | Build it from |
|---|---|
| Label, rich text, typewriter | `UIText` |
| Button, icon button, image button | `UIButton` (`icon`, `texture` + state textures) |
| Check box, switch | `UIButton {toggle: true}` with `onTexture` (a checked box image) or `onColor` |
| Radio buttons, tab bar | `UIButton {toggle: true, group: "tabs"}`; `onToggle` shows the matching page (`visible`) |
| Slider, progress / health bar | `UISlider`; `interactable: false, handle: false` for a bar; `UIImage {fill}` for a textured bar |
| Text field | `UIInput` |
| Drop-down / option list | `UIDropdown` |
| Scroll list, inventory grid | `UIPanel` + `UIScroll` + `UILayout` (`direction: "grid"`), rows instantiated from a prefab |
| Window, dialog, modal | `UIPanel` (+ `blockInput` backdrop, `UIMotion` entrance) |
| Toast, tooltip | a hidden `UIPanel` + `UIText`; show it from `onPointerEnter` / a timer, place it with `input.mouse()` |
| Number stepper | two `UIButton`s and a `UIText` in a horizontal `UILayout` |
| On-screen controls | `UIButton {key}`, `UIJoystick` |
| World-space marker (name tag, damage number) | not UI: a `Sprite` or a `UIText` moved by a script each frame |

Not available yet: radial fill (round cooldown), drag-and-drop helpers.

Every widget above exists in the three engines that were compared, and none of the components duplicates another:
`UIPanel` is the container (clip, modal, scroll view), `UIImage` the picture (sheet frames, fill, aspect).

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
Android and web players report every active finger. Web fingers retain DOM
identifiers and press order, and `began` is true only for their first simulation
step. The first finger also acts as MouseLeft; releasing another finger cannot
release that mouse press. See [TOUCH.md](TOUCH.md) for cancellation/focus rules
and its open items.

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

## Status

- [ ] Web: the `PlatformWeb.cpp` text path (KeyboardEvent.key -> `InputState::text`) compiles and `runtime/web` was
      rebuilt, but it was not run: package a project with a `UIInput`, `oe serve` it and type in a browser. Phones in
      a browser get no on-screen keyboard (needs a focused hidden `<input>` element); the clipboard is internal.
- [ ] Android: `Window::SetTextInput` shows/hides the soft keyboard through JNI (`InputMethodManager`, fallback
      `ANativeActivity_showSoftInput`) and `KeyCharacterMap` turns key events into text. Compiled into
      `runtime/android`, **not run on a device or emulator**: check that the keyboard appears, that letters arrive,
      Backspace (`AKEYCODE_DEL`) and that Back closes the keyboard. Composed text (Korean/Japanese IME, swipe typing)
      needs a Java `InputConnection` class in the APK (the packager writes no dex today); the clipboard is internal.
- [ ] `UIInput`: IME composition preview (Windows shows only committed text), word-wise movement (Ctrl+arrows),
      double-click to select a word, undo.
- [ ] `UIImage` radial fill (needs a shader change in both renderers and `sokol-shdc`).
- [ ] Not verified: the new widgets in the GPU renderer and in the editor inspector / Add menu (`oe editor --screenshot`),
      `UIJoystick` axes inside a lockstep/rollback match.
