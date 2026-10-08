# VFX assets and headless authoring

Create, tune and verify asset-backed `VFXSystem` and direct `Trail` effects through the same command API
used by CLI, HTTP and MCP. No play session or editor window is needed. The simulation and rendering
contract remains in [PARTICLES.md](PARTICLES.md); the larger implementation plan is [VFX_DESIGN.md](VFX_DESIGN.md).

## Portable acceptance presets

Effects > New Effect and `vfx.edit {path,new:true,preset}` offer six complete expression presets:
`ice_pillar`, `tornado`, `shield_dome`, `shockwave`, `rain_snow`, `healing_aura`.
`vfx.presets {tag:"acceptance"}` discovers them; `vfx.create {path,preset}` writes a new asset.
All meshes/textures/materials install into a versioned project directory without sample dependencies.
File conflicts are refused. Parameters and budgets are preserved. `vfx.apply_preset` attaches these six
as linked VFXSystem components with scene undo; they require replace mode. Installed files survive undo.
All 15 presets attach linked assets. The original nine support merge into unparameterized assets;
parameterized instances use named-block `vfx.set` edits or replace. [Consolidation contract](VFX_PHASE6.md).

## Motion channels and pivot

Effects can animate mesh and billboard particles without gameplay scripts. Use `positionOffset`,
`rotation3D`, `scale` and `custom0..3` for captured birth inputs, and `positionOverLife`,
`velocityOverLife`, `rotationOverLife`, `scaleOverLife`, `custom0OverLife` through `custom3OverLife`
for normalized-age curves. `vfx.schema.channels` publishes units, composition and fixed evaluation order.
Vector keys are `[[age,[x,y,z]],...]`; signed vectors are accepted, scale must be nonnegative.
Values are finite and bounded to -65504..65504; each curve has at most 16 ascending keys.

Position curves add an offset to separately integrated physics, without accumulating last frame's
curve displacement. World particles use the captured emitter frame; local particles follow their owner.
Velocity curves replace velocity before gravity, turbulence and drag every step; they are a prescribed
velocity, so gravity does not accumulate across steps while that curve is active. Removing the curve
resumes normal integration from the last velocity. XYZ rotation adds to captured `rotation3D` inside
legacy Z spin. Axis scale multiplies captured `scale`, then legacy scalar size and `meshScale` once.
Existing size/color/opacity curves retain their established behavior.

`pivot` is an unscaled geometry point fixed at the particle center. A cube's base is `[0,-0.5,0]`:
keep scalar size at 1 and animate only scale Y to grow vertically with constant width. The curve-only
proof is `samples/VFXLab/effects/ice_pillar_curves.vfx.json`; load `scenes/channels.scene.json`, or open
that asset in Effects. Curves offers X/Y/Z selection and signed scalar plots, sharing document undo.
Its material reads `particleCustom0` and `particleCustom3` for glow/dissolve; all four material ops also
work in vertex displacement, both renderers, and batching, without taking graph uniform slots.

`vfx.state {id,particles:true}` includes evaluated scale/rotation/custom, birth inputs,
`integratedPosition`, accepted birth `index` and immutable accepted batch `count`. Native snapshots
retain these values; scene JSON excludes them. Hot reload updates curves while preserving live birth
inputs. Bounded expression text now drives Birth/Update channels; value nodes are described in [VFX_PHASE5C.md](VFX_PHASE5C.md).
Rain/snow ground-height killing remains a later candidate; finite lifetime can end falling particles.
Verification and limits: [VFX_PHASE5A.md](VFX_PHASE5A.md).

## Typed expressions

Effects > Expressions edits Birth/Update channel text with explicit Apply/Clear and document undo.
Use `expressions:{update:{position:"vec3(cos(age*2*pi),age*2,sin(age*2*pi))"}}` to animate motion without
scripts. Birth channels capture initial values once; Update channels replace curves and preserve the
separate physics integration base. Numeric asset parameters are read as `param.name`; `targets:[]`
is valid for expression-only parameters. Scalar/vector types, own bounded math and conservative
instruction budgets keep evaluation portable and bounded. `vfx.expression_eval` checks a source with
supplied inputs; `vfx.check` reports compiled/reserved costs. `vfx.schema.expressions` lists inputs,
functions, limits and reusable snippets. Open `effects/spiral_expressions.vfx.json` or
`scenes/expressions.scene.json` in VFXLab. Full contract and evidence: [VFX_PHASE5B.md](VFX_PHASE5B.md).

