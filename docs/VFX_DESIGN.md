# VFX authoring: design and implementation plan

Handoff spec for extending particles and trails into an authorable, reusable, AI-drivable VFX system.
Read `AGENTS.md`, `docs/DESIGN.md` and `docs/PARTICLES.md` first. This file is the plan; the work log
at the end is the source of truth for progress (DESIGN.md section 5). Facts below were checked in the
code; anything not checked is listed under "Verify first" and must be confirmed before building on it.

## 1. Goal and the rule that decides everything

An AI agent must be able to create, look at, verify and tune an effect with no GUI and no guessing:
`describe intent -> pick preset -> patch fields -> preview -> analyze -> adjust`. The editor is a thin
layer over the same commands (DESIGN.md 1.1), shared live with agents through `--connect`.

Design rules that follow:

1. **Headless first.** Every capability is a command with a `Params()` schema, then a Lua binding when
   gameplay needs it, then an editor view. No editor-only logic.
2. **Text first.** Assets are ordered pretty JSON, defaults not written, small enough to patch and diff.
3. **Discoverable.** Field names, ranges, docs and examples come from commands (`vfx.schema`, presets),
   never from memory.
4. **Fail with a fix.** Unknown fields, bad ranges and bad curves are errors with `hint` (and a
   "did you mean" when a close field name exists). Nothing is silently ignored.
5. **Verify with numbers.** PNGs show what it looks like; `vfx.analyze` states whether it works.
6. **Do not break determinism.** Existing frame hashes stay identical (see 8).

## 2. What exists today (checked)

- `ParticleEmitter` (`engine/scene/Components.h:91`): one flat struct of about 50 reflected fields plus
  runtime state (`particles`, `randomState`, `carry`, `elapsed`, `cycle`, `initialBurstEmitted`,
  `emitted`). Per-particle values are snapshots taken at birth (`struct Particle`, line 78); only the
  curves, texture and turbulence are live settings.
- Simulation: `UpdateParticles` (`engine/scene/Systems.cpp:186`) and `BurstParticles` (line 350).
  `Pool<T>()` is a `std::map`, so iteration is in entity id order. RNG is xorshift32 seeded
  `seed ^ id*0x9e3779b9`; a feature draws random numbers only when it is used (this is what keeps old
  hashes stable). Turbulence uses integer-hash value noise (no libm).
- Rendering: `GatherRenderItems` (`engine/render/RenderScene.cpp:386`) emits one `RenderItem` per
  particle sharing a static quad, unlit, no shadows; trails build one ribbon mesh per entity (line 477).
  `RenderItem.materialOverride` exists (`RenderScene.h:26`) but particles never set it.
- Prefabs (`engine/scene/Prefab.cpp:47`): `InstantiatePrefab` copies JSON and adds `Prefab{path}`. No
  link, no overrides; `prefab.save` does not change existing instances.
- Shader graph: `engine/render/ShaderGraph.h` (bounded SSA instructions, max 48 nodes and 8 uniforms,
  ops shared with the generated GPU evaluator); materials bind it via `.mat.json` `shader` and
  `shaderUniforms`; editor in `engine/editor/EditorShaders.cpp`.
- Editor: `engine/editor` contains no Particle or Trail code. Particles simulate only in a play session.
- `engine/app/AssetPreview.cpp:54` already instantiates a prefab into a separate preview `Scene`. Use it
  as the precedent for isolated previews.
- Prebuilt `runtime/web` and `runtime/android` are older than V1/V2 (PARTICLES.md work log): packaged
  web/Android games ignore the newer fields and `Trail` until rebuilt.

## 3. Verify first (not checked, do not assume)

Record the answer for each in the work log before the phase that depends on it.

- [x] Are `ParticleEmitter.particles` and `Trail.points` included in `sim.save_state` / rollback
      checkpoints (`engine/app/Engine.h:77-79`)? Needed for phase 2 (state lives in `VFXSystem`).
- [x] Does `Mesh` support per-vertex color? Decides how per-emitter batching (phase 4) is done.
- [x] What does `component.set` currently return for an unknown field name? Phase 1 must match or
      improve on it, with a closest-name hint.
