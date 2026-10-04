# Gameplay building blocks

Prefabs, scene changes, messages/timers, in-game UI and audio. Everything here is reachable from Lua scripts **and** from the command API, so an agent can build and play-test a game without a window. The `oe new` template is a complete example: a two-level coin game (`templates/default/`).

## Prefabs

A prefab is an entity tree saved as `prefabs/*.prefab.json` (format `ownengine.prefab`, same entity layout as a scene). Instances are copies; their root gets a `Prefab {path}` component recording where they came from.

| | |
|---|---|
| Save | `prefab.create {id, path}` |
| Place | `prefab.instantiate {path, name?, parent?, position?}` |
| List | `prefab.list` (files + instances in the scene) |
| Lua | `scene.instantiate("prefabs/coin.prefab.json", {position = {x = 1, y = 0.6, z = 0}, name = "Coin 9"})` → root id |

## Scene changes and game data

`game.loadScene(path)` (Lua) or `game.load_scene {path}` (API) switches scene at the end of the current frame. The Lua state, timers and **game data** survive; entity scripts restart (`onStart` runs again), physics is rebuilt, sounds owned by entities stop. `sim.stop` always returns to the scene you started from.

Game data is a JSON object for cross-scene state (score, lives, unlocked levels): `game.set(key, value)`, `game.get(key)`, `game.get()` (all). Tools read it with `game.state` (also shows the running scene).

## Pausing the game

`game.pause(true)` (Lua) or `game.pause {paused}` (API) freezes the world for a pause menu, a level-up choice or a cutscene
dialog: physics, built-in systems, particles, timers and `time.now()` stand still, while scripts keep receiving `onUpdate`
with `dt = 0` and the UI stays clickable — so a dialog script can read keys and call `game.pause(false)`. Scripts that move
things with `dt` stop by themselves; a script that must do nothing while paused returns early when `dt == 0`. Count frames
(`time.frame()`) for animations that should run during a pause. `sim.state.gamePaused` shows the state; a scene change or
`sim.stop` clears it. Network matches should not use it (it is local state, not part of the synchronized simulation).

`time.date()` gives the calendar day for daily rewards; it is not simulation state, and `time.date {set: "2030-01-31"}` pins it in tests.

## Messages and timers

Progress between launches uses `save.get/set/delete/flush`, independently of game
session data and scene undo. Tool sessions default to memory; `--save-dir <dir>`
opts into JSON files. Slots, corruption handling and implementation status:
[SAVE.md](SAVE.md).

```lua
scene.send(id, "takeDamage", 10)      -- calls takeDamage(self, 10) on that entity's script, returns its result
scene.broadcast("onCoinCollected", coinId)   -- every script that has the method; returns how many were called
self:send(other, "open")

local t = timer.after(1.5, function() game.loadScene("scenes/level2.scene.json") end)
local tick = timer.every(0.25, function() ... end)
timer.cancel(tick)
```

Timers run on simulated time at the start of each frame, in creation order (deterministic). Errors inside a message or timer are reported in `script.errors`; a failing target script is stopped, not the sender.

## In-game UI

Screen-space components laid out on a **1280×720 reference canvas** scaled to the screen: `UIText` (TrueType fonts, any language, wrap, outline, rich text), `UIPanel` (rounded, bordered, clipping container), `UIButton`, `UIImage` (9-slice, fill bars), `UISlider` (slider or progress bar), `UILayout` (column/row/grid), `UICanvas` (scaling). Elements nest: a UI entity's rectangle is inside its parent UI entity. Full reference: [UI.md](UI.md).

| Component | Main fields |
|---|---|
| `UIText` | `text` (UTF-8, `\n`, `<color=..>`/`<b>`), `font` (`default`, `pixel`, `assets/fonts/x.ttf`), `size`, `color`, `width` (wrap box), `align`, `outlineWidth`, `shadowDistance` |
| `UIPanel` | `width`, `height`, `color`, `opacity`, `radius`, `borderWidth`, `clip` |
| `UIButton` | `text`, `width`, `height`, `size`, `color`, `textColor`, `radius`, `interactable`; a click calls `onClick(self)` on the entity's Script |
| `UIImage` | `texture`, `color`, `frame`/`columns`, `slice`, `fill`, `preserveAspect` |
| `UISlider` | `value`, `min`, `max`, `step`, `interactable`; dragging calls `onValueChanged(self, value)` |
| all | `anchor` (9 points + `stretch*`), `x`, `y` offsets in reference px (+y down), `width`, `height`, `opacity`, `visible`, `order` |

The anchor is both the point of the parent (or screen) and the element's pivot: `anchor = "bottom-right", x = -20, y = -20` puts the element's bottom-right corner 20 px from the corner.

UI is drawn in the game view (scene camera) and skipped for free cameras (editor scene view, `render.screenshot {camera: ...}`), unless `render.screenshot {ui: true}`. `ui.layout` lists every element's pixel rectangle for a screen size.

Clicking from tools: take a screenshot, then `input.click {x, y}` with the pixel coordinates **of that screenshot** (default 640×360; pass `width`/`height` for other sizes). The result says which button is under the point; the click is delivered on the next `sim.step`. `input.mouse` moves/presses without clicking, adds relative motion for mouse look (`{dx, dy}`, read by scripts with `input.mouseDelta()` on the next step) and sets the mouse lock (`{locked}`; `sim.state.mouseLocked` shows it). Scripts read `input.mouse()` (normalized 0..1) and `input.pressed("MouseLeft")`. In the editor, clicking the Game view while simulating sends the same `input.click`.

## Audio

48 kHz stereo software mixer driven by the simulation: each simulated frame mixes exactly 800 frames, so audio is deterministic and can be verified without speakers.

| | |
|---|---|
| One-shot from Lua | `audio.play("sounds/coin.wav", {volume = 0.8, pitch = 1.2})` → voice id; `audio.stop(id)`, `audio.stopAll()` |
| Music / ambience | `AudioSource {clip, volume, pitch, loop, playOnStart}` component — stops when the entity or component goes away |
| Make sounds | `audio.generate {path, preset}` — presets `coin, jump, hit, explosion, powerup, blip, success` (new projects get coin/success/jump) |
| Check | `audio.state` — playing voices and every sound played this session with its frame number |
| Measure | `audio.capture {action:"start"}` … `sim.step` … `audio.capture {action:"stop", path?}` → seconds, peak, RMS, audible seconds (+ WAV) |

WAV files: PCM 8/16/24/32-bit or float, mono/stereo, any sample rate (resampled). Speakers are used by `oe run` and `oe editor` (`--mute` to disable); headless modes (`exec`, `script`, `mcp`, tests) mix without output.

## Frame order (reference)

1. Timers, then scripts `onStart` / `onUpdate` (entity id order)
2. UI pointer: hover, press, slider drags (from this frame's mouse)
3. Built-in systems (Animator, ParticleEmitter, SpriteAnimation)
4. Physics step, then `onCollision*` / `onTrigger*`
5. UI callbacks: `onPointerExit` / `onPointerEnter`, `onClick`, `onValueChanged`
6. Pending scene change
7. AudioSource updates, audio mix