Six script-free acceptance effects are available in VFXLab: `ice_pillar`, `tornado`, `shield_dome`,
`shockwave`, `rain_snow` and `healing_aura`. Open `effects/<name>.vfx.json` or
`scenes/<name>.scene.json`. Contracts, contact sheets and recorded command results:
[VFX_PHASE5B_PLUS.md](VFX_PHASE5B_PLUS.md).

Effects **Value Nodes** edits individual Birth/Update channel expressions through typed nodes,
with the same document undo and validation as source editing. `vfx.expression_graph` and
`vfx.expression_source` expose the conversion for agents. Assets retain source text only;
positions stay in `.oe/` layout sidecars. See [VFX_PHASE5C.md](VFX_PHASE5C.md) for usage and limits.

## Agent recipe

Start with `vfx.schema` for reflected field names, types, defaults, ranges, options and fixed-order
module groups. `vfx.presets` lists eight texture-independent starting points with intent sentences:
`fire`, `smoke`, `sparks`, `hit_burst`, `portal_ring`, `fireworks`, `trail_swing`, `trail_projectile`.
Filter by an exact `tag`, such as `burst` or `trail`. These settings are derived from VFXLab; solid
quads keep them portable. Import a soft glow/smoke texture for the desired final appearance.

Feed these JSON lines to `oe script <project>` (add `--save` to persist the scene; asset edits write immediately):

```json
{"command":"vfx.create","args":{"path":"effects/impact.vfx.json","preset":"hit_burst"}}
{"command":"entity.create","args":{"name":"Impact","components":{"Transform":{},"VFXSystem":{"asset":"effects/impact.vfx.json"}}}}
{"command":"vfx.set","args":{"path":"effects/impact.vfx.json","emitter":"hit","values":{"burst":24,"maxParticles":48,"lifetime":0.3,"startColor":"#33aaff"}}}
{"command":"vfx.check","args":{"id":"Impact"}}
{"command":"vfx.preview","args":{"id":"Impact","times":[0.1,0.2,0.4,0.6],"size":256,"out":"previews/impact.png"}}
{"command":"vfx.analyze","args":{"id":"Impact","duration":1,"render":true}}
```

Expect `peakCount:24`, `budgetRatio:0.75`, and completion shortly after 0.3 seconds. Inspect the PNG,
then patch the fields and repeat. A count of zero at time zero is expected: startup emission happens
on the first fixed step. Software PNG/hash output is identical with one or multiple render threads.

## Commands and validation

| Command | Behavior |
|---|---|
| `vfx.schema {}` | Reflection-derived fields, eight module groups (EmitterSettings is asset reflection, not a scene component), limits and the parameter/target syntax |
| `vfx.presets {tag?}` | Preset names, intent, tags and emitter/trail blocks |
| `vfx.apply_preset {id,preset,mode?}` | One undoable edit on the named entity; default `replace` attaches a fresh linked asset and resets runtime. `merge` writes a separate asset with patched preset fields, preserving unspecified authored settings; parameterized assets require replace or named-block edits. Other components and children remain intact. Adds Transform when needed. |
| `vfx.check {id|path}` | Validate the entity and its descendants; return `ok:true` plus actionable `warnings`. Invalid settings return an API error. |
| `vfx.preview {id|path,times?,size?,renderer?,camera?,motion?,out?,inline?}` | Restart an isolated copy and return a contact sheet, actual sampled times, per-tile counts/bounds/hashes and the sheet hash. PNG path when `out` is supplied; otherwise `png_base64`. |
| `vfx.analyze {id|path,duration?,step?,render?,camera?,motion?}` | Restart an isolated copy; return completion time, particle peak/time, point peak, capacity ratio, sampled counts/bounds and a metrics hash. Optional software-frame mean luminance. |

