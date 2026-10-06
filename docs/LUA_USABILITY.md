# Lua usability improvements

Status: scoped M0–M5 implementation and pilots are complete as of 2026-10-07.
Required-only project reload is implemented in 6.10; physical/platform/provider checks
and deferred APIs remain in 6.9.

## Summary

- Official embedded modules: oe.entities, oe.ui, oe.input and oe.math (SCRIPTING.md).
- Snapshot-aware identities, reserved loader/tools and digest agreement: 6.1–6.2.
- Native camera/canvas/text queries and their presentation-only rules: 6.4–6.5.
- UI/input pilots and DongleZoo edit-oriented split/data guide: 6.3, 6.6–6.7.
- Default template + regenerated Hello now demonstrate the modules and EDITING.md: 6.8.
- Transactional project helper reload and allocator benchmarks: 6.10; optimization pending approval.
- Current suites: native 235 / Wasm 199 pass; exact execution evidence in section 8.
- Unverified/deferred work and required inputs are consolidated in 6.9.

OwnEngine should let a human make a small gameplay change without understanding camera math,
component plumbing, input normalization, or unrelated game systems. AI-written projects should
meet the same readability standard. The goal is to expose game intent in scripts and make the
location of common edits predictable, rather than simply reduce line counts.

This proposal combines engine API improvements, an official Lua library, and project structure
changes. It follows [DESIGN.md](DESIGN.md), with the public scripting surface described in
[SCRIPTING.md](SCRIPTING.md) and the UI behavior in [UI.md](UI.md).

## 1. Findings before migration

These are the retained pre-migration findings, not a current defect list. Implemented
outcomes and measured before/after evidence are in 6.1–6.8; this was not an exhaustive audit.

| Evidence | Problem | Proposed response |
|---|---|---|
| `games/DongleZoo/scripts/lib/ui.lua` and `games/OnlineRPG/scripts/lib/widgets.lua` implement different UI wrappers | Finding a widget, discovering its component, and updating text/visibility are reinvented per game | Provide one supported widget interface |
| `games/DongleZoo/scripts/app.lua` is 722 lines and combines layout, play, shop, dialogs, missions, and rewards | A small change requires navigating unrelated responsibilities | Split by editing purpose and pass dependencies explicitly |
| DongleZoo `App:pointer()` implements its own orthographic pointer conversion | Scripts must understand camera projection and screen dimensions | Expose engine-backed coordinate queries |
| OnlineRPG `scripts/lib/space.lua` projects using a pitch-only camera and a fixed 720-pixel reference height | A reusable-looking helper contains assumptions about one game's camera and canvas | Use the actual engine projection and canvas layout |
| DongleZoo `UI.textWidth()` and OnlineRPG HUD `ems()` estimate widths from character categories | Bubble and toast sizing approximates the font rather than using its metrics | Expose text measurement using the UI text layout path |
| Wickbound `scripts/run/player.lua` includes keyboard direction, normalization, and drag-stick handling | Input plumbing obscures movement and health rules | Provide common input composition; retain intentional game-specific gestures |
| Wickbound `scripts/lib/balance.lua` separates many gameplay values | A useful editable-data pattern already exists | Standardize this pattern in templates and project guidance |

The engine already provides `self:position()`, physics helpers, timers, tweening, `UIJoystick`,
`UILayout`, `UICanvas`, and script parameter inference. Inventory these capabilities before
adding equivalents. Some code needs migration to existing functionality, not a new subsystem.

## 2. Where each improvement belongs

### Engine functionality and Lua bindings

Put a capability in the engine when correctness depends on engine-owned information or when it
is a basic operation that should behave consistently across projects and renderers.

- Coordinate conversion using the actual camera, world transforms, viewport, and canvas scaling.
- Text measurement using the same font and layout rules that render the text.
- Consistent widget operations across supported UI components.
- Position and physics convenience operations on entities other than the attached script entity.
- Access to computed UI geometry where existing commands already expose it.

Reuse existing internal math and layout code. Keep these as small additions to the public API;
a broad rewrite of the engine is not a prerequisite.

### Official Lua modules

Use Lua for reusable composition of existing capabilities and for policies that users may want
to inspect or customize:

- Entity/widget handles wrapping the supported engine operations.
- Input action composition, vector math, and common formatting helpers.
- Screen/dialog coordination, toasts, and simple state transitions.
- Optional effect spawning and cleanup built on timers or completion state.

An API may look like one widget interface while its implementation mixes engine bindings and
Lua wrappers. Decide ownership per operation rather than implementing the whole interface in C++.

### Project scripts and data

Keep scoring, merge rules, rewards, weapons, inventory rules, camera behavior, and presentation
choices in the project. Reusable gameplay examples may be copied and modified, but should not
become mandatory engine systems merely because several games need them.

This extends the existing engine/project boundary: a behavior expressible in Lua stays in Lua,
while common infrastructure is supplied once instead of recreated by every project.

## 3. Proposed public surface

Coordinates use the implemented M2a named request schema in 6.1.G.1. M0
settles namespace ownership, delivery, and semantic requirements; the M1a epoch signature is
fixed in 6.1.B.1. Text measurement is implemented in M2b (6.5); fitting remains a future explicit-box feature.
The minimal official find/tryFind/id/get/set and focus forwarding surface is now available;
see SCRIPTING.md. The pilot widget methods in 3.2 are implemented in M1b; unsupported entity conveniences remain candidates.

### 3.1 Coordinates and text: engine-backed queries

Implemented coordinate and text operations:

```lua
local screen = camera.worldToScreen({camera=cameraId, position=worldPosition})
local ray = camera.screenRay({camera=cameraId, screen=screenPosition})
local point = camera.screenToPlane({camera=cameraId, screen=screenPosition, plane=plane})
local canvasPoint = ui.screenToCanvas({canvas=canvasId, screen=screenPosition})
local size = ui.measureText({text=text, style=style, wrapWidth=maxWidth})
```

The following M0 semantic contracts are implemented by M2a/M2b; fitting and painted bounds remain deferred:

- Screen coordinates use logical game-view pixels, origin at top left. Existing normalized
  `input.mouse()` behavior remains unchanged; conversions to pixels are explicit.
- Require a camera argument so multi-camera projects do not depend on an implicit selection.
- Support perspective and orthographic cameras with their full transform.
- A screen point defines a ray, not a unique world point. Plane intersection must specify a
  plane; parallel or backward intersections return an explicit no-hit result.
- Projection reports depth/visibility with documented clipping semantics. A point outside the
  viewport is distinguishable from one behind the camera.
- Canvas conversion follows actual reference resolution, scaling, and viewport configuration.
- Text measurement returns layout dimensions in canvas reference pixels for a specified view
  and effective canvas scale. It accounts for font, size, wrapping, rich text, and spacing;
  outlines/shadows are excluded. Painted bounds are outside the initial scope.
- Headless calls receive explicit view dimensions or use the documented engine view-size state.
- Determinism: these are presentation-only queries. Results depend on view, canvas, font
  resources, and rounded raster size; atlas packing order alone is not evidence of different
  metrics. Use the exact simulation/input restrictions and read-only verification rule in 6.1.
- Cost: define caching guidance (positions per frame, text sizes per string/style) and keep
  measurement cheap enough to call for a few dozen labels per frame without a cache.

Expose matching registry queries for tools. Lua, commands, and renderer behavior must share the
same implementation rather than maintain separate approximate formulas.

### 3.2 Widget and entity convenience interfaces

Widget usage (implemented in M1b; setPosition remains a later entity candidate):

```lua
local ui = require("oe.ui")
local entities = require("oe.entities")

local score = ui.find("Score")
local popup = ui.find("PauseDialog")
local player = entities.find("Player")

score:text("100")
popup:show()
popup:hide()
player:setPosition(0, 1, 0)
player:set("Sprite", { flipX = true })
```

- Namespace: use `oe.ui` as the official composition module; keep global `ui` for engine
  primitives. The module forwards `focus`/`focused` and public UI query primitives with their
  original signatures, so a local `ui` alias retains those operations. See 6.1 for the decision.
- Entity identity: ordinary creation does not recycle deleted IDs, but replacement/restoration
  can reuse them. Adopt the snapshotted Scene epoch plus Lua session token in 6.1.B.1; a name
  comparison or existence check alone is insufficient.
- Provide strict `find` and optional `tryFind` variants. Missing required objects produce errors
  with their names and expected capabilities, rather than silently skipping updates.
- Check capabilities: text, value, focus, and interactability are not valid on every widget.
- Define behavior when an entity carries multiple UI components; use explicit selection or
  report ambiguity rather than guessing from undocumented component order.
- Treat handles as session-local. Detect destroyed entities and invalidate across scene changes;
  cached IDs must never silently target an unrelated replacement entity.
- Keep generic `get/set` available for less common fields. Preserve partial-update semantics.
- Use existing script physics behavior for entity convenience methods; distinguish local and
  world positions explicitly and document hierarchy behavior.
- Widget visibility changes only its own component. Existing ancestor visibility suppresses
  descendants without overwriting their flags; retain that behavior for dialogs (6.1).

Mutations must reuse the engine's validated operations. Provide registry operations for any
new capability; wrappers may compose existing commands without inventing an alias command for
every convenience method. Preserve each execution surface's current undo/revision semantics.

### 3.3 Input actions and math

Offer named actions and vector composition so gameplay does not repeatedly inspect individual
keys and normalize directions. Start with keyboard, existing pad axes, and `UIJoystick` output.
Keep custom floating-stick behavior optional rather than changing a game's controls implicitly.

- Specify keyboard/stick precedence or combination, diagonal normalization, dead zones,
  pressed/held behavior, and axis orientation.
- Respect focused text fields and local UI input handling.
- In network games, compose input from declared actions/axes and the appropriate
  `input.player(id)` source. Do not accidentally read device-local gameplay input on the server.
- Keep injection and deterministic replay available through existing input commands.
- Supply a small math module: clamp, lerp, distance, and zero-safe vector normalization.
  Avoid introducing a second incompatible vector representation.

Do not add generalized input configuration until the offline and network contracts are clear.

## 4. Official module delivery and lifecycle

Reserve `oe.*` for official modules. Adopt `engine/script/lua/oe/*.lua` as inspectable source,
embedded into the engine at build time. Packaged players resolve it without project copies or
developer filesystem paths. The loader/tooling/lifecycle contract is specified in 6.1.

- The sandboxed `require` loader now resolves canonical official names before project/cache
  lookup. Nonreserved `a.b` still maps to project `a/b.lua`; reserved variants cannot bypass
  origin checks through physical paths (`engine/script/ScriptHost.cpp:1121`).
- Keep tooling working: adopt read-only virtual paths for official sources in `script.read`
  and `script.list`, shared by check/params source resolution. Local module fields are not
  currently checked as globals; preserve that behavior and existing parameter inference.
- Prefer Lua composition first. Embedded source and loader changes require player runtime
  rebuilds even without new gameplay bindings. Project-file-only Lua edits do not; native
  bindings follow where accurate engine data is required (projection, text measurement).
- Keep official modules inspectable in the repository and document their public interfaces.
- Protect the reserved namespace from accidental project-file shadowing. Custom behavior uses
  project modules wrapping the official ones.
- Keep shared modules stateless by default. Put timers, handles, subscriptions, and screen state
  in explicit owner objects with cleanup methods.
- Define destruction, scene change, simulation restart, and hot-reload cleanup. Prevent duplicate
  callbacks and timers after reload.
- Keep mutable per-script data on script instances and maintain compatibility with recording,
  rollback, and bounded replay. Do not introduce hidden mutable module state without proving
  that it is captured/reset under the current scripting contract.
- Package identical behavior on desktop, web, and Android. Native binding changes require the
  corresponding player runtime rebuilds or a documented pending rebuild.

Avoid adding a large framework with implicit lifecycle rules. Introduce only modules supported
by concrete use cases in the migration pilots.

## 5. Make common edits predictable

| User wants to change | Recommended location |
|---|---|
| One character's speed, health, or jump strength | `Script.params`, editable in the inspector |
| Enemy/weapon/reward values across the game | `config/balance.lua` or a focused data table |
| Labels and translations | `config/text.lua` or the project's existing localization module |
| Shop products and stage contents | Dedicated data tables |
| Scoring, merging, damage, or win conditions | Small gameplay functions in the relevant module |
| Screen layout | Scene UI/layout components; scripts only for genuinely dynamic policies |

Preserve literal parameter defaults so existing `script.params` inference continues to work.
Do not move every number into global configuration; expose values that have a real editing use.
Document units, defaults, and valid ranges. Keep project naming conventions where migration
would otherwise create churn; these paths are recommendations for new or reorganized projects.

Each project should have a short `EDITING.md` mapping common changes to files, parameters, and
the command or play flow used to verify them.

For DongleZoo, split `app.lua` by responsibility: startup/orchestration, drop/merge gameplay,
home/shop/dialog UI, and reward coordination. Preserve the existing board, save, audio, and
localization modules. Pass required state/services explicitly rather than replace one large
file with many modules communicating through unrestricted globals.

## 6. Delivery order and scope

| Milestone | Deliverable | Exit evidence |
|---|---|---|
| M0: inventory and contracts — complete | Evidence/design in 6.1; identity, grammar and digest decisions adopted; M2 schema subsequently selected in 6.4 | Static estimates clearly separated from execution |
| M1a/M1b: UI/entity foundation and pilots — complete | Embedded modules/tools/agreement and DongleZoo/OnlineRPG widget pilots (6.2–6.3) | Native/Wasm checks and packaged runtime refresh evidence in section 8; device limits in 6.9 |
| M2a/M2b: accurate queries — complete scoped surface | Camera/canvas and renderer-backed layout measurement, pilot replacement (6.4–6.5) | Camera/text/render parity, read-only future checks, costs and runtime evidence; fitting/paint bounds excluded |
| M3: input composition — complete | Pure offline/network input and math, Wickbound/NetArena pilots (6.6) | Native 230 / Wasm 194 tests at that stage; physical hardware pending |
| M4: project readability — complete scoped pilot | DongleZoo split, single shop source, EDITING.md and actual edits (6.7) | 13 identical checkpoints, 231 native tests; required-helper live reload remains a follow-up |
| M5: defaults and optional flows — complete | Two template scripts adopt official modules, Hello regenerated, editing guide published; optional flows declined with evidence (6.8) | 10 identical checkpoints, parameter parity, 232 native tests, fresh desktop/web startup smokes |

The scoped delivery sequence above is complete. Avoid treating it as migration of all projects.
Use DongleZoo for everyday editing and OnlineRPG for camera, UI, and multiplayer constraints.
Use Wickbound to validate that abstractions remain useful in more elaborate gameplay.

Existing `scene.*`, input calls, and scripts remain compatible. New projects adopt the common
surface; existing projects migrate incrementally. Do not remove old APIs as part of this effort.

### 6.1 M0 results (2026-10-06)

Method: read-only source investigation of the current working tree, including existing local
changes. References are repository-relative `file:line` anchors at investigation time. No edit
task, engine process, build, test, or screenshot was executed. **Verified** means verified by
source inspection; **decision** means a contract adopted for future work, not implemented
behavior. **Unverified** means execution or evidence is still missing.

#### A. Existing-function inventory

Classification is for the repeated operation as a whole: **Already present** means migrate or
wrap an existing capability; **Partial** means foundations exist but the requested Lua surface
or generality is missing; **Absent** means no corresponding composition interface was found
in the inspected scripting registrations. It does not imply that engine math is absent.

| Repeated operation and script evidence | Engine and documentation comparison | Classification / next action |
|---|---|---|
| Find UI entity: DongleZoo `scripts/lib/ui.lua:9`, Wickbound `scripts/lib/ui.lua:9`, OnlineRPG `scripts/lib/widgets.lua:14` | `scene.find`/`has`/`exists` registered at `engine/script/ScriptHost.cpp:1599`; name lookup implementation at `:297`; `docs/SCRIPTING.md:81` | Already present for lookup/existence. Migrate wrappers to common Lua; widget capability discovery and safe handle lifetime are Partial |
| Set UI text/fields: DongleZoo `scripts/lib/ui.lua:34`, `:43`; Wickbound `scripts/lib/ui.lua:29`, `:38`; OnlineRPG `scripts/lib/widgets.lua:34`, `:38` | Partial component updates already supported by `engine/script/ScriptHost.cpp:216` and `docs/SCRIPTING.md:77` | Already present for mutation. Common widget methods compose `scene.set`; no new text setter engine feature required |
| Show/hide widgets: DongleZoo `scripts/lib/ui.lua:39`, Wickbound `scripts/lib/ui.lua:34`, OnlineRPG `scripts/lib/widgets.lua:43`, `:47` | `scene.set` above; render subtree stops at a hidden primary widget (`engine/render/UI.cpp:866`), hit-test ancestor visibility at `:1433`; modal backdrop at `docs/UI.md:160` | Already present for visibility/modal primitives. Migrate composition; do not add recursive field rewriting |
| Focus: OnlineRPG `scripts/lib/widgets.lua:55` | `ui.focus`/`focused` registered at `engine/script/ScriptHost.cpp:1622`; `docs/SCRIPTING.md:111` | Already present. Forward existing signatures from the module |
| Pointer to world: DongleZoo `scripts/app.lua:81` relies on its `layout()` at `:61` | Normalized pointer and view size exist at `engine/script/ScriptHost.cpp:997`, `:1007`, `docs/SCRIPTING.md:110`; camera matrices exist at `engine/render/SoftwareRenderer.cpp:769` | Partial. Reuse engine projection/math, add a public ray/plane query; current game-specific formula is a migration target after that |
| World to screen/canvas: DongleZoo `scripts/app.lua:217`; OnlineRPG `scripts/lib/space.lua:20`; Wickbound `scripts/run/hud.lua:63` (coordinates at `:81`) | Camera uses full world transform at `engine/render/SoftwareRenderer.cpp:773`; canvas scale at `engine/render/UI.cpp:1332`; Lua registrations at `engine/script/ScriptHost.cpp:1599`, `:1622` expose neither conversion | Partial internally; absent as a public Lua conversion. Add queries sharing existing render math |
| Approximate text width: DongleZoo `scripts/lib/ui.lua:78`; Wickbound `scripts/lib/ui.lua:88`; OnlineRPG caller outside lib at `scripts/hud.lua:55` | Internal `MeasureText` at `engine/render/UI.cpp:1532` exists but disables rich text, spacing, wrapping; full shaping at `:206`, `:662`; no measurement binding at `engine/script/ScriptHost.cpp:1622`; font behavior documented at `docs/UI.md:121` | Partial. Extend/reuse the actual shaping path and expose it; do not promote character-category estimates |
| Keyboard/stick direction and normalization: Wickbound `scripts/run/player.lua:40` | Key/axis/player sources at `engine/script/ScriptHost.cpp:949`, `:961`, `:987`; `docs/SCRIPTING.md:87`, `:145`; fixed-region joystick exists at `engine/render/UI.cpp:1294`, `docs/UI.md:279` | Partial. Migrate fixed-region joystick use to existing `UIJoystick`; combined direction helper is Absent. Drag-anywhere origin-following stick remains an intentional project policy |
| Local input scope check | DongleZoo pointer/drop at `scripts/app.lua:81`, `:688`; OnlineRPG lib has no keyboard/stick composition found; its `space.lua:10` supplies distance math | No evidence of the same direction composer in all three requested directories. Wickbound supplies the concrete input pilot; do not claim three-project duplication |
| Layout, tween, position, and timing support adjacent to these helpers | `engine/script/ScriptHost.cpp:1152` (instance helpers), `:1192` (timers); `docs/SCRIPTING.md:115`, `:122`; `engine/render/UI.cpp:1342`; tool `ui.layout` at `engine/api/Commands.cpp:966` | Already present. Migrate/reuse before inventing new systems; Lua access to computed layout remains Partial |

Script paths in the first column are relative to the named project's root. Inventory covers
the specified repetitive categories; it does not claim every project helper was audited.

#### B. Entity identity and handle lifetime

| Event | Verified behavior and evidence | Consequence |
|---|---|---|
| Ordinary create/delete/create | `Scene::Create` advances `nextId_` (`engine/scene/Scene.cpp:42`); `Destroy` removes pools/record without decreasing it (`:54`) | No recycling on normal creation within an uninterrupted scene, excluding integer overflow (not audited) |
| Forced IDs / scene replacement | `Create` accepts an unused forced ID (`:43`); `CreateFromJson(..., true)` uses serialized IDs (`:181`); `FromJson` clears then recreates (`:219`); `Clear` resets cursor to 1 (`:35`) | Reuse is possible. A numeric ID is not a persistent identity across world replacements |
| Tool scene load | `engine/app/Engine.cpp:122` calls `FromJson`, then `ResetRuntime` (`:130`); runtime reset calls `scripts_->Reset` (`:230`) | New IDs may equal old ones; the Lua VM/cache is discarded |
| Lua scene transition | `ApplySceneChange` calls `FromJson` then only `ResetInstances` (`engine/app/Engine.cpp:237`, `:250`); that method only unrefs instances (`engine/script/ScriptHost.cpp:1677`) | Same Lua VM and `require` cache survive. Module tables, closures, timers, and network callbacks can retain stale IDs; cleanup is not automatically supplied by `onDestroy` here |
| `sim.stop` | Restores `playSnapshot_` with `FromJson` when one exists (`engine/app/Engine.cpp:269`), then resets runtime; VM is closed at `engine/script/ScriptHost.cpp:1705` | Runtime entities disappear; IDs/cursor derive from the restored edit scene. Without a play snapshot the current scene remains, but the VM is still reset |
| `sim.load_state` and native rollback | `LoadState` calls `RestoreNative` (`engine/app/EngineSync.cpp:220`); scene and Lua are restored together (`:58`); scene copy preserves `nextId_` (`engine/scene/Scene.cpp:25`); heap and registry restored at `engine/script/ScriptHost.cpp:2179` | Cursor rewinds with the snapshot. IDs from a discarded future can be assigned again on a new branch. Handles in the saved Lua heap must restore consistently with the saved scene |
| Editor replacement paths | Undo/redo also use `FromJson` (`engine/app/Engine.cpp:1005`, `:1017`) | Any future tool-side cache requires replacement invalidation too |

**Decision:** handles are scoped to one Lua session and one world incarnation. Normal deletion
needs existence/capability checks, not a recycled-slot generation. A world-incarnation token is
required for scene replacement even when the same ID and name reappear; checking `game.scene()`
or an entity name is insufficient for loading the same scene again.

**Decision:** native restoration restores handles that belonged to the snapshot, together with
their identity token. References belonging only to a discarded future must not survive outside
the restored state. Do not invalidate every saved Lua handle by incrementing an unsnapshotted
C++ counter on every rollback; that would invalidate valid restored closures. External handles
must be reacquired after restoration and cannot be serialized as portable identities.

M1a-1 exposes the incarnation token through `scene.epoch()` and the read-only registry query.
M1a-2 supplies the official handle module/session registry in the Lua heap. A per-entity generation is not justified by
ordinary deletion, but must be reconsidered if forced-ID replacement becomes available without
a world-incarnation change.

##### B.1 Decision: snapshotted Scene epoch plus a Lua session token

**Candidate comparison (M0 design record):** the selected design is now implemented in M1a-1/2;
the alternatives remain proposals.
Candidate (a) is selected. Candidate (b) requires native hooks too; it is not a pure Lua fix.
Candidate (c), an Engine counter outside snapshots that increments on restore, is rejected.