- [x] Is the `.shader.json` format name-based or index-based? Only matters for phase 5.
- [x] How does `AssetManager` hot reload trigger and notify? Phase 2 adds a VFX asset kind.
- [x] sokol_gfx compute availability on WebGL2 and GLES3 (only for the optional GPU phase).

## 4. Architecture

```
.vfx.json  (asset: emitters[] + trails[] + params + budget)
   | parsed, validated, cached, hot-reloaded by AssetManager
   v
VFXSystem component  (asset path, param overrides, playing, autoDestroy)  -- scene instance
   | owns runtime-only state, never serialized
   v
EmitterInstance[]  = a plain ParticleEmitter value (not a component) + owner id + emitter index
   | stepped by StepEmitter(...), gathered by the same code as ParticleEmitter
   v
render: RenderItem (phase 4: materialOverride -> .mat.json -> .shader.json)
```

- **Reuse the existing struct as a value.** Emitter blocks in the asset use the exact
  `ParticleEmitter` field names and are parsed with the existing `TypeRegistry`/`FieldFromJson`, so
  schema, ranges, docs and validation are not duplicated.
- **Split the step.** Extract `StepEmitter(Scene&, ParticleEmitter&, EntityId owner, const Mat4& world,
  uint32_t seedMix, float dt)` and the matching burst function out of `UpdateParticles` /
  `BurstParticles`. The `ParticleEmitter` component calls it with `seedMix = 0`; `VFXSystem` calls it per
  emitter with the emitter index mixed into the seed so streams are independent. Do not change draw
  order inside the function.
- **Modules, not a free stack.** Group fields into fixed-order modules (Emission, Shape, Motion,
  Appearance, Curves, Timing, Render). Each can be toggled but never reordered. A reorderable stack
  would break the "draw random numbers only when used, in a fixed order" contract that keeps hashes.
- **Asset vs prefab.** The `.vfx.json` asset defines what the effect looks and behaves like and is
  live-linked (hot reload, overrides, one play/stop/finished state). A prefab places an effect on a
  character or weapon together with transform offsets and scripts. Prefab-based effects (VFXLab)
  keep working unchanged.
- **Trails** in an asset follow the system root plus a local offset. Per-particle ribbons are out of
  scope.
- **Budget.** Add a system-level `budget.maxParticles` across all emitters of one `VFXSystem`
  (per-emitter cap stays 10000).
- **GPU.** CPU simulation is the reference and the only gameplay-affecting path. A later GPU mode is
  cosmetic only, excluded from hashes (`simulation: "cpu"|"gpu"`); if compute is unavailable the
  alternative is closed-form particles evaluated in the vertex shader, with the software renderer
  evaluating the same formula.

## 5. File format (`*.vfx.json`)

```json
{
  "format": "oe.vfx",
  "version": 1,
  "name": "Campfire",
  "description": "Warm fire with rising smoke.",
  "params": {
    "intensity": {"default": 1, "min": 0, "max": 2, "doc": "Overall strength.",
                  "targets": [{"to": "flames.rate", "scale": 40}, {"to": "smoke.rate", "scale": 12}]},
    "tint": {"default": "#ff8a3d", "doc": "Flame color.", "targets": [{"to": "flames.startColor"}]}
  },
  "emitters": [
    {"name": "flames", "emitter": {"shape": "circle", "shapeSize": [0.3, 0, 0], "blend": "add",
                                     "sizeOverLife": [[0, 0.1], [0.4, 0.35], [1, 0]]}},
    {"name": "smoke", "emitter": {"delay": 0.2, "turbulence": 1.5, "pixelArt": false}}
  ],
  "trails": [],
  "budget": {"maxParticles": 600}
}
```

Rules: emitters and trails are referenced by `name`, never by array index; `emitter`/`trail` objects use
the component field names and omit defaults; unknown top-level keys, unknown emitter fields and
out-of-range values are errors. `params` are intent-level knobs that map onto one or more fields
(scale and override), so agents and Lua can change strength or color without knowing the internals.
The `material` field arrives in phase 4. Old files must keep loading (DESIGN.md 1.8): bump `version` only
with a loader for the old one.