`vfx.create` and `vfx.set` strictly validate emitter asset edits; `component.add/set` validate Trail before
reflection can clamp values. Unknown fields return `unknown_field` with a nearest-name suggestion;
invalid numeric ranges return `invalid_range`; malformed or negative-size/out-of-range-opacity curves
return `invalid_curve`. Edits fail before changing any fields. Other component types retain their
existing validation. Legacy ParticleEmitter scene entries are skipped with a migration hint. Empty curves use the existing start/end values.

Warnings include zero size/opacity throughout life, no automatic births, a stopped one-shot that
cannot finish, lifetime above 30 seconds, current live count above capacity, capacity far below
rate times lifetime, missing texture, and world-space emission without a movement driver on the
owner. Movement warnings are heuristics: parent scripts or external commands can still move it.
Trail checks also explain that motion is required. Every warning includes a recovery hint.

Phase 1 addresses existing entity subtrees. `.vfx.json` paths, live-linked VFX assets, system parameters,
VFXSystem and Lua `vfx.*` are phase 2. The Effects panel is phase 3; these commands do not add editor UI.

## Preview and analysis contract

Only selected effect settings and the necessary transforms enter a separate Scene. Entity ids and
ancestor transforms are retained, preserving the existing seed mix. Live particles/points are not
copied: every request starts the effect again. Scripts, physics, other geometry, lights and camera
post-processing are not simulated. The live scene, input, frame, epoch, revision, undo, play session
and particle state remain unchanged. Textures use the existing asset cache; GPU preview may initialize
the GPU backend. Output files are restricted to the project by `Engine::ResolvePath`.

Times round to the nearest 1/60-second frame, with strictly increasing resulting frames. Default preview
times are `[0.1,0.3,0.6,1]`; time zero is before startup emission. Tiles are ordered left to right, then
top to bottom, up to four columns. Each tile is 32..512 pixels (default 256). A camera fitted to the
union of the sampled bounds is shared by every tile; empty effects use a default view. Set explicit
`camera:{eye:[0,2,8],target:[0,1,0],fov:45}` for repeatable framing across different edits. Optional
`projection:"orthographic",orthoSize:5` selects an orthographic half-height.

Bounds cover renderable particle billboards and trail geometry in world coordinates, excluding texture
alpha masks; invisible/empty effects have `bbox:null`. GPU preview is available with `renderer:"gpu"`,
but its hashes depend on hardware/driver. Software is the deterministic reference.

Analysis defaults to 5 seconds with samples every 0.25 seconds, including zero and the final frame.
Simulation always runs at 60 Hz. Peak counts and `finishedAt` examine **every fixed frame**, even between
samples. Looping emitters and emitting trails never finish; otherwise completion follows the existing
`ParticlesFinished`/trail contract. `finishedAt:null` means completion was not observed during the
requested interval, not necessarily that the effect can never finish. `budget` is the sum of emitter
capacities, and `budgetRatio` is peak particles divided by that sum. Trail points are reported separately.
The hash fingerprints returned metrics, not a serialized simulation snapshot. With `render:true`, mean
luminance is the mean normalized Rec.709 luminance of a 128x128 software frame **including the background**.

Requests are bounded: duration/time at most 60 seconds, 16 preview tiles, 241 analysis samples,
256 selected entities, 512 entities including ancestors, combined particle/point capacity at most
40000, and at most 20000000 capacity-times-fixed-steps. Preview and rendered analysis use two passes,
counted in that work bound, to share a camera without storing many full particle snapshots. Errors
suggest reducing duration or capacity. Large sample capacities can require a shorter preview.

## Trails and moving owners

An isolated preview does not execute a projectile/swing's gameplay script. Supply `motion` to simulate
the root's position explicitly: 2..32 keys `[seconds,[x,y,z]]`, increasing within 0..60 seconds.
Positions are offsets from the original root position in its parent's coordinates, interpolated
linearly and held outside the key interval. Rotation and scale stay at their authored values.
Transforms, motion and generated preview geometry must remain finite within +/-1000000 coordinates;
oversized geometry returns a range error with a shorter-duration/smaller-scale hint.

```json
{"command":"vfx.apply_preset","args":{"id":"Projectile","preset":"trail_projectile"}}
{"command":"vfx.preview","args":{"id":"Projectile","times":[0.1,0.3,0.6,1],"motion":[[0,[0,0,0]],[1,[4,1,0]]],"out":"previews/trail.png"}}
```