| Existing B event / evidence | (a) Scene epoch copied in NativeState | (b) Lua heap token replaced by native lifecycle hooks | (c) Unsnapshotted Engine counter |
|---|---|---|---|
| Ordinary creation/destruction (`engine/scene/Scene.cpp:42`, `:54`) | Epoch unchanged; validate existence/capability. Forced-ID creation outside replacement is outside the supported handle contract | Token unchanged; same existence checks | Counter unchanged; same checks |
| `Clear` and `FromJson` (`engine/scene/Scene.cpp:35`, `:214`) | `Clear` increments once before clearing. `FromJson` uses that increment, not a second one. Format rejection before Clear leaves epoch unchanged; failures after Clear invalidate old handles | Scene cannot directly own a Lua reference without introducing a dependency; every engine call that clears/loads needs a ScriptHost hook, including direct Scene calls | Must hook every path, including direct Scene calls; not automatically covered |
| Tool scene load (`engine/app/Engine.cpp:122`, `:130`) | Scene epoch increments; VM reset replaces the private Lua session token | Reset VM creates fresh token; no need to retain the old token | Invalidate counter/session, but duplicate hooks risk inconsistent coverage |
| Lua scene transition (`engine/app/Engine.cpp:244`, `:250`; `engine/script/ScriptHost.cpp:1677`) | Scene epoch increments while the VM/session token survives; old handles fail epoch comparison | Replace heap token after scene replacement before callbacks resume; explicit hook needed because ResetInstances preserves modules/timers | Increment on transition works, but snapshot restore remains wrong |
| `sim.stop` (`engine/app/Engine.cpp:269`; `engine/script/ScriptHost.cpp:1705`) | Restored JSON increments epoch if a play snapshot exists; without it, epoch is unchanged. Both discard VM/session token, so old handles cannot be imported into the next VM | Reset discards token, next VM creates one; same result | Needs separate session invalidation when no scene replacement occurs |
| `sim.load_state` / RestoreNative / rollback (`engine/app/EngineSync.cpp:58`, `:220`; `engine/script/ScriptHost.cpp:2179`) | Scene copy restores epoch exactly, and Lua heap restores session token/handle records together. No bump on restore; future-only references disappear with discarded heap | Heap restores token/records correctly, but external C++ paths have no Scene-owned identity to inspect | Bump on restore wrongly rejects handles inside restored closures. Restoring a counter instead turns this into a snapshot-aware alternative |
| Editor undo/redo (`engine/app/Engine.cpp:1000`, `:1012`) | `FromJson` increments even for the same path/IDs/names; old VM-held handles fail if a VM exists | Must add hooks to both paths; current undo/redo do not reset ScriptHost | Needs hooks; restoration ambiguity remains |

**Concrete data design:** add runtime-only `uint64_t incarnation_ = 1` and an `Epoch()` getter
to Scene. `Clear()` checks overflow, increments the current value, then clears. Never wrap or
reset it to 1 on Clear. Copy construction/assignment copy it, as they already copy `nextId_`
(`engine/scene/Scene.cpp:20`). NativeState already contains a Scene by value
(`engine/app/EngineSync.cpp:31`, `:51`), so no parallel epoch field is needed. Do not serialize
epoch into scene JSON, prefabs, or world hashes (`engine/scene/Scene.cpp:166`);
it is a local lifetime marker, not a synchronized gameplay value.

Distinguish *copy/restore* from *replacement*. Add `Scene::ReplaceWith(const Scene&)`: clone
content using existing copy semantics, but assign the receiver's previous epoch plus one,
with checked overflow. Use it for adopting a separately constructed scene, notably prefab
isolation at `engine/app/Engine.cpp:1072`. Keep `RestoreNative` assignment unchanged. Prefab
close already uses `FromJson` (`engine/app/Engine.cpp:1114`). This prevents unrelated temporary
scenes from importing an equal/lower epoch; constructors/probe scenes do not need global IDs.
Check all Scene assignment sites during implementation. Epoch equality across independent
engines is meaningless and never authorizes transfer of a handle.

One additional inspected assignment is local-preview initialization: a newly created/opened
peer copies the edit scene (`engine/app/EngineNetwork.cpp:68`, `:71`). Copying epoch is safe
there because no source-engine Lua handle/session is transferred; keep the copy semantics and
test independent-session rejection. Current incremental API creation/paste and authority
creation pass `keepId = false` (`engine/api/Commands.cpp:700`, `:803`;
`engine/app/EngineAuthority.cpp:191`), supporting the decision not to add recycled-slot
generations in M1a. Arbitrary direct C++ mutation outside documented lifecycle paths remains
outside the scripting/recording contract.

**Minimum public API, adopted for M1a:** `scene.epoch() -> string`, an opaque fixed-width
16-digit lowercase hexadecimal encoding of Scene's uint64 epoch. Match it with registry query
`scene.epoch {}` returning `{ "epoch": "0000000000000001" }`. Treat the string as equality-only,
scoped to this engine/session; do not parse/increment it in Lua or transmit it as an identity.
Strings avoid uint64 rounding through JSON numbers. No argument, no mutation, no Lua execution
is necessary for the registry query. A new-session handle must be acquired through the module,
even if `scene.epoch()` returns the same value after a runtime-only reset.

**Handle validation algorithm (design pseudocode):** `oe.entities` owns one private session
table and a weak-key record map in the Lua heap; `oe.ui` uses that same constructor/validator.
The map makes hand-built or JSON-recreated tables invalid handles. Export tables themselves
remain immutable by convention. This is a correctness contract, not a security boundary.

```lua
local session = {}                       -- allocated once for this VM's official module
local records = setmetatable({}, { __mode = "k" })

local function makeHandle(id, kind)
    assert(scene.exists(id), "missing entity")
    local handle = setmetatable({}, Handle)
    records[handle] = { session = session, epoch = scene.epoch(), id = id, kind = kind }
    return handle
end

local function validate(handle, requiredKind)
    local record = records[handle]
    assert(record and record.session == session, "foreign or reconstructed handle")
    assert(record.epoch == scene.epoch(), "handle belongs to a replaced world; find again")
    assert(scene.exists(record.id), "entity was destroyed")
    if requiredKind then assert(scene.has(record.id, requiredKind), "missing capability") end
    return record.id
end
```

Every accessor and mutation validates first. Stale handles never silently re-find by name.
Strong references held by a caller keep their weak-map record available. Saved Lua heap images
include session/records/handles; restore them together with Scene. A branch may reuse an epoch
number or entity ID after rewind, but a discarded future handle has no surviving supported
reference. Saving handle data through `game.set`/save/JSON or holding native pointers to Lua
objects is not supported; re-find after such boundaries. A lazy epoch check invalidates handles
even when required modules survive a transition. It does not invoke `onDestroy` or automatically
cancel project timers/RPC callbacks; M1a adds no owner automation and must not claim it does.

**Final verdict:** pure Lua with the current API cannot implement this contract reliably.
Clear/FromJson, same-scene reload, undo/redo, and native restoration are not observable as a
unique incarnation through existing names/IDs. The minimum native addition is the Scene epoch
and binding/query above. Required C++ edits: `engine/scene/Scene.h`, `Scene.cpp`,
`engine/app/Engine.cpp` (replacement adoption), `engine/script/ScriptHost.cpp`, and
`engine/api/Commands.cpp`; snapshot copy coverage is checked in `engine/app/EngineSync.cpp`.
No changes to snapshot-heap machinery were needed. Executed desktop evidence is in section 8;
platform checks outside that evidence remain **Unverified**.

#### C. Global `ui` versus `oe.ui`

Current global `ui` has only `focus` and `focused`
(`engine/script/ScriptHost.cpp:1622`); `Widget:focus` calls it directly
(`games/OnlineRPG/scripts/lib/widgets.lua:55`).

| Choice | Advantages | Costs |
|---|---|---|
| Extend global `ui` with every helper | One name; existing scripts discover helpers without imports | Implicit dependency, grows the sandbox global, combines primitives and higher-level owner state |
| Put composition in `require("oe.ui")`, forwarding UI primitives | Explicit dependency, inspectable Lua implementation, clear module ownership | A local `ui` alias must forward the full documented primitive surface or existing focus/query calls can break |

**Recommendation / decision:** keep global `ui` for native primitives; put widget composition
in `oe.ui`. That module forwards `focus`, `focused`, and future documented UI query primitives
with unchanged signatures. Examples prefer `local widgets = require("oe.ui")`; the shorter
`local ui = ...` is supported without losing those operations. No automatic installation of
the module into `_G.ui`. Publish the split in SCRIPTING.md during implementation, not this
documentation-only task. Mutation of shared module export tables is unsupported.

#### D. Official module loading, tools, reload, and packaging

**Verified M0 loading:** `L_Require` allowed letters/digits/underscore/dot/slash/hyphen,
rejects `..`, converts dotted names to paths unless `.lua` is already present, and caches by
path in registry `oe.loaded` (`engine/script/ScriptHost.cpp:1111`). It executes project files
through `LoadFile`/`ResolvePath`, text chunks only, with the instruction budget (`:1771`).
A nil module result becomes true (`:1140`); false is also cached because the check is non-nil.
At M0 there was no in-progress marker, so recursive module cycles were not explicitly detected.
No standard `package` searcher is available (`:1579`).

**Verified M0 reload distinction:** attached script modules live in `modules_` and are reloaded
in place by `ReloadModule` (`engine/script/ScriptHost.cpp:1785`, `:1811`);
`PollHotReload`/`ReloadAll` iterate only that map (`:1850`, `:1881`). `L_Require` instead
calls `LoadFile` directly and does not register that dependency in `modules_`. Consequently
reload of a required-only helper is not guaranteed, and reloading a caller keeps the old
required value. General reload wording at `docs/SCRIPTING.md:244` must be clarified at M1.

**Adopted loader contract:**

1. Embed a manifest of canonical `oe.*` names and Lua source in the shared player engine,
   generated from `engine/script/lua/oe/`. Existing embedded font generation provides a
   build precedent (`CMakeLists.txt:132`); no new runtime filesystem dependency is introduced.
2. Normalize accepted name/path aliases before choosing origin and cache key. Reserve canonical
   `oe.*` and equivalent `oe/...(.lua)` paths before project resolution. Unknown reserved
   names fail with a module-not-found diagnostic; never fall back to a project `oe/` file.
   Apply the fixed grammar and virtual paths in D.1. Nonreserved project imports retain current
   behavior; official lookup does not inherit host filesystem case/path rules.
3. Use separate origin-tagged canonical cache keys and a loading sentinel for official modules.
   Load text only, preserve instruction limits, report source/line with a virtual official path,
   clear a failed-load marker, and diagnose import cycles. These official-loader behaviors are
   implemented in M1a-2; executed desktop evidence is in section 8.
4. Official modules are immutable for a running VM and not watched for project hot reload.
   `script.reload` does not replace official exports. The initial M1 implementation kept
   required-only project helpers cached; section 6.10 supersedes that limitation with watched,
   transactional dependency reload. Recording/active-match restrictions remain unchanged.
5. VM reset clears official exports/cache. Lua scene changes keep stateless exports but must
   release owner state and reject stale handles. `ResetInstances` does not invoke cleanup by
   itself (`:1677`); removal-time `onDestroy` exists on a different path (`:1965`). Do not rely
   on an unimplemented automatic lifecycle hook.
6. Keep cache, owner state, and tokens in snapshot-compatible state. Native heap restore already
   captures the Lua registry and module tables (`engine/script/ScriptHost.cpp:2171`); immutable
   source lookup is compatible. New external mutable registries are not automatically covered.

| Tool | Current source evidence | Adopted M1 behavior |
|---|---|---|
| `script.list` | Enumerates project `.lua` files, then runtime status (`engine/api/Commands.cpp:1227`) | Add official entries with virtual paths and `origin: embedded`, `readOnly: true`; preserve project entries and current consumers |
| `script.read` | Reads only a resolved project file (`engine/api/Commands.cpp:1247`) | Shared source resolver recognizes canonical `@oe/ui.lua` virtual paths per D.1; returns embedded source read-only |
| `script.check` | Compiles without running and checks global uses, not member names (`engine/script/ScriptCheck.cpp:221`); command source resolver at `engine/api/Commands.cpp:1258` | Resolve official virtual paths too. `local widgets = require("oe.ui")` needs no module-field allowlist; `require` is already a known sandbox global (`engine/script/ScriptHost.cpp:1598`, `:1659`). Do not claim it checks exported methods or module existence |
| `script.params` | Tokenizes source for params aliases/defaults (`engine/script/ScriptCheck.cpp:262`); uses the same resolver (`engine/api/Commands.cpp:1293`) | Preserve literal-default inference on project scripts; official library source normally has no instance params. Do not move defaults into opaque factory calls |
| `script.write` and related source writes | Project source reading has no embedded origin distinction today | Reject official virtual-path writes; customization belongs in a project wrapper. New list entries must not invite writes to embedded source |

**Packaging recommendation / decision:** compile official modules into each runtime. `GameFiles`
collects project data excluding tools/web/server/dotfiles (`engine/app/Project.cpp:82`);
`WriteGamePak` writes that list (`:97`). Desktop packaging copies project files and a player
exe (`tools/oe/main.cpp:1150`, `:1155`); web packaging writes game.pak and copies existing
JS/Wasm (`:1103`, `:1106`); Android uses existing `.so` libraries and game.pak (`:885`,
`:904`). Players read the extracted data on Android and `/game` on web
(`tools/player/main.cpp:187`, `:214`). No official sources need to be duplicated into game.pak.

This corrects the earlier pure-Lua/no-runtime-rebuild claim: changing project Lua requires
repackaging data, but changing embedded Lua or its loader requires rebuilding desktop, web,
and Android players even without adding a gameplay binding. Runtime freshness is Unverified.
Project-file shadowing is rejected by resolution, not by silently trusting a copied `oe/` file.

Embedded sources sit outside the current project resource hash (`engine/app/EngineSync.cpp:76`).
Adopt manifest-digest inclusion in ContentHash and the existing blueprint barrier (D.2).
Protocol version (`engine/net/Session.h:19`) alone does not prove source agreement.

##### D.1 Decision: module grammar and canonical virtual source paths

**Verified foundation:** current `L_Require` rejects backslashes/colons/`@`, allows upper-case
names, transforms dots unless `.lua` is present, and caches the resulting path
(`engine/script/ScriptHost.cpp:1113`, `:1118`, `:1125`). `ResolvePath` joins with the project
directory and performs a lowercased containment check (`engine/app/Engine.cpp:171`). Its
filesystem normalization (`engine/core/FileSystem.cpp:109`, `:116`) is for project paths;
it cannot choose embedded origin or define a portable import grammar. The following is a
new official-namespace contract, not a claim that these spellings already work.

Official module segments match `[a-z][a-z0-9_]*`. Accept either all-dot segments after `oe.`
or all-forward-slash segments after `oe/`, optionally ending in `.lua` for the slash form
only. No mixed separators, uppercase, whitespace, empty segments, backslashes, drive paths,
leading `./`, or traversal. One canonical name (`oe.ui`) maps to one canonical source path
(`@oe/ui.lua`) and one origin-tagged cache key (`embedded:oe.ui`). Nested modules use
`oe.math.vec2` / `oe/math/vec2.lua` / `@oe/math/vec2.lua` under the same grammar.

| Input | `require` behavior | Tool `path` behavior (`script.read/check/params`) |
|---|---|---|
| `oe.ui` | Accept; canonical name `oe.ui` | Reject reserved alias; hint `@oe/ui.lua` |
| `oe/ui` | Accept; same cached export | Reject reserved alias; hint `@oe/ui.lua` |
| `oe/ui.lua` | Accept; same cached export | Reject physical reserved alias; hint `@oe/ui.lua` |
| `@oe/ui.lua` | Reject; virtual source paths are not import names | Accept canonical embedded source, read-only |
| `oe.ui.lua` | Reject ambiguous dotted extension; hint `oe.ui` | Reject reserved alias; hint `@oe/ui.lua` |
| `OE.UI`, `Oe/ui`, `oe/UI.lua`, `@OE/ui.lua` | Reject reserved spelling; never try project lookup | Reject noncanonical reserved spelling; hint canonical lowercase path |
| `oe\ui`, `oe\ui.lua`, `@oe\ui.lua` (actual backslash characters) | Reject; use forward slashes | Reject official-path backslashes on every platform, including Windows |
| `oe`, `oe.`, `oe/`, `oe//ui`, `oe/ui.extra` | Reject bare/invalid reserved name | Reject; no filesystem fallback |
| `./oe/ui.lua`, `oe/../scripts/x.lua`, drive/absolute names | Reject import path aliases/traversal | Detect physical top-level `oe/` origin after normalizing a project path; reject reserved conflicts rather than read project bytes |
| `oe.unknown`, `oe/unknown.lua`, `@oe/unknown.lua` | Valid grammar but missing official module: fail | Valid virtual grammar but missing manifest entry: not-found; no fallback |
| `scripts.lib.ui`, `scripts/lib/ui.lua`, `scripts.oe.ui` | Preserve existing nonreserved project behavior | Preserve existing project path resolution; `scripts/oe/` is not the reserved top-level namespace |