## 6. Command and Lua API

All commands print `{"ok":..}` JSON, validate with `Params()`, and throw `ApiError(code, message, hint)`.
Read-only commands use `mutates=false` and must not change the live scene: previews and analysis run on
an isolated copy (like `AssetPreview`). Phase 1 targets an entity (`id`) with `ParticleEmitter` and/or
`Trail`; phase 2 additionally accepts `path` to a `.vfx.json`.

| Command | Phase | Input | Output |
|---|---|---|---|
| `vfx.schema` | 1 | `{}` | modules with fields `{name,type,default,min,max,options,doc,module}` for emitter and trail, derived from reflection, plus the param/target syntax |
| `vfx.presets` | 1 | `{tag?}` | `[{name, intent, tags, emitters, trails}]`; `intent` is one plain sentence an agent can match against |
| `vfx.apply_preset` (mutates) | 1 | `{id, preset, mode:"replace"\|"merge"}` | applied field list; undoable like `component.set` |
| `vfx.check` | 1 | `{id\|path}` | `{ok, warnings:[{code, message, hint, emitter?}]}` |
| `vfx.preview` | 1 | `{id\|path, times?:[s...], size?, renderer?:"software"\|"gpu", camera?, out?}` | contact-sheet PNG (file path or base64 as `render.screenshot` does), the `times` used, frame hash |
| `vfx.analyze` | 1 | `{id\|path, duration?, step?, render?:bool}` | `{duration, finishedAt, peakCount, peakAt, budgetRatio, samples:[{t,count,bbox,meanLuminance?}], hash}` |
| `vfx.create/get/list` | 2 | `{path, vfx}` / `{path}` / `{}` | asset JSON, atomic write inside the project |
| `vfx.set` (asset edit) | 2 | `{path, emitter\|trail?, values}` partial patch, `{path, params}` for params | resulting block; validates before writing |
| `vfx.diff` | 2 | `{path, against?}` | semantic changes, e.g. `flames.rate 40 -> 60` |
| `vfx.from_entity` | 2 | `{id, path}` | packs an entity's ParticleEmitter/Trail (and children) into an asset |
| `vfx.spawn` | 2 | `{path, position?, parent?, params?}` | new entity id; `autoDestroy` on |
| `vfx.play/stop/burst/clear/set_param/state` | 2 | `{id, ...}` | state, same style as `particles.*` |

Error codes (stable): `unknown_field` (hint names closest field), `invalid_range`, `invalid_curve`,
`unknown_preset`, `no_emitter`, `invalid_vfx` (names the JSON path), `path_outside_project`.

`vfx.check` warnings include at least: effect never visible (opacity or size zero over the whole life),
never finishes while `loop:false`, live count above budget, lifetime above a sane bound, missing texture,
`maxParticles` far below `rate * lifetime`, world space with no moving owner. Each has a fix in `hint`.

Lua (phase 2, `L_*` with `Guard`, documented in `docs/SCRIPTING.md`): `vfx.spawn(path, {position=, params=})`,
`vfx.play(id)`, `vfx.stop(id, {kill=})`, `vfx.burst(id)`, `vfx.set(id, name, value)`, `vfx.state(id)`.

Undo, runtime and hot reload:
- `VFXSystem` fields are a normal component, so scene undo covers them.
- Asset edits are file edits in an isolated edit document with its own history (like `prefab.edit` and the
  shader editor), saved atomically.
- `play/stop/burst/clear` are runtime-only, like `particles.*`: no undo, not serialized.
- Hot reload: live settings update at once; values captured at birth apply to new particles; a change in
  the emitter list restarts the instance.

## 7. Editor (phase 3)

An Effects panel built on the phase 1/2 commands, modeled on the Shaders panel: grouped module inspector,
curve and gradient widgets for `sizeOverLife`/`colorOverLife`/`opacityOverLife`, a preview viewport that
runs an isolated scene while the game is stopped (loop, scrub, live count against budget), edits through
`vfx.set` with explicit `vfx.save`, drag an asset into the scene to create a `VFXSystem`. Edits go only through `Impl::Call`.
UI text through `Tr()` with Korean and Japanese rows in `EditorText.cpp`. Agents attached with
`--connect` edit the same document and appear as notices.