Without motion, a newly restarted trail is stationary and its preview has no ribbon. These motion keys
belong to the preview request, not the component or gameplay simulation.

## Reusable assets

A `*.vfx.json` groups named emitter/trail settings and intent-level parameter definitions. `VFXSystem`
links an owner to that asset; no child emitter entities are created. ParticleEmitter is an internal
settings/runtime value, not a scene component. Old ParticleEmitter files load with a removal hint and
skip that component; export them with a pre-removal build or author replacement assets. Direct Trail
components remain supported. Create a document with `vfx.create {path,vfx}`:

```json
{
  "format": "oe.vfx", "version": 1, "name": "Impact",
  "emitters": [{"name": "sparks", "emitter": {
    "loop": false, "burst": 16, "rate": 0, "lifetime": 0.4, "maxParticles": 32
  }}],
  "params": {"strength": {"default": 1, "min": 0, "max": 2,
    "doc": "Scale the impact particle count.",
    "targets": [{"to": "sparks.burst", "scale": 16}]}},
  "budget": {"maxParticles": 32}
}
```

Block and parameter names are unique ASCII letters/digits/underscore/hyphen, 1..64 characters;
block names are unique across emitters and trails. Documents allow 1..32 total blocks, 32 parameters,
32 targets per parameter, and at most 1 MiB. Settings use component reflection and omit default fields
on save. Unknown fields give nearest-name hints and their JSON location; ranges, enums and curves
are checked before any file or scene edit. Target mappings override a field, multiplying numeric
values by optional `scale`. Parameter bounds and the target field's own constraints both apply.
Targets use `blockName.fieldName`; parameter definitions and instance overrides must be objects.

Each block accepts an optional local `transform:{position,rotation,scale}`. Trails follow the owner's
world transform plus this offset; world-space particles retain their birth positions, while local-space
particles follow the block transform. `vfx.from_entity` writes a 16-number column-major affine `matrix`
instead, preserving nested/nonuniform transforms exactly. A block cannot contain both representations.
Root position is the placement, so exported offsets are relative to the selected root. Packing rejects a
singular root and ignores scripts, meshes, lights and other placement components.

| Command | Behavior |
|---|---|
| `vfx.create {path,vfx}` | Validate and atomically write the canonical document; replaces an existing asset. |
| `vfx.get {path}`, `vfx.list {}` | Read canonical asset JSON or list project-relative asset paths. |
| `vfx.set {path,emitter,values}` | Partially patch one named emitter; use `trail` for a trail. |
| `vfx.set {path,params}` | Replace parameter definitions. |
| `vfx.diff {path,against?}` | Return `{field,before,after}` changes using block names; default comparison is component defaults and identity offsets. `against` accepts another asset path. |
| `vfx.from_entity {id,path}` | Pack the selected VFXSystem/Trail subtree, preserving relative matrices, parameter defaults/targets, expression references and death links. |
| `vfx.spawn {path,name?,parent?,position?,params?,autoDestroy?}` | Create Transform + linked VFXSystem in one scene undo step. Position is parent-local; autoDestroy defaults true. |
| `vfx.state {id,particles?}` | Read effective params, playback/completion/error, aggregate counts/budget and per-block counts/emitted totals; particles:true adds detailed snapshots in each emitter block. |
| `vfx.play {id,restart?}` | Restart blocks by default, preserving parameter overrides; `restart:false` resumes emission clocks. |
| `vfx.stop {id,kill?}` | Stop automatic emission and drain particles/trail points; `kill:true` empties them immediately. |
| `vfx.burst {id,emitter?,count?}` | Manually emit one named emitter or all in asset order. Default count is each configured burst. Counts respect both caps. Manual births do not consume the startup burst. |
| `vfx.clear {id,restart?}` | Empty particles and trail points. Omitted restart retains clocks/RNG; supplied restart resets counters/clocks/RNG and rearms startup births only when true. Playback/settings are retained. Suppress autoDestroy until play/stop/burst so an empty instance can be reused. |
| `vfx.set_param {id,name,value}` | Set a validated transient parameter override. Use component.set on VFXSystem.params for a saved instance override. |

