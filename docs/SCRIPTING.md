# Lua scripting

Gameplay logic is written in Lua 5.4 (vendored in `third_party/lua`) and attached to entities with the `Script` component. Scripts only run while simulating (`sim.play`, `sim.step`, `oe run`, `oe render --frames N`).

## A script file

A script is a project file that **returns a table**. The engine creates one instance per entity; the instance's methods are looked up in that table.

```lua
-- scripts/spinner.lua
local Spinner = {}

function Spinner:onStart()               -- once, before the first update
  self.speed = self.params.speed or 90   -- params come from the Script component
end

function Spinner:onUpdate(dt)            -- every fixed step (dt = 1/60 s)
  self:rotate(0, self.speed * dt, 0)
end

function Spinner:onDestroy()             -- when the Script is removed/disabled
end

-- Physics callbacks (see docs/PHYSICS.md): other = the other entity's id
function Spinner:onCollisionEnter(other) end
function Spinner:onCollisionExit(other) end
function Spinner:onTriggerEnter(other) end
function Spinner:onTriggerExit(other) end
function Spinner:onClick() end               -- UIButton on the same entity was clicked
function Spinner:onPointerEnter() end        -- pointer moved onto this UIButton / UISlider
function Spinner:onPointerExit() end
function Spinner:onValueChanged(value) end   -- UISlider dragged (see docs/UI.md)

return Spinner
```

Attach it:

```bash
oe exec my_game component.add '{"id":"Cube","type":"Script","values":{"path":"scripts/spinner.lua","params":{"speed":180}}}' --save
```

