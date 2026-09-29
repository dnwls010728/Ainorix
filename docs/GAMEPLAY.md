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

## Messages and timers

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

Screen-space components, laid out on a **1280×720 reference canvas** scaled to the screen height:

| Component | Fields |
|---|---|
| `UIText` | `text` (ASCII; `\n` for lines), `size` (line height), `color` |
| `UIPanel` | `width`, `height`, `color`, `opacity` |
| `UIButton` | `text`, `width`, `height`, `size`, `color`, `textColor`; a click calls `onClick(self)` on the entity's Script |
| all | `anchor` (top-left … bottom-right, center), `x`, `y` offsets in reference px (+y down), `visible`, `order` |

The anchor is both the screen point and the element's pivot: `anchor = "bottom-right", x = -20, y = -20` puts the element's bottom-right corner 20 px from the screen corner.

UI is drawn in the game view (scene camera) and skipped for free cameras (editor scene view, `render.screenshot {camera: ...}`), unless `render.screenshot {ui: true}`. The built-in font is a 5×7 pixel font; non-ASCII text (e.g. Korean) shows `?` until TTF support lands.

Clicking from tools: take a screenshot, then `input.click {x, y}` with the pixel coordinates **of that screenshot** (default 640×360; pass `width`/`height` for other sizes). The result says which button is under the point; the click is delivered on the next `sim.step`. `input.mouse` moves/presses without clicking. Scripts read `input.mouse()` (normalized 0..1) and `input.pressed("MouseLeft")`. In the editor, clicking the Game view while simulating sends the same `input.click`.

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
2. UI button hit test (clicks from this frame)
3. Built-in systems (Rotator, Velocity, PlayerController)
4. Physics step, then `onCollision*` / `onTrigger*`
5. `onClick` for the clicked button
6. Pending scene change
7. AudioSource updates, audio mix