Preview/check/analyze accept exactly one `id` or `path`. Linked entity previews use authored instance
parameters and restart transient simulation; runtime controls are excluded. Preview-only motion keys
also work on assets. Asset writes have no scene undo; an open Effects document has its own history and explicit save. Runtime controls have no scene undo and leave authored
scene JSON unchanged; their parameters/playback/RNG/particles/points are copied by native snapshots.
External controls and asset writes are blocked while recording or in a network match, while deterministic
Lua gameplay can control instances. `vfx.state` never modifies the live instance.

`budget.maxParticles` is a total cap (0..40000, default 10000), in addition to each emitter's cap.
Emitters consume available capacity in asset order; overflowing births are dropped. Lowering the budget
trims later emitters first on the next step. Capacity freed by a later emitter becomes available to earlier
emitters on the following step. Index zero retains the legacy RNG seed, so an identity one-emitter asset
with the same owner id matches its plain component frame for frame. Later indices use independent fixed
seed mixes; changing block order changes these streams. No wall-clock randomness enters simulation.

Settings hot reload preserves active particles, birth snapshots, elapsed clocks, RNG and trail points when
block names/order/counts stay the same. Live curves, texture and appearance update immediately; birth
properties affect future particles. Structural changes restart all blocks. Invalid reloads keep the last
valid runtime and expose the error in state; a corrected file clears it on the next reload/step. Changing
the linked path resets transient controls after the new definition validates. Polling follows the existing
asset reload rules and is disabled during recording/network replay.

`autoDestroy` destroys the owner and its descendants only after all blocks finish. Looping emitters and
emitting trails prevent natural completion; stop them to drain. Delayed one-shots wait for startup and
expiry. A stopped, never-started one-shot warns in check. `game.pause` freezes automatic simulation and
cleanup; scripts can still call manual burst/clear while paused.

VFXLab keeps `scenes/main.scene.json` as the component reference and adds `scenes/assets.scene.json`,
whose campfire links `effects/campfire.vfx.json`. The `intensity` parameter scales three emission rates.
Regenerate both paths with `python samples/VFXLab/tools/make_scene.py`. See [SCRIPTING.md](SCRIPTING.md)
for the Lua bindings. The Effects panel provides visual authoring; particle materials/batching and sub-emitters remain later phases.

## Shared Effects document

The native Effects panel and attached command callers use one isolated authoring document per Engine.
Standalone CLI processes each own a separate session: use `oe script` for multiple document commands,
or `--connect` to share the editor session.

| Command | Result |
|---|---|
| `vfx.edit {path,new?,preset?,discard?}` | Open a validated document; `new:true` starts an unsaved preset at a nonexistent path. Dirty switching requires save or explicit discard. |
| `vfx.edit_state {}` | Read `{open,path,document,dirty,undo,redo}`. |
| `vfx.set {path,emitter,values,merge?}` | Edit the open draft when paths resolve to the same asset; other paths still write immediately. `trail` or `params` are alternative patch modes. |
| `vfx.edit_replace {vfx,merge?}` | Validate/replace the full draft, including block structure, budget and transforms. |
| `vfx.undo {}`, `vfx.redo {}` | Move through up to 64 document edits. Consecutive equal nonempty merge keys coalesce dragging. |
| `vfx.save {}` | Atomically save after checking that the original disk text has not changed; external changes return `asset_changed`. |
| `vfx.close {discard?}` | Close; dirty documents require explicit discard or save. |

`vfx.get`, `vfx.check`, `vfx.diff`, `vfx.preview` and `vfx.analyze` read the draft when the path is open,
including a new path that has not been saved. Preview captures definitions at request start and never
reloads them mid-simulation. Scene instances use saved assets until explicit save and normal asset hot
reload; source draft edits do not create scene history or change the scene's dirty flag. `vfx.create`
and `vfx.from_entity` refuse to overwrite the currently open asset. Document changes use the engine's
revision notification, so attached callers and the UI see the same validated values. Document commands
are blocked during recording/network matches, like external VFX asset edits.

The UI groups fields and exposes curves, gradients, isolated playback and scene placement.
See [EDITOR.md](EDITOR.md) for controls and screenshot flags.

## Materials, mesh particles, ribbons and death chains (phase 4)

