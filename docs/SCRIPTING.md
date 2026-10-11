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
function Spinner:onToggle(on) end            -- UIButton {toggle} changed its `on` state
function Spinner:onPointerEnter() end        -- pointer moved onto this UIButton / UISlider
function Spinner:onPointerExit() end
function Spinner:onValueChanged(value) end   -- UISlider dragged, UIDropdown option chosen (see docs/UI.md)
function Spinner:onTextChanged(text) end     -- UIInput text edited
function Spinner:onSubmit(text) end          -- Enter pressed in a UIInput

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
| `animation.play(id, clip, options?)` | Play a glTF clip on MeshRenderer; add Animator if needed. options: speed, loop, restart (default true). Returns playback state; invalid clips raise a Lua error. Attached scripts can call `self:play(clip, options)`. See [ANIMATION.md](ANIMATION.md) |
| `vfx.spawn(path, options?)` | Create a linked .vfx.json effect; returns owner id. Options: name, parent, position, params, autoDestroy (default true). See [VFX.md](VFX.md). |
| `vfx.play(id, {restart=true}?)`, `vfx.stop(id, {kill=false}?)` | Restart/resume emission or stop and drain; kill empties particles/trails. |
| `vfx.burst(id, countOrOptions?)`, `vfx.clear(id, options?)` | Accepted manual births, capped per emitter/system; options may include emitter and count. Clear keeps clocks/RNG when restart is omitted; restart=true/false resets clocks/counters/RNG and rearms/consumes startup births. Playback is retained. |
| `vfx.set(id, name, value)`, `vfx.state(id, options?)` | Validated transient parameter override; effective params, playback/completion/error, counts, budget and named block statistics; {particles=true} adds detailed per-emitter snapshots. Controls preserve authored scene JSON and native snapshot state. |
| `trail.clear(id)` | Forget a Trail's path, e.g. after teleporting the entity, so no ribbon joins the old and new place. |
| `scene.set(id, type, values)` | Partial update: `scene.set(id, "Transform", {position = {y = 2}})` keeps x and z |
| `scene.add(id, type, values?)` / `scene.remove(id, type)` / `scene.has(id, type)` | Component management |
| `scene.create(name?, components?)` | New entity id. `components` like the `entity.create` command |
| `scene.destroy(id)` | Destroys the entity and its children |
| `scene.find(name)` | Entity id or `nil` |
| `scene.exists(id)`, `scene.name(id)`, `scene.parent(id)` | Entity info |
| `scene.epoch()` | Read-only opaque 16-digit lowercase hexadecimal world token. Compare for equality only. Changes on Clear/scene replacement, stays unchanged on ordinary create/destroy, and restores with native snapshots. Not serialized in scene JSON or usable as a portable/network identity. Handles must also belong to the current Lua session and check `scene.exists`; VM resets may leave the epoch unchanged. |
| `scene.children(id)` / `scene.child(id, name)` | Direct children in id order / the child with that name or `nil` (parts of a prefab: body sprite, light, sensors) |
| `scene.all(type?)` | Array of entity ids (optionally only those with a component) |
| `scene.withTag(tag)` | Array of entity ids whose `Tag` contains `tag` |
| `input.down(key)` / `input.pressed(key)` | Key held / pressed this frame: letters `"A"`..`"Z"`, digits `"0"`..`"9"`, `"Space"`, `"Left"`/`"Right"`/`"Up"`/`"Down"`, `"Shift"`, `"Control"`, `"Escape"`, `"Enter"`, `"Tab"`, `"Backspace"`, `"Delete"`, `"Home"`, `"End"`, `"LeftBracket"`, `"RightBracket"`, gamepad buttons (`"GamepadA"`, …) |
| `input.axis(name)` | Gamepad stick LeftX/LeftY/RightX/RightY (-1..1; +Y up) or LT/RT (0..1), with a 0.15 scalar dead zone rescaled to full range. Buttons use `input.down/pressed("GamepadA")` etc.; see [INPUT.md](INPUT.md) |
| `time.dt()`, `time.frame()`, `time.now()` | Step length (0 while the game is paused), frame number, simulated seconds (they wait during a pause) |
| `time.date()` | Calendar date `"YYYY-MM-DD"` in local time, for features outside the simulation (daily rewards). Tools pin it with `time.date {set}` |
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
| `game.quit()` | Asks the game window to close after this frame (`oe run`, the packaged player; a web page stops its frame loop and keeps the last picture). Tools and the editor do not close: they report `game.state.quitRequested`. Cleared by `sim.stop` |
| `game.arg(name)` | A launch parameter of this process (`oe run`/`serve-game ... --set name=value`, `--set-file name=path` for a file's text, the packaged player takes the same flags), or `nil`. Always a string. For what differs between processes running the same project: which channel a server is, where a service listens. Survives `sim.stop` and match starts; tools use `game.args {set}` |
| `game.pause(on?)` / `game.paused()` | Gameplay pause for menus and dialogs: scripts and UI keep running with `dt = 0`; physics, built-in systems, timers, particles and simulated time wait. Cleared by scene changes and `sim.stop` |
| `host.call(name, data?)` | Asks the application that embeds the player (the web page's `window.OEHost`, later a store SDK layer) for something the engine does not do: an ad, a purchase. Returns `true` when the host handles `name`, `false` when there is none - then use your own fallback. See [GAMEPLAY.md](GAMEPLAY.md#host-application-ads-purchases) |
| `host.poll()` | Messages from the host since the last call: `{ {name=, data=}, ... }` (the ad result, a finished purchase). Call it every frame while waiting |
| `host.language()` | The user's language as an ISO 639-1 code (`"en"`, `"ko"`, ...; `"en"` when unknown) for the first-launch language |
| `save.get(key, default?, slot?)` / `save.set(key, value, slot?)` | Save slots; finite JSON values only. Default slot is `default` |
| `save.delete(key, slot?)` / `save.flush(slot?)` | Remove a key / persist pending changes. Tools default to memory; `--save-dir` enables files ([SAVE.md](SAVE.md)) |
| `audio.play(path, {volume, pitch, loop})` / `audio.stop(id)` / `audio.stopAll()` | Sound |
| `input.mouse()` | Mouse position in the game view (0..1) |
| `ui.focus(id?)` / `ui.focused()` | Keyboard/gamepad focus of the UI (`UICanvas {navigation: true}`): focus a button or slider (no argument clears it), read the focused entity id or `nil`. See [UI.md](UI.md) |
| `camera.worldToScreen({camera, position, view?})` | Project a world point with an explicit Camera entity/id/name. Returns logical pixel x/y, forward depth, ndcDepth, visible and status; no clamping. See coordinate queries below |
| `camera.screenRay({camera, screen, view?})` | Ray from the near plane through a logical screen point: origin and normalized direction in world space |
| `camera.screenToPlane({camera, screen, plane, view?})` | Ray/plane intersection: hit + point/distance, or hit=false + reason parallel/backward |
| `ui.screenToCanvas({screen, canvas?, view?})` | Logical screen pixels to effective canvas reference coordinates; returns x/y, width/height, scale and canvas ID (or nil for fallback). Also forwarded as require("oe.ui").screenToCanvas |
| `ui.measureText({text, style?, wrapWidth?, view?, canvas?})` | Actual text layout {width,height,lines,lineWidths,scale} in reference pixels at effective local view/canvas scale; rich/fallback/wrap/spacing/bold supported; paint/fitting excluded. Forwarded by oe.ui |
| `input.viewSize()` | Game view size in pixels (`width, height`): the window, or in headless sessions the project's `window` size until a tool passes another one (`input.click {width, height}`). For layouts that adapt to the screen shape (portrait phones, tablets): `local w, h = input.viewSize()` |
| `input.touches()` | Every finger on a touch screen: `{ {id=, x=, y=, began=}, ... }` (`x, y` normalized like `input.mouse()`, `began` = put down this step). The first finger also acts as the mouse. `UIButton.key` turns buttons into on-screen keys (docs/UI.md) |
| `input.wheel()` | Mouse wheel notches this step (> 0 = away from the user); `UIScroll` panels use it by themselves |
| `tween.to(id, type, values, seconds, opts?)` | Animates numeric, vector (`{x=,y=,z=}` or `{1,2,3}`) and colour fields of a component from their current to the given values. opts: `ease` (`linear`, `inQuad`, `outQuad`, `inOutQuad`, `inCubic`, `outCubic` (default), `outBack`, `outElastic`), `delay`, `loop` (`"loop"` / `"pingpong"`), `scaled` (true = waits while the game is paused; default false, so UI keeps moving), `onDone` (function). Returns a handle |
| `tween.from(id, type, values, seconds, opts?)` / `tween.cancel(handle)` / `tween.stop(id, type?)` | Start at `values` and settle on the current ones / stop one tween / stop every tween of an entity (or of one of its components) |
| `input.mouseDelta()` | Relative mouse motion in pixels since the last step (`dx, dy`; `dy` > 0 = down) — for mouse look |
| `input.lockMouse(on?)` / `input.mouseLocked()` | Capture the mouse for mouse look: hidden cursor kept in the view (Windows), Pointer Lock (web, editor Game view). **Escape** releases it and the next click captures it again (that click is not passed to the game). Where the browser refuses pointer lock, clicks go through and `input.mouse()` keeps working |
| `draw.line(a, b, color?, s?)` / `draw.box(c, size, color?, s?)` / `draw.sphere(c, r, color?, s?)` | Debug lines (default: this frame only) |
| `require("lib.util")` | Loads `lib/util.lua` from the project once per session; `require("oe.ui")` and `require("oe.entities")` use embedded official sources |

Instance helpers (from the built-in base class): `self:get(type)`, `self:set(type, values)`, `self:add`, `self:has`, `self:remove`, `self:destroy()`, `self:position()`, `self:setPosition(x, y, z)`, `self:translate(x, y, z)`, `self:rotate(x, y, z)`, and for physics (CharacterBody, CharacterBody2D, RigidBody or RigidBody2D) `self:grounded()`, `self:velocity()`, `self:setVelocity(x, y, z)`, `self:addImpulse(x, y, z)`, `self:contacts()`.

`scripts/rotator.lua` and `scripts/player_controller.lua` in every new project are the implementation of rotating and WASD/arrows + Space movement (there are no built-in equivalents); tests `TemplateRotatorScript`, `TemplatePlayerScript` and `TemplatePlayerScriptWithCharacterBody` cover them. For constant linear motion call `self:translate(x*dt, y*dt, z*dt)`.

## Coordinate queries (M2a)

All four functions and same-named registry commands share one implementation. Commands
are read-only. Requests are named tables; reuse the request table for many labels in a frame.
World points and plane point/normal are {x,y,z}; screen points are {x,y}; view is
{width,height}, integer logical game-view pixels in 1..16384. Screen origin is top-left.
A camera argument is mandatory and must resolve to an entity with Camera. Perspective,
orthographic and the full parent/world camera transform use the renderer's exact matrices.
Omitting view reads input.viewSize() (headless: project window, default 1280x720 until
explicit input view changes). No hidden active-camera selection occurs.

worldToScreen returns {x,y,depth,ndcDepth,visible,status}. depth is forward world distance;
ndcDepth is the clip-space -1..1 depth. visible requires positive depth and membership in all
three clip axes. status is visible, outside (including near/far clipping), or behind
(depth <= 0). Pixel positions remain unclamped even outside/behind; at undefined homogeneous
projection (abs(w) <= 1e-6), x/y/ndcDepth are nil (JSON null), with depth/status retained.
screenRay returns {origin={x,y,z},direction={x,y,z}}; origin is on the near plane.
screenToPlane takes plane={point={x,y,z},normal={x,y,z}}. It returns
{hit=true,point={x,y,z},distance} or {hit=false,reason="parallel"|"backward"}, with no
point/distance on misses. It never returns a whole-query nil. Parallel means normalized
abs(dot) <= 1e-6; backward means signed ray distance < -1e-5. Smaller negative distances
snap to zero. Invalid/missing entities, malformed or nonfinite values, zero normals, invalid
view sizes and degenerate camera matrices raise Lua errors / command invalid_query errors.
Registry schema failures (missing/unknown arguments) retain standard registry errors.

screenToCanvas follows UI.cpp's existing first-UICanvas-by-ID scale; omitted canvas uses
that entity, or height-720 fallback if absent. An explicit canvas must be that same effective
entity; independently rendered multiple canvases are outside scope. Returned x/y are top-left
reference pixels; width/height describe the view in those units, not a centered anchor offset.
The unchanged global ui.focus/focused primitives coexist with ui.screenToCanvas; oe.ui forwards
it without altering the global. Text measurement is implemented separately below (M2b).

Validation tolerances: 0.05 logical pixels for projection, 1e-3 world units for ray round trips,
and 1e-5 for normalized directions. Queries do not modify scene/game data, clocks, RNG,
callbacks, or native recording; they require no simulation tick in an editor/tool session.

> Camera projection, screen/canvas conversion, UI layout, and text measurement are
> presentation-only queries. Their results may depend on local view dimensions, canvas scale,
> font resources, and effective raster size. Use them for local HUD placement and visual
> layout. Do not use these results to branch simulation rules, select authoritative outcomes,
> seed randomness, determine physical movement, or change replicated state. For local pointer
> picking, transmit an explicit declared action or validated world-space intent through the
> supported input/RPC path; the server validates it against authoritative state. Recording a
> local result alone does not make it deterministic across peers. Any future simulation use
> requires a separate fully specified, shared-input query contract.

## Text layout measurement (M2b)

ui.measureText uses the same ParseRich/ResolveFont/Shape path as UIText rendering;
its same-named registry command is read-only. Request text is required. Optional style
contains font (default "default"), size (32), letterSpacing (0), lineSpacing (1), bold
(false) and rich (true). wrapWidth is reference pixels: omitted/zero disables wrapping.
Optional view and canvas follow the coordinate query contract above. TTF sizes round via
max(1,lround(size*scale)); pixel-font units remain fractional. Width/height/lineWidths
are shaped at that effective scale, then divided by scale. lines is the number of lines.
An empty string has one empty line, width zero and the renderer's primary-font ink height.

This is layout size, not painted glyph bounds. Outlines, shadows and typewriter reveal
are excluded; no paintedBounds or fitting option is provided. For already auto-shrunk text,
pass the resolved size. Missing/unloadable project fonts and unsupported glyphs use exactly
the existing renderer fallback policy; measurement does not add multilingual font coverage.
Project font paths must stay inside the project. Very long strings are bounded to 65536
UTF-8 bytes; size 0.125..1024 and effective size <=1024, abs(letterSpacing)<=1024,
lineSpacing >0 and <=16, wrapWidth 0..65536. Wrong types, unknown style fields, invalid
view/canvas and bounds violations raise Lua errors / command invalid_query (standard
registry schema and project-path errors are preserved). Valid empty text returns a result, never nil.

Reuse request tables if measuring many labels. Existing font/glyph caches may warm; no
measurement-result cache is added. Caller-owned Lua cache keys must include text, font
resource revision, size/spacing/bold/rich, wrapWidth and effective scale; invalidate on
view/canvas/font changes and keep cache state in the snapshot-compatible Lua heap.
Outline/shadow/typewriter should not alter those layout keys. A message can outlive a
view resize: remeasure visible speech bubbles at the current scale.
Measurements are presentation-only under the exact quoted rule above; cached local results
do not acquire an authoritative or peer-deterministic simulation contract.

## Official Lua modules

Official modules are embedded in the engine/player. They need no project Lua files and cannot
be overridden by a project `oe/` directory. `require("oe.ui")`, `require("oe/ui")`, and
`require("oe/ui.lua")` return the same cached export. Segments must match `[a-z][a-z0-9_]*`;
use either dots or forward slashes, with `.lua` only on slash imports. Uppercase, backslashes,
mixed separators, traversal, leading `./`, absolute/drive paths, and `oe.ui.lua` are rejected.
The `oe` root is reserved, including case/path variants; missing official modules never fall
back to project files. Nonreserved imports, including `scripts.oe.ui`, retain project behavior.
Imports load text only, preserve the instruction budget, diagnose official import cycles, and
clear failed official cache entries for retry. A nil export becomes true; false is cached too.

Source tools use exactly `@oe/entities.lua` and `@oe/ui.lua` (lowercase, forward slashes).
`script.list` adds entries with `origin: "embedded"`, `readOnly: true`, and reports conflicting
project paths separately in `reservedConflicts`. `script.read/check/params` resolve these paths
without running modules in the game's Lua VM; inline source still takes precedence, with a
canonical diagnostic path. Official source writes are rejected. Checking compiles source and
warns about globals; it does not check exported methods or execute/validate imports.

```lua
local entities = require("oe.entities")
local widgets = require("oe.ui")
local player = entities.find("Player", "Transform")
player:set("Transform", { position = { y = 2 } }) -- partial update keeps x/z
local optional = widgets.tryFind("Optional label", "UIText") -- nil if absent or wrong capability
local label = widgets.find("Score label", "UIText")          -- strict; descriptive error if missing
label:set("UIText", { text = "Score: 10" })
widgets.focus(widgets.find("Chat", "UIInput"):id())
```

| Module operation | Contract |
|---|---|
| entities.find(name, capability?) / tryFind | Entity handle; strict or optional acquisition |
| widgets.find(name, component?) / tryFind | Widget handle; infer exactly one primary UI component, or select it explicitly. Missing entity/component: error for find, nil for tryFind. Ambiguity raises an error in both |
| widgets.list(prefix, count) | Strict numbered widgets prefix..1 through prefix..count, in index order |
| handle:id() | Validated native entity ID; never silently re-find a stale handle |
| entity:get(type) / set(type, values) | Validated component access and partial update |
| widget:get() / set(values) | Access/update the selected UI component; partial fields preserve all other fields |
| widget:get(type) / set(type, values), widgets.get/set(widget,...) | Explicit component access retained for M1a callers; validate requested component and acquisition capability |
| widget:text(line) | UIText, UIButton or UIInput only; unsupported type error identifies widget name and expected text capability |
| widget:show(on?) / hide() / visible() | Show defaults true, false hides; read/change only selected widget visibility. Descendant visibility fields remain untouched |
| widget:focus() | UIButton, UIInput, UISlider or UIDropdown only; validates lifetime/capability then forwards native focus |
| widgets.focus(idOrNameOrNil) / focused() | Forward global ui signatures unchanged; module-level primitive retains its original behavior |

Primary widget components are UIText, UIButton, UIInput, UIPanel, UIImage, UISlider,
UIDropdown and UIJoystick. UICanvas/UILayout/UIMotion/UIScroll are adjunct layout or
behavior components, not automatically selected primary widgets. Multiple primary
components require an explicit component argument. Generic widget:get() exposes the
native fields (including input text, slider value or dropdown selected index); there is
no generic value helper that pretends every widget supports it. Generic set still uses
native field validation. A selected component remains part of the handle's capability.

Hiding a parent widget already suppresses descendant rendering/hit testing through the
native UI hierarchy. This does not rewrite child fields, implement recursive show/hide,
or create a dialog framework. Re-showing the parent preserves individual child visibility.

Handles have private heap-owned records for one Lua session and one scene epoch. Every operation
checks the record, epoch, existence, acquisition capability, and requested component. Deleted
entities, replaced worlds, or lost capabilities raise errors; handles never silently re-find.
Use `tryFind` for optional acquisition, not to disguise stale-handle errors. Native snapshots
restore module cache/records/closures and Scene together; discarded future handles disappear.
Do not serialize handles via game/save/JSON, transfer them between engines/VMs, or retain native
Lua object pointers across restoration. Reacquire at those boundaries. Epoch equality alone does
not authorize a handle. Module exports/metatables must not be mutated; this is a correctness
contract, not protection against Lua rawset. This surface adds no owner cleanup, timers,
dialogs, coordinate/text queries, or input helpers.

Official exports survive attached-script hot reload and Lua scene transitions within a VM;
stale handles are rejected lazily. Reset/stop creates fresh exports and records. Embedded Lua
changes require rebuilding the player. ContentHash always includes the official manifest digest
and primitive-contract revision, even for projects that never import these modules. The existing
v4 match blueprint rejects different embedded sources/revisions in lockstep, rollback, authority,
and authoritative late join. Old v4 runtimes may connect to a lobby but cannot start/join a match
with these players. This fingerprint is compatibility metadata, not cryptographic authentication.
Standalone recording also rejects a changed fingerprint with the existing state_resource error.
Desktop, web and both Android ABI players were rebuilt on 2026-10-06; web import was exercised
in a packaged browser build, while Android device execution remains unverified.

## Official input and math composition (M3)

The embedded oe.input module supplies named project-defined actions without changing
input injection, wire declarations, or the global input table. Define actions in Lua:

```lua
local controls = require("oe.input")
local actions = {
    move = {kind="vector", left={"A","Left"}, right={"D","Right"},
            up={"W","Up"}, down={"S","Down"}, axisX="LeftX", axisY="LeftY"},
    jump = {kind="button", keys={"Space","GamepadA"}},
}
local source = controls.localSource() -- offline; fails during a network session
local move = controls.read(source, actions, "move") -- {x,y,z=0}, +Y up
local jump = controls.read(source, actions, "jump") -- {held,pressed}
```

Aliases in one direction are ORed, opposite directions subtract, processed axes add,
and only vectors longer than 1 are normalized. Smaller analog magnitudes survive.
UIJoystick and a physical pad share axis names; a held UI stick writes those same axes,
so the module does not add a second stick value. The native scalar 0.15 dead zone is
applied once by input.axis/input.player.axis; the module applies none. Digital and
analog directions sum, potentially cancel, and cap to 1. +Y is up; map movement onto
XZ with z=-move.y. Screen/touch Y grows downward. Binding axes are optional; a keyboard-only game can leave them out.

Buttons OR each alias's held and first-frame pressed values independently. Repeated
reads do not consume input. Engine onUpdate keyboard suppression while UIInput is
focused already applies; mouse/gamepad remain active. The same suppression happens
before network sampling. Tool script.eval outside onUpdate is not a suppressed
input callback. Modules do not pause: scripts/UI still run at dt=0 and the gameplay
caller decides whether to move. They do not inspect/consume touches or pointer data.

For network gameplay, explicitly select the entity's player and mirror the project's
network.actions/network.axes declarations (literal engine key names, not named Lua
actions). Only include bound declared names; an undeclared alias fails even if another
alias is held. Example for a game that declares WASD and LeftX/LeftY:

```lua
local declarations = {actions={"W","A","S","D","Space","R"}, axes={"LeftX","LeftY"}}
local actions = {move={kind="vector",left={"A"},right={"D"},up={"W"},down={"S"},
                      axisX="LeftX",axisY="LeftY"}}
local source = controls.playerSource(self:get("NetPlayer").player, declarations)
local move = controls.read(source, actions, "move")
```

playerSource captures native input.player(id) closures and copies allowlists into its
Lua heap closures. It supplies only down/pressed/axis, rejects undeclared reads and
non-running matches (including none/idle/lobby), and never reads device-local mouse/touches/undeclared keys.
Unknown players are neutral. Lockstep/rollback peers have all streams; authoritative
servers have player streams and clients have their own stream (remote is neutral).
localSource refuses any non-none/non-idle session at read time, even if cached before
joining. Lobby UI uses existing global input rather than the gameplay source API.
Neither source guesses a player/role. Declarations remain a project author obligation:
this is an ergonomic boundary, not a security sandbox against arbitrary project Lua
or forged allowlists. Existing global input APIs stay available for local UI.

Tables/actions are caller-owned. Shared modules retain no mutable owner state or
native registry; closures and imported cache are in the recorded Lua heap. Enable
sim.record_state before require/evaluation. A caller may cache its explicit source
on the script instance; no subscriptions or cleanup are needed. Required-only helper
hot reload retains the existing limitation documented in the official module section.

| oe.math operation | Contract |
|---|---|
| clamp(value, low, high) | Inclusive finite bounds; inverted bounds error |
| lerp(a, b, t) | a+(b-a)*t, t is not clamped |
| distance(a, b) | Euclidean length using {x,y,z}; omitted coordinates zero |
| normalize(v) | Fresh unit {x,y,z}, or exact zero for zero vector; finite coordinates required |

The module does not introduce positional-array vectors, RNG/time consumption, mutable
native caches, or scene changes. normalize rescales before squaring to handle very
large finite coordinates. Arithmetic overflow in distance/lerp follows Lua numbers;
use finite representable gameplay ranges.

## Networking (M4–M6)

`net` is available in every script. Without enabled networking, `net.isServer()` is true,
`net.isHost()`/`net.isClient()` false, `net.localPlayer()` is 1 and `net.players()` is `{1}`.
`net.rpc` calls its registered handler immediately in this mode (`others` has no recipient).
No session, socket or network polling is initialized. With networking enabled, M4 supplies
lobbies/RPC, M5 supplies lockstep/native rollback and M6 supplies authoritative replication. See [NETWORK.md](NETWORK.md).

| Lua | Contract |
|---|---|
| `net.state()` / `net.stats()` | Session role/state/error and bounded peer diagnostics |
| `net.isHost()` / `net.isServer()` / `net.isClient()` | Host role, server role and client role |
| `net.localPlayer()` / `net.players()` | Local id (0 before join); sorted array of lobby player ids |
| `net.host({name?, seed?, room?})` | Explicitly host using project settings; seed default 1; loopback room defaults to gameId |
| `net.join({name?, address?, port?})` | Async join: numeric IPv4, browser ws/wss URL or in-process loopback room |
| `net.leave()` / `net.kick({player, reason?})` | Graceful bounded leave or host-only removal |
| `net.ready({ready=true})` | Lobby readiness; does not start synchronized simulation |
| `net.start()` | Ready host begins a lockstep/native rollback/authoritative match; all peers reset at the ready barrier |
| `net.entities()` | Authoritative netId/local-id/owner mappings and received replication fields |
| `net.desync_report()` | First confirmed mismatch with bounded diagnostic scene JSON texts |
| `input.player(id)` | `{down(key), pressed(key), axis(name)}` closures for the merged player input; supports dot/colon calls. Unknown players are neutral; none/player 1 is local |
| `net.on(name, fn)` / `net.on(name, nil)` | Register/replace or remove a handler (at most 64 names) |
| `net.rpc(target, name, ...)` | Reliable RPC: target `server`, `all`, `others`, player id (number/string), or `owner` with an entity id/name as the first payload argument |
| `net.sender()` | Sender player id inside an RPC handler; 0 outside it |
| `net.name(id)` | The name that player joined with (`net.join {name}`), or `nil` for an unknown id. Names are what clients claim, not identities |
| `net.admit(id)` | Server with `network.admission: "script"`: lets that lobby player into the running world once it is ready. Without the call it waits in the lobby (see Authoritative gameplay) |

```lua
function Game:onStart()
  net.on("chat", function(text)
    log.info("player " .. net.sender() .. ": " .. text)
  end)
end
function Game:onPlayerJoined(id) end
function Game:onPlayerLeft(id) end
function Game:onNetState(state) end -- connecting/lobby/leaving/offline/error
-- Works in single-player too:
net.rpc("all", "chat", "hello")
```

Networking is polled at the beginning of each fixed frame. Network callbacks/RPC run after
script updates and before built-in systems, in player/sequence order. Polling continues through
`sim.step`; a paused editor does not advance timeouts/handshakes. Scripts can run while joining;
the host seed is applied when Welcome arrives. Execution in the lobby is independent; net.start aligns the game at frame zero.
The host forwards client RPCs with their verified player id; games must validate sender/arguments
before modifying state. v2 cookies isolate connections; they do not authenticate player identity
or encrypt data. Browser clients connect to native `oe serve-game` with transport websocket, or a compatible binary WebSocket endpoint.

RPCs accept at most 16 JSON-compatible arguments, depth <=8, strings <=1024 bytes,
collections <=64 entries, total serialized size <=8192 bytes (Lua traversal <=512 nodes).
Cycles, nonfinite numbers and functions/userdata are rejected. Local nested RPC is limited to
eight callbacks and shares its instruction budget. Each Lua state holds one handler per name;
scene changes preserve handlers, so replace/remove callbacks that capture old entities.
`sim.stop` clears handlers and networking. None mode has no join/state notifications.

## Links to other services

`link` is framed TCP from the game to something that is not an engine session: a login server, a
lobby or matchmaking service, a database front end. One message on the wire is
`u32 length (of what follows, <= 65536)`, `u16 type`, payload, little endian; what the types mean is
between the game and the service.

| Function | Description |
|---|---|
| `link.open {address=, port=}` | Starts connecting to a numeric IPv4 address (default `127.0.0.1`). Returns a link id, or `nil, message`. Never blocks |
| `link.poll(id)` | `{state=, error=, messages={ {type=, data=}, ... }}`: `state` is `"connecting"`, `"open"` or `"closed"` (then `error` says why), `messages` are those received since the last poll with `data` as a string of bytes. `nil` for an unknown id. Call it every frame while the link matters |
| `link.send(id, type, payload?)` | Queues one message (`type` 0..65535). A string is sent as its bytes, `nil` as an empty payload, anything else as the engine's bounded binary Json. Returns `true`, or `false, message`. Sending while connecting is fine |
| `link.decode(data)` / `link.encode(value)` | Binary Json to a value (`nil` when the bytes are something else) and back. For fixed layouts use `string.pack` / `string.unpack` |
| `link.close(id)` | Closes and forgets the link |

```lua
function Title:onUpdate()
  if not self.login then self.login = link.open {address = "127.0.0.1", port = 7001} end
  local p = link.poll(self.login)
  if p.state == "closed" then self:failed(p.error) end
  for _, m in ipairs(p.messages) do
    if m.type == 2 then self:loggedIn(link.decode(m.data)) end
  end
end
```

Limits: 8 links, 65534 payload bytes, 256 KiB waiting to be sent and 512 unread messages per link
(more closes that link), a connect attempt gives up after 300 frames. Links are device state like
the mouse: they are moved once per frame and are not part of the simulation. They close whenever
the Lua state is reset (`sim.stop`, the start of a match), so a client's links end when it enters a
world. A lockstep or rollback match has none, and a client cannot open one while it predicts an
authoritative match: use them before joining, after the match stopped, or on the server and pass
results by RPC. Browsers have no TCP (`link.open` fails there). Nothing is encrypted. Tools:
`link.open/send/poll/close/state` (open them after the play session began: starting it resets
the Lua state).

## Sandbox and determinism

- Available libraries: base, `string`, `table`, `math`, `utf8`, `coroutine`. Not available: `io`, `os`, `debug`, `package`, `load`, `loadfile`, `dofile`.
- Every callback has an instruction budget (20M instructions). An infinite loop becomes a script error instead of freezing the engine.
- `math.random` is seeded with 0 in single-player or the negotiated host seed in a network lobby; string hashing uses a fixed seed. The Lua state is recreated for every play session (`sim.stop` discards it).

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

The editor polls project file timestamps every 0.5 seconds while playing; headless Step
polls before stepping. Both attached scripts and successfully required project files are
watched. `script.write` forces the written path even when its timestamp is unchanged.
`script.reload` also compares required source bytes, so a preserved timestamp cannot hide
an edit from the explicit command.

Changed required modules and their transitive project importers reload together. Automatic
polling reloads affected attached scripts; the explicit command additionally reloads every
loaded attachment module (including retained previous-scene modules). Dependencies are learned while loading and from runtime require call frames.
Official oe.* exports stay immutable and retain their cached identity.

Instances retain fields/metatables; onStart is not rerun. Top-level required table exports
are refilled in place, so `self.services.scoring` still sees new methods. Removed fields are
removed. Returning the same shared table is a no-op; changing a forwarding alias boundary
that would overwrite another cached project/official export is rejected (restart required). Affected module-local state and nested exported tables reinitialize; unrelated
modules remain untouched. Save mutable run state on instances/owner objects. Independently
captured functions, scalar exports, nested tables and existing timer/RPC closures may still
refer to old code. Prefer `module.method(...)` rather than saving a function reference.
Reload does not automatically recalculate fields or redraw existing translated labels.

Replacement initializers must only build exports: engine bindings (including queries),
timer/tween operations, coroutine operations and RNG calls are rejected, even through
aliases or a caught pcall. Register callbacks in owner lifecycle methods, not at module top
level. Initial loading retains its existing behavior; old projects with top-level native
queries/registration can continue running, but must move those operations before live reload
can succeed. Existing callbacks run once and are not automatically replaced/re-registered.

A reload batch captures the Lua heap and host metadata. Syntax/runtime/cycle/budget/forbidden
call failures restore the previous code, cache, globals, module state and instance fields;
script.errors reports the failure. Failed files are retried on later polls until fixed.
Cycles receive a `cyclic project import` diagnostic; failed initial imports clear their marker.
Large export graphs are bounded to depth 64 / 100000 visited tables/functions.

`script.reload` returns additive arrays: `reloaded` (attached paths), `required` (canonical
project paths replaced), `failed` (failed path; the whole batch was rolled back), `skipped`
(no_lua_session). Host-level direct calls additionally report recording/match_active skips.
Command calls during recording/active matches retain the existing state_recording error;
no polling or code replacement occurs there. `sim.record_state` still must precede Lua
execution/physics, and recorded resources stay immutable. Watch/dependency/source metadata
is restored with native snapshots. The address-stable allocator is now used by every Lua VM;
ordinary frames take no images/journal, only an actual reload captures a temporary image.

## Commands for agents

| Command | Use |
|---|---|
| `script.write {path, source}` | Create/overwrite a `.lua` file (works over MCP without file access) |
| `script.read {path}` | Read project or embedded source (e.g. @oe/ui.lua) |
| `script.list` | Project/embedded sources, users, running instances, reservedConflicts |
| `script.eval {code, entity?}` | Run Lua now. `"scene.find('Player')"` returns the id; with `entity`, `self` is that entity's live instance (e.g. `"self.speed"`) |
| `script.errors {clear?}` | Error list |
| `script.check {path or source}` | Syntax errors and global-variable warnings with line numbers, without running anything |
| `script.params {path or source}` | Params the script reads (`self.params.x or default`): name, type, default, options, description |
| `script.reload` | Transactional project dependency/attachment reload; report reloaded, required, failed, skipped; official exports unchanged |

Typical loop: `script.write` → `script.check` → `component.add Script` → `sim.step {frames: 60}` → `script.errors` → `render.screenshot` → fix → repeat.


During a match, use declared actions/axes and iterate `net.players()` in its sorted order:
```lua
function Game:onUpdate()
  for _, id in ipairs(net.players()) do
    local player = input.player(id)
    if player.down("W") then -- advance the world entity controlled by id
    end
  end
end
```
Lobby RPC and player callbacks are withheld during matches. `net.on("net.desync", function(report)
... end)` and `onNetState("desync")` run once after a terminal mismatch. Input frame waits still
poll the transport but do not call onUpdate/timers/physics/audio. Plain input sees the quantized
local player; mouse/touch coordinates are not synchronized. Native rollback preserves opaque closures, timers and physics solver history in bounded
checkpoints and replays at most eight frames. See [NETWORK_NOTES.md](NETWORK_NOTES.md) (M5) for bounds and recording contracts.

### Authoritative gameplay

Game values on an entity (cooldowns, charges, mana, a state name) go in the `Data` component: `values` is a flat
object of up to 64 names, each a finite number, a string (up to 256 bytes) or a boolean. It is saved with the
scene, shown in the inspector and replicated like any field (`NetSync.fields["Data.values"]`). With
`network.prediction: "owned"` a correction rewinds components, not script variables, so anything the prediction
depends on belongs there rather than in `self`:

```lua
local d = self:get("Data").values
d.cooldown = math.max(0, (d.cooldown or 0) - dt)
if keys.pressed("Q") and d.cooldown == 0 then self:dash(); d.cooldown = 3 end
self:set("Data", { values = d })   -- the whole object is replaced
```

With `network.mode: "authoritative"`, put `NetSync` on replicated entities and `NetPlayer` on
controlled entities. `network.playerPrefab` can spawn one per ready player automatically.
The server owns state; a client's frame-tagged inputs only enter its authenticated player stream.
For example, a player script reads `input.player(self:get("NetPlayer").player)`. The template `player_controller.lua` reads the local controls, so a networked game uses its own player script.
On an authoritative client, scripts on replicated (NetSync) entities it does not own do not run; put cosmetic motion of such an entity on a plain entity's script.

Clients predict their owned entities, interpolate remote replicated entities and continue running
local/unreplicated scripts (UI/cameras). Guard server-only level/spawn/score logic with
`if net.isServer() then ... end`. Select the physics velocity fields as well as Transform fields
in NetSync.fields for physical players. Corrections preserve Lua state and replay only unacknowledged
inputs; RPC callbacks are journaled, repeated sends suppressed and speculative audio deferred.
Authoritative scene transitions require a new ready barrier after leaving/stopping. Joining a running match needs
`network.lateJoin: true`: the host keeps its lobby open, and a player that joins and calls `net.ready` is let into the
running world (its `playerPrefab` avatar spawns then; its scripts start from the server's current state). A dedicated
server with `lateJoin` starts the world at once and keeps it running without players; player ids are never reused, so
do not index fixed tables by them. `onPlayerJoined(id)` runs on the host when a player reaches the lobby (before it is
ready), `onPlayerLeft(id)` when it is gone; lobby RPC works in both directions meanwhile (a login server can answer and
let the client leave without ever entering its world). With `network.admission: "script"` being ready is not
enough: the player enters when the server's script called `net.admit(id)`, typically after checking something the
client sent (its join name carrying a ticket, verified over a `link`); `net.kick {player=, reason=}` may be called
from the server's scripts to refuse it. An admission is for one visit. When a match stops because the server is gone, a client's
scripts keep running locally: check `net.state()` (`state: "error"`, `sync.state: "stopped"`) or `onNetState` and tell
the player.

Mouse look in an authoritative match: `input.mouseDelta()`, `input.lockMouse()` and `input.mouseLocked()`
work in local scripts (cameras), but raw mouse motion is not part of any player's input stream. A correction
restores Lua state and game data to the acknowledged frame and replays without mouse motion, so a turn kept
in `self`, in `game.set` or sent by RPC is taken back on every snapshot. Store it with `input.setLook(x, y)`
instead (each -1..1, kept on the device outside checkpoints, ignored during replays) and declare `LookX`
and/or `LookY` in `network.axes`: they use the RightX/RightY wire slots without a dead zone. Read it with
`input.look()` or `input.player(id).look()` (returns x, y). lua` turns it into the facing on the server and in prediction.
Lockstep and rollback matches carry the look axes too but still hide `mouseDelta`/`lockMouse` from scripts.

Local input in an authoritative match: declared actions and axes are what the input stream carries (the same on the
server and in prediction). Everything else the device reports stays local and is still visible to this peer's UI and
unreplicated scripts through plain `input.*`: the pointer (`UIButton` clicks, `input.mouse`), touches, the wheel, typed
text (`UIInput`) and keys that are not declared actions (`input.pressed("Enter")` to open a chat field, `"I"` for a
menu). It is never sent; tell the server with `net.rpc`. Corrections replay it with the frames it belonged to. While a
`UIInput` has the focus the keyboard is also left out of the stream, so typing does not move the player. With
networking enabled but no session (`net.state().state == "idle"`) `net.rpc` has nobody to send to and raises an error;
A small wrapper can call the handler directly when offline.

`net.rpc("owner", "notice", self.id, "Hit!")` routes to the entity's NetSync.owner (0 means server).
The handler receives the routing entity argument too: `net.on("notice", function(entityId, text)
... end)`. Entity ids are local; use `net.entities()` when mapping a received server entity id to
this peer's entity. Owner-only fields filter network transmission, not packaged resource contents.
See [NETWORK_NOTES.md](NETWORK_NOTES.md) (M6, M7) for limits, relevance, snapshot rates and platform verification.


Dedicated servers started by `oe serve-game` or a packaged `--server` reserve player
id 1 as an empty protocol input stream; `net.isServer()` is true and `net.isHost()`
is false. No playerPrefab is instantiated for that reserved slot. Use isServer for
server-authority gameplay. `net.simulate`, `net.spawn_local_peers`, `net.peer_call`
and `net.serve` are tool commands; they do not add Lua callbacks or bindings.


### Playable network examples

Lobby buttons call net.host/join/ready/start before the match. Start restores the shared
edit-time world, so local lobby messages never enter the deterministic match hash. During
a match, gameplay uses synchronized reset actions rather than a local session control.

UIButton.key mouse/touch gestures are converted to declared actions before network input
sampling, including short click pulses. Coordinates and gestures are local device state;
rollback/prediction replays only the recorded actions. NetPlayer/NetSync fields are reserved
replication metadata and must not be included in NetSync.fields. Ordinary reflected fields
such as Transform.position, MeshRenderer.color and UIText.text may be selected.


## Default project examples and editing

`oe new` now demonstrates the embedded modules in its offline two-level coin game.
[player_controller.lua](../templates/default/scripts/player_controller.lua) names the
original keyboard move/jump actions, owns an explicit oe.input.localSource(), and maps
the result to a zero-safe oe.math XZ direction. No gamepad axes/buttons are added and no
playerSource is guessed; converting this project to a network game requires an explicit
player/declaration migration. speed=4, jumpSpeed=5 and fallback gravity=12 remain literal
Script.params defaults for inspector/inference. CharacterBody uses its native gravityScale.

[game_manager.lua](../templates/default/scripts/game_manager.lua) acquires oe.ui widgets
onStart with explicit UIText/UIButton selection and retains partial text/visibility
updates. Scene transitions reacquire handles; there is no global handle cache. Simple
coin/rotation/bob/restart scripts keep their existing scene/self operations.

[EDITING.md](../templates/default/EDITING.md) maps actual game edits to params, scene UI,
coin score/count and verification. `tests/fixtures/Hello` is a generated copy of this template used by tests.
Required-only helper caching and widget single-component visibility limits remain as
documented above; no optional dialog/effect owner framework was added.