## 8. Compatibility and hash contract

- `ParticleEmitter`, `Trail`, their fields and commands stay as they are.
- The `ParticleRenderers` hashes must not change: 2D `eeb1529ca41029c9`, 3D `4ea73d9e1b71d479`
  (see `docs/PARTICLES.md`). A refactor that changes them is a failed refactor.
- New random draws must happen only when the new feature is used, after all existing draws.
- `VFXSystem` seeds mix owner id and emitter index; document the exact formula in `docs/VFX.md`.

## 9. Phases

Each phase ends with the Definition of Done in DESIGN.md section 4 and a commit that ticks its work-log line.

### Phase 1: headless authoring loop (target: existing `ParticleEmitter`/`Trail` entities)

- Add: `vfx.schema`, `vfx.presets`, `vfx.apply_preset`, `vfx.check`, `vfx.preview`, `vfx.analyze`.
- Files: `engine/api/Commands.cpp` (registration), new `engine/app/Vfx*.cpp/.h` (presets, check, analyze,
  preview using an isolated `Scene`, patterned on `app/AssetPreview.cpp`), preset data (JSON files
  embedded or under `templates/`), `docs/VFX.md` (new, with agent recipes), `docs/PARTICLES.md` link,
  AGENTS.md and CLAUDE.md (identical) command table row, `docs/API.md` regenerated, three READMEs if
  user-visible.
- Presets: at least fire, smoke, sparks, hit burst, portal ring, fireworks, trail swing, trail projectile
  (derive from `samples/VFXLab`), each with an `intent` sentence.
- Tests (`tests/tests.cpp`, driven through `Engine::Call`): `VfxSchemaCoversReflection`,
  `VfxPresetsAreValid` (each preset passes `ParticleSettingsValid` and `vfx.check` without errors),
  `VfxPreviewIsDeterministic` (same hash twice and with 1 vs N threads), `VfxAnalyzeReportsFinished`,
  `VfxCheckFindsInvisibleEffect`, `VfxPreviewDoesNotMutateScene`, `VfxAgentLoop` (preset -> patch ->
  preview -> analyze, asserting on the numbers).
- Verify like a user: run the CLI loop on a sample, read the contact-sheet PNG, record unverified items.
- Difficulty: low-medium. Risks: preview stepping must not touch the live scene; analyze cost must be
  bounded (cap duration and step count).

### Phase 2: VFX asset and `VFXSystem`

- Prereq: "Verify first" items 1, 3, 5 answered.
- Add: `VFXSystem` component, `.vfx.json` loader/validator, `StepEmitter` extraction, shared gather code in
  `RenderScene.cpp` for `ParticleEmitter` and `VFXSystem`, `AssetManager` support and hot reload, the
  phase 2 commands and Lua functions, `autoDestroy`, system budget.
- Files: new `engine/scene/Vfx.h/.cpp`, `Components.h/.cpp`, `Systems.cpp`, `RenderScene.cpp`,
  `engine/assets/AssetManager.*`, `Commands.cpp`, `engine/script/ScriptHost.cpp`, `docs/VFX.md`,
  `docs/SCRIPTING.md`, `docs/API.md`, AGENTS.md/CLAUDE.md.
- Migrate: `samples/VFXLab` campfire (or one effect) to an asset as a second path; keep the prefab version.
- Tests: all existing particle tests and hashes unchanged; `VfxAssetRoundTrip`, `VfxSystemMatchesEmitter`
  (a one-emitter asset equals the plain component frame for frame), `VfxParams`, `VfxHotReload`,
  `VfxSpawnAutoDestroy`, `VfxLua`, `VfxUnknownFieldHint`, `SampleVfxAsset` (run twice, same hash).
- Difficulty: medium-high. Risks: refactor changing random order or hashes; seed mixing; whether runtime
  state belongs in snapshots/rollback; entity lifetime when `autoDestroy` fires.

### Phase 3: editor