Emitter `material` and `Trail.material` bind ordinary `.mat.json` assets, including graphs authored
in Shaders. Particle materials use the emitter's texture when specified, otherwise the material's own
base texture. Trail fades multiply the explicit trail image or the material's base texture. Materials can
be lit (`unlit:false`); effects still cast no shadows. Missing materials/meshes are magenta at runtime;
`vfx.check` reports broken material dependencies and missing/animated/oversized meshes.

Three zero-argument graph operations are available in both vertex and fragment stages: `particleAge`
(normalized age in 0..1), `particleRandom` (stable birth value in [0,1)) and `particleSpeed` (world meters
per second). Outside particles they are zero; ribbons supply normalized point age and zero random/speed.
Batched effects use vertex attributes; unbatched effects use a dedicated draw input with shared meshes.
Neither consumes named graph uniforms, so all eight slots remain available. Random
uses `hash = randomState XOR uint32(emitted + birthIndex)` after legacy birth draws, then xor-shift 16,
multiply 0x7feb352d, xor-shift 15, multiply 0x846ca68b, xor-shift 16; upper 24 bits / 16777216.
It does not consume another PRNG draw. Native snapshots retain this value and death-chain depth.

Set `mesh` to a static project mesh or a built-in name (`crescent` is included), and `meshScale` to XYZ
scale. Lifetime size multiplies this scale; emitter orientation and particle rotation/spin orient the
mesh. World-space mesh particles capture emitter orientation at birth. Skinning/animation is excluded.
The authoring limits are 65536 vertices and 196608 indices per source mesh and 1000000 source vertices
multiplied by `maxParticles` per emitter. Runtime gathering also guards the live geometry budget.

`batching:true` combines an emitter into a single mesh/draw with per-vertex color, opacity, age, random
and speed. Particles sort internally back to front, but each emitter sorts as a whole relative to other
transparent objects. Leave it false when overlapping transparent objects must interleave. Legacy
billboards and their reference hashes keep their original per-particle draw path. Multi-material meshes
without an explicit material override retain separate particle draws. Batching reduces draw submission,
but rebuilding a CPU mesh costs more than collecting shared quads; it is not a universal CPU speedup.

A `Trail` with `mode:"ribbon"` sweeps a surface from `baseOffset` to `tipOffset` in owner/block coordinates.
Optional `baseAnchor` and `tipAnchor` identify unique scene entity names; offsets then use those entity
coordinates. Missing or ambiguous names suppress new samples and produce a checker hint. Both endpoints
are sampled, so a stationary blade base with a moving tip still leaves a ribbon. Camera rotation does
not change the swept geometry. `duration` limits emission, then existing points drain for `lifetime`;
zero keeps the old indefinite emission. `trail.state {points:true}` includes each ribbon point's `tip`.
Isolated path/editor previews use local offsets; named external anchors belong to scene instances.
Entity previews copy referenced anchor transforms without running their scripts.

Within a `.vfx.json`, set an emitter's `onDeath` to another emitter's name and `deathCount` to 0..256.
Death bursts are dispatched after every emitter has stepped, in source block/particle order, at the
expired particle's world position. Newborns start at age zero regardless of target block order. The
target's settings/orientation are used, but these births are always world-space. A target with
`playing:false` receives explicit death bursts without automatic emission. All births obey the target
cap and the shared system budget. Cycles are allowed but stop after generation depth 4. Stopping the
system also suppresses death births while it drains. This link is asset-only; standalone components
must be packed with `vfx.from_entity` to form a shared system.

`sword_slash` is the ninth preset: a two-anchor ribbon, mesh crescent and sparks. Multi-emitter presets
use the first emitter on the requested entity and reuse/create named child entities for later layers.
`samples/VFXLab/effects/sword_slash.vfx.json` adds a scrolling texture and lifetime dissolve graph;
`scenes/slash.scene.json` runs repeated sword sweeps through `scripts/slash.lua`. Regenerate its authored
files with `python samples/VFXLab/tools/make_slash.py`. Use `vfx.preview` with motion keys, as with trails.

Screen-space distortion/refraction is not implemented: surface graphs cannot sample the scene color,
and the software surface sampler only exposes the base texture. The GPU composite samples the scene
for postprocessing, but has no particle distortion pass. Soft/depth-faded particles remain optional
future work. Effects also provides the structure graph described below.