Classification precedes cache access/project lookup. A case-insensitive namespace detector
recognizes the reserved *root* (`oe`, `oe.`, `oe/`, `oe\`, or virtual `@oe/` variants), then
a strict parser either accepts a canonicalizable official name or emits an error. It does
not lowercase the whole project namespace. Leading-dot/absolute/drive physical aliases must
not bypass reserved-root detection: require rejects them; tool project-path handling computes
the contained normalized relative path and rejects a top-level `oe/` collision. Never call
`ResolvePath` on a recognized virtual path. Reject unknown `@...` paths explicitly.

**Fixed tooling spelling:** `@oe/<segments>.lua`, lowercase, forward slash, extension included.
`script.list.files` adds official entries with this path, `origin: "embedded"`, and
`readOnly: true`; diagnostics use the same spelling (the Lua chunk-name marker is separate).
`script.read`, the shared check/params source resolver, and source write guards all use the
same manifest resolver. Inline `source` still takes precedence for check/params as today
(`engine/api/Commands.cpp:1258`), but a reserved path used as a diagnostic label must itself
be canonical. Source checking does not execute or validate imports.

A physical project `oe/` tree cannot shadow exports. Keep conflicting project paths out of
executable `files` entries and report them in an additive `reservedConflicts` list with their
physical path and explanatory hint; other project entries remain unchanged. Source tools reject
reserved physical aliases and official virtual writes. Project wrappers use `scripts/...`.
Read-only denotes the tool contract, not protection against Lua's base `rawset`; scripts must
not mutate official exports. This policy is consistent across desktop/web/Android; actual
cross-platform execution is **Unverified**.

##### D.2 Decision: embedded manifest digest in existing blueprint agreement

| Option | Advantages | Costs and compatibility |
|---|---|---|
| Include a manifest digest in `ContentHash()` | Existing lockstep/rollback and authority blueprints both consume it; manual state loading checks it; no new wire fields | Mismatch is discovered at the match barrier rather than lobby connection; conservative rejection for any official-source change |
| Negotiate a runtime/library version in Session handshake | Earlier explicit diagnostic; can model optional capabilities | New handshake data/compatibility policy and Session implementation changes; current v4 only compares protocol/game/mode/tick, not embedded bytes |

**Recommendation / adopted design:** include the complete official-source manifest digest and
a manually maintained primitive-contract revision in ContentHash. Do not add separate runtime
negotiation or change protocol v4 for M1a.

Use existing FNV-1a 64-bit (`engine/core/Image.h:24`) as a compatibility fingerprint, not a
cryptographic integrity/authentication guarantee. Canonical digest input is:

- Domain `ownengine.official-lua.v1`, then explicit primitive-contract revision `1`.
- Entries sorted by canonical module name, independent of filesystem enumeration.
- Each name and UTF-8 source encoded with decimal byte-length prefixes and delimiters.
  Normalize CRLF/CR to LF before both embedding and hashing; preserve other bytes, including
  whitespace, and reject BOM-prefixed sources in the generator.
- Digest is formatted as 16 lowercase hex digits. No build paths, timestamps, compiler names,
  absolute directories, or local scene epoch enter it. Source changes affect it automatically;
  behavior changes to primitives used by official modules require a revision bump.

Prefix the existing project manifest with a distinct record
`official-lua-v1:<primitiveRevision>:<digest>\n` in `Engine::ContentHash`, unconditionally.
This record has a fixed origin and is not a project file or part of the 4096-file bound.
OfficialModules supplies immutable manifest/source/digest metadata without creating a Lua VM.
Blueprint construction remains unchanged at `engine/app/EngineSync.cpp:101` and
`engine/app/EngineAuthority.cpp:72`; saved state resource checks remain at
`engine/app/EngineSync.cpp:223`. This closes embedded-source agreement, not agreement about
every unspecified engine/compiler behavior.

**Verified protocol paths:** FrameSync already writes/compares a uint64 blueprint
(`engine/net/sync/FrameSync.cpp:95`, `:110`); Authority already sends/compares its decimal
blueprint string (`engine/net/sync/Authority.cpp:81`, `:112`), including the late-join barrier
(`:89`). Session writes and checks its configured version (`engine/net/Session.cpp:264`,
`:297`), default 4 (`engine/net/Session.h:19`). No field layout, message number, or parser
change is required to change the blueprint value; `Session.h/.cpp` need no planned edits.

**Compatibility consequence:** matching new builds continue speaking v4. An old v4 build
may connect to the lobby, but its legacy ContentHash produces a different match blueprint
and the existing mismatch path refuses the match; late join is also refused. Do not advertise
old/new match interoperability or silently omit the digest when a project does not import
`oe.*`. No migration of snapshot files is needed because snapshots are engine-owned in-process
objects. M1a-3 tests verify matching peers and changed/legacy fingerprint rejection in all three
modes, plus authoritative late-join rejection, using the existing mismatch diagnostics. The
legacy provider reproduces the old project-only ContentHash input; an actual historical runtime
binary was not executed against the new build. See section 8 for platform evidence.

#### E. Presentation-only and recording contracts

**Verified dependencies:** camera matrices depend on aspect/full scene transform
(`engine/render/SoftwareRenderer.cpp:769`); `input.viewSize` reads device input
(`engine/script/ScriptHost.cpp:1007`). Headless sessions seed it from project window size
(`engine/app/Engine.cpp:75`), falling back to InputState defaults of 1280x720
(`engine/scene/Systems.h:69`). Canvas scale uses the first canvas in ID order, or height/720
without one (`engine/render/UI.cpp:1332`), not independently chosen per canvas.

Text shaping multiplies style size/spacing by canvas scale (`engine/render/UI.cpp:662`) and
rounds raster size (`:213`). It requests glyphs and kerning (`:250`); font fallback is explicit
(`:108`). `FontFace::GetGlyph` derives advance from font metrics plus faux-bold growth
(`engine/render/Font.cpp:139`, `:166`). Atlas caching/allocation is real (`:129`), but the
inspected code does not establish that different atlas packing order changes text width.
The defensible sources of differing results are dimensions, effective raster size, font
bytes/fallback, and style; do not claim atlas order alone causes desync.

Network input decode constructs a default InputState from declared keys/axes/look, not view
dimensions (`engine/app/EngineSync.cpp:14`, `:113`). Authority merges device-local dimensions
for a peer (`engine/app/Engine.cpp:482`, `:488`), but the dedicated host skips that merge
(`engine/app/EngineAuthority.cpp:315`). Thus dedicated host, clients, synchronized input, and
headless tools can see different view-dependent values. This is source evidence of permitted
differences, not a measured multi-peer test.

**Adopted exact documentation rule:**

> Camera projection, screen/canvas conversion, UI layout, and text measurement are
> presentation-only queries. Their results may depend on local view dimensions, canvas scale,
> font resources, and effective raster size. Use them for local HUD placement and visual
> layout. Do not use these results to branch simulation rules, select authoritative outcomes,
> seed randomness, determine physical movement, or change replicated state. For local pointer
> picking, transmit an explicit declared action or validated world-space intent through the
> supported input/RPC path; the server validates it against authoritative state. Recording a
> local result alone does not make it deterministic across peers. Any future simulation use
> requires a separate fully specified, shared-input query contract.

**Adopted units/visibility contracts:** screen input/output uses logical game-view pixels,
top-left origin, explicit camera and positive view dimensions; absent dimensions use the
documented local view state. Canvas output uses reference pixels with actual `UIScale`.
Initial queries follow the existing single effective canvas policy; they must not imply
independent multi-canvas rendering. World coordinates follow scene world transforms. Projection
returns depth and separate behind-camera/outside-frustum status; it does not clamp screen
positions. Plane queries require an explicit plane and return no hit for parallel/backward
intersection. M2a numerical thresholds and result fields are fixed in 6.1.G.1.

**Adopted measurement contract:** use the existing shaping rules at the effective view scale,
then report layout width/height in reference pixels. Rich text, font fallback, wrapping,
letter/line spacing, and bold are explicit style inputs. Exclude outlines/shadows and typewriter
reveal from layout size. Auto-shrink requires an explicit fitting box; whether to include that
option is excluded from M2b: both pilots use fixed sizes; callers supply resolved sizes
for already shrunk text (6.5). Cache keys
include effective scale/font/style, not just the string.

**Recording compatibility:** recording must precede Lua execution and physics creation
(`engine/app/EngineSync.cpp:200`, `engine/script/ScriptHost.cpp:2166`;
`docs/NETWORK_NOTES.md:384`). Resources are immutable and hot reload disabled while recording
(`docs/NETWORK_NOTES.md:386`, `engine/app/Engine.cpp:304`). Native restore captures scene,
input, Lua heap, and allocation cursor together (`engine/app/EngineSync.cpp:49`,
`engine/scene/Scene.cpp:25`). Stateless immutable embedded modules fit this contract; eager
runtime `require` before `sim.record_state` does not. Tool source inspection must not execute
official modules in the live simulation. `SandboxGlobals` currently uses a separate temporary
host (`engine/script/ScriptHost.cpp:1659`), not that engine's live Lua VM.

Queries may populate font/asset caches but must not mutate scene/game data, consume RNG, install
callbacks, or advance time. Font/asset caches are not included in the inspected NativeState
(`engine/app/EngineSync.cpp:49`), so restoring simulation must not depend on their allocation
history. M2 verification must compare authoritative/simulation state before and after unused
queries, and future evolution under the same recorded inputs; pixel/frame hashes are only
comparable at the same view/resources. Updating a cosmetic HUD can legitimately change rendered
pixels and, in lockstep/rollback, scene hashes containing UI. Presentation-only is not automatic
hash exclusion: isolate such writes or keep synchronized UI updates based on shared inputs.

#### F. DongleZoo baseline: source-reading simulation only

Counts below are predicted edit locations for the stated narrow task. Data blocks count as
zero edited functions; generated scene files count separately from hand-authored sources.
Numbers are not measured edits, timings, or proof of correctness. Generator references are
read-only evidence; no generator was executed.

| Section 7 task / concrete interpretation | Predicted files / functions to edit | Unrelated concepts required and source evidence | Support |
|---|---|---|---|
| Change player movement speed | N/A; 0 attempted edits | DongleZoo is a pointer-controlled drop puzzle (`games/DongleZoo/scripts/app.lua:81`, `:198`), not a moving-character game. Do not substitute physics fall speed or drop cooldown | Unsupported in this pilot; use default controller/Wickbound in a later baseline |
| Change the Resume button label in both locales | Runtime: 1 source file, 0 functions (`scripts/lib/i18n.lua:42`). To update stopped-editor text too: also 1 generated scene, still 0 hand-edited functions | 2 ancillary concepts: `t:pause.resume` localization mapping (`scenes/main.scene.json:2535`, `scripts/lib/ui.lua:58`); generated edit-time English text (`tools/make_content.py:47`, `:185`). UI runtime overwrite means editing only scene text is insufficient | Already a simple runtime edit. Generator execution/visual fit remains unverified |
| Add one more coin-pack shop product using existing reward behavior | 2 authored files: `scripts/lib/defs.lua:67` data and `tools/make_content.py:24`, `:306` (`scene()`, 1 function); plus 1 generated `scenes/main.scene.json`, total predicted 3 files | 3 ancillary concepts: duplicate product-ID lists; fixed row geometry/dialog height; action/entity naming (`scripts/app.lua:430`, `:604`; `tools/make_content.py:315`). Fourth row extends below the existing 1010-high box, requiring layout work. Existing coin reward dispatch at `scripts/lib/meta.lua:158` need not change | Product reward data supported; the strict task of adding a product without layout/code work is unsupported. Host-store SKU provisioning is outside this local estimate and unverified |
| Change merge combo multiplier from 0.25 to another factor | 1 file, 1 function: `scripts/app.lua:257`, expression at `:263` | 2 ancillary concepts colocated in the function: save-stat/mission updates (`:265`); audiovisual/UI effect dispatch (`:271`). They need not be changed or fully understood. If changing only an animal's base score instead, use `scripts/lib/defs.lua:16`: 1 file, 0 functions | Supported; current rule location is inside a long orchestration file |
| Increase pause popup width and change its title in both locales | Persistent generator route: 2 authored files (`tools/make_content.py:285`, `scene()`, 1 function; `scripts/lib/i18n.lua:41`, data), plus generated scene = 3 files. Direct scene route: 2 files, 0 functions, but regeneration overwrites geometry | 3 ancillary concepts: generator ownership; named panel/localization tag association (`scenes/main.scene.json:2417`, `:2485`); anchor/size units (`tools/make_content.py:197`). No projection or font-width estimate is involved in this fixed popup | Supported with scene-authoring work; fitting longer text and visual verification unperformed |

Paths in baseline cells are relative to `games/DongleZoo/` unless fully qualified. A new item
effect, new reward type, or real storefront integration would be a different task with more
files; do not extrapolate the coin-pack estimate to those cases. Three requested edit tasks are now measured in 6.3; broader human editing difficulty
remains M4 work.

**Acceptance revision:** retain a one-location target for a scalar rule or localized label.
Do not claim every task can edit one file: generated geometry plus localized text has separate
sources, and movement speed is not applicable here. Shop data/row unification is a concrete M4
goal. Measure reduction in unrelated concepts and avoid duplicating sources of truth.

#### G. Decision closure and remaining evidence

| Item | Status / adopted outcome | Evidence / next step |
|---|---|---|
| Snapshot-aware world token | **Decision, implemented M1a-1/2:** Scene epoch copied/restored with NativeState, plus private Lua session/handle records; `scene.epoch() -> string`; pure Lua alone was insufficient | B.1; executed desktop lifetime/native-restore evidence in section 8; automatic rollback with official handles and platform checks remain later integration work |
| Reserved names and virtual source paths | **Decision, implemented M1a-2:** lowercase official grammar, accepted dotted/slash aliases, backslashes/uppercase rejected, one tool spelling `@oe/ui.lua`; no project fallback | D.1; `engine/script/OfficialModules.cpp:44`, `:63`, `:72`; desktop tooling/package evidence in section 8 |
| Embedded version agreement | **Decision, implemented M1a-3:** include manifest/primitive-revision fingerprint in ContentHash; existing blueprint comparison, v4 wire unchanged | D.2; engine/app/EngineSync.cpp:77, :103, :225; engine/app/EngineAuthority.cpp:72; OfficialLuaBlueprintMismatchBarriers and recording tests pass on desktop/Wasm; section 8 |
| M2a coordinate schema/signatures | **Decision:** named requests, explicit camera, local optional view, statuses and tolerances below | G.1 compares actual pointer/nameplate/50-query callers; M2b measurement/fitting remains deferred; multi-canvas rendering outside scope |
| Performance and actual edit difficulty | **Verified for scoped pilots:** M2 query timing and M4 applicable edit tasks executed | 6.4/6.5 record query method/results; 6.7 records executed M4 tasks. Static M0 counts remain estimates; human editing time unmeasured |

##### G.1 Decision: M2a named request tables (alternative B)

Recorded before implementation. All four native queries take one request table;
registry commands use the same named object schema. M2b text measurement is not
decided/implemented here. Single effective canvas behavior remains unchanged.

| Real caller | Alternative A | Alternative B (chosen) |
|---|---|---|
| DongleZoo normalized pointer -> z=0 world | camera.screenToPlane(self.camera, {x=mx*w,y=my*h}, plane, options) | camera.screenToPlane({camera=self.camera,screen={x=mx*w,y=my*h},plane=plane}) |
| OnlineRPG name plate -> canvas | camera.worldToScreen(id,position,options), then ui.screenToCanvas(canvas,screen,options) | camera.worldToScreen({camera=id,position=position}), then ui.screenToCanvas({screen=screen}) |
| 50 points/frame | Reuse position/options tables, mutate position for each call; positional arguments avoid outer table | Reuse request.position and request.view for all 50 calls; mutate coordinates, no fresh Lua request table in the loop |

Call-site rewrites used for the decision (A is hypothetical, B is implemented;
plane, position, view and cameraId are the caller's existing values):

```lua
-- DongleZoo pointer, A / B
local worldA = camera.screenToPlane(self.camera, {x=mx*w,y=my*h}, plane, {view=view})
local worldB = camera.screenToPlane({camera=self.camera,screen={x=mx*w,y=my*h},plane=plane,view=view})
-- OnlineRPG label, A / B
local pixelsA = camera.worldToScreen(cameraId, position, {view=view})
local labelA = ui.screenToCanvas(nil, pixelsA, {view=view})
local pixelsB = camera.worldToScreen({camera=cameraId,position=position,view=view})
local labelB = ui.screenToCanvas({screen=pixelsB,view=view})
-- 50 labels, A: reusable position/options; B: reusable request/position/view.
local options = {view=view}
for i=1,50 do position.x=points[i].x; position.y=points[i].y; position.z=points[i].z
  consume(camera.worldToScreen(cameraId, position, options))
end
local request = {camera=cameraId,position=position,view=view}
for i=1,50 do position.x=points[i].x; position.y=points[i].y; position.z=points[i].z
  consume(camera.worldToScreen(request))
end
```

B avoids a separate positional-to-registry adapter, spells units/view/plane explicitly,
and is easier to extend/validate. Its outer request allocation is avoidable by reuse;
both alternatives still allocate result tables and cross the native binding. The
benchmark in 6.4 measures the implemented Lua/native path, not a guessed allocation cost.
Pilot callers may use short-lived tables for clarity; no persistent native cache is added.

Final signatures (Lua and command names match):
camera.worldToScreen({camera, position={x,y,z}, view?={width,height}})
camera.screenRay({camera, screen={x,y}, view?={width,height}})
camera.screenToPlane({camera, screen={x,y}, plane={point={x,y,z},normal={x,y,z}}, view?})
ui.screenToCanvas({screen={x,y}, canvas?=idOrName, view?={width,height}})

Camera is mandatory and must carry Camera; a full world transform is used, including
parents/rotation. Missing view uses input.viewSize() local state (headless: project
window or 1280x720 fallback). Dimensions are positive integers, maximum 16384 each.
Screen points use logical game-view pixels, top-left origin; no clamping.

| Result | Fields / policy |
|---|---|
| Projection | x,y (unclamped; nil only when homogeneous division is undefined), depth (positive forward world distance), ndcDepth (OpenGL -1..1 depth), visible boolean, status visible/outside/behind. Outside includes near/far clipping; behind means depth<=0 and takes priority |
| Ray | origin={x,y,z} on near plane, direction={x,y,z} normalized toward far plane; works for orthographic/perspective |
| Plane | hit=true, point={x,y,z}, distance from ray origin; or hit=false, reason=parallel/backward, no point/distance. Never nil for a valid no-hit |
| Canvas | x,y in top-left reference pixels, width,height in reference pixels, scale=pixels/reference pixel, canvas=id or nil when fallback is used |

Canvas omission uses UIScale's first canvas in ID order or height/720 fallback.
An explicit canvas must be that effective canvas; selecting another is an error rather
than pretending independent multi-canvas support. Conversion is scale-only; anchors
remain a caller's widget placement policy.

Malformed/nonfinite inputs, missing components, degenerate transforms/planes/projection,
invalid dimensions or a noneffective explicit canvas raise descriptive errors (Lua)
or ApiError invalid_query (commands). No query-level nil result for invalid input.
Normalized ray/plane denominator abs<=1e-6 is parallel; distance<-1e-5 is backward,
with smaller negative roundoff snapped to zero. Homogeneous abs(w)<=1e-6 is undefined.
Tests allow 0.05 logical pixels for projection/raster agreement, 1e-3 world units for
round trips, and 1e-5 for normalized vectors; these are validation tolerances, not
simulation guarantees. These presentation queries must follow the exact rule in E.

### 6.2 M1a implementation plan (M1a-1/2/3 implemented)

M1a implements the identity, official-source loading/tooling, and digest foundation. It may
provide minimal `oe.entities`/`oe.ui` constructors and existing primitive forwarding to test the
contract. Full widget ergonomics and game migrations belong to M1b; projection/text queries
belong to M2. No timers/dialog owner framework, gameplay input rewrite, or project migrations
are included. The map retains the completed M1a plan; release evidence and remaining device checks are in section 8.
The implementation and executed verification are recorded in section 8.

#### File-level change map

| Planned file | Change / reason | Existing integration evidence |
|---|---|---|
| `engine/scene/Scene.h`, `Scene.cpp` | Add runtime epoch/getter, checked Clear increment, copy preservation, and ReplaceWith; exclude epoch from JSON | `Scene.h:30`, `Scene.cpp:20`, `:35`, `:166`, `:214` |
| `engine/app/Engine.cpp` | Use ReplaceWith for adopting isolated prefab scene; verify scene loads, stop, undo/redo invalidate through Clear/FromJson while VM reset remains unchanged | `:122`, `:157`, `:244`, `:275`, `:1005`, `:1017`, `:1072`, `:1114` |
| New `engine/script/OfficialModules.h/.cpp` | Canonical namespace/origin parser, immutable manifest lookup/source/digest, common source resolver; no mutable native handle registry or live Lua execution for inspection | Current loader `engine/script/ScriptHost.cpp:1111`; project source resolution `engine/api/Commands.cpp:1258` |
| `engine/script/ScriptHost.cpp` (header only if a shared loader declaration is needed) | Add guarded epoch binding, embedded require branch/cache/cycle diagnostics; preserve project require and attachment reload behavior | `:1111`, `:1599`, `:1771`, `:1850`, `:2171` |
| New `engine/script/lua/oe/entities.lua`, `ui.lua` | Private snapshot-compatible handle records/session token; validate all operations; UI forwards focus/focused and uses entities validator | Existing primitives/prelude `engine/script/ScriptHost.cpp:1152`, `:1622` |
| `CMakeLists.txt`; generated build-only `generated/EmbeddedLuaModules.cpp` | Generate embedded sources/manifest with configure dependencies, normalized UTF-8/LF inputs, and stable names; append generated C++ to shared engine sources | Embedded font precedent `CMakeLists.txt:132`; generated files stay out of Git |
| `engine/api/Commands.cpp` | Register read-only scene.epoch query; add virtual-source list/read/check/params resolution and write guards; report reserved conflicts | `:1227`, `:1247`, `:1258`, `:1293` |
| `engine/app/EngineSync.cpp` | Prefix ContentHash with official fingerprint; inspect existing Scene/Lua copy and restore for epoch coverage without duplicate state | `:49`, `:58`, `:76`, `:101`, `:223` |
| `tests/tests.cpp` | Add the focused test cases below using Engine::Call/TempProject; direct Scene fixture only where Clear/copy/overflow cannot be driven through commands | Snapshot fixture `:1876`, branching fixture `:1889`, check/params fixture `:9552` |
| `docs/SCRIPTING.md`, `docs/API.md`, `docs/LUA_USABILITY.md` | Publish fixed contracts and loader limitations, regenerate API, record milestone evidence and platform gaps | Definition of done in `docs/DESIGN.md:93` |
| `runtime/web/oe_player.js/.wasm`, `runtime/web/README.md`; affected Android runtime artifacts/notes | Refresh the actual shipped players and record what was executed | Packaging paths recorded in 6.1.D |

Review-only integration paths: `engine/app/EngineAuthority.cpp:72`,
`engine/app/EngineNetwork.cpp:71` (fresh preview Scene copy),
`engine/net/sync/FrameSync.cpp:110`, `engine/net/sync/Authority.cpp:112`,
`engine/net/Session.h:19`, and ScriptHost snapshot heap restore. No wire-format, Session,
renderer, game, or template changes are expected in M1a. If implementation reveals a necessary
change, document the reason rather than treating this map as proof that no change is needed.
Updating the AGENTS/CLAUDE code map requires authorization in that future task; both are
explicitly excluded from this follow-up.

#### Order and proposed commit boundaries

1. **M1a-1: identity primitive and validation.** Implement Scene epoch/copy/ReplaceWith,
   epoch binding/query, and focused lifetime/snapshot tests. Verify same-scene replacement and
   restore before implementing cached handles. Update scripting/API contracts and mark runtime
   rebuilds pending if they are not performed in this unit.
2. **M1a-2: official source foundation.** Add manifest generator/parser/loader and virtual-source
   tools together, so embedded imports are inspectable as soon as they exist. Add minimal handle
   modules, primitive forwarding, cycle/error/read-only tests, and reserved-conflict reporting.
   Preserve project behavior and document required-only helper reload limitations.
3. **M1a-3: agreement and release verification.** Include manifest/revision in ContentHash;
   run matching/mismatching lockstep/rollback/authority/late-join checks and manual recording
   regressions. Rebuild affected players, verify packaged module resolution, and record evidence
   or exact remaining platform checks. Finalize M1a work-log entries; leave M1b pilots unchecked.

Each future commit must build and pass its relevant checks. These are proposed review units;
the M1a-1/2 tasks implement only their authorized units and create no commits or pushes.

#### Test names and observable assertions (candidates)

| Candidate in `tests/tests.cpp` | Required assertion |
|---|---|
| `SceneEpochClearCopyAndReplacement` | Clear/FromJson/ReplaceWith invalidate; ordinary delete/create does not bump epoch; copies retain it; failed format validation is unchanged; checked overflow never wraps |
| `LuaHandlesRejectDestroyedAndReplacedWorlds` | Destroyed IDs and same-scene/same-name replacement fail; component loss fails; stale handles do not silently re-find |
| `LuaHandlesRestoreWithNativeSnapshots` | Saved closure/handle works after load_state; discarded-branch registry records disappear; entity creation after rewind cannot target the future object; automatic rollback/prediction preserves saved handles |
| `LuaHandleSessionAndEditorReplacement` | Stop/new VM needs new handles; undo/redo and prefab isolation/close reject old incarnation records even where IDs recur |
| `OfficialLuaRequireAliasesAndReservedNames` | Three accepted aliases return the same export; uppercase/backslash/mixed/traversal/physical-path bypasses fail; project `oe/` cannot shadow or serve unknown names |
| `OfficialLuaRequireCycleFailureAndBudget` | Cycles give a bounded diagnostic; failed cache entries are cleared; original source path/line and instruction budget are preserved |
| `OfficialLuaSourceToolsAreReadOnly` | List/read/check/params share canonical virtual paths; conflicts are reported; writes rejected; inspection creates no live simulation Lua state before record_state |
| `OfficialLuaProjectImportsAndReloadCompatibility` | Nonreserved imports keep existing semantics; required-only helper edits now reload transactionally (6.10); official exports survive reload and scene transition with stale handles rejected |
| `OfficialLuaUIForwardsFocusPrimitives` | Local `ui` alias keeps focus/focused signatures and uses the shared identity validator |
| `OfficialLuaManifestFingerprintIsStable` | Enumeration/CRLF normalization yields same fingerprint; changed source/revision changes it; metadata paths/timestamps and Scene epoch do not affect it |
| `OfficialLuaBlueprintMismatchBarriers` | Use controlled fixture digests to verify ContentHash-to-blueprint propagation, matched startup, mismatched refusal, and authority late-join refusal through existing messages |
| `OfficialLuaRecordingRestoresCacheAndHandles` | Enable recording before Lua, require/save/branch/restore, then verify cache/closure/handle behavior and future state; querying epoch is read-only |

Use a test-only fingerprint provider/fixture helper, not a production CLI option that can spoof
embedded agreement. Existing regressions to retain include
`SimulationReferenceSnapshotAcrossSceneChanges` (`tests/tests.cpp:1876`),
`SimulationNativeSnapshotBudgetAndBranching` (`:1889`),
`NetworkFrameSyncMismatchBarrierAndLargeWorldHash` (`:2004`), and `ScriptCheckAndParams`
(`:9552`). The first four tests now use official handles while retaining the M1a-1 guarantees.
The five loading/tooling/reload/focus tests are implemented in M1a-2. Fingerprint, blueprint,
and combined recording-agreement tests are implemented and pass in M1a-3. The snapshot test covers manual native restoration and
discarded branches. Automatic rollback/prediction with official handle records remains a later
integration assertion; the existing networking regressions cover the current snapshot machinery.

#### Runtime rebuild and verification scope

Scene/binding/loader/ContentHash C++ and embedded Lua enter every player. Rebuild desktop `oe`,
`oe_tests`, and `oe_player`; refresh web JS/Wasm and build/test the Wasm test target where
available; rebuild Android `liboe_player.so` for supported shipped ABIs. Repackage pilot data
when needed, but no new official-module files belong in game.pak. Package smoke tests must prove
imports work without the source checkout on desktop, web, and Android. Browser/device execution,
runtime freshness, and all performance results are **Unverified** in this documentation task.
Record missing SDK/device/toolchain work explicitly; a successful desktop test does not close
the web/Android lines. Existing official modules change with the runtime, not hot reload.

### 6.3 M1b UI/entity pilots (2026-10-06)

Gap analysis was performed before editing either project wrapper. Baseline captures
use software rendering, fixed date 2031-03-03, and the same pointer/step sequence.

| Actual wrapper operation / evidence before migration | M1a surface | M1b action |
|---|---|---|
| DongleZoo lib/ui.lua:9 id lookup/cache, :20 kind discovery; app.lua:223 tween primitive ID | Entity handles require an explicit component; validated id exists | Widget discovery checks all supported primary UI components; ambiguity error unless explicitly selected. Project UI.id acquires a fresh widget then returns its validated id; remove raw ID/kind caches |
| DongleZoo lib/ui.lua:34 set, :43 text; app.lua and ui/button.lua callers | get/set require a component name | Add selected-component widget get()/set(values), keep explicit get(type)/set(type,values) compatibility; text checks UIText/UIButton/UIInput |
| DongleZoo lib/ui.lua:39 show; OnlineRPG widgets.lua:43-52 show/hide/visible | Partial scene.set foundation only | Add own-widget visibility methods; parent layout/render visibility already suppresses descendants, with no recursive child rewrites |
| OnlineRPG widgets.lua:14 find, :24 list; hud.lua:24-31 and connect.lua:208-214 | Explicit find/tryFind; no widget list | Add strict automatic UI selection and deterministic numbered list(prefix,count) |
| OnlineRPG widgets.lua:30 get, :35 set; hud.lua:94-97 inventory fields | Generic component get/set | Same partial-update widget methods; generic get reads text/value/selected as appropriate, no speculative value helper |
| OnlineRPG widgets.lua:55 focus; hud.lua:79 chat focus | Module forwards ui.focus/focused | Add Widget:focus() capability check for UIButton/UIInput/UISlider/UIDropdown; keep module-level native signatures unchanged |
| DongleZoo localization, RGB/number formatting, toast lifetime, textWidth; OnlineRPG space/project and bubbleSize | Project policy / M2 math | Retain unchanged. No new C++ binding, input synthesis, dialog owner framework or engine query |

Widget records hold an oe.entities handle in the Lua heap. Operations validate its
session/epoch/existence/component before mutation. Retained widget handles never re-find.
DongleZoo's compatibility wrapper deliberately performs fresh named acquisition per call;
OnlineRPG's per-instance handles are acquired in onStart, which is rerun for replacement
script instances. Neither wrapper uses a hidden native registry or changes global ui.

#### Pilot results and measured edit tasks

OnlineRPG UI entities have no NetSync component (audited all UI entities in
scenes/main.scene.json). ReplicatedWorld enumerates NetSync entities only
(engine/app/EngineAuthority.cpp:80); local pointer/text/undeclared keys are merged
on clients (engine/app/Engine.cpp:481). The replacement wrapper changes only validated
UI fields/focus, not authoritative gameplay, actions, RNG, timers or game data.
The existing hud.lua still writes cosmetic avatar/item fields and debug chat/bag game
data; this migration does not claim all HUD scene writes are automatically excluded
from every kind of hash. Existing authority/offline tests and real-process tests pass.
Connect/hud widget handles are per-script-instance and acquired in onStart
(connect.lua:207, hud.lua:22). Entering an authoritative world resets script instances;
game scene changes stop/start instances (Engine.cpp:237). There is no module-level
handle cache to carry into a replacement world. Snapshots restore widget and entity
records together. DongleZoo no longer caches raw IDs by name.

| Screenshot checkpoint | Before software hash | After software hash | Result |
|---|---|---|---|
| zoo-menu | bcfda4dfb05568df | bcfda4dfb05568df | Identical PNG bytes |
| zoo-play | 50468c5f1f131f56 | 50468c5f1f131f56 | Identical PNG bytes |
| zoo-pause | 73791447d24e945f | 73791447d24e945f | Identical PNG bytes |
| rpg-title | 58b738d0cf7dd3c7 | 58b738d0cf7dd3c7 | Identical PNG bytes |
| rpg-hud | 48a38cbecd374dea | 48a38cbecd374dea | Identical PNG bytes |

These are rendered pixel hashes, not network blueprints: the latter intentionally
change because official Lua source changes the manifest digest. DongleZoo view is
540x960, OnlineRPG 1280x720. Each starts at fixed date 2031-03-03, steps 3 frames, then
named clicks and 3 steps: Home:play, HUD:pause; or Offline. No save-dir persists data.
All after images were inspected. Early menu/pause captures retain the pre-existing
entrance fade/tutorial overlay at frame 3/9; the edit captures below also inspect the
fully settled pause dialog. Screenshots and driver outputs are in build/m1b/.

The following are **executed edits**, restored byte-for-byte afterward. Counts describe
edit locations, not completion times or a measured human comprehension score.

| Section 7 task | M0 predicted files / functions | M1b measured files / functions | Performed change / check | Ancillary concepts still needed |
|---|---|---|---|---|
| ② Resume label in both locales | Runtime 1 / 0; optional editor scene +1 | Runtime 1 / 0 | i18n.lua Resume/계속하기 -> Continue/이어하기; both runtime labels asserted and pause images captured | 2: t:pause.resume mapping; runtime localization overwrites scene labels. Stopped-editor text was not changed |
| ⑤ Pause width and title in both locales, generator route | 3 / 1 (2 authored +1 generated) | 3 / 1 (2 authored +1 generated) | make_content.py scene() width 520->580, i18n title Paused/일시정지 -> Break time/휴식; invoked only scene(), asserted width/title and inspected images in both locales | 3: generator ownership, panel/title localization association, reference width/anchor units |
| ④ Combo multiplier | 1 / 1 | 1 / 1 | app.lua App:onMerge 0.25->0.5; spawned tier-2 result and exercised onMerge at combo 2; gain 2 equals rounded base*1.5 | 2 adjacent concepts: save/mission updates and audiovisual effect dispatch; neither changed. No projection/input knowledge required |
| ① Player movement speed | N/A | N/A, not attempted | DongleZoo is a drop puzzle | No substitute task |
| ③ Data-only shop product | Unsupported strict data-only task | N/A, not attempted in M1b | Requires duplicated product/row geometry edits per M0 | Deferred, no substitute task |

Counts did not improve for these scalar/data edits: localization was already one place,
and generator ownership/scoring orchestration are unchanged by the deliberately narrow
M1b migration. Common widget code/lifetime checks are now shared; M4 remains responsible
for restructuring app.lua. No timers/owner framework, M2 queries, M3 input, M4 split,
M5 templates, or Wickbound migration was implemented.

### 6.4 M2a camera/canvas coordinates (2026-10-06)

#### Adopted implementation and evidence

Named request alternative B is decided in 6.1.G.1. The native camera table and global
ui.screenToCanvas use shared read-only QueryCoordinates, also used by registry commands.
The following are inspected facts, not separate approximate projection formulas:

| Contract / change | Evidence |
|---|---|
| Explicit camera matrices, complete parent/world transform; active camera renderer policy preserved | engine/render/SoftwareRenderer.cpp:769 (MakeCameraView), :788 (MakeSceneView delegates); engine/render/Renderer.h:84. GPU and software continue to consume the same RenderView |
| Shared request validation, projection, inverse near/far ray and plane intersection | engine/app/CoordinateQueries.cpp:45; camera required, local/default or explicit logical view, statuses, unclamped coordinates and no-hit policies in 6.1.G.1 |
| Effective canvas scale is existing rendering scale | engine/render/UI.cpp:1332; engine/app/CoordinateQueries.cpp:58. First canvas by ID or height/720 fallback; explicit noneffective canvas rejected |
| Guard-wrapped Lua / read-only command surface | engine/script/ScriptHost.cpp:572 and :1662; engine/api/Commands.cpp:975 (Params, mutates=false) |
| Minimal official forwarding | engine/script/lua/oe/ui.lua:4 forwards native screenToCanvas; no widget redesign, native registry or global ui mutation. Digest 58adf79c7598d716 -> 2b06cf1f108cb7cf; primitive revision remains 1 |
| DongleZoo pointer and floating-label projection | games/DongleZoo/scripts/app.lua:81 and :221. Explicit z=0 plane; top-center anchor offset retained. Layout fitting, scoring/save/localization and textWidth remain project policy |
| OnlineRPG nameplates/bubbles | games/OnlineRPG/scripts/lib/space.lua:19. Camera parameter renamed cameraId to avoid shadowing the native camera table. Existing depth<0.1 suppression/offscreen policy retained; no text-width/network rule change |

World/UI queries are presentation-only under the exact rule in 6.1.E and SCRIPTING.md.
The local DongleZoo pointer picking behavior is preserved; this migration does not authorize
using unvalidated view-dependent results as server simulation rules. The OnlineRPG helper is
used for local nameplate/bubble placement, not authoritative movement or scoring.

#### Visual equivalence

The five initial menu/play/pause/title/HUD captures were taken before implementation.
Additional drop/bubble/resized-view baselines were captured with the preserved pre-M2a
executable and exact legacy pilot formulas, then the intended source bytes restored in finally.
Their five overlapping captures match the initial baseline. This supplemental comparison
is legacy-runtime execution, not a claim that all richer captures preceded source editing.
Final new-runtime captures were repeated after the last validation correction.

Method: fixed date 2031-03-03, tool pipe session, software inline render.screenshot; three
initial frames, UI-layout-centered button clicks and three frames per click. DongleZoo
resumes pause, presses/releases at (200,500) for two frames each, then steps 30 frames.
OnlineRPG enters Offline, calls HUD:say(1,'P1','M2a bubble'), steps three frames, then
sets local view 1600x900 with input.mouse and steps three more. Drivers/results/PNG evidence
are in build/m2a/compare.py, capture-legacy.py, rich.py and *-rich-results.json.

| Capture | Logical view | Before = after hash | Result |
|---|---|---|---|
| zoo-menu | 540x960 | bcfda4dfb05568df | Identical hash and PNG bytes |
| zoo-play | 540x960 | 50468c5f1f131f56 | Identical hash and PNG bytes |
| zoo-pause | 540x960 | 73791447d24e945f | Identical hash and PNG bytes |
| zoo-drop | 540x960 | 5df697769efd751c | Identical hash and PNG bytes |
| rpg-title | 1280x720 | 58b738d0cf7dd3c7 | Identical hash and PNG bytes |
| rpg-hud | 1280x720 | 48a38cbecd374dea | Identical hash and PNG bytes |
| rpg-bubble | 1280x720 | d14043d265cf356a | Identical hash and PNG bytes |
| rpg-bubble-900 | 1600x900 | cb7a8855a0fa399c | Identical hash and PNG bytes |

The pointer sample changes from (-0.972222313,-0.277777513) to
(-0.972222269,-0.277777493): maximum difference 4.4e-8 world units. Drop and 900px
bubble/nameplate images were inspected directly; no layout regression was observed.
OnlineRPG has no UICanvas in its current scene: the actual renderer's height-720 fallback
matches the old 720 assumption. Therefore even the 900px test intentionally preserves
pixels. Other canvas ratios and full camera rotations are verified by coordinate tests,
not claimed as an existing-game visual change.

#### Tests and approximate cost

CoordinateProjectionRayPlaneAndCanvas (tests/tests.cpp:2198) covers perspective/orthographic,
multiple explicit/local view sizes, parent rotation, outside/behind/undefined projection,
near/far clipping, normalized world-to-screen-to-ray round trips, parallel/backward/invalid
planes, effective/fallback canvas, unequal-aspect match=0/0.5/1, and rejected second canvas.
CoordinateQueriesRasterAndRecordedFuture (tests/tests.cpp:2277) checks a projected cube
against the software renderer's actual entity-ID pixel buffer. Recording begins before Lua;
all four query paths preserve frame/epoch/scene/game data and replayed RNG/timer future after
save/branch/restore. Queries also work before a simulation step in a tool session.

Desktop Release build and all 226 tests pass, zero failed checks, exit 0, including
GpuRendererMatchesSoftware, TemplateGamePlaythrough, DongleZooGame, WickboundSample,
OnlineRPGGame and identity/agreement regressions. The separate backend build and actual-process
tests/online_rpg_test.py pass (exit 0); its own spawned PIDs are cleaned by the harness.
All four oe exec queries and official @oe/ui.lua script.check pass (zero errors/warnings).
API.md was regenerated. Build/log evidence: build/m2a/build.log, tests.log, backend-build.log,
rpg-e2e.log and cli-results.json. Runtime results are recorded in the M2a work-log entry below.

Approximate desktop performance: 100 alternating batches, each 50 worldToScreen calls,
after ten warm-up batches. Python perf_counter times script.eval through JSON pipes; Lua
reuses one request/position table. Median batch 1.287ms, mean 1.291ms, empty-loop/IPC median
1.197ms; the median difference suggests about 1.81 microseconds/query. This includes Lua/JSON
conversion and matrix work, and subtracting IPC is noisy; it is not a pure math benchmark or
portable latency guarantee. No persistent matrix cache or hidden native state was added.
Method/data: build/m2a/perf.py and performance.json. Result-table allocations remain.
Timing was sampled while desktop/backend regression work was also active; treat it as
an approximate loaded-machine observation, not an isolated performance limit.

M2b text measurement/fitting, M3 input, M4 app split and M5 templates remain out of scope.
Wickbound scripts and CLAUDE.md/AGENTS.md are unchanged. Proposed future code-map addition
only: engine/app/CoordinateQueries (shared presentation query requests), plus renderer
MakeCameraView and camera/ui native tables; no instructions file was edited here.

### 6.5 M2b text measurement

#### Decision recorded before implementation (2026-10-06)

Signature: ui.measureText({text, style?={font,size,letterSpacing,lineSpacing,bold,rich},
wrapWidth?, view?={width,height}, canvas?=idOrName}). Lua and the read-only registry command
share the named request shape and M2a local view/single effective canvas validation.
Style defaults match UIText: default font, size 32, letterSpacing 0, lineSpacing 1,
bold false, rich true. wrapWidth is reference pixels; omitted/zero means no wrapping.
Return {width,height,lines,lineWidths,scale}, all lengths in reference pixels; lines is
an integer, lineWidths is a dense list. Empty text follows the renderer: one empty line,
zero width and primary-font ink height. No paintedBounds field in M2b. Outline/shadow and
typewriter reveal do not enter shaping/layout; measurement does not accept fitting or
paint fields. Caller supplies the resolved size for already shrunk text.

Fitting is excluded: DongleZoo Toast1:text uses size 30 with no minSize and an expanding
pill (scripts/lib/ui.lua:77, tools/make_content.py:386); OnlineRPG Bubble Text uses size 18,
wrap true, richText false and no minSize (scripts/hud.lua:60, tools/make_scene.py Bubble Text).
Neither requires auto-shrink. Bubble height must follow actual wrapped lines, while the
existing maximum width and padding remain project policy. A future fitting query must take
an explicit box/minSize and reuse the renderer's one-reference-pixel shrink loop.

Use UI.cpp Shape(ParseRich(...), ResolveFont(...)) at size*UIScale, letterSpacing*UIScale
and wrapWidth*UIScale, then divide widths/heights by scale. The same TTF lround/max(1)
raster size, pixel-font fractional unit, fallback, kerning, faux-bold advance growth,
CJK/space wrapping and final-line advance apply. Layout size is not glyph paint bounds;
negative bearings/outline/shadow may paint outside it. Render code and old MeasureText API
remain unchanged in behavior. Numeric comparison tolerance is 0.01 reference pixels.

Invalid inputs raise Lua errors / invalid_query (standard registry schema errors still apply),
never whole-query nil; existing project-path errors are preserved. Limits: text <=65536 UTF-8 bytes; size in [0.125,1024], effective
size*scale <=1024, abs(letterSpacing)<=1024, lineSpacing in (0,16], wrapWidth in [0,65536].
Unknown style fields/types are errors. Missing/unloadable project font follows renderer
fallback to default; it is not a new font-load error policy. Project font paths must resolve
inside the project. Bounds protect query work and atlas sizes without changing rendering.

Presentation-only rule from 6.1.E applies verbatim. Existing asset/glyph caches may warm;
no measurement-result/native mutable cache is added. Metrics depend on immutable font bytes,
code point, effective integer size and bold, not atlas page/UV placement or allocation order
(Font.cpp GetGlyph computes advance before shelf packing). Tests must warm unrelated glyphs
and replay recording with warm caches. Caller cache keys include text/font resource version,
size, spacing, bold/rich, wrapWidth and effective scale; invalidate for view/canvas/font reload.

#### Implementation and evidence

| Change / fact | Source evidence |
|---|---|
| TextMetrics MeasureText overload shares the exact rich/shaping path and returns reference units | engine/render/UI.cpp:1532; canonical ParseRich/Shape at :150/:206; renderer ShapeTextOf at :662. Existing renderer and old MeasureText body are unchanged |
| Shared native query validates local/effective canvas scale and text/style bounds | engine/app/CoordinateQueries.cpp:58/:64; engine/render/UI.cpp:1332 |
| Guard binding/read-only Params command/official forwarding | engine/script/ScriptHost.cpp:584/:1666; engine/api/Commands.cpp:987; engine/script/lua/oe/ui.lua:4 |
| Asset/glyph caches exist but logical metrics do not use page placement | engine/render/UI.cpp:109 (ResolveFont), :249 (fallback); engine/render/Font.cpp:134 (GetGlyph), :138 (advance), :151 (bold), :179 (shelf packing); no new result cache |
| Toast estimate -> actual label font/style | games/DongleZoo/scripts/lib/ui.lua:60/:77. Size/max/padding/lifetime unchanged |
| Bubble estimate -> natural and wrapped measurements; reflow for live view/style | games/OnlineRPG/scripts/hud.lua:53/:67/:133. Existing cap/padding/message life and server rules unchanged |

TextMeasurementMatchesRenderedLayout (tests/tests.cpp:2303) compares actual LayoutUI
natural width and line height with query lengths at 540/720/900 heights and effective
UICanvas scale. It covers default/pixel/project Jua/missing-font fallback; CJK/multiline/
space wrapping, rich tags, bold, letter/line spacing, empty text and 65536/65537-byte bounds,
invalid fields/types/path/width. Known pixel dimensions and faux-bold growth are checked.
Outline/shadow/typewriter leave measured/natural layout unchanged; BuildUIQuads produces
actual glyph quads. Layout dimensions are intentionally not asserted equal to painted ink.
TextMeasurementCacheOrderAndRecordedFuture (tests/tests.cpp:2362) warms unrelated glyphs,
sizes/pages and repeats identical metrics, then checks recording begun before Lua, save/
branch/restore, Lua forwarding and unchanged scene/game/frame/epoch/future RNG/timer state.

#### Before/after visual classification

All six baseline images were captured with the previous executable before source edits.
Each opens a fresh project pipe session, fixed date 2031-03-03, three frames, clicks Home:play
or Offline using actual UI layout, steps three frames, injects a toast/HUD:say and steps
three frames. Logical views: DongleZoo 540x960; OnlineRPG 1280x720. Strings:
EN Wonderful! A new friend joined!; KO 새로운 친구가 동물원에 왔어요!;
JA 新しい友だちが動物園に来ました！. Driver, box dimensions, hashes and PNGs:
build/m2b/capture.py, before-results.json/after-results.json and before-/after-*.png.
Each changed before/after image was directly inspected; no clipping/layout regression
was observed. Font coverage is a separate existing limit: Jua/default lack these Japanese
glyphs, so both images show missing-glyph boxes, whose real advances are now measured.

| Screen | Before hash | After hash | Reference panel size | Classification |
|---|---|---|---|---|
| donglezoo-en | be308d58b698217f | d460fa69a7fa816f | 555.6x60 -> 495.38x60 | More accurate: measured pill width |
| donglezoo-ko | 9ad917382ef89ba1 | 8db2d87a18bd26d5 | 501x60 -> 431.32x60 | More accurate: measured pill width |
| donglezoo-ja | 5c7c2062638ebe78 | 27b55754f395d573 | 523.2x60 -> 424.79x60 | More accurate: measured pill width; existing missing glyphs unchanged |
| onlinerpg-en | a02a2525be2afe4c | 6ad299243ffe81b8 | 240x64 -> 240x59 | More accurate: actual wrapped line height |
| onlinerpg-ko | ea49b6d029a316df | a13b5807cffd4206 | 240x64 -> 240x36 | More accurate: actual wrapped line height |
| onlinerpg-ja | 87ffa2eb36142be6 | 15e52ba8e5d8adfc | 240x64 -> 240x36 | More accurate: actual wrapped line height; existing missing glyphs unchanged |

Additional render controls replay M2a's established sequence: Zoo menu/play/pause/drop and
RPG title/HUD preserve all six recorded M2a hashes/PNG bytes. These are comparisons against
M2a's stored baseline, not new pre-M2b captures. The M2a-bubble and 1600x900 bubble checkpoints
change as intended (4250483f22937c70 / 739abf91bd65743f): tighter real width/height; resized
bubble images were inspected. Entry animations, score/save/input/physics remain unchanged.
Controls and evidence: build/m2b/controls.py, controls-results.json and controls-*.png.

#### Executed edit task and cost

| Section 7 task | M1b measured baseline | M2b measured result | Required concepts |
|---|---|---|---|
| ⑤ Pause popup width/title, EN/KO | 3 files / 1 function (2 authored + 1 generated), 3 ancillary concepts | Actually changed generator width 520->580 and i18n Paused/일시정지 -> Break time/휴식; ran scene() only; asserted width/title, captured/inspected popup, restored original bytes in finally. Same 3 files / 1 function | Same 3: generator ownership, title/localization association, reference width/anchor units; zero font-width/projection/character-category knowledge |

Measured edit result does not improve this count: the selected pause popup already needed
no width-estimate knowledge in M1b. The new measurement removes approximation knowledge
from toast/bubble sizing, not generator ownership. Driver/results: build/m2b/edit.py,
edit-results.json, edit-popup-en.png/edit-popup-ko.png. No task substitution or M4 split.

Performance: 100 alternating batches of 50 warmed ui.measureText calls using one request,
Jua size 18, mixed EN/Hangul and wrapWidth 216. Python perf_counter around JSON-pipe
script.eval: median 1.023ms/50 including IPC, mean 1.035ms; empty-loop median 0.619ms.
Subtracting medians suggests ~8.07 microseconds/query, an approximate IPC-subtracted warm
observation, not a pure math guarantee or cold font-load cost. Font load/atlas warming
occurs outside timed batches. Method/data: build/m2b/perf.py and performance.json.
Caller key/invalidation guidance is published in SCRIPTING.md; no native result cache added.

Runtime/full-suite/browser evidence is in the M2b work log below. Fitting, paintedBounds,
M3 input, M4 app split, M5 templates and Japanese font provisioning remain outside scope.

### 6.6 M3 input/math decisions and results

**Pre-implementation decision (2026-10-06):** implement stateless oe.input / oe.math
composition in Lua. No native binding or registry command is required.

| Decision | Contract and inspected evidence |
|---|---|
| Named actions | Project Lua tables map names to vector or button bindings; read(source, actions, name) returns a fresh {x,y,z=0} or {held,pressed}. No project.json settings framework. Literal Script.params remain unchanged. |
| Composition | Vector bindings OR aliases within each direction, subtract opposite directions, add processed axis values, then cap length above 1. Positive Y is up; XZ movement negates Y. Optional axes are explicit, never automatic. Keyboard and stick add; native UIJoystick shares pad axes (held stick overwrites those axes), not a second independent device. Native axis applies 0.15 exactly once (engine/script/ScriptHost.cpp:992, INPUT.md logical contract); Lua adds no dead zone. Button held/pressed OR aliases independently, without consuming edges. |
| Focus and pause | UI focus is shared: ui.focused reads UIFocus (engine/script/ScriptHost.cpp:597); TextInputActive tests that id for UIInput (engine/app/Engine.cpp:736). engine/app/Engine.cpp:379 suppresses keyboard for onUpdate, :462 before network sampling; pad/mouse remain available. No new focus query needed. script.eval outside onUpdate is not an input-suppressed callback. dt=0 still runs scripts/UI (engine/app/Engine.cpp:375); modules read normally, gameplay caller gates movement. |
| Explicit sources | localSource() refuses reads whenever net.state().state is neither none nor idle, including a source cached before a match. playerSource(id,{actions,axes}) captures input.player closures and allowlists literal declarations, rejecting undeclared reads; no mouse/touch methods. Callers mirror network declarations explicitly, never infer a local player from hidden globals. engine/app/EngineSync.cpp:15 decodes declared fields, :116 creates sorted player inputs; player fallback (:108) can expose local input offline, so playerSource refuses non-running matches (including none/idle/lobby). |
| Network limitation | Existing global input remains available; authoritative client local UI input is intentionally merged (engine/app/Engine.cpp:481). The module restricts its own source API, not arbitrary Lua or forged declaration tables. Server device pointer/undeclared keys cannot enter a legitimate declared player stream. Authoritative clients have only their owned stream; remote player input is neutral, while lockstep/rollback peers receive all streams. Matching declarations are a project author obligation, checked by tests; no network format/injection changes. |
| Touch | engine/app/Engine.cpp:519 converts UIJoystick touches to existing axes before scripts/sampling, +Y up (:553); input.touches remains raw local gesture data. Wickbound origin-following drag stick remains project code and retains its keyboard-first fallback. |
| Math | clamp, unclamped lerp, distance, zero-safe normalize use existing {x,y,z}; missing coordinates default to 0. normalize returns unit vector or exact zero. Finite inputs required, clamp rejects inverted bounds. Wickbound/player.lua:40 and NetArena/player.lua:14 duplicate composition; OnlineRPG/player.lua:35 contains a similar wish vector but is read-only in M3. |
| Lifecycle | Module tables contain functions only. Source closures capture explicit player/allowlist data in the Lua heap; no subscriptions, timer, native registry or cleanup. Bindings/actions are caller-owned tables. Recording must start before require/evaluation; restored Lua heap restores closures/cache as before (engine/app/EngineSync.cpp:54/63). |
| Pilot choice | NetArena declares W/A/S/D and LeftX/LeftY (samples/NetArena/project.json), already covered by NetworkSamples playthrough in tests/tests.cpp:1586. Its summed/capped policy matches the adopted composer. Wickbound binds keyboard only to preserve its existing pad behavior and floating-stick precedence. NetChase declares look/jump but lacks the equivalent pad movement path; left read-only. |

Pre-edit deterministic evidence: build/m3/before-results.json and before-arena-results.json.
Wickbound east/diagonal/pad/combat hashes are 670d3c997ee5cded / c7655be8796f5eb3 /
853f4049bd0dcd99 / 9fe70a453a505a35. NetArena two-peer host/client positions agree,
errors/desync empty, screenshot hash 8706041085a3da95. Querying script.eval during a match
was correctly rejected by match_active; the driver uses read-only scene.get instead.
#### Implemented surface and verification

The implementation is pure Lua: engine/script/lua/oe/input.lua:7/15/46 supplies
localSource, playerSource and read; engine/script/lua/oe/math.lua:16/21/25/30 supplies
clamp, lerp, distance and normalize. Native input/focus/sampling/decoding, registry
commands, injection, protocol v4 and primitive revision 1 are unchanged. playerSource
reads require sync.state == running, including when the source was cached earlier;
lobby input.player fallback cannot expose device data through it. Caller-supplied
allowlists do not establish trust for arbitrary scripts. Reacquire a source if the
controlled player changes; source closures are not portable player identities.

API shape (full contracts/examples in SCRIPTING.md and INPUT.md):

```lua
local controls = require("oe.input")
local actions = {move={kind="vector",left={"A"},right={"D"},up={"W"},down={"S"},
                      axisX="LeftX",axisY="LeftY"}, jump={kind="button",keys={"Space"}}}
local offline = controls.localSource()
local player = controls.playerSource(id, {actions={"W","A","S","D","Space"},axes={"LeftX","LeftY"}})
local vector = controls.read(offline, actions, "move") -- use player only in a running match
```

Wickbound migration at samples/Wickbound/scripts/run/player.lua:8/44 only replaces
keyboard plumbing; floating origin, 8px drag dead band, thumb snap, keyboard priority
and pause reset remain. Pad axes stay ignored to preserve the original controls.
NetArena player.lua:16/17 mirrors its existing declarations and XZ sign mapping;
speed 4, clamps, game/score/reset logic and literal params are unchanged. Default
controller, NetChase/NetCoop/NetDuel, OnlineRPG and templates remain read-only.

| Same injected replay, 640x360 | Before hash | After hash | Verdict / position evidence |
|---|---|---|---|
| Wickbound east, 45 steps | 670d3c997ee5cded | 670d3c997ee5cded | Identical; (3.09374905,0,0) |
| Wickbound diagonal, next 45 | c7655be8796f5eb3 | c7655be8796f5eb3 | Identical; (5.28135872,2.18761086,0) |
| Wickbound pad-only, next 30 | 853f4049bd0dcd99 | 853f4049bd0dcd99 | Identical; unchanged position, pad intentionally ignored |
| Wickbound combat/rest, next 90 | 9fe70a453a505a35 | 9fe70a453a505a35 | Identical; unchanged position, automatic weapons continue |
| NetArena two-peer movement / settling | 8706041085a3da95 | 8706041085a3da95 | Identical; both peers P1=(0.666667402,.5,2), P2=(0,.5,0.666668534), no errors/desync |

Drivers build/m3/capture.py and arena.py use fresh JSON-pipe processes, fixed date
2031-03-03, real menu/Start pointer click, injected key/axis sequences; NetArena
uses net.spawn_local_peers count=1 seed=71, 180 warmup steps, 40 movement and 100
settling steps. Before artifacts were captured before project/module edits. All
five before/after PNG byte streams and positions match (audit.py). East, diagonal,
combat and arena images were opened directly; before diagonal/arena also inspected.
No intentional visual/gameplay change or diagonal speed change is introduced.

Tests at tests/tests.cpp:2304/2364 cover named vectors/buttons, zero/diagonal
composition, 0.575 raw -> 0.5 processed and 0.15 -> 0, mixed keyboard/stick, first
pressed vs held, focused UIInput keyboard suppression with pad preserved, dt=0,
headless UIJoystick input.touch -> axes (+Y contract retained), recorded Lua source
closures/cache save/branch/restore/future equality, finite/zero/huge math boundaries,
declared/undeclared/player/local source restrictions in all three network modes,
and explicit rollback under 3-frame delay (nonzero rollbacks assertion). The embedded
source tooling test now compares against OfficialManifest size instead of hardcoding
2 entries (tests/tests.cpp:2137). All original network/late-join/mismatch/recording,
TemplateGamePlaythrough, DongleZooGame, WickboundSample, OnlineRPGGame and
GpuRendererMatchesSoftware regressions pass in the full desktop suite.

Runtime and exact execution/limitations are recorded in the M3 work log below.


### 6.7 M4 DongleZoo readability pilot (2026-10-07)

#### Design and preservation baseline recorded before game edits

The original app has 728 lines. The inventory below records the original
`games/DongleZoo/scripts/app.lua` line numbers, all directly referenced instance fields,
direct assignments (including multi-assignment; effects also mutate fx through a local alias), and service calls. This is a
code-reading inventory, not a claim that each function is pure.

| Function | Original line | Instance fields read/referenced | Direct instance writes | Services | Destination |
|---|---:|---|---|---|---|
| `onStart` | 23 | background, camera, canvas, combo, comboLeft, dialog, frame, fx, guide, holder, line, mode, params, pop, score, vh, vw | background, camera, canvas, combo, comboLeft, dialog, frame, fx, guide, holder, line, mode, pop, score, vh, vw | I, UI, Meta, Audio, Ads, Board | app |
| `layout` | 61 | aspect, background, camY, camera, canvas, ortho, vh, vw | aspect, camY, ortho, vh, vw | D | screens |
| `pointer` | 81 | camera | none directly | owner/native calls only | screens |
| `applyTheme` | 90 | background, line, lineColor | lineColor | D, UI, Meta | screens |
| `refreshWallet` | 99 | none | none directly | UI, Meta | screens |
| `refreshItems` | 103 | none | none directly | D, UI, Meta | screens |
| `showHome` | 111 | guide, holder, mode | mode | UI | screens |
| `refreshHome` | 123 | none | none directly | I, UI, Meta | screens |
| `setDialog` | 129 | dialog, mode | dialog | UI | screens |
| `newGame` | 155 | aiming, bestCombo, combo, comboLeft, cooldown, cur, doubled, finalized, fx, holderX, mode, nxt, overFrames, revived, score, target, warned, wasBest | aiming, bestCombo, combo, comboLeft, cooldown, cur, doubled, finalized, fx, holderX, mode, nxt, overFrames, revived, score, target, warned, wasBest | D (via randomTier), UI, Meta, Board | gameplay |
| `refreshScore` | 184 | score | none directly | UI, Meta | screens |
| `refreshHolder` | 189 | cooldown, cur, guide, holder, holderX, mode, nxt, target | holderX | D, UI | screens |
| `drop` | 202 | cooldown, cur, holderX, nxt | cooldown, cur, nxt | D, UI, Meta, Audio, Board | gameplay |
| `toUI` | 221 | camera, canvas | none directly | owner/native calls only | effects |
| `popText` | 227 | pop | pop | UI | effects |
| `burst` | 236 | fx | fx | D, UI | effects |
| `updateFx` | 249 | fx | none directly | owner/native calls only | effects |
| `onMerge` | 263 | bestCombo, combo, comboLeft, score | bestCombo, combo, comboLeft, score | D, I, UI, Meta, Audio | gameplay |
| `earned` | 298 | score | none directly | D | rewards |
| `finalize` | 303 | finalized, score, wasBest | finalized, wasBest | Meta, Ads | rewards |
| `gameOver` | 312 | guide, holder, mode, overFrames, target | mode, overFrames, target | UI, Audio | gameplay |
| `showOver` | 322 | doubled, revived, score, wasBest | none directly | I, UI, Meta, Audio | screens |
| `revive` | 337 | cooldown, mode, overFrames, revived, warned | cooldown, mode, overFrames, revived, warned | D, I, UI, Audio, Board | gameplay |
| `useItem` | 352 | dialog, mode, target | target | I, UI, Meta, Audio, Board | gameplay |
| `cancelPick` | 380 | target | target | UI | gameplay |
| `pick` | 387 | cooldown, target | cooldown | D, I, UI, Meta, Audio, Board | gameplay |
| `openItemDialog` | 416 | itemWanted | itemWanted | D, I, UI | screens |
| `refreshShop` | 428 | none | none directly | D, I, UI, Meta | screens |
| `refreshBook` | 445 | none | none directly | D, I, UI, Meta | screens |
| `refreshMissions` | 456 | none | none directly | D, I, UI, Meta | screens |
| `refreshDaily` | 472 | none | none directly | D, I, UI, Meta | screens |
| `refreshSettings` | 487 | none | none directly | I, UI, Meta | screens |
| `open` | 494 | none | none directly | owner/native calls only | screens |
| `close` | 509 | mode | none directly | owner/native calls only | screens |
| `claimDaily` | 514 | none | none directly | I, UI, Meta, Audio | rewards |
| `leaveGame` | 525 | none | none directly | Ads | rewards |
| `onAction` | 531 | dialog, doubled, itemWanted, mode, score | doubled | D, I, UI, Meta, Audio, Ads, Board | app dispatch; split handlers in gameplay/screens/rewards |
| `updatePlay` | 653 | aiming, combo, comboLeft, cooldown, frame, holderX, line, lineColor, target, warned | aiming, combo, comboLeft, cooldown, holderX, warned | D, UI, Audio, Board | gameplay |
| `onUpdate` | 701 | dialog, frame, mode, overFrames | frame, overFrames | UI, Ads, Board | app |
| `hash` (local) | 138 | argument only | none | none | gameplay seed helper |
| `randomTier` (local) | 144 | drop weights / RNG | consumes existing RNG | D | gameplay; receive D explicitly |

One attached App instance remains the owner of run state: score/combo, drop selection,
items-in-use, dialog/mode, frame counters and effect lifetimes. Its `services` context
explicitly supplies defs, widgets, board, meta/save, audio, localization and ads.
Stateless method modules receive that owner as `self`; they import no service or sibling
module. Calls between responsibilities use the owner's existing methods. Immutable
method tables are installed in a fixed order by app.lua; there is no cyclic require graph.
Existing board/meta/audio/i18n/UI/ads modules keep their existing state and recording
contract; no additional mutable module singleton is introduced. Meta remains the owner
of persistent coins/items/settings and `donglezoo.save.v1`, Board of pieces/order.

Planned boundaries: app.lua (startup/frame orchestration and action dispatch),
gameplay.lua (drop/merge/items/danger), screens.lua (layout/home/HUD/dialogs),
rewards.lua (finalization/gifts/store/ad actions), effects.lua (visual lifetimes),
scoring.lua (pure combo rounding rule). Public App method names remain available to
button messages and existing tests; no extra Script component or callback registration.

Shop decision: authored `config/shop.json` is the only product definition. The existing
Python generator emits a deterministic `scripts/config/shop.lua` table and scene rows;
defs.lua references the generated table. Python reads JSON rather than parsing Lua, and
the sandbox needs no JSON/file API. Existing three products preserve scene construction
order. For larger catalogs the generator uses an existing UIScroll viewport for product
rows, preserving the original three-row scene. New coin packs need one JSON edit and
generation, not action/name/layout edits. Real store SKU provisioning remains external.
Pause width alone becomes `config/layout.json`; translated title remains i18n.lua.
Gameplay constants remain in defs.lua; the combo rule alone moves to scoring.lua.
The existing literal Script.params fallbacks in ui/button.lua remain unchanged; the
App's optional locale override remains a per-instance parameter, not balance data.

**Historical M4 limitation, superseded by 6.10.**

Hot-reload constraint found before implementation: `L_Require` caches required-only
project modules (`engine/script/ScriptHost.cpp:1142`), whereas `PollHotReload` iterates
attached modules only (`:1934`; `script.reload` at `engine/api/Commands.cpp:1232`).
App reload continues to retain instance state; extracted helper edits require Stop/Play
or a fresh process. Preserving live reload for edits moved out of app.lua cannot be
achieved with the existing require contract without an engine change or additional
script instances. Both are outside M4; document this workflow difference explicitly.

Preservation replay was executed before game edits with the existing executable, fixed
date 2031-03-03, 540x960 view, fresh memory save. `build/m4/replay.py` records 13 PNGs,
hashes, run state, save values and host call records in `before-replay.json`. It includes
actual pointer drops/physics merges, two arranged equal-tier pairs contacting in one
frame for combo, hammer/shake, pause, shop, demo coins_s purchase, daily reward and KO
localization. Arranged tiers/contact pairs and item target coordinates are explicit
test fixtures, not a claim of a purely manual playthrough. Save key/version and t: tag
IDs/values are retained in the artifact. Host sequence is game.ready then iap.purchase
with product coins_s; both are unhandled and exercise existing fallback behavior.

#### Implementation and completed evidence

| File under scripts/ | Lines | Responsibility |
|---|---:|---|
| `app.lua` | 92 | Startup, onUpdate order, action routing |
| `app/gameplay.lua` | 302 | Drop/merge/items/danger |
| `app/screens.lua` | 322 | Layout, home/HUD/dialogs, settings |
| `app/rewards.lua` | 139 | Finalization/gifts/store/ad actions |
| `app/effects.lua` | 54 | Visual effects and lifetimes |
| `app/scoring.lua` | 8 | Pure combo multiplier/cap/rounding |

All modules take the App owner explicitly; referenced services are taken from
`self.services`. screens/gameplay/rewards provide separate action handlers; orchestration
keeps the original Ads.action → busy guard → click sound order. There are no new attached
script instances, timers/subscriptions, engine bindings or official-module changes.
The scoring rule retains the original floor(base*(1+0.25*min(combo-1,6))+0.5).
Board/meta/audio/i18n/UI/ads implementation and save schema remain untouched.

Evidence: `games/DongleZoo/scripts/app.lua:23` supplies the service context;
`scripts/app/gameplay.lua:88` delegates scoring; `scripts/app/scoring.lua:4` owns the pure
rule; `scripts/lib/defs.lua:67` reads generated product data. `tools/make_content.py:47`
serializes JSON data; `:62` validates products before generating the Lua table; scene()
uses those same IDs and enables existing UIScroll for catalogs larger than three.
No new reward-type framework is claimed. Catalogs remain EN/KO coin packs plus the
existing no_ads special case; real store provisioning is unverified and external.

The unchanged ui/button.lua literal target fallback still infers via script.params.
Balance/reward tables stay in defs.lua; selected pause geometry is layout.json, product
data is shop.json, localization stays in i18n.lua. Every new location and the remaining
shared prefab/defs geometry constraint are described in games/DongleZoo/EDITING.md.

#### Preservation replay and save compatibility

| Checkpoint | Before hash | After hash | Result |
|---|---|---|---|
| zoo-menu | bcfda4dfb05568df | bcfda4dfb05568df | Identical PNG/state/save/host records |
| zoo-play | 50468c5f1f131f56 | 50468c5f1f131f56 | Identical PNG/state/save/host records |
| zoo-pause | 73791447d24e945f | 73791447d24e945f | Identical PNG/state/save/host records |
| zoo-drop | d072dfc205cd9f6b | d072dfc205cd9f6b | Identical PNG/state/save/host records |
| merge | e67dee3a717aa232 | e67dee3a717aa232 | Identical PNG/state/save/host records |
| combo | 03e721dfbbbc8417 | 03e721dfbbbc8417 | Identical PNG/state/save/host records |
| items | c809327f233dd40e | c809327f233dd40e | Identical PNG/state/save/host records |
| pause-items | 0a16fe200faf26c0 | 0a16fe200faf26c0 | Identical PNG/state/save/host records |
| shop | 7011a68fb240f72e | 7011a68fb240f72e | Identical PNG/state/save/host records |
| purchase-demo | fbaca3e48e22957d | fbaca3e48e22957d | Identical PNG/state/save/host records |
| purchased | 4d7a28279d29406d | 4d7a28279d29406d | Identical PNG/state/save/host records |
| reward | e1be1978860e122b | e1be1978860e122b | Identical PNG/state/save/host records |
| korean | 1ca5838e2330f1c6 | 1ca5838e2330f1c6 | Identical PNG/state/save/host records |

The full before/after JSON artifacts are equal, including entity summary, t: tag IDs,
score/combo/items, coins, RNG-selected current/next tiers, save values and host call frames.
All 13 PNG SHA-256 values also match; no intended visual/rule change occurred.
Menu/play/pause/drop match the stored M2a/M2b control hashes. The scene generator emits
the same semantic JSON for the original three-product catalog; the original working-tree
scene bytes were restored after generation (newline-only differences).

Persistent `donglezoo.save.v1` keeps version, coins, best, items, themes/theme, seen,
stats, settings, daily, missions, noAds and tutorial fields unchanged. The new
DongleZooModuleSnapshotsAndCatalog test loads a version-1 pre-split save and checks
coins/best/items/KO locale, compares runtime products with authored JSON, verifies
native restore of service/module aliases and identical future scene state, and checks
attached App reload retains score/context without rerunning startup.

#### Actual editing measurements (all changes reverted)

| Task | M0 predicted / M1b measured / M2b measured | M4 actually edited (authored + generated; functions) | Ancillary concepts before → after | Executed correctness check |
|---|---|---|---|---|
| 1. Player speed | N/A throughout | N/A; no substitute attempted | N/A | This puzzle has no moving player |
| 2. Resume EN/KO | 1 source / 0 functions; optionally +1 scene; M1b 1/0 | 1 source (i18n.lua) +1 scene for stopped-editor text; 0 functions | 2 → 2; t: localization and generated/runtime overwrite contract remain | Continue / 이어하기 asserted on UIButton, both screenshots inspected; reverted |
| 3. Add one coin pack | M0 2 sources +1 scene, 1 function; M1b not executed | 1 source (config/shop.json) +2 generated (shop.lua, scene); 0 functions, 0 manual scene edits | 3 → 1; regeneration ownership remains, duplicate ID list/row geometry/action naming removed | coins_m4 appeared in EN/KO, fourth row revealed in existing UIScroll, real button click opened demo, adBuy granted exactly 2,500 coins and host SKU matched; reverted |
| 4. Combo factor | M0/M1b 1 source / 1 function, onMerge | 1 source / 1 function, scoring.mergePoints | 2 → 0; save/mission and audiovisual dispatch no longer share the edited function | Factor 0.25 → 0.5; combo-2 score asserted as round(base*1.5)=2; reverted |
| 5. Popup width/title | M0/M1b/M2b 2 sources +1 scene / 1 generator function | 2 sources (layout.json + i18n.lua) +1 scene; 0 functions | 3 → 2; generation ownership/reference units remain, locating generated panel/tag association is no longer needed | Width 580 and Break time / 휴식 asserted, both rendered screenshots inspected; reverted |

Counts are executed source edits, not estimates or human time measurements. Data rows
count as zero edited functions; generator execution is not an edited function. Label
location is not improved, scoring file/function counts are unchanged, popup authored
file count is unchanged, and the shop still produces two generated artifacts. The
improvement is focused responsibilities and removal of duplicate product/layout edits.
Ancillary counts use the same M0 categories; ordinary domain knowledge (coin amount,
locale strings, multiplier) is not counted as unrelated engine plumbing.
build/m4/tasks.py restores saved bytes in finally blocks; edit-results.json and EN/KO
PNGs record assertions and outcomes. Product/popup/label images were directly viewed.
No task required projection or text-width approximation. No claim of measured human
editing speed is made.

#### Verification and limitations

- build.bat completed, then build/bin/oe_tests: **231 tests, 0 failed checks, exit 0**.
  DongleZooGame, DongleZooModuleSnapshotsAndCatalog, Wickbound/OnlineRPG playthroughs,
  GpuRendererMatchesSoftware and AgentInstructionsInSync pass. The first test run had
  four checks fail because the new test attempted save.set/script.reload after enabling
  immutable recording. Its setup was corrected: seed save before recording and exercise
  live reload on a separate unrecorded engine. Original and final outputs are retained.
- All 15 project Lua files: script.check errors 0/warnings 0; button script.params
  inspected directly. Scene-only generation is idempotent for data/semantic scene and
  preserves prefab bytes. The early --scene-only option initially lacked CLI wiring,
  so an invocation also rewrote pristine prefabs with LF. CLI wiring was fixed and all
  13 originally clean prefab files restored to their original Git content/CRLF. No
  prefab/asset content change remains; make_art/import_art/make_audio were not executed.
- Desktop and web packages created. Desktop runtime loaded packaged game from an
  external working directory; a test-only Play/quit hook in the packaged copy reached
  gameplay and exited 0. The original packaged App was restored afterward. Its GPU
  initialization emitted the existing generic sokol warning; no Lua/import error.
  An initial --frames attempt was terminated by its own PID: that flag limits dedicated
  servers, not the interactive player. Browser smoke used the actual unmodified web
  package through oe serve on port 17864: WebGL2 menu → Play clicked and HUD displayed,
  no Lua/import/console errors. web-menu.jpg/web-play.jpg are evidence; owned tab/server
  were closed by tab/PID. Case-sensitive Wasm filesystem resolves the new require paths.
- Required-only module hot reload remains the stated workflow difference; attached
  App state retention and native snapshots are verified. Fresh Stop/Play is required
  after helper/data edits. This historical engine limitation is superseded by 6.10,
  not by a project-Lua workaround.
- Project-only Lua/data and a native test were changed. Engine/official modules and
  their digest are unchanged; web/Android runtime rebuilding is unnecessary. All runtime
  file hashes match the captured pre-M4 manifest. Android device execution, real store
  purchases/ads, and full packaged-web replay are unverified; only menu→Play was requested
  and performed. No M5/template/Hello or Wickbound/OnlineRPG structure changes.

### 6.8 M5 defaults and completion (2026-10-07)

#### Decisions recorded before template edits

| Template script | Decision | Readability/behavior reason |
|---|---|---|
| player_controller.lua | Use oe.input localSource/read and oe.math.normalize | Name keyboard move/jump bindings once, map +Y to -Z explicitly, retain zero-safe constant-speed XZ intent. Only the original WASD/arrows/Space are bound; no pad axis/button or network source is added. The math normalization makes the XZ intent explicit; oe.input already caps digital diagonals, so this does not add analog behavior. |
| game_manager.lua | Use oe.ui widget handles | Replace repeated entity/component text/visibility plumbing; explicit UIText/UIButton capability selection. tryFind preserves optional Score/Play Again behavior, Message remains required. Handles live only on the scene's script instance. |
| coin.lua | Keep unchanged | It has no HUD text/visibility code: it broadcasts onCoinCollected and destroys itself. Adding a UI import would couple collectibles to presentation without removing duplication. |
| bob.lua | Keep unchanged | One sinusoidal expression and rotation; no useful composition gap. |
| rotator.lua | Keep unchanged | Three clear angular updates; replacing arithmetic with imports adds indirection. |
| play_again.lua | Keep unchanged | Two calls reset total/load scene; no widget acquisition required. |

Keep literal speed=4, jumpSpeed=5, gravity=12 fallbacks and existing scene overrides;
script.params inference must match before/after (diagnostic line numbers may move).
Do not introduce stick support, change jump/gravity/physics paths, or alter scene data.
EDITING.md belongs in the template because section 5 requires a short project edit map.
It covers only actual speed/jump/body params, coin total scoring, labels, stage/prefab
content and UI geometry; shop/dialog/merge tasks are not invented for this game.

**Optional flows: no new official module.** DongleZoo's three fixed toast slots maintain
a queue/fade using real frames, including pause (games/DongleZoo/scripts/lib/ui.lua:68,
:83, :91); its dialogs coordinate game pause/mode and rewards. OnlineRPG bubbles are
per-speaker HUD instance records expiring at untilFrame (scripts/hud.lua:64, :125),
reproject/reflow with avatars, and clean up disappeared players (:165). They share
validated widget/text/query primitives, not one repeated owner/lifetime operation.
An owner-bound, Lua-heap-only cancel/cleanup/snapshot contract could be designed, but
no second matching API/use case demonstrates it. Do not add generalized timers,
callbacks, dialog subtree policy or native state for hypothetical reuse. Existing
project owners remain unchanged; official source/digest/runtime rebuilds are unnecessary.

**Hello generation mode:** the current samples/Hello contains .oe/editor.ini but no
.oe/team/team.json. CmdNew seeds a team unless --no-team (tools/oe/main.cpp:247);
SeedDefaultTeam writes actual agent/settings state (engine/team/TeamDefaults.cpp:111).
The original historical CLI flag is unconfirmed; current no-team state is verified.
Regenerate with --name Hello --no-team, preserving the existing editor.ini separately.
The default new-project smoke will omit --no-team to verify normal team seeding.

**Pre-edit preservation evidence:** build/m5/before-replay.json contains ten 640x360
software hashes/PNG SHA-256 values and game/save/player state, captured before any
template code edit. The injected playthrough mirrors TemplateGamePlaythrough
(tests/tests.cpp:3530): W110, clear100, D130, click Play Again, S110, clear150;
a separate diagonal/jump/opposed-key replay covers movement composition. Original
script.params for all six scripts is stored in before-params.json. Template and Hello
original bytes/private editor settings are backed up under ignored build/m5/initial.

#### Implementation and executed verification

Only player_controller.lua and game_manager.lua changed. The controller owns its
localSource on the script instance and names move/jump actions once. XZ mapping and
zero-safe direction replace handwritten key/length arithmetic; no new inputs are bound.
The manager acquires capability-selected widgets onStart and uses text/set/show partial
updates. Existing scene transitions reset script instances and reacquire handles; no
module-global handle, timer owner or native registry was added. coin/bob/rotator/play_again
remain byte-identical. Original literal defaults and nextScene fallback are preserved.

Template AGENTS.md now points to EDITING.md and the embedded modules. EDITING.md maps
actual params, score/coin count, labels/stage content/prefab/UI geometry and verification;
CharacterBody.gravityScale is explicitly distinguished from fallback Script.params.gravity.
Hello was regenerated with --name Hello --no-team and its editor.ini restored unchanged.
The historical generation flag is still unconfirmed, not retroactively claimed as fact.
Default oe new Smoke seeded a team; explicit Hello --no-team did not.

| Replay checkpoint | Before hash | After hash | Outcome |
|---|---|---|---|
| initial | f315ad4d4c4469cf | f315ad4d4c4469cf | Identical PNG/game/save/player state |
| level-clear | 2eaca188e3ce472c | 2eaca188e3ce472c | Identical PNG/game/save/player state |
| level-two | e8dd185206211fc9 | e8dd185206211fc9 | Identical PNG/game/save/player state |
| win | 58e12cb9e42e2920 | 58e12cb9e42e2920 | Identical PNG/game/save/player state |
| restart | 0e8661fdad9f9f9f | 0e8661fdad9f9f9f | Identical PNG/game/save/player state |
| fall | b4be25028dcf6c4d | b4be25028dcf6c4d | Identical PNG/game/save/player state |
| respawn | 05a373ecd24c346d | 05a373ecd24c346d | Identical PNG/game/save/player state |
| diagonal-jump | 82affbd39623de05 | 82affbd39623de05 | Identical PNG/game/save/player state |
| diagonal-land | e182290072e1cdfb | e182290072e1cdfb | Identical PNG/game/save/player state |
| opposed | 312d7cdb8b67d254 | 312d7cdb8b67d254 | Identical PNG/game/save/player state |

All ten PNG SHA-256 hashes and full recorded JSON artifacts match exactly. Baselines
were captured before edits; final state includes highScore=7 and the expected restart
and respawn flow. Initial, clear, level-two, win/restart, fall/respawn and diagonal/
opposed inputs are covered. Win before/after images and the Smoke render were viewed.
script.params on all six scripts is unchanged except diagnostic line numbers.
All six scripts return script.check errors=0/warnings=0.

build.bat completed; build/bin/oe_tests: **232 tests, 0 failed checks, exit 0**.
TemplateGamePlaythrough, TemplateRotatorScript, TemplatePlayerScript,
TemplatePlayerScriptWithCharacterBody and new TemplateScriptParametersAndChecks pass.
The new test checks all six script diagnostics and literal speed/jump/gravity inference.
All existing render/network/project tests and AgentInstructionsInSync pass.

The first full run reported 232 tests / 2 failed checks / exit 1 in
NetworkEngineNativeSessionTcpUdp. Its socket/RPC fixture retained the offline template
controller; localSource correctly rejected a live network session. The fixture now
removes only that controller Script from both peers before network stepping, retaining
socket/RPC assertions and the final empty-script-errors assertion. The guard was not
weakened, no network controller was added to the template, and no engine C++ changed.
Original/final test output is preserved in build/m5/tests.log and tests-final.log.

Fresh oe new Smoke with default team: render --frames 60, desktop package and web package
completed. Packaged desktop ran from the external system-temp working directory; a
test-only auto-quit hook in its copied game_manager reached initialized simulation and
exited 0. The copied manager was restored afterward; the source project/template was
not modified by this smoke hook. The desktop
GPU emitted the existing generic sokol initialization warning, with no Lua/import error.
The unmodified web package was served locally on port 17865 and opened in the actual
browser: WebGL2 player/HUD and all three imported modules execute without Lua/console
errors. web-smoke.jpg records it. The owned browser tab and server PID were closed.
These are startup/import smokes, not full interactive packaged playthroughs.

Final Hello diff is exactly AGENTS.md, new EDITING.md, and two script files. Project JSON,
scenes, prefab, sounds, unchanged scripts and private editor settings are byte-preserved.
No Hello-specific README contradicts the two-level game; repository README descriptions
remain accurate, so they were not changed. The two root instruction files receive only
the audited code-map/template guidance updates and remain byte-identical.

No official source/digest, engine C++, command/component schema, or runtime binary changed.
All runtime files match the captured pre-M5 SHA-256 manifest. Web/Android rebuild and
Wasm test rerun are therefore unnecessary for this unit; the last executed Wasm suite
remains M3's 194 tests. docs/API.md is not regenerated because commands are unchanged.

Deviation: automatic approval review rejected recursive removal of Hello and the
temporary smoke folders (policy block; no detailed reason supplied). Hello was safely
moved to ignored build/m5/previous-Hello before regeneration. Smoke/project/package
folders remain in ignored build/m5 for manual cleanup; no server/browser/player process
was left running. No deletion through another API or shell was attempted. Original
Hello backups and execution logs are retained deliberately for recovery/evidence.

### 6.9 Consolidated follow-ups and unverified work

This is the current completion ledger. Earlier dated logs retain their original stage
scope/status; they are not claims that every platform/device feature is now verified.

| Follow-up / unverified item | Current fact / reason | Needed input or next work |
|---|---|---|
| SnapshotHeap offline allocation cost | **Measured (6.10):** +15.0% Wickbound / +26.8% DongleZoo simulation; 4.18x allocation loop; identical hashes/state | Approval for a separate allocator optimization prototype retaining transactional rollback; no engine change in this follow-up |
| Required-only module hot reload | **Implemented and verified (6.10):** watched project dependencies, stable table exports, transactional failure recovery; native 235/Wasm 199 pass; live DongleZoo scoring edit/reload/restore | Module-local/nested state resets; captured functions/timer/RPC closures and computed UI need owner refresh. Stateful edits can still need restart; see SCRIPTING.md/EDITING.md |
| Android device/emulator execution | Both ABI runtimes and signed packaging were rebuilt/checked in prior milestones; no actual device execution | Android device or usable emulator/ADB; launch, input, audio, font/query/import validation |
| Physical touch/controller input | Injection/composition/source/focus tests and browser keyboard pilot passed; physical adapters not exercised | Touch device and gamepad on target platforms; real gestures, multi-touch, axes/dead zone/focus tests |
| Japanese font glyphs | M2b observed unavailable Japanese glyphs in existing pilot fonts; fallback limits are documented | Licensed Japanese-capable font/assets and localization strings; rerun visual/font fallback checks |
| Real store SKU/ads | Demo and simulated host callbacks verified; real provider/store authentication and rewards are not | Store/provider accounts, provisioned SKUs/ad units, SDK integration/test devices; no credentials belong in docs |
| Unmigrated project duplicates | OnlineRPG player.lua retains its network wish-vector composition; Wickbound lib/ui.lua retains project UI/measurement code; examples intentionally migrated incrementally | Separate pilot scope and pre-edit replay/visual/network baselines; do not silently apply template offline localSource to network gameplay |
| Widget subtree/dialog hide contract | Widget show/hide changes one selected UI component. Ancestor visibility suppresses children; no owner-bound subtree flag mutation/dialog framework | Two concrete matching owner/cleanup use cases and explicit show/restore policy before adding a module |
| ui.fitText / fitting / painted bounds | measureText is layout-only; wrap supported, auto-shrink/box fitting and paint bounds deliberately excluded | Explicit box/style/min-size/error/unit semantics, real fitting caller and renderer parity tests |
| Broad packaged/platform validation | Desktop/web startup imports verified here; prior native/Wasm suites recorded separately. Full packaged replay/Android execution and other OS runs are not M5 evidence | Platform environments and repeatable packaged input/replay runs; preserve exact execution reports |
| Optional dialog/effect modules | No common two-owner API proven; toast queue/fade and per-speaker projected bubbles have different lifetimes | Demonstrate repeated operation and Lua-owned cancel/cleanup/snapshot contract; no speculative module now |
| Additional entity convenience candidates | Section 3's setPosition-style conveniences remain candidates, not implemented handle methods; use validated scene/self operations | A concrete caller and explicit local/world transform semantics before extending the module |
| Human editing time | M4 source edits/counts/assertions measured, human difficulty/time not measured | Human editor trial with agreed tasks/time protocol if needed; do not infer speed from line counts |
| Temporary smoke-folder cleanup | Recursive removal blocked by automatic approval review; ignored build/m5 smoke/packaged folders retained, processes stopped | User may remove those disposable folders manually; source working tree is not affected |

### 6.10 Required-only project module reload

#### Design recorded before implementation (2026-10-07)

Pre-edit model (line references are from the saved working-tree baseline at
build/reload/ScriptHost.cpp, not final source): `L_Require` caches `project:<path>` exports in `oe.loaded` without a
watch record (engine/script/ScriptHost.cpp:1142-1195). Attached files alone enter
`modules_`; PollHotReload compares their mtimes (1934-1963). Tick polls at 0.5 s;
Step polls once before stepping (engine/app/Engine.cpp:304-315). ReloadModule refills
attached tables, keeping instance fields/metatables and not calling onStart (1869-1923).
Captured helper tables/functions remain old, including DongleZoo self.services.Scoring.
Native Lua snapshots already capture cache/closures and C++ module metadata (2238-2277).
Engine command and automatic-poll barriers disable reload during recording/active matches
(engine/app/Engine.cpp:304, :314, :1160-1164). These barriers must also cover direct host calls.

| Candidate | State/lifecycle/failure semantics | Decision |
|---|---|---|
| A: invalidate helpers and reload attached importers without retained dependency edges | Instance fields survive, but finding transitive importers requires either scanning or a graph. Cache replacement alone leaves instance-held tables stale; singleton reset can affect unrelated services | Reject as underspecified |
| B: tracked reverse dependency closure and stable table exports | Reload changed required files and transitive required importers; reload affected attached files. Manual reload additionally refreshes all attached files. Refill required table exports in place so self.services references remain usable. Affected module-local state resets; unrelated modules remain intact | **Adopt** |
| C: clear every project export and reload every attached file | Simple but resets unrelated board/save/audio services; old instance-held tables still need rebinding. Official exports can remain immutable, but the broad reset is disruptive | Reject |

Transaction: use the existing address-stable Lua allocator in every VM, taking an image
only during an actual reload or opted-in recording. Capture Lua heap plus host metadata
before executing any replacement. On any load/initialization/cycle/budget failure restore
the image and metadata, then append script.errors. No discarded-branch finalizers run.
During replacement initializers, reject engine bindings, timers/tweens, and RNG calls;
initializers must only build exports. This prevents scene/save/host/RPC effects and duplicate
subscriptions. Existing timers/callback closures remain installed once; onStart is not rerun.
This deliberately narrows reload initialization: move native registration to owner lifecycle
methods. Initial load retains existing behavior. Both command and poll use this transaction.

Dependencies are canonical project paths recorded for require calls, including runtime calls
from Lua source frames. Cycles get an explicit loading-sentinel error with retry cleanup.
Official exports are never invalidated. Returned tables retain their top-level identity;
removed fields disappear and fresh fields replace old ones. Affected private local state and
nested exported tables reset. Independently captured functions/nested tables and existing timer
or RPC closures may retain old code: they are not rewritten or rerun. Prefer table.method
lookups and instance-owned state. No claim of automatic onUpdate reconciliation of cached
translated strings, generated scene structure, or already computed instance values.

Recording still must precede Lua execution/physics and resources remain immutable. Active
lockstep/rollback/authoritative matches skip all direct/automatic reload; no wire changes.
Dependency/watch metadata joins native snapshots so save/restore remains coherent. The
ordinary allocator change adds bookkeeping but no periodic image/journal in offline games.

| Comparison axis | A: invalidate/importer search | B: dependency closure (adopted) | C: all project modules |
|---|---|---|---|
| Instance fields / onStart | Can retain fields; must not rerun startup | Retained; no startup rerun | Same, but more attached files execute |
| Module locals/singletons | Only if importers can be found; naive cache swaps leave stale captures | Affected locals reset; unrelated services preserved | Every project singleton resets |
| Timer/RPC ownership | Initializer guard needed; existing closures may remain old | Guard prevents registration/cancel; existing owner callbacks retained once | Same guard necessary; more initializer incompatibilities |
| Failure / cycles | Requires a transaction and sentinel, not just invalidation | Whole Lua image/metadata rollback plus sentinel diagnostic | Transaction possible, but broad rollback/image work |
| Official exports | Can exclude embedded origin | Immutable, excluded | Must explicitly retain embedded origin |
| Recording / snapshots | Disable during recording; track watch state | Same; graph/source/watch metadata captured | Same; larger affected state |
| Active network matches | Must disable command/poll replacements | Existing barriers plus direct host guard | Same |
| Editor flow | Cannot identify importer set reliably without extra tracking | Helper edit -> shared transaction -> report; manual reload also refreshes all attachments | Predictable but unrelated board/save/audio state is disrupted |

Implementation map: ScriptHost.h/.cpp (watch/dependency metadata, loader sentinel, stable
exports, transaction, reports/snapshots); Engine.h/.cpp (shared read-only barrier query);
Commands.cpp (additive reloaded/required/failed/skipped report); tests/tests.cpp (helper,
field/startup, failure/cycle/budget, singleton/timer, polling, recording/native restore,
official and match barriers). Update current scripting/game instructions and historical
limitation rows with a superseding reference. Rebuild desktop and, once at the end, web and
both Android ABIs; run complete native/Wasm suites. Root agent guides and official Lua sources
remain unchanged. Executed results follow below; no build/test result is claimed here.

#### Executed implementation and results

| Implementation | Evidence in final source |
|---|---|
| Canonical project loader, watch record, runtime source-frame dependencies and cycle sentinel | engine/script/ScriptHost.cpp:1894; L_Require :1154; project exports remain Lua registry values |
| Stable required table refill, including self-export upvalues | engine/script/ScriptHost.cpp:1859, :1881; top-level identity only; separately captured functions/nested values can remain old |
| One command/poll batch with byte-check on explicit reload and image rollback | engine/script/ScriptHost.cpp:2045, :2128; command engine/api/Commands.cpp:1232; script.write :1373 forces a path |
| Side-effect guard and protected aliases/coroutine wrappers | engine/script/ScriptHost.cpp:45, :61, :1699; stored function identities are membership checks, never iteration order used by gameplay |
| Shared recording/active-match barrier | engine/app/Engine.cpp:302, :310, :320; existing Engine::Call side-effect rejection retained |
| Watch/dependency/source/protected-function metadata in native snapshots | engine/script/ScriptHost.cpp:2403, :2425, :2434; explicit recording admission remains unchanged |

Executed tests: RequiredLuaReloadDependenciesStateAndCallbacks (transitive helper update,
private counter resets, unrelated singleton preserved, fields/onStart retained, timer/RPC
installed once, export identity/self-reference, official identity, command and poll);
RequiredLuaReloadFailureCycleBudgetAndSideEffects (syntax, cyclic import, instruction budget,
Lua global rollback, caught forbidden native call, aliases, timers, RNG, wrapped coroutine,
retry after fixing source, full batch recovery after a later attachment failure,
forwarding table aliases, and refusal to mutate an official shared export); RequiredLuaReloadRecordingAndNativeRestoration (command/direct
barriers and identical future after native save/load). Existing
OfficialLuaProjectImportsAndReloadCompatibility intentionally now expects changed helper value
2 rather than cached value 1. OfficialLuaBlueprintMismatchBarriers additionally checks direct
and command barriers in matching lockstep/rollback/authority sessions. No wire/digest changes. Returning an existing shared table is an in-place no-op;
changing a forwarding alias boundary cannot refill an unrelated or official export and is
rejected with a restart hint. This protects official immutability even for project wrappers.

Desktop: build.bat exit 0 (affected .cpp timestamps refreshed after header edits); complete
235 tests, zero failed checks, exit 0. Network/native snapshot/recording, official, GPU/software,
editor and game regressions pass. The earlier full run also passed 235; the final hardened
source was rebuilt and rerun. Final native snapshot worst was 7.567 ms across 20 restores/four
samples. This is a fixture result, not a reload timing benchmark.

Live DongleZoo: persistent oe script session, fixed date 2031-03-03, frame 3. Editing
app/scoring.lua factor 0.25 -> 0.50 followed by script.reload changed points(10,2) 13 -> 15
and actual onMerge gain 125 -> 150. Reload reported required=[scripts/app/scoring.lua] and
attached=[scripts/app.lua,scripts/ui/button.lua]. Existing score 125, frame, probe field,
services/Scoring table identity, coins and board count survived. Restoring original bytes and
reloading restored 13; script.errors empty. No Stop/Play; owned process terminated by PID.
Scoring file is byte-identical to its pre-test state. Evidence: build/reload/live-results.json.

The 13-checkpoint M4 replay was rerun after restoring scoring. All PNG/frame hashes, full
checkpoint state/save/host records, localization tags, final save/host and scene summary
match build/m4/after-replay.json exactly (build/reload/replay-comparison.json). Thus the
allocator/loader changes preserve unedited game behavior, including EN/KO and host fallback.

Web: build_web.bat twice (review correction described below), EMSDK=E:/emsdk, Release, BINARYEN_CORES=1, parallelism 2, exit 0.
The script configures tests OFF; configure tests ON and build oe_tests afterwards (no second
player rebuild). Node build-web/bin/oe_tests.js: 199 tests, zero failed checks, exit 0.
One existing unused ProcessFixtureDir warning; runtime bytes/size/hash/provenance recorded in
runtime/web/README.md. Android: build_android.bat twice (same review correction), SDK build/android-sdk, NDK r28c,
arm64-v8a and x86_64 both exit 0/refreshed. Existing unrelated initializer warnings remain;
no reload-source warning. Bytes/hash/provenance in runtime/android/README.md.
Android device/emulator execution: **unverified / 기기 실행 미검증**. No device launch,
new APK signing, or packaged browser smoke was executed in this unit; earlier evidence stays
historical. Runtime rebuild and Wasm tests were requested and actually executed.

Plan refinements: final review found same-table forwarding refill could clear exports and
shared official exports needed protection. Add identity no-op/shared-alias rejection and
regression coverage, then rebuild/retest the final source. Therefore both player runtimes
were rebuilt twice, contrary to the requested single final pass; final artifact hashes refer
to the corrected pass. All native/Wasm final tests and the live/replay scenario were rerun.
Use the snapshot allocator in every VM to make late offline failure rollback
possible; images still exist only during reload/explicit recording. Explicit reload checks
source bytes as well as mtimes. Block even read-only engine queries/coroutines in replacement
initializers to guarantee that native resources/registrations cannot escape rollback; this
narrows reload compatibility but leaves initial execution unchanged. Root CLAUDE/AGENTS and
all official/game/template source files remain untouched. Existing generated template/Hello
guides still recommend restart after helper edits; that conservative instruction remains safe,
but their blanket cached-only description is outdated and left for a scoped guide refresh.
No commit; no process was killed by image name.

#### Allocator performance and template-guide follow-up (2026-10-07)

**Method and isolation.** Release desktop builds, Windows x64, Intel Core i7-13700K
(24 logical processors). Save the current executable, then build the same dirty-tree
source with only ScriptHost::Open's allocator condition changed from `heap_` to
`snapshotsEnabled_` (engine/script/ScriptHost.cpp:1608–1610): ordinary offline VMs use
`luaL_newstate`; recorded VMs retain SnapshotHeap. This is an allocator-path experiment,
**not** a historical whole-engine baseline. Neither benchmark enables recording or calls
reload. The experimental ordinary VM cannot safely perform the new transactional reload;
it is not a production alternative. Restore the exact source bytes, touch ScriptHost.cpp,
and rebuild all native targets (including oe_player) before final tests. Source, root
instructions and all checked-in runtime hashes match their pre-experiment copies.

Use persistent `oe script` subprocesses and Python perf_counter around synchronous API
responses. Eight fresh paired processes per workload: discard the first pair, retain seven;
alternate baseline/current order. No builds/tests run concurrently with measurements.
Microbenchmark: 100,000 iterations allocating an outer table, nested table and formatted
string; checksum 5001938895. Force full GC before/after each chunk, two warmup chunks and
three timed chunks per process; use each process's median, then seven-process median.
Thus micro timings include GC and IPC, not only allocation. Game replays: fixed date
2031-03-03, startup three frames, 60-frame warmup, ten 60-frame sim.step batches (600 timed
frames); exclude setup/input/eval and final software screenshot. Wickbound loads the run
scene and alternates D each batch; final phase playing, 8 kills, HP 100, oil 83.4249991.
DongleZoo enters play, resets drop cooldown/tier before each batch and performs ten drops
at the unchanged holder position; final score 22, seven merges, two pieces. The drop
method ignores the extra experiment argument; this is a fixed-position workload, not
pointer-location coverage. Table values/checksums, full save/state/error records and final
hashes match baseline/current in **every pair**, including warmup. Game hashes:
Wickbound `14c0dceb2f495d64`; DongleZoo `e26f09576132bbc6`.

| Workload | luaL_newstate median ms (min–max) | SnapshotHeap median ms (min–max) | Increase | Simulation ms/frame |
|---|---|---|---|---|
| micro | 45.834 (45.457–46.078) | 191.591 (188.984–192.739) | 318.0% | N/A |
| Wickbound | 277.050 (275.065–279.158) | 318.537 (314.343–321.168) | 15.0% | 0.462 → 0.531 ms |
| DongleZoo | 183.462 (181.392–186.395) | 232.630 (232.021–238.682) | 26.8% | 0.306 → 0.388 ms |

Windows GetProcessMemoryInfo on each owned process, sampled after the final screenshot
(or micro chunks); seven-process medians below. Peak working set is the OS process peak,
not a continuously sampled Lua allocation peak. Resident/private values include assets,
physics, fonts and screenshot buffers; screenshots are outside timed simulation.

| Workload | Baseline peak WS / resident WS / private MiB | SnapshotHeap peak WS / resident WS / private MiB |
|---|---|---|
| micro | 11.301 / 11.301 / 3.996 | 11.594 / 11.363 / 4.039 |
| Wickbound | 197.680 / 139.336 / 131.410 | 198.680 / 140.965 / 133.062 |
| DongleZoo | 74.715 / 58.992 / 50.914 | 75.914 / 60.559 / 52.773 |

**Judgement: meaningful overhead, not noise.** Game ranges do not overlap; the allocator
adds about 0.069 ms/frame in this short Wickbound replay and 0.082 ms/frame in DongleZoo.
Absolute times remain small here, but +15.0%/+26.8% simulation time and 4.18x allocation
loop time do not support a blanket negligible-cost claim. No production logic changes
are made in this follow-up. Single loaded desktop machine, no CPU pinning/power-mode
control, short early-game fixtures and API round-trip timing limit generalization;
these are simulation batch averages, not rendered-frame percentiles. No mobile/web
performance measurements or reload-image latency measurements were made.

**Proposed optimization, approval required for a separate implementation:** preserve
always-tracked VM allocations and transactional/native image semantics, but prototype
size-class pools and a lower-cost allocation ownership/index path. Existing code creates
a shared Block and map node per allocation and allocates/copies on every nonzero resize
(engine/core/SnapshotHeap.h:13–18, :24–34, :47). An optimized path must retain old blocks
while images reference them, restore exactly the captured live set and pass the current
failure/recording/future-replay tests. Benchmark it against both paths before adoption.
A cheaper ordinary allocator with tracking from VM creation and image-aware retention is
another candidate, but requires explicit block adoption/ownership design. Simply switching
luaL_newstate VMs to SnapshotHeap immediately before reload is unsafe: preexisting live
blocks are absent from its live set, so rollback misses them; lua_setallocf alone cannot
migrate/register the whole VM. Opting out of tracking until Stop/Play sacrifices the new
late offline transactional reload guarantee. These are designs, not implemented features.

**Top-level initializer audit (read-only).** Lexical Lua token/block scan excludes comments,
strings and function bodies across 131 files in templates/default/scripts, samples/*/scripts
and games/*/scripts. Review all distinct remaining root callees and project helpers; no
prohibited native engine/RNG calls were found. This is source inspection, not execution of
every game's reload path; dynamic/eval/generated future code remains unverified.

| Top-level site / candidate | Transitive inspection | Reload impact |
|---|---|---|
| samples/NetArena/scripts/player.lua:4; samples/Wickbound/scripts/run/player.lua:9: Controls.localSource() | engine/script/lua/oe/input.lua:7–13 only constructs closures; net.state at :4 is called later by check during input reads | Safe initializer; no blocked editing flow found. These are not native calls during module initialization |
| samples/Wickbound/scripts/lib/world.lua:21: W.reset() | :8–20 only assigns Lua fields/tables | Allowed initialization; affected module-local/shared export state resets under the documented reload semantics |
| samples/Wickbound/scripts/lib/balance.lua:40–49: E(); lib/defs.lua:87, :111, :137, :154: index() | balance.lua:33–37 builds data; defs.lua:81–85 builds an id map with ipairs | Pure data construction; allowed |
| Remaining root require/ipairs/rawget and math.sin/cos/rad/sqrt calls | Imports/data processing and pure math; no math.random/randomseed found at root. games/DongleZoo/scripts/lib/ads.lua:27 rawget(_G,"host") is a lookup, not host.call | No prohibited engine call found; import initializers still obey the same transitive guard |
| templates/default/scripts (all six) | player_controller.lua:20 creates localSource inside onStart, not at root; root imports build exports | No template initializer blocks live reload |

Template EDITING.md and project AGENTS.md now describe script.reload, initializer restrictions
and retained instance/computed-label state. Hello regeneration uses --name Hello --no-team;
verify absolute paths, move old Hello to build/allocator/previous-Hello, regenerate and restore
its private .oe/editor.ini. Full file SHA256 comparison: **only AGENTS.md and EDITING.md differ**;
existing scripts, sounds, scenes and private settings are byte-identical. Before/after Hello
120-frame software hash: `1bc172584a16114c`; new no-team Smoke project has the same hash.
Smoke's six script.check calls return zero errors/warnings. Final build.bat and full native
235-test suite exit 0, zero failed checks, including template, reload, network and snapshots.
No API schema changes: docs/API.md regeneration unnecessary. No final engine/official source
or checked-in runtime change: web/Android rebuilds unnecessary and not executed in this unit.
All owned benchmark/smoke processes were closed; ignored evidence/backups remain in
build/allocator/{bench.py,bench.log,results.json,root-calls.json,build-final.log,
build-guides.log,native-tests.log,smoke-check.json,hello-before-render.json,
hello-after-render.json}. No commit, game-script edits or root guide changes.

## 7. Verification and acceptance

Record an editing baseline before migration, then repeat these tasks after each relevant pilot:

1. Change player speed through one documented parameter or data location.
2. Change a button label through one documented text location.
3. Add a shop product without editing unrelated gameplay or layout code, where the shop supports
   data-driven entries; explicitly document any remaining scene authoring step.
4. Change a merge score rule in its dedicated function or table.
5. Adjust a popup and its text without understanding projection or font-width approximations.

Track edit locations, unrelated concepts required, and correctness after the change. Adopt the
M0 baseline-specific acceptance revision in 6.1: one location for scalar rules/labels; record
generated artifacts separately, and mark inapplicable tasks rather than substitute them. No
knowledge of projection, font metrics or input plumbing should be needed for those edits. Line
count is supporting evidence only. Prefer reviewable small functions over compressed one-line code.

Required implementation checks:

- Build and `oe_tests`; add meaningful API-driven tests for new behavior.
- Coordinate tests: rotated and parented cameras, perspective/orthographic modes, offscreen and
  behind-camera points, plane no-hit cases, view sizes, and canvas scaling.
- Text tests: fonts, CJK, multiline/wrap, spacing, and rich text consistent with actual layout.
- Handle/module tests: missing or ambiguous widgets, destroyed entities, scene changes, reload,
  owner cleanup, simulation restart, and `sim.save_state`/`sim.load_state` (cached handles must
  not survive a restore pointing at the wrong entity).
- Determinism tests: unused presentation queries preserve simulation state and future behavior
  at the same explicit view/resources; no query result drives simulation rules in the pilots.
  Cosmetic rendering changes are not expected to preserve screenshot hashes (6.1).
- Tooling tests: `script.check` and `script.params` on scripts that `require("oe.*")`.
- Input tests: injected keyboard/pad/stick input, zero vectors, focus suppression, pause, and
  existing authoritative/lockstep/rollback input contracts where affected.
- Migration checks: deterministic behavior under equivalent inputs, relevant existing
  playthroughs, and visual screenshots inspected for layout-sensitive changes.
- Registry and Lua documentation; regenerate `docs/API.md` for command/component changes.
- Update templates and project instructions where needed (including the `engine/script` code
  map entry for any `engine/script/lua/oe/` directory and the SCRIPTING.md API table);
  preserve identical `AGENTS.md` and `CLAUDE.md`. Rebuild affected prebuilt runtimes or record the remaining platform work.

Match existing test scope to each milestone rather than require unrelated platform work for
a documentation-only or pure Lua change. The original proposal was documentation-only;
implementation and execution evidence are recorded below.

## 8. Implementation status (work log)

- [x] Write the improvement proposal from current project and scripting API observations.
- [x] Review pass: added namespace, entity-ID, determinism, tooling, performance and baseline-number requirements (docs only).
- [x] M0: investigation and follow-up design complete; M2 schema selection explicitly deferred to M2 start (6.1.G.1).
- [x] M0 inventory: compare repeated script categories against bindings, UI implementation, and docs with file:line evidence.
- [x] M0 identity investigation: trace normal deletion, forced IDs, scene replacement, stop, and native restoration.
- [x] M0 namespace/delivery decisions: adopt `oe.ui` forwarding and embedded modules; document tools, cache/reload, and packaging effects.
- [x] M0 semantic contracts: record coordinate/measurement units, visibility, presentation-only rules, and snapshot constraints.
- [x] M0 static baseline: predict edit files/functions and ancillary concepts for all five tasks; mark unsupported cases.
- [x] M0 follow-up: select Scene epoch/Lua session design, module grammar and `@oe/` source spelling, and embedded fingerprint agreement; compare two M2 signatures without selecting one.
- [x] M1a planning only: record file map, implementation order/commit units, candidate tests, and required runtime rebuilds in 6.2.
- [x] M1a-1: implement runtime Scene epoch/copy/ReplaceWith and read-only Lua/command queries; add four identity/lifetime tests using a minimal test-only Lua handle pattern.
- [x] M1a-1 desktop validation: Release build, all 213 tests (zero failed checks, exit 0), CLI scene.epoch query, and API regeneration.
- [x] M1a-2: implement official source loader, minimal embedded entities/ui modules, and virtual-source tools; convert M1a-1 handle scenarios to the official module.
- [x] M1a-2 desktop validation: Release build, all 218 tests (zero failed checks, exit 0), direct CLI source tools/import, three-frame import render, and isolated packaged-player import (exit 0).
- [x] M1a-3: implement official manifest agreement; desktop 221 tests and Wasm 186 tests pass (exit 0); isolated desktop/web package imports and signed Android APK verified. M1a foundation is complete.
- [x] M1a-1/2/3 runtime rebuilds: desktop player, web JS/Wasm, Android arm64-v8a and x86_64 refreshed; contain scene.epoch, official modules and agreement prefix.
- [x] M1a-3 digest-to-ContentHash/blueprint integration, simulated mixed-build barriers and browser import verified; protocol v4 unchanged.
- [ ] Android device/emulator execution: no device/emulator available; import, networking and 16 KB device execution remain unverified. Requires a connected device/emulator; see runtime/android/README.md.
- [x] M2a start: choose named coordinate requests/result schema and tolerances using three pilot callers (6.1.G.1).
- [x] M2b start: adopt named measurement schema and exclude fitting after pilot inspection (6.5).
- [x] Later evidence: M2a/M2b/M3 costs recorded; all applicable DongleZoo edit tasks executed/reverted at M4 (6.7), player speed N/A.
- [x] M1b: common widget methods and DongleZoo/OnlineRPG UI pilots; 223 desktop tests and real-process OnlineRPG end-to-end pass; five before/after image hashes identical; three edit tasks measured and restored.
- [x] M2a: implement camera/canvas coordinate queries and replace DongleZoo/OnlineRPG approximations; desktop/Wasm, images and runtimes verified below.
- [x] M2b: renderer-backed measurement and toast/bubble pilots; 228 desktop/192 Wasm tests, OnlineRPG process and web package/browser smoke pass (6.5/8).
- [x] M3: pure Lua input/math modules, Wickbound/NetArena pilots, desktop 230 / Wasm 194 tests and packaged browser keyboard movement verified (6.6); physical touch/controller and Android execution remain unverified.
- [x] M4: split DongleZoo with explicit services, unify shop data, add EDITING.md; identical 13-checkpoint replay, five-task measurement (speed N/A), native 231 tests and desktop/web package smoke verified (6.7). Required-only reload restart limitation documented.
- [x] M5: update two default scripts/agent guidance and EDITING.md, regenerate no-team Hello, validate 232 native tests and exact replay; optional dialog/effect modules declined with evidence (6.8).
- [x] M1b affected runtime rebuilds: desktop, web JS/Wasm and Android arm64-v8a/x86_64 completed. Android device execution remains unchecked separately.

At proposal, M0 investigation, and M0 follow-up time only source/doc inspection and design
documentation had been performed. Static baseline estimates remain unexecuted edit measurements.
The M1a-1 entry below supersedes the earlier no-implementation/no-execution status for that unit
alone. M1a-1 claimed no project migration or visual verification. No commit was made, as requested.

M1a-2 additionally inspected the isolated import smoke render below; at that unit no game migration or
layout acceptance test for an existing project is claimed.

### M1a-1 identity primitive and validation (2026-10-06)

Implemented runtime-only `uint64_t` epoch, starting at 1, preserved by Scene copies and omitted
from JSON. Clear checks exhaustion before touching state; exhausted Clear/FromJson/ReplaceWith
throw `std::overflow_error` without wrapping or changing content. ReplaceWith clones content
before adopting it and advances the receiver's epoch, including self-replacement
(`engine/scene/Scene.h:33`, `engine/scene/Scene.cpp:12`, `:33`, `:43`, `:62`). Ordinary create/destroy
does not change epoch. A private friend test fixture reaches the exhaustion boundary without
adding a public epoch setter. Formatting is a fixed 16-character lowercase hex string, independent
of locale, RNG, or clock (`engine/scene/Scene.cpp:51`). Lua binding uses Guard and the existing
function-table registration (`engine/script/ScriptHost.cpp:309`, `:1610`); the command has empty
Params and mutates=false (`engine/api/Commands.cpp:633`). SCRIPTING/API documentation is updated.

Prefab isolation now adopts with ReplaceWith (`engine/app/Engine.cpp:1072`). Load, Lua scene
transition, stop with play snapshot, undo/redo, and prefab close already pass through FromJson;
their VM reset behavior is unchanged. NativeState capture/restore already copies Scene alongside
Lua (`engine/app/EngineSync.cpp:31`, `:51`, `:60`), so EngineSync needs no change or duplicate state.
Fresh preview-engine Scene assignment also remains unchanged (`engine/app/EngineNetwork.cpp:71`);
independent Lua sessions cannot authorize each other's handles even with equal epoch strings.

Added `SceneEpochClearCopyAndReplacement` (`tests/tests.cpp:1896`),
`LuaHandlesRejectDestroyedAndReplacedWorlds` (`:1962`),
`LuaHandlesRestoreWithNativeSnapshots` (`:1986`), and
`LuaHandleSessionAndEditorReplacement` (`:2014`). The Lua tests use private session/weak-record
tables plus epoch/existence checks, not an official module. They cover component loss, deletion,
same-scene/same-ID reload, saved closure restoration, future/branch handles disappearing, ID reuse
after rewind, foreign/reconstructed records, stop with/without a play snapshot, editor undo/redo,
prefab entry/close, and scene.load. Recording begins before Lua execution and fixture resources
are written before recording. Only read-only stopped-editor probes call ScriptHost::Eval directly
to avoid clearing the redo stack or marking the prefab dirty; no recording is active for those
probes. This is outside the recording contract and does not relax it.

Validation: Windows Release `build.bat` succeeded (exit 0) with no compiler warnings. After the
Scene.h change, engine/tool/test C++ translation units were touched conservatively to avoid the
reported dependency issue; timestamps only, no unrelated source edits. Full `build/bin/oe_tests`
completed with **213 tests, 0 failed checks, exit 0** (captured in `build/m1a1-tests.log`), including
all four new tests, `SimulationReferenceSnapshotAcrossSceneChanges`,
`SimulationNativeSnapshotBudgetAndBranching`, `NetworkFrameSyncMismatchBarrierAndLargeWorldHash`,
`ScriptCheckAndParams`, and `AgentInstructionsInSync`. CLI `oe exec samples/Hello scene.epoch` returned
`{"ok":true,"result":{"epoch":"0000000000000002"}}`. Regenerated API with the built executable's
`oe api --markdown > docs/API.md` output. No official loader, Lua module, virtual path, ContentHash,
protocol, game, template, or prebuilt runtime content changed in this unit. Web/Android rebuilds
are deferred by scope; device/browser behavior is Unverified. No user decision is required.

### M1a-2 official source foundation (2026-10-06)

Added immutable OfficialModules manifest descriptors, strict reserved-root/import grammar,
canonical virtual-source resolution, project-path conflict guards, and a length-prefixed
FNV-1a manifest digest with primitive revision 1 (`engine/script/OfficialModules.cpp:35`, `:44`,
`:63`, `:72`, `:83`, `:93`). The digest is computed only; EngineSync/ContentHash/blueprint and
Session are unchanged. CMake enumerates official UTF-8 sources in fixed order, rejects BOMs,
normalizes CRLF/CR to LF, and emits build-only `generated/EmbeddedLuaModules.cpp` into the shared
engine, with configure/glob dependencies and no output rewrite for identical bytes
(`CMakeLists.txt:146`). Generated output is under the ignored build directory.

The guarded require branch resolves official aliases before origin/cache lookup, uses
`embedded:<canonical-name>` and `project:<existing-project-path>` cache keys, and never falls
back for missing reserved names (`engine/script/ScriptHost.cpp:1121`). The official loader's
heap-owned sentinel detects cycles; failures clear markers, nil becomes true, false remains
cached, text-only chunks use canonical virtual paths in diagnostics, and nested official imports
inherit the caller's instruction budget (`:1810`). Project import return values, path aliases,
attachment reload, and required-only cache behavior remain compatible. LoadFile also applies
the common project-source guard, so attachment is not a reserved-path bypass.

`oe.entities` keeps session/weak handle records solely in the Lua heap. find/tryFind take an
explicit optional component capability; every id/get/set validates records, epoch, existence,
acquisition capability, and requested component. Unknown capabilities follow the same descriptive
failure path. `oe.ui` shares this validator and forwards native focus/focused function values
without altering global ui. No owner framework, gameplay composition/migration, or M2 query
was added. SCRIPTING.md publishes the minimal operations and reload/handle limitations.

Source tools share ResolveScriptSource. List adds embedded/readOnly entries and reservedConflicts;
read/check/params support canonical @oe paths without executing imports or opening the live
simulation VM; inline source precedence is preserved with validated diagnostic labels. Write
rejects virtual and normalized physical reserved paths (`engine/api/Commands.cpp:1235`, `:1268`,
`:1280`, `:1345`). SandboxGlobals retains its temporary host and ScriptCheck needs no code change.
API command descriptions clarify virtual paths and attached-only reload; regenerate API after build.

Added five OfficialLua tests (aliases/reserved names, cycles/failure/budget, read-only tools,
project imports/reload, and UI focus forwarding). Existing LuaHandle scenarios now use the actual
official module, including native restore and editor undo; the broader fingerprint/blueprint/
recording-agreement candidates remain for M1a-3. **Test design adjustment:** synthetic immutable
source descriptors exercise the production LoadOfficialModule method through a test-only Lua
dispatcher, rather than shipping cycle/budget fixtures in the official manifest. A private
friend fixture accesses test Lua state; no public Lua/CLI source override or hidden native handle
registry was added. Direct synthetic-loader/editor probes run without recording; actual snapshot
scenarios still start recording before Lua execution through Engine::Call.

Build history: the first attempt failed with C2248 (test accessing private State); corrected
with the private test friend. The next broad rebuild encountered MSVC C1001 at xmemory:1949;
an unchanged-source retry succeeded, with no compiler/optimization workaround. Header changes
were followed by conservative engine/tool/test C++ timestamp touches, without unrelated content
edits. Final validation results are recorded below. Web/Android rebuilds,
digest agreement, project migrations, and M2 queries remain deferred by task scope. No commit
was made and no user decision is required.

Executed validation:

- Final Windows Release `build.bat`: exit 0, no compiler warnings. Editing the official Lua
  input automatically reran configure and rebuilt EmbeddedLuaModules.cpp; its output is ignored
  by Git (`git check-ignore build/generated/EmbeddedLuaModules.cpp`).
- Final `build/bin/oe_tests`: **218 tests, 0 failed checks, exit 0**; output in
  `build/m1a2-tests.log`. The five new OfficialLua tests and the three updated LuaHandle tests
  pass; the Scene epoch fixture and existing native/scene-change/branching/network tests also pass.
  An earlier full run also passed 218 tests before the unknown-capability diagnostic refinement.
- Isolated temporary project outside the checkout, containing only project.json, one scene,
  and scripts/smoke.lua: `oe exec ... script.eval` confirms oe.ui alias identity; no official
  project files exist. Direct `oe exec ... script.list/read/check` exposes both official sources,
  embedded/readOnly flags, canonical @oe/ui.lua source, and **0 errors/0 warnings**. script.params
  returns an empty parameter list. Captured final tool results are in
  `build/m1a2-cli-list.json`, `m1a2-cli-read.json`, and `m1a2-cli-check.json`.
- `oe render ... --frames 3 --out build/m1a2-smoke.png`: frame 3, software renderer,
  hash `64142b1c80816077`; script assertions pass and log M1A2_IMPORT_OK. Image inspected: the
  label reads "Embedded modules work", set through the official handle's partial update.
- `oe package` copies only the fixture's three project files and the rebuilt player. Run the
  packaged OfficialSmoke.exe from its isolated package directory outside the checkout, with
  `--set smoke_quit=1`: assertions/import log M1A2_IMPORT_OK, then self-exit **0**. No official
  Lua files or source checkout are in that package; no image-name termination or forced process
  termination was needed. Logs: `build/m1a2-package.stderr.log` and package manifest JSON in
  `build/m1a2-package.json` (temporary fixture location in `build/m1a2-smoke-root.txt`).
- API regenerated from the final built executable with `oe api --markdown > docs/API.md`;
  `git diff --check` passes. Before/after SHA-256 checks confirm CLAUDE/AGENTS, EngineSync.cpp,
  and existing web Wasm content were preserved. No web/Android rebuild or digest-agreement
  test was run; those remained explicitly unchecked at M1a-2; see the completed M1a-3 entry below.

### M1a-3 agreement and release verification (2026-10-06)

**Implementation:** Engine ContentHash now starts with
`official-lua-v1:1:9ba77240e176a039\n` before sorted project records
(`engine/app/EngineSync.cpp:77`). The record is unconditional and does not consume
the 4096-project-file allowance. OfficialModules' pure digest overload sorts module
names and normalizes CRLF/CR to LF; no filesystem metadata or Scene epoch enters it
(`engine/script/OfficialModules.cpp:99`). The production provider always uses the
compiled manifest; only a private Engine friend seam in tests can replace its record
(`engine/app/Engine.h:253`, `tests/tests.cpp:72`). No public command, Lua API or
launch option can spoof it. The manually maintained primitive revision remains 1.

**Shared paths:** lockstep/rollback blueprint construction at EngineSync.cpp:103,
authority at EngineAuthority.cpp:72, and the Authority late-join barrier at
engine/net/sync/Authority.cpp:89 all consume the same hash. No separate authority
edit or duplicate digest state was necessary. Protocol v4, message numbers and
wire layouts are unchanged. Recording checks ContentHash before restoring
(`EngineSync.cpp:225`); native Scene/Lua snapshots need no added field.

**Tests:** OfficialLuaManifestFingerprintIsStable verifies ordering, source newline
normalization, path/metadata/epoch independence, source/revision changes and the
legacy project-only hash. OfficialLuaBlueprintMismatchBarriers exercises matching,
changed and legacy fingerprints in lockstep/rollback/authority, plus accepted and
refused authoritative late joins with existing mismatch diagnostics. This is a
controlled mixed-runtime model, not execution of an archived runtime binary.
OfficialLuaRecordingRestoresCacheAndHandles enables recording before Lua execution,
then requires modules, saves, branches, restores and replays; it checks cached aliases,
saved closures/handles, removal of future globals/entities, repeatable RNG/timer
state, read-only epoch queries, and state_resource rejection after a fingerprint change.
The previous snapshot/frame-sync/handle regressions remain passing.

| Target | Executed verification | Result / limits |
|---|---|---|
| Desktop | build.bat; build/bin/oe_tests; oe api --markdown; isolated desktop oe package and OfficialSmoke.exe --set smoke_quit=1 | Build/player linked; 221 tests, zero failed checks, exit 0; AgentInstructionsInSync passes; packaged import marker and exit 0 |
| Web | EMSDK=E:/emsdk, build_web.bat; OE_BUILD_TESTS=ON and oe_tests target; Emsdk Node 24.19.0 build-web/bin/oe_tests.js | Player build exit 0; 186 tests, zero failed checks, exit 0 |
| Web package | oe package --web; oe serve on isolated port; Codex in-app Chromium browser | Source-independent game.pak import, alias identity, focus forwarding, handle get/set and scene.epoch succeed; marker ok and rendered text observed; tab/server cleaned up |
| Android | build_android.bat with local SDK/NDK; oe package --android; apksigner verify; aapt2 dump badging; zipalign -c -P 16 4 | arm64-v8a and x86_64 build exit 0; APK signed with debug key; v2/v3 valid, both ABIs and API 26/35 present, alignment exit 0 |
| Android device | No connected device/emulator available | **Unverified (기기 실행 미검증)**; requires device/emulator for Lua import, UI/network and 16 KB device checks |

**Artifacts / provenance:** base commit
eca56058cf63b79e0535d5f16c6ea33fcb3a3148 plus current uncommitted source, including
pre-existing networking edits; no commit was created. Updated web JS is 127,442 bytes,
Wasm 4,295,326 bytes; Android arm64 library 6,975,920 bytes and x86_64 library
7,332,072 bytes. The temporary both-ABI APK is 14,336,681 bytes. Runtime READMEs
record SHA256, toolchains and source/fingerprint provenance. Replacing these players
is required to ship M1a-1/2 primitives/embedded sources and M1a-3 agreement; repackaging
project scripts alone is insufficient.

**Corrections / deviations:** the first desktop suite ended with 221 tests and one
failed check because the new fingerprint test used a nonexistent template script
name; it now selects an actual script, and the complete suite was rerun successfully.
The first Wasm test compile exposed an existing unguarded EditorAddPresetsAreValid
test; adding OE_NATIVE_EDITOR around only that test restores web compilation without
changing engine/editor behavior. Headless Edge launch was rejected by automatic
execution approval without a detailed reason, and the browser connector reported
Edge unavailable; verified in-app Chromium instead. Existing network-source Clang
aggregate-initializer warnings, the existing Wasm-only unused ProcessFixtureDir test helper
warning, and Java native-access warnings remain; no new warning
was observed in M1a-3 engine changes. Header consumers were touched before full builds
to avoid the environment's missing header dependencies.

**Scope / handoff:** M1a is closed for implementation and available release checks.
Android device checks remain explicitly open; M1b project pilots and M2 schemas/queries
are untouched. CLAUDE.md/AGENTS.md received only the requested engine/script map addition
and remain identical. Unrelated working-tree source/game changes were preserved.
No user decision is needed for the implemented contract; provide a device/emulator
only when device validation is scheduled.

**Files changed by M1a-3 (content, separate from earlier dirty work):**

| Group | Files |
|---|---|
| Engine / tests | engine/script/OfficialModules.h, engine/script/OfficialModules.cpp, engine/app/Engine.h, engine/app/EngineSync.cpp, tests/tests.cpp |
| Documentation / code map | docs/LUA_USABILITY.md, docs/SCRIPTING.md, docs/NETWORK_NOTES.md, docs/PLATFORMS.md, docs/ANDROID.md, runtime/web/README.md, runtime/android/README.md, CLAUDE.md, AGENTS.md |
| Rebuilt binaries with changed bytes | runtime/web/oe_player.wasm, runtime/android/arm64-v8a/liboe_player.so, runtime/android/x86_64/liboe_player.so |
| Re-executed generation with identical bytes | docs/API.md regenerated; runtime/web/oe_player.js rebuilt/copied, but unchanged from the pre-M1a-3 working tree |

APK extraction also verified both packaged libraries' SHA256 equal the refreshed
runtime libraries. Pre-turn hashes of already modified tracked files were compared;
only the intended M1a-3 files changed content. Platform docs and Android binaries
that were previously clean are included above. Temporary fixtures/packages/logs
are outside tracked sources. No project, template or M1b/M2 source was edited.

### M1b UI/entity pilots (2026-10-06)

- [x] Gap analysis before wrapper edits; adopted methods and caller evidence in 6.3.
- [x] Embedded oe.ui adds capability-aware selected-component widget methods; private records
  retain oe.entities identity; global ui untouched. No C++ binding or primitive revision change.
- [x] DongleZoo generic lookup/field/text/visibility delegates to official widgets with fresh
  acquisition. Localization tag iteration, RGB/number formatting, toast state and textWidth
  estimation remain project code. OnlineRPG widgets.lua is a two-line import compatibility shim.
- [x] Before/after five render hashes and PNG bytes identical; images inspected; fixed date,
  view, pointer clicks and frame counts recorded in 6.3 and build/m1b/compare.py/results.
- [x] Resume label, generator-owned pause width/title and combo multiplier actually edited,
  verified in runtime, and restored byte-for-byte. Generator invocation was scene() only,
  not prefabs(). Final app.lua/i18n.lua/generator/scene have no diff from their pre-task bytes.
- [x] Official @oe/ui.lua script.check: zero errors/zero warnings. Post-experiment final captures also match the original five hashes. git diff --check passes.
- [x] Desktop build.bat and all 223 tests pass, zero failed checks, exit 0. New
  OfficialLuaWidgetCapabilitiesAndAmbiguity and OfficialLuaWidgetsRestoreAndRejectReplacement
  cover selection, messages, partial updates, focus, visibility, destruction/component loss,
  scene replacement and recorded heap restoration. TemplateGamePlaythrough, DongleZooGame,
  WickboundSample, OnlineRPGGame and earlier identity/agreement regressions pass.
- [x] games/OnlineRPG/server/build.bat and tests/online_rpg_test.py exit 0. Real-process
  login/channel/tickets/world, pickup/chat/drop, leaving, forged/reused/misdirected tickets,
  server death popup/quit and channel death/recovery pass. The harness cleans its own PIDs.
- [x] Final web build_web.bat (EMSDK=E:/emsdk, BINARYEN_CORES=1) and build_android.bat
  (NDK r28c, both supported ABIs) each run once after official source was finalized; exit 0.
  Updated runtime hashes, sizes and provenance are in both READMEs.
- [ ] M1b-specific Wasm suite/browser execution and Android device/emulator execution:
  not run in this unit. M1a-3 browser/Node/APK results are historical evidence, not M1b tests.
  A device/emulator remains required for Android UI/import execution.

**Agreement:** manifest digest changed from 9ba77240e176a039 to 58adf79c7598d716
(primitive revision remains 1). Matching runtimes must be deployed together; existing v4
blueprint mismatch barriers still apply. Only Lua composition changed; no command/component
schema changed, so API.md regeneration was unnecessary.

**Deviations / limitations:** an initial test compile used the two-argument SetParent
call; the existing three-argument signature was applied and the desktop build/full suite
then passed. A first capture-driver attempt inadvertently ran the original Python test
module on import; it completed its end-to-end run and cleanup before capture. The driver
now extracts only JsonProcess and uses inline screenshots to write outside project data;
all reported baseline/final captures completed before/after wrapper edits respectively.
No image-name process termination, background server leak, commit, CLAUDE/AGENTS change,
Wickbound edit, M2/M3 query/input, app.lua split or template edit occurred.

**Files changed by this unit:** games/DongleZoo/scripts/lib/ui.lua;
games/OnlineRPG/scripts/lib/widgets.lua; engine/script/lua/oe/ui.lua; tests/tests.cpp;
docs/SCRIPTING.md, docs/LUA_USABILITY.md, docs/DONGLEZOO.md, docs/ONLINERPG.md;
runtime/web/README.md, runtime/android/README.md; rebuilt runtime/web/oe_player.wasm
and both runtime/android/<abi>/liboe_player.so files. Web JS was rebuilt/copied with
identical bytes. entities.lua and C++ implementation files were unchanged.

### M2a coordinate queries and pilots (2026-10-06)

- [x] Before coding, select named request alternative B, with three real caller rewrites,
  result/no-hit/error schemas, units, explicit camera/local view and numerical tolerances
  (6.1.G.1). Text measurement/fitting remains a separate M2b decision.
- [x] Extract MakeCameraView from the exact existing renderer matrices; retain first-active
  renderer policy and fallback. Shared QueryCoordinates backs four Guard-wrapped bindings
  and read-only Params commands. No new platform-specific code or hidden native cache.
- [x] Replace DongleZoo pointer/label math and OnlineRPG pitch/reference-height projection;
  preserve anchoring, near-depth policy, layout/scoring/save/text estimates and local UI use.
- [x] Minimal oe.ui.screenToCanvas forwarding only; global ui focus signatures remain.
  Digest changes 58adf79c7598d716 -> 2b06cf1f108cb7cf. Existing primitive behavior remains
  unchanged, revision 1; additive native queries are distributed in all rebuilt players.
- [x] Final desktop build.bat (oe/oe_player/oe_tests), all 226 tests, zero failed checks,
  exit 0. CoordinateProjectionRayPlaneAndCanvas and CoordinateQueriesRasterAndRecordedFuture
  cover 6.4's coordinate/raster/recorded-future contract. Existing GPU, three game,
  template, scene identity and blueprint/recording regressions pass.
- [x] Four direct oe exec queries and official script.check pass; API.md regenerated.
- [x] Eight before/after software render hashes and PNG bytes identical; drop and resized
  nameplate/bubble images directly inspected; pointer difference <=4.4e-8 world units.
  Initial five captures precede implementation; supplemental richer captures use preserved
  legacy executable/formulas and restore source bytes. Exact method/evidence in 6.4.
- [x] Query timing measured: 100 warmed batches of 50 reusable-request calls, median
  1.287ms including IPC; approximately 1.81us/query above empty-loop/IPC median, noisy
  loaded-machine estimate. Results/method in build/m2a/performance.json and 6.4.
- [x] games/OnlineRPG/server/build.bat and tests/online_rpg_test.py exit 0; actual-process
  login/channel/world/ticket/pickup/chat/drop/failure/recovery checks pass. Harness cleans
  its own process IDs; no taskkill by image or persistent server/browser was used.
- [x] Final build_web.bat with EMSDK=E:/emsdk, BINARYEN_CORES=1, parallelism 2, exit 0;
  web player rebuilt once after source finalization. Configure OE_BUILD_TESTS=ON, build
  oe_tests and run Emsdk Node 24.19.0 build-web/bin/oe_tests.js: **190 tests, zero failed
  checks, exit 0**, including both coordinate tests and M1b widgets. This supplies the
  M1b-deferred Wasm suite execution without changing its historical work-log claim.
- [x] Final build_android.bat, local NDK r28c, API 26, parallelism 2: arm64-v8a and x86_64
  rebuilt once after source finalization, exit 0. runtime/ libraries refreshed; source/digest,
  artifact sizes and SHA256 recorded in runtime READMEs and build/m2a/artifacts.json.
- [ ] Android device/emulator execution: **unverified (기기 실행 미검증)**. No connected
  device/emulator available; requires physical/emulated pointer/UI/network execution.
- [ ] M2a browser interaction/package smoke: **unverified**, not executed in this unit.
  Wasm Node validation does not claim browser DOM/WebGL/pointer behavior or an APK run.

**Artifacts:** web JS 127,442 bytes (rebuilt/copied with identical bytes), Wasm 4,319,405
bytes; Android arm64-v8a 7,004,128 bytes, x86_64 7,364,584 bytes. Changed binaries contain
shared native camera/canvas queries and updated official source/digest; no packaging of
project Lua alone can supply these native operations. Base checkout commit
da530f75508eac9f2ec63266aba94aa9761925f2 plus the existing uncommitted working tree and
M2a changes; no commit was created. No APK/signing run is claimed in M2a.

**Corrections / deviations:** initial build failed with Commands.cpp C2664 (Register takes
const char*, not std::string); operation.c_str() fixes registration. The first coordinate
fixture omitted Transform on explicitly created cameras and terminated before suite
completion; fixtures now add it. The next suite reported 226 tests, 4 failed checks:
ray requests accidentally retained the position argument and the command schema correctly
returned unknown_argument. Dedicated ray requests and explicit response checks fix the test,
and the final complete suite passes. Header consumers were touched before full desktop build
because this environment misses header dependencies. No renderer approximation, wire-format,
network rule or scope expansion was needed. Legacy pitch math was correct for the current
OnlineRPG scene/fallback; its removal introduces no intended pixel difference here.
Existing Clang network aggregate-initializer warnings and Wasm unused ProcessFixtureDir
warning remain; no warning in the new coordinate implementation was observed.

**Files changed by M2a (content, separate from earlier dirty work):**

| Group | Files |
|---|---|
| Engine / module | engine/app/CoordinateQueries.h, engine/app/CoordinateQueries.cpp (new); engine/render/Renderer.h, engine/render/SoftwareRenderer.cpp; engine/script/ScriptHost.cpp; engine/api/Commands.cpp; engine/script/lua/oe/ui.lua |
| Tests | tests/tests.cpp |
| Games | games/DongleZoo/scripts/app.lua; games/OnlineRPG/scripts/lib/space.lua |
| Documentation | docs/LUA_USABILITY.md, docs/SCRIPTING.md, docs/API.md, docs/DONGLEZOO.md, docs/ONLINERPG.md; runtime/web/README.md, runtime/android/README.md |
| Rebuilt binaries with changed bytes | runtime/web/oe_player.wasm; runtime/android/arm64-v8a/liboe_player.so; runtime/android/x86_64/liboe_player.so |
| Rebuilt/copied identical bytes | runtime/web/oe_player.js |

Build logs and temporary PNG/JSON/drivers are under ignored build/m2a; no generated source,
fixture or test package is added to project data. Pre-task hashes of CLAUDE.md/AGENTS.md,
Wickbound HUD and oe.entities are unchanged; unrelated dirty source/game files were preserved.
Code-map update is proposed only in 6.4. M2a implementation and requested available checks
are complete; M2b/M3/M4/M5 and device/browser follow-ups remain open. No user decision is
needed for the adopted contract; a device/emulator is only needed for later device validation.

### M2b text measurement and pilots (2026-10-06)

- [x] Before code, decide named request ui.measureText, reference-pixel schema/line widths,
  errors/bounds and fitting exclusion with real toast/bubble evidence (6.5). Painted bounds
  excluded; caller provides resolved size for already shrunk text. Exact presentation rule
  remains in SCRIPTING.md; cache keys include effective scale, resource/style and wrapping.
- [x] Add public TextMetrics overload around the existing ParseRich/ResolveFont/Shape path,
  with the same effective-scale rounding/fallback/kerning/wrapping/bold growth. Renderer
  shaping/drawing and old MeasureText behavior remain unchanged.
- [x] Shared CoordinateQueries branch, guarded ui.measureText binding, read-only Params command
  and thin oe.ui forwarding. No mutable native measurement-result cache. Digest
  2b06cf1f108cb7cf -> dff0818781f4b2b4; additive API, primitive revision 1 unchanged.
- [x] Replace DongleZoo textWidth estimate and OnlineRPG ems/row-height estimate. Actual label
  style used; visible bubbles remeasure for view/style changes. Cap/padding/life, scores,
  localization and server/authoritative rules unchanged. No app split/template/Wickbound edit.
- [x] Six multilingual pre-edit captures and after images inspected: tighter measured pill
  widths/line heights, no layout regression. Japanese missing glyph boxes remain a prior font
  coverage limitation. Six unrelated UI control hashes/PNG bytes unchanged against M2a's
  stored baseline; 900px bubble change directly inspected. Tables/drivers in 6.5/build/m2b.
- [x] Task ⑤ actually edited generator popup width and EN/KO title, invoked scene() only,
  asserted/captured/inspected and restored saved source/scene bytes. Same 3 files/1 function
  and 3 ancillary concepts as M1b; zero character-category/font-width knowledge required.
- [x] Performance: warmed 100 batches of 50 queries, median 1.023ms including IPC; estimated
  IPC-subtracted ~8.07us/query. Method, limitations and caller cache guidance recorded in 6.5.
- [x] Desktop build.bat (including oe_player), final all 228 tests, zero failed checks,
  exit 0. TextMeasurementMatchesRenderedLayout and TextMeasurementCacheOrderAndRecordedFuture
  pass, plus GPU/software, DongleZoo/Wickbound/OnlineRPG/template, coordinate/identity/
  agreement regressions and AgentInstructionsInSync. Header consumers touched before build.
- [x] Direct CLI ui.measureText and official script.check: success, zero errors/warnings;
  docs/API.md regenerated. SCRIPTING/game docs and authorized code-map lines updated;
  CLAUDE.md and AGENTS.md remain identical, prior unrelated edits preserved.
- [x] OnlineRPG backend build and tests/online_rpg_test.py exit 0; actual login/channel/world,
  tickets, pickup/chat/drop and failure/recovery scenarios pass; harness cleans owned PIDs.
- [x] Final web player build_web.bat EMSDK=E:/emsdk, Emscripten 6.0.10, BINARYEN_CORES=1,
  parallelism 2, exit 0. Player built once after final engine/official source. Tests target
  rebuilt after a test-only warning fix; final Node 24.19.0 run: **192 tests, zero failed
  checks, exit 0**, after build completion and sequentially after desktop suite.
- [x] M2a-deferred package/browser smoke: --web isolated TextSmoke project outside checkout,
  oe serve and in-app Chromium/WebGL2. Official aliases/entities and new measureText,
  worldToScreen/screenRay/screenToPlane/screenToCanvas assert; rendered text, console and DOM
  marker observed. Only project/scene/probe are game.pak data; no official Lua files.
  Own server stopped by PID and browser tab closed. Evidence: package-web.log,
  browser-results.json and the observed browser screenshot. No Edge run claimed.
- [x] Final build_android.bat NDK r28c API 26, arm64-v8a/x86_64, exit 0; each player rebuilt
  once, runtime files refreshed. Both READMEs include bytes/SHA256/digest/source provenance.
- [ ] Android device/emulator execution: **unverified (기기 실행 미검증)**, none available.
  Device/emulator required; no new APK signing/device/network/audio/16 KB execution claim.

**Corrections / deviations:** implementation scope matches the decision. A test range-loop
copied strings; changed to const reference after Clang warned. Initial concurrent desktop/
Wasm execution collided in shared build/test_projects fixtures (TempProject uses fixed names):
Wasm reported 192 tests, 3 failed checks in NetworkAuthoritativePhysicsPrediction (CreateProject,
position.x>10, script errors); the concurrent desktop attempt logged a ReadTextFile failure
and exited 1. These are not successful validations. Desktop was rerun sequentially and all
228 pass. A first sequential Node launch overlapped its test-target relink and failed to
instantiate mismatched JS/Wasm (Import #0 env: module is not an object or function).
After compiler/linker completion, the final fresh Node run passes all 192 tests, exit 0.
No engine/network fixture behavior was changed to hide these execution-order errors.
Existing network initializer warnings and unused Wasm ProcessFixtureDir warning remain;
no new warning remains in M2b engine or tests. First browser tab binding was stale; a fresh
background tab loaded the same served package successfully. No unsafe process termination.

**Artifacts:** web JS 127,442 bytes (identical), Wasm 4,326,485 bytes; Android arm64
7,011,840 bytes, x86_64 7,373,752 bytes. Changed players include renderer-backed text queries
and embedded forwarding/digest; matching runtimes must be deployed. Base checkout
da530f75508eac9f2ec63266aba94aa9761925f2 plus pre-existing uncommitted work and M2b changes; no commit made.
Detailed hashes: runtime READMEs / build/m2b/artifacts.json. Logs: build.log/build-final.log,
tests.log (initial clean success), tests-sequential.log (final success), web-build.log,
web-tests-build-final.log, web-tests-final.log, android-build.log, rpg-e2e.log. Failed attempts
remain in tests-final.log, web-tests.log and web-tests-sequential.log for accurate handoff.

**Files changed by this unit (separate from earlier dirty work):**

| Group | Files |
|---|---|
| Engine / official module | engine/render/UI.h, engine/render/UI.cpp; engine/app/CoordinateQueries.cpp; engine/script/ScriptHost.cpp; engine/api/Commands.cpp; engine/script/lua/oe/ui.lua |
| Tests | tests/tests.cpp |
| Games | games/DongleZoo/scripts/lib/ui.lua; games/OnlineRPG/scripts/hud.lua |
| Docs / authorized code map | docs/LUA_USABILITY.md, docs/SCRIPTING.md, docs/API.md, docs/DONGLEZOO.md, docs/ONLINERPG.md; CLAUDE.md, AGENTS.md (only authorized map additions); runtime/web/README.md, runtime/android/README.md |
| Changed rebuilt binaries | runtime/web/oe_player.wasm; runtime/android/arm64-v8a/liboe_player.so; runtime/android/x86_64/liboe_player.so |
| Rebuilt/copied identical bytes | runtime/web/oe_player.js |

Temporary edit generator/i18n/main.scene.json bytes were restored; Wickbound lib/ui.lua
matches its pre-task hash. Other source/game dirty work was preserved. No engine/header/API
change followed final player builds; the only later source change was the test loop reference.
M2b and available requested verification are closed. Fitting/paint bounds, font provisioning,
Android device execution and M3/M4/M5 remain separate follow-ups. No user decision is needed
for the adopted measurement API; Japanese font coverage is an existing content limitation.

### M3 input/math composition (2026-10-06)

- [x] Record source/composition/focus/lifecycle decisions before implementation (6.6).
- [x] Add pure Lua oe.input / oe.math; migrate Wickbound keyboard + NetArena declared move.
- [x] Release build and full desktop suite: 230 tests, zero failed checks, exit 0
      (build/m3/build-source-final.log, tests-reviewed.log); oe_player linked.
- [x] Full sequential Wasm suite: 194 tests, zero failed checks, exit 0
      (web-tests-build-reviewed.log, web-tests-reviewed.log); explicit delayed rollback passes.
- [x] Five deterministic before/after hashes, PNG bytes and player positions identical;
      two NetArena peers agree with empty errors/desync; images inspected (6.6).
- [x] Web / Android arm64-v8a + x86_64 rebuilt and refreshed; README source/hashes updated.
- [x] Final isolated web package/browser import + real keyboard button movement verified;
      screenshot build/m3/browser-keyboard.jpg, x=0->1, pressed=true. Owned server/tab cleaned.
- [ ] Android device/emulator execution: no device available; requires connected device.
- [ ] Physical browser/device touch UIJoystick and gamepad: not exercised; requires touch
      or controller hardware/browser input test. Headless input.touch path passed.

No new native binding/command was necessary: existing shared focus and axis read paths
already implement suppression/dead zone. No header changes/touch workaround needed.
No command schema changed, so docs/API.md was not regenerated. CLAUDE.md/AGENTS.md
were untouched in M3 and remain byte-identical; existing official-module directory
code map already covers the two added Lua sources. No commit; unrelated dirty work
preserved. M4/M5/template generation and OnlineRPG migration remain outside scope.

Manifest digest and runtime sizes: 403839f0fff280c5; revision 1. Web JS 127,442 bytes
(identical), Wasm 4,331,275; Android arm64-v8a 7,016,672, x86_64 7,378,648. Changes
carry new official Lua sources/agreement bytes, not platform input adapter changes.
Runtime README tables include SHA256 and base checkout/source provenance.

Corrections/deviations: first complete desktop attempt had 230 tests / 17 failed
checks, exit 1 (tests.log): localSource initially treated nonnetwork state none as an
active session, disabling Wickbound; authoritative client expectation incorrectly
assumed remote input streams. Both corrected, then full suite passed. Final review
also tightened playerSource against native lobby fallback and added false-coordinate
validation. The runtime rebuild was repeated after that review (two passes rather
than requested one final pass); keeping the first binaries would omit the safeguard.
A baseline match-time script.eval was rejected as designed; scene.get replaced it
in the read-only driver. No unexecuted test/build/device check is marked complete.

| M3 changed-file group | Files (only this unit's edits) |
|---|---|
| Engine/modules | engine/script/lua/oe/input.lua, engine/script/lua/oe/math.lua (new); no C++/loader/ContentHash edits |
| Tests | tests/tests.cpp (two behavior tests and manifest-count generalization) |
| Samples | samples/Wickbound/scripts/run/player.lua; samples/NetArena/scripts/player.lua |
| Documentation | docs/LUA_USABILITY.md, docs/SCRIPTING.md, docs/INPUT.md, docs/NETWORK_SAMPLES.md; samples/Wickbound/README.md; runtime/web/README.md, runtime/android/README.md |
| Rebuilt changed binaries | runtime/web/oe_player.wasm; runtime/android/arm64-v8a/liboe_player.so; runtime/android/x86_64/liboe_player.so |
| Rebuilt/copied identical | runtime/web/oe_player.js |

Ignored drivers/logs/images/packages reside under build/m3 or the temporary isolated
InputSmoke directory, not project data. No user decision is required for this scope.


### M4 DongleZoo project readability (2026-10-07)

- [x] Pre-code function/state/service inventory and acyclic module boundaries (6.7).
- [x] Pre-code deterministic fixed-date menu/drop/merge/combo/item/shop/purchase/reward baseline.
- [x] App orchestration + stateless gameplay/screens/rewards/effects/scoring with explicit services.
- [x] One authored JSON product catalog; generator emits runtime Lua and scene, existing UIScroll handles additional rows.
- [x] EDITING.md: edit map, params/config choice, generated/authored ownership and reload instructions.
- [x] Identical 13 frame/PNG hashes, run/save/tag state and host call sequence after split.
- [x] Four applicable edit tasks actually applied, asserted/rendered, then byte-restored; player speed N/A.
- [x] Native build and all 231 tests exit 0; new save/catalog/context/snapshot/reload test passes.
- [x] All 15 Lua script.check results error/warning 0; script.params literal fallback intact.
- [x] Desktop package Play/quit smoke exit 0; packaged web served and clicked menu→Play in browser.
- [x] Project-only changes: runtime hash manifest unchanged, no web/Android rebuild required.
- [x] Historical M4 helper reload gap resolved by subsequent engine work in 6.10.
  M4 itself used Stop/Play and introduced no engine workaround.
- [ ] Android execution/real storefront and full packaged-web replay: not executed in M4.

Artifacts: build/m4/{before,after}-replay.json and PNGs, edit-results.json/edit-*.png,
script-check.json, build.log/build-final.log, tests.log/tests-final.log, package logs,
player-check.stderr.log/player-check.exit, web-menu.jpg/web-play.jpg, runtime-before.json.
Build/test rerun was needed only for the added project regression test and corrected test
setup. No engine API or official source changed; docs/API.md was not regenerated.
The only implementation deviation is required-helper reload (6.7); no user decision is
needed within the authorized project-only scope. No commit was made.


### M5 defaults and scoped completion (2026-10-07)

- [x] Pre-code per-script decisions, optional-flow evidence and EDITING.md scope (6.8).
- [x] player_controller uses explicit localSource + named actions + oe.math; HUD uses oe.ui.
- [x] Literal defaults unchanged; all six script checks clean and params identical except lines.
- [x] Template agent/edit guide and regenerated Hello; no team seeded, editor settings preserved.
- [x] Ten before/after replay hashes/PNGs/game/save/player states identical.
- [x] Native build and all 232 tests exit 0, including every template test and code-map sync.
- [x] Fresh default-team Smoke render, desktop package/runtime exit 0, web package/browser startup.
- [x] Optional flow module not justified; no official source/digest/engine/runtime change.
- [x] Header/summary/milestone table/code maps and consolidated follow-up ledger reflect current scope.
- [x] No API regeneration or runtime rebuild required; existing runtime hashes unchanged.
- [ ] Smoke folders removed: automatic approval policy rejected recursive deletion;
      disposable build/m5 folders remain, while owned processes/tabs are stopped.

Artifacts: build/m5/{before,after}-replay.json, PNGs and params JSON, audit.json, build logs,
tests.log (initial exit 1, two checks) / tests-final.log (exit 0), new/render/package logs,
player.exit/player.stderr.log and web-smoke.jpg. First test-fixture correction and the
safe move-before-regeneration deviation are detailed in 6.8. Follow-ups in 6.9 are not
silently marked complete. No engine C++/reload behavior or other project structure changed;
no commit. Root instructions are kept identical; existing unrelated changes preserved.

### Required-only module reload (2026-10-07)

- [x] Pre-code design/current-model evidence and A/B/C decision recorded in 6.10.
- [x] Dependency watches, transitive reload, stable project table exports and additive report.
- [x] Atomic Lua/metadata recovery; initializer side-effect/coroutine/RNG guard; cycle diagnostics.
- [x] Instance/startup/owner callback semantics, singleton reset and stale-capture limits documented.
- [x] Recording/active-match direct and command barriers; watch metadata/native restore regression.
- [x] Updated legacy cached-helper assertion intentionally; three new regression tests.
- [x] Final desktop build/full 235 tests exit 0; live DongleZoo edit/reload/restore passes.
- [x] Full 13-checkpoint DongleZoo replay/PNG/state/save/host/tag baseline equality.
- [x] Corrected web/Android players rebuilt; Node/Wasm full 199 tests exit 0.
  Two passes were needed for the shared-export review fix; deviation recorded in 6.10.
- [x] Runtime READMEs/source provenance/sizes/hashes; current docs and API regenerated.
- [ ] Android device/emulator execution: unverified; no usable device run in this unit.
- [ ] New packaged browser/APK signing smoke: not executed in this unit (not requested).
- [x] Generated template/Hello helper-reload guide wording refreshed in the follow-up below.

Final logs: build/reload/{build-release,native-release,web-reviewed-build,wasm-release-build,
wasm-release-tests,android-reviewed-build,live-release,replay-release}.log, live-results.json, replay-comparison.json, artifacts.json. These are local
ignored evidence, not source outputs. Existing dirty work was preserved. No user decision is
required to use the implemented stateless helper reload; stateful migration/closure reconciliation
and physical/platform/provider testing remain separate follow-ups.

### Required-only reload follow-up: allocator measurement and guides (2026-10-07)

- [x] Same-source luaL_newstate/SnapshotHeap experimental binaries; source restored byte-for-byte.
- [x] Allocation-heavy microbenchmark and 600-frame Wickbound/DongleZoo replays, seven measured pairs after warmup; medians/ranges/process memory and matching hashes/state recorded in 6.10.
- [x] Meaningful overhead documented; optimization design proposed, implementation deferred pending user approval.
- [x] Audit 131 project/template Lua files and inspect transitive root helpers; no prohibited native initializer call found; no scripts modified.
- [x] Template EDITING/AGENTS refresh; Hello move-backup regeneration --name Hello --no-team with editor.ini preserved; exactly two guide files differ.
- [x] Final native build and full 235 tests exit 0; six Smoke script.check calls clean; Hello before/after and Smoke 120-frame hashes identical.
- [x] Root instructions/engine source/checked-in runtime byte hashes unchanged; no API regeneration or web/Android rebuild required.
- [ ] Allocator optimization implementation: requires approval and separate benchmark/regression scope.

Evidence is retained under ignored build/allocator. The temporary baseline allocator path was
never deployed to runtime/; the restored native production build uses SnapshotHeap. Prior
unrelated working-tree changes are preserved. No commit.