- Effects panel, widgets, isolated preview, `Tr()` rows, `docs/EDITOR.md`.
- Files: new `engine/editor/EditorVfx.cpp`, `Editor.cpp`, `EditorInternal.h`, `EditorText.cpp`.
- Tests: `EditorTranslations`, `NativeEditorHeadless` driving the panel with window events; screenshot
  check with `oe editor <project> --screenshot`.
- Difficulty: medium. Risks: curve widget usability; keeping the panel on commands only.

### Phase 4: materials, batching, sub-emitters

- `material` on emitters and trails -> `RenderItem.materialOverride`; new particle shader inputs
  (`ParticleAge`, `ParticleRandom`, ...) as `ShaderOp`s; per-emitter mesh batching (depends on Verify
  item 2); sub-emitters (spawn on death) with bounded depth; optional soft particles.
- Files: `ShaderGraph.*`, `engine/render/shaders/Shaders.glsl` (+ regenerated `.h`), both renderers,
  `RenderScene.cpp`, `docs/RENDERING.md`, `docs/POSTPROCESS.md` if relevant.
- Tests: `GpuRendererMatchesSoftware` for the new ops, performance numbers recorded in the work log.
- Difficulty: high. Risks: software/GPU parity, the 8-uniform limit, performance.

### Phase 5 (optional): text-expression update rules

Only if a need is proven. A bounded expression language over named inputs (`t`, `age`, `rand`, `speed`,
...) evaluated per particle, e.g. `size = 0.3 * sin(t * 3.14)`, with a node canvas as a view of the text,
not the source of truth. Reuse the `ShaderGraph` compile/validation approach with its own op subset and
instruction budget. Must stay deterministic (same saturation rules as `EvaluateShaderGraph`).

### Optional later: GPU cosmetic simulation

See section 4. Never affects gameplay, hashes or collisions.

### Parallel chore

Rebuild `runtime/web` and `runtime/android` (needs Emscripten / NDK; see CLAUDE.md) so packaged games
get the V1/V2 fields and `Trail`; do it before phase 2 ships to those targets, then update
`runtime/web/README.md`. If it cannot be done in the current environment, leave the unchecked line below.

## 10. Risks (summary)

| Risk | Mitigation |
|---|---|
| Refactor changes hashes | Hash tests from section 8 gate every phase; no reordering inside the step |
| Reorderable stack breaks the RNG contract | Fixed-order modules only |
| Agent guesses field names | `vfx.schema`, strict validation, "did you mean" hints |
| Preview/analyze mutate or slow the live scene | Isolated scene copy, bounded duration and steps |
| Per-particle `RenderItem` cost | Phase 4 batching before any GPU work |
| Stale web/Android runtimes | Parallel chore, tracked in the log |

## Implementation status (work log)

Keep this last. One line per milestone; commit and tick it in the same commit.

- [x] Verify-first items answered and recorded (section 3). Source review, 2026-10-07:
      `EngineSync.cpp` CaptureNative stores a Scene by value; component-pool Clone copies all particle,
      point and RNG state. Both SaveState and rollback checkpoints use CaptureNative. The JSON copy
      still excludes transient state; phase 2 must add direct VFXSystem snapshot/rollback regression tests.
      `Mesh.h` has positions/normals/UVs/tangents but no vertex-color stream; phase 4 needs a shared
      renderer extension or an alternative packing strategy. `component.set` previously returned
      invalid_component_values with all valid field names and clamped ranged numbers; phase 1 adds
      strict ParticleEmitter/Trail authoring validation and closest-name hints without changing legacy loaders.
      ShaderGraph.cpp uses prior-node indices in args/output slots, and names for uniforms.
      Assets.cpp PollChanges compares cached asset mtimes, reloads entries and returns changed paths;
      Engine Tick/Step and the editor poll it, with recording/network restrictions in Engine.cpp.
      Vendored sokol_gfx.h documents compute on GLES3.1+ but not GLES3.0 or WebGL2. Current GL backend
      uses GLES3/WebGL2, and Android requests an ES3 context; GLES3.1 compute cannot be assumed without
      capability checks and a supported context/shader path. No compute/device execution was performed.