## Effects structure graph

Open a `.vfx.json` in Effects and enable **Structure Graph**. It shows Emitter, Trail, Material,
Param and Output nodes, plus read-only Anchor reference nodes for unique named scene transforms.
This is a view of the existing asset document, not a per-particle behaviour graph.

- Add Emitter / Add Trail create document blocks. Target Field chooses the extra component input
  shown on blocks; existing parameter targets remain visible. Add Parameter creates a typed default
  and its first target on the selected block. Parameter definitions, names and target scales remain
  editable in the inspector; existing scales are preserved when adding a wire.
- Drag a Material output onto `material`; double-click the Material card to open its material and
  source graph in Shaders. Material Reference adds an existing project material to the palette.
- Drag an Emitter output onto another emitter's `trigger` to set the source's `onDeath`. The source
  has one death target; the target can receive several sources. `deathCount` stays a component field.
- Anchor Reference lists unique scene names with Transform. Connect an Anchor to a Trail's
  `baseAnchor` / `tipAnchor`. These are scene references; the isolated preview uses local offsets.
- Drag a Param output onto a component field. The command validator rejects missing fields, wrong
  types and invalid ranges without changing the draft. Controlled fields reject direct reference
  wires until their parameter target is disconnected. Refused wires outline the target card and show
  the command hint (also on hover).
- Right-click an input to disconnect. A trigger input removes all incoming death references;
  disconnecting a parameter's final target removes its definition, because assets forbid orphans.
  Undo restores the document and those references. Output edits the shared `maxParticles` budget.
- Emitter / Trail modules expand in the node in their fixed order. Their fields, booleans, curves and
  gradients reuse the field inspector controls. Drag the header to move a node; middle-drag pans;
  the wheel zooms. Auto Arrange resets positions; Fit Graph frames every card. The normal field view
  retains the full preview controls; the graph keeps a compact independent preview alongside it.

Every document edit uses `vfx.edit_replace` or `vfx.set`, sharing draft validation, attached-agent
updates and document undo/redo/save. Node positions, pan/zoom and unused palette references are stored
atomically in `.oe/vfx-layout/<asset-path-hash>.json`, with the asset path checked on load. Positions
are keyed by block/parameter name or reference path. Missing layouts use automatic positions; invalid
layouts report a notice and fall back. Layout edits never dirty the asset or change `vfx.diff`.
The shared `EditorNodeCanvas` owns only pan/zoom, card dragging, pin hit areas and wire drawing;
shader and effect documents retain their own semantics.

For an automated screenshot:

```text
oe editor samples/VFXLab --effects effects/sword_slash.vfx.json --effects-graph --frames 20 --screenshot build/effects-graph.png
```

## Implementation status (work log)

- [x] Phase 1: six commands, eight portable presets, strict component authoring validation and bounded
      isolated preview/analysis. Windows Release build is warning-free; all 254 tests pass, including
      nine VFX tests and unchanged ParticleRenderers hashes. The actual VFXLab CLI preset/patch/check/
      preview/analyze loop passed twice with identical hashes: campfire software d597a3c59617fb55,
      D3D11 3f0b16a1e8477484, analysis 53e15b08e391bab0 (peak 62, nine samples), moving orb/trail
      559f75cedb007766. Software, GPU and trail contact sheets were inspected. API reference and
      English/Korean/Japanese READMEs are updated; AGENTS.md and CLAUDE.md match.
      The earlier editor workflow/shader graph branch is also integrated into this checkout;
      the combined Windows Release build passes all 261 tests, including node dragging and material
      authoring. A Korean ShaderLab editor screenshot was inspected after integration.