Instance fields: `self.id` (entity id), `self.name`, `self.params` (the component's JSON params as a table). Anything you store on `self` persists for the play session.

### Declaring params

Read each param once with a literal default, and tools understand your script: `script.params` (and the native editor's inspector, which shows typed fields for them) finds

```lua
function Target:onStart()
  local p = self.params                 -- an alias works too (until the next function)
  self.move = p.move or "none"          -- string; compared below, so it becomes a choice
  self.distance = p.distance or 2       -- number
  self.spin = self.params.spin or {0, 45, 0}   -- vec3
  self.sensitivity = p.sensitivity or 0.12     -- degrees per pixel   <- the comment becomes the tooltip
end

function Target:onUpdate(dt)
  if self.move == "strafe" then ... elseif self.move == "bob" then ... end
end
```

`script.params {path}` -> `[{name, type, default, options?, description?, line}]` with types `number`, `boolean`, `string`, `vec3`, `array`, `object` (no default: `any`). Strings compared with two or more literals (`self.move == "bob"`) get `options`; strings like `"#ff8800"` edit as a color in the inspector.

## Engine API available to scripts

| Function | Description |
|---|---|
| `scene.get(id, type)` | Component as a table, or `nil`. Vec3 fields are `{x,y,z}`, colors `{r,g,b}` |
| `scene.set(id, type, values)` | Partial update: `scene.set(id, "Transform", {position = {y = 2}})` keeps x and z |
| `scene.add(id, type, values?)` / `scene.remove(id, type)` / `scene.has(id, type)` | Component management |
| `scene.create(name?, components?)` | New entity id. `components` like the `entity.create` command |
| `scene.destroy(id)` | Destroys the entity and its children |
| `scene.find(name)` | Entity id or `nil` |
| `scene.exists(id)`, `scene.name(id)`, `scene.parent(id)` | Entity info |
| `scene.all(type?)` | Array of entity ids (optionally only those with a component) |
| `scene.withTag(tag)` | Array of entity ids whose `Tag` contains `tag` |
| `input.down(key)` / `input.pressed(key)` | Key held / pressed this frame (`"W"`, `"Space"`, `"Left"`, …) |
| `time.dt()`, `time.frame()`, `time.now()` | Step length, frame number, simulated seconds |
| `log.info(...)`, `log.warn(...)`, `log.error(...)`, `print(...)` | Engine log (`log.get`) |
| `physics.raycast(origin, dir, maxDist?)` | `{entity, point, normal, distance}` or `nil` |
| `physics.overlapSphere(center, radius)` / `physics.contacts(id)` | Entity ids |
| `physics.addImpulse(id, {x,y,z})` | Push a dynamic body |
| `tilemap.get(id, col, row)` / `tilemap.set(id, col, row, ch)` / `tilemap.fill(id, col, row, w, h, ch)` | Read/change Tilemap cells (row 0 = top); changes update graphics, autotiles and collision |
| `tilemap.solid(id, col, row)` / `tilemap.collision(id, col, row)` / `tilemap.size(id)` | Cell collides? / `"none"`, `"solid"`, `"oneway"`, `"shape"` / width, height in cells |
| `tilemap.cellAt(id, {x,y,z})` / `tilemap.cellCenter(id, col, row)` | World point → `col, row`; cell → world position (see [2D.md](2D.md)) |
| `scene.instantiate(path, {position, name, parent})` | Spawn a prefab, returns the root id |
| `scene.send(id, method, ...)` / `scene.broadcast(method, ...)` | Call methods on other scripts |
| `timer.after(s, fn)` / `timer.every(s, fn)` / `timer.cancel(id)` | Timers on simulated time |
| `game.set(k, v)` / `game.get(k)` / `game.loadScene(path)` / `game.scene()` | Cross-scene data and scene changes |
| `audio.play(path, {volume, pitch, loop})` / `audio.stop(id)` / `audio.stopAll()` | Sound |
| `input.mouse()` | Mouse position in the game view (0..1) |
| `input.touches()` | Every finger on a touch screen: `{ {id=, x=, y=, began=}, ... }` (`x, y` normalized like `input.mouse()`, `began` = put down this step). The first finger also acts as the mouse. `UIButton.key` turns buttons into on-screen keys (docs/UI.md) |
| `input.mouseDelta()` | Relative mouse motion in pixels since the last step (`dx, dy`; `dy` > 0 = down) — for mouse look |
| `input.lockMouse(on?)` / `input.mouseLocked()` | Capture the mouse for mouse look: hidden cursor kept in the view (Windows), Pointer Lock (web, editor Game view). **Escape** releases it and the next click captures it again (that click is not passed to the game). Where the browser refuses pointer lock, clicks go through and `input.mouse()` keeps working |
| `draw.line(a, b, color?, s?)` / `draw.box(c, size, color?, s?)` / `draw.sphere(c, r, color?, s?)` | Debug lines (default: this frame only) |
| `require("lib.util")` | Loads `lib/util.lua` from the project once per session |

Instance helpers (from the built-in base class): `self:get(type)`, `self:set(type, values)`, `self:add`, `self:has`, `self:remove`, `self:destroy()`, `self:position()`, `self:setPosition(x, y, z)`, `self:translate(x, y, z)`, `self:rotate(x, y, z)`, and for physics (CharacterBody, CharacterBody2D, RigidBody or RigidBody2D) `self:grounded()`, `self:velocity()`, `self:setVelocity(x, y, z)`, `self:addImpulse(x, y, z)`, `self:contacts()`.

`scripts/rotator.lua` and `scripts/player_controller.lua` in every new project are line-by-line Lua ports of the built-in `Rotator` and `PlayerController` components; tests check they behave identically.

## Sandbox and determinism

- Available libraries: base, `string`, `table`, `math`, `utf8`, `coroutine`. Not available: `io`, `os`, `debug`, `package`, `load`, `loadfile`, `dofile`.
- Every callback has an instruction budget (20M instructions). An infinite loop becomes a script error instead of freezing the engine.
- `math.random` is seeded with 0 and string hashing uses a fixed seed, so `pairs()` order and random numbers are the same on every run. The Lua state is recreated for every play session (`sim.stop` discards it).

## Errors

A failing callback stops that entity's script (reported once, not every frame) until the file is fixed. Errors carry `file:line`, the entity id and the frame:

```bash
oe exec my_game script.errors
```

They are also written to the log (`log.get`, the editor console) and counted in `sim.state.scriptErrors`.

Check a script without running it:

```bash
oe exec my_game script.check '{"path":"scripts/player.lua"}'
# {"ok": false, "errors": 1, "warnings": 1, "diagnostics": [
#   {"line": 12, "severity": "error", "message": "'end' expected (to close 'function' at line 5) near <eof>"},
#   {"line": 8, "severity": "warning", "message": "unknown global 'lgo' - typo, or a missing 'local'?"}]}
```

Errors are syntax errors (the file does not compile). Warnings come from the compiled code: assigning a global (every script shares globals - usually a missing `local`) and reading a global that is neither part of the sandbox API nor assigned in the file (usually a typo). Pass `source` to check unsaved text; the native editor does this while you type.

## Hot reload

Saving a script file while simulating reloads it within half a second (and before every `sim.step`). Running instances keep their `self` state and pick up the new functions; a faulted script resumes. `script.reload` forces a reload.
`script.write` forces the written module to reload immediately, even when consecutive writes have the same filesystem timestamp.

## Commands for agents

| Command | Use |
|---|---|
| `script.write {path, source}` | Create/overwrite a `.lua` file (works over MCP without file access) |
| `script.read {path}` | Read a script |
| `script.list` | Script files, the entities using them, running instances |
| `script.eval {code, entity?}` | Run Lua now. `"scene.find('Player')"` returns the id; with `entity`, `self` is that entity's live instance (e.g. `"self.speed"`) |
| `script.errors {clear?}` | Error list |
| `script.check {path or source}` | Syntax errors and global-variable warnings with line numbers, without running anything |
| `script.params {path or source}` | Params the script reads (`self.params.x or default`): name, type, default, options, description |
| `script.reload` | Reload all modules |

Typical loop: `script.write` → `script.check` → `component.add Script` → `sim.step {frames: 60}` → `script.errors` → `render.screenshot` → fix → repeat.