- [x] Phase 1: `vfx.schema`, `vfx.presets`, `vfx.apply_preset`, `vfx.check`, `vfx.preview`, `vfx.analyze`,
      presets, `docs/VFX.md`, tests, AGENTS.md/CLAUDE.md and API.md updated.
      Implementation adds optional preview-only motion keys so trail presets can be verified without
      executing gameplay scripts. Entity subtrees retain ids and ancestor transforms but restart effect
      state in isolated scenes. Bounds/camera fit and numeric analysis use fixed steps with explicit
      request-cost limits. Presets use solid quads by default so they have no project texture dependency.
      Windows Release build and all 254 tests pass (nine new VFX tests); ParticleRenderers reference
      hashes are unchanged. Actual VFXLab CLI workflows passed twice with identical hashes, and
      software/D3D11 campfire and moving-trail contact sheets were inspected. See docs/VFX.md for
      the exact preview hashes and limits. No new editor UI is part of this phase. Linux/EGL,
      Node/WASM and Android device execution remain unverified; prebuilt runtimes are not rebuilt.
      Earlier editor workflow/shader graph commits are integrated as well, preserving the existing
      user work in this checkout. The combined Release build passes all 261 tests; Korean ShaderLab
      editor screenshot verified after integration.
- [x] Phase 2 (2026-10-08): `VFXSystem`, strict sparse `.vfx.json`, shared StepEmitter/BurstEmitter/
      StepTrail and particle/trail gather, asset commands and atomic subtree packing, named parameters,
      transient runtime controls and Lua API, hot reload, system budgets and autoDestroy. VFXLab keeps
      its component scene and adds campfire.vfx.json plus assets.scene.json. Native snapshots copy
      particles, trail points, RNG, elapsed clocks and runtime controls; JSON saves only authored fields.
      Local TRS or exact affine matrices retain block offsets. Settings reload preserves runtime unless
      block names/order/counts change; malformed reload retains the last valid definition and reports
      an error. Asset files stay outside scene undo; editor document history belongs to phase 3.
      Warning-free Windows Release build, all 271 tests pass (ten new tests, including single-emitter
      frame parity, params, hot reload recovery, budget/drain, Lua, packing, previews and native restores).
      Legacy reference hashes unchanged: 2D eeb1529ca41029c9, 3D 4ea73d9e1b71d479. Actual software/
      D3D11 asset contact sheets and alternate scene screenshots inspected; CLI analysis passed.
      See docs/VFX.md for hashes and verification details. API, scripting, READMEs and agent guides updated.
      Web/Android rebuild attempts could not find Emscripten/NDK; prebuilt binaries were not refreshed.
      Linux/EGL, Node/WASM, Android device and multiplayer VFX integration remain unverified.
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
- [x] Phase 4 prep (2026-10-08): shared EditorNodeCanvas pan/zoom/drag/pins/wires extracted
      without changing Shader document semantics; shader editor tests and D3D11 screenshot verified.
      Effects layout uses `.oe/vfx-layout/` sidecars outside asset content and document history.
- [x] Phase 4 (2026-10-08): particle/trail materials, age/random/speed inputs, opt-in batching, bounded
      death chains, two-anchor ribbons, static mesh particles and sword_slash preset/sample. Windows/
      D3D11 comparison and portable-core tests; details, performance and limits: docs/VFX_PHASE4.md.
- [x] Phase 4b (2026-10-08): Effects structure graph, expandable fixed modules, material/parameter/
      death/anchor reference wires, budget node, shared document undo and layout sidecars. Warning-free
      Windows build and all 282 tests pass; D3D11 graph screenshots inspected. See docs/VFX_PHASE4B.md.
- [ ] Phase 5 (optional): expression update rules.
- [x] Rebuild `runtime/web` and `runtime/android` (2026-10-08): Emscripten and NDK players refreshed;
      Node/WASM suite verified. Runtime README files and docs/VFX_PHASE4.md record the evidence.
- [ ] Not verified anywhere yet: Android device execution of VFX assets; Linux/EGL rendering suite.