- [x] Phase 2 (2026-10-08): strict version-1 assets, sparse round trips, named parameter targets,
      atomic asset commands and subtree export, linked VFXSystem with runtime controls, Lua bindings,
      hot reload and total budgets, shared simulation/render gathering, autoDestroy and alternate VFXLab
      campfire asset scene. Native snapshots retain runtime particles/points/RNG/controls; scene JSON
      excludes them. External controls/writes are blocked during recording/network matches.
      Windows Release build is warning-free; all 271 tests pass (ten additional asset/runtime tests).
      Single-emitter frame-by-frame software parity and native restore tested; legacy 2D/3D hashes remain
      eeb1529ca41029c9 / 4ea73d9e1b71d479. Actual asset CLI checks, previews, analysis and alternate scene
      rendering passed. Inspected software/D3D11 192px contact sheets: 0efdfb4b39ef66d2 /
      e14f3901d15e0d63. Two-second analysis: d0190970a65b1f8a (peak 67 of 208, nine samples).
      Alternate asset scene at frame 132, 640x360: 2c41322aa12063c8, inspected. API reference,
      scripting guide, all READMEs and identical AGENTS.md/CLAUDE.md updated.
      Rebuild attempts returned Emscripten not found / Android NDK not found; shipped Web/Android
      binaries still require rebuilding before these assets can be used on those targets.
- [x] Phase 3 (2026-10-08): Effects workspace with module groups, typed parameters, scalar curves/color
      gradients, isolated 60 Hz playback/loop/scrub/motion/counts, scene placement and asset drag/drop.
      Shared Engine authoring document exposes edit/edit_state/edit_replace/undo/redo/save/close;
      vfx.set routes active-path patches to its draft, with 64-edit history, drag coalescing and explicit
      atomic save. Dirty close/switch/quit prompts and external disk conflict protection preserve work.
      Preview/check/get/diff/analyze can read unsaved drafts, while live instances use saved assets.
      Attached-agent changes update the panel and appear as notices. English/Korean/Japanese labels
      and wrapping controls, editor screenshot flag, API/area docs, all READMEs and agent guides updated.
      Warning-free Windows Release build and all 274 tests pass: two document tests plus native window
      events for fields, curves, gradients, separate undo/save, remote notices, dirty prompts, placement
      and asset-card drag into Scene. Legacy particle hashes remain unchanged. Actual nine-call CLI
      document workflow passed, including an unsaved 160px four-frame preview: 0bd1c245bd6ac6a4.
      English curve/gradient workflow and Korean/Japanese D3D11 editor screenshots inspected.
      Web/Android prebuilt players still need the tracked Emscripten/NDK rebuild. Linux/EGL, Node/WASM
      and Android device execution remain unverified.
- [x] Phase 4 (2026-10-08): materials, age/random/speed graph inputs, opt-in batching, bounded death
      chains, static mesh particles, two-anchor ribbons and sword_slash preset/sample. Details and
      validation evidence are in [VFX_PHASE4.md](VFX_PHASE4.md).
- [x] Phase 4b (2026-10-08): shared shader/effect canvas, structure nodes and reference wires, in-node
      module fields, compact preview and `.oe/` layout sidecars. Verification: [VFX_PHASE4B.md](VFX_PHASE4B.md).
- [x] Phase 5a (2026-10-08): captured birth state, non-accumulating position/velocity curves, XYZ
      rotation/scale, pivot and four material custom channels, signed/vector Effects curves.
      Details and evidence: [VFX_PHASE5A.md](VFX_PHASE5A.md).
- [ ] Platform follow-up: Linux/EGL, browser WebGL2 and Android device execution of phase 4 effects.
      Windows/D3D11 and Node/WASM tests cover the native and portable core; prebuilt players refreshed.

- [x] Phase 5b (2026-10-08): typed Birth/Update expressions, deterministic math, located errors,
      numeric budgets, diagnostics and Effects source editing. See [VFX_PHASE5B.md](VFX_PHASE5B.md).

- [x] Phase 5b+ (2026-10-08): six text-only acceptance effects, procedural meshes/materials, script-free
      scenes and recorded headless/renderer proofs. See [VFX_PHASE5B_PLUS.md](VFX_PHASE5B_PLUS.md).

- [x] Phase 5c (2026-10-08): compiler-backed value nodes, bounded graph/source commands, typed
      validation, shared source/history and layout sidecars. See [VFX_PHASE5C.md](VFX_PHASE5C.md).

- [x] Phase 5d (2026-10-08): portable six-effect presets, bundled dependencies, fresh-project creation,
      shared drafts, linked placement and conflict refusal. See docs/VFX_PHASE5D.md.
