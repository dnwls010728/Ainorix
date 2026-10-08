# VFX phase 5d: reusable acceptance presets

The six script-free acceptance effects are now built-in presets. A fresh project can create them
without a VFXLab checkout: the executable installs their meshes, textures and surface materials.
There are 15 presets in total; `vfx.presets {"tag":"acceptance"}` lists the six new ones.

## Use in the editor

Open **View > Effects**, click **New Effect**, choose a preset and a new `.vfx.json` path, then **Create**.
The complete effect opens as an unsaved draft. **Save Effect** writes it; **Place in Scene** creates a
linked `VFXSystem`. **Structure Graph** shows blocks/references; **Value Nodes** edits the selected
Birth/Update expression. Source, nodes, agents and document undo share the same validated draft.
Preset intent appears on hover; the expanded selector shows all 15 choices on a normal-height window.

| Preset | VFXLab scene | Controls | System particle budget |
|---|---|---|---:|
| `ice_pillar` | `scenes/ice_pillar.scene.json` | Base-pivot growth, frost and fragments | 45 |
| `tornado` | `scenes/tornado.scene.json` | `height`, `rise` | 128 |
| `shield_dome` | `scenes/shield_dome.scene.json` | `radius`, rim pulse and fade | 1 |
| `shockwave` | `scenes/shockwave.scene.json` | `speed`, radial growth and dissolve | 1 |
| `rain_snow` | `scenes/rain_snow.scene.json` | World-space rainfall and drifting snow | 88 |
| `healing_aura` | `scenes/healing_aura.scene.json` | `radius`, `height` | 48 |

![Korean preset selector](images/effects-bundled-presets-ko.png)

## Use through commands

```json
{"command":"vfx.create","args":{"path":"effects/ice.vfx.json","preset":"ice_pillar"}}
{"command":"vfx.check","args":{"path":"effects/ice.vfx.json"}}
{"command":"vfx.preview","args":{"path":"effects/ice.vfx.json","times":[0.15,0.75,2.4,3.1],"size":128,"out":"build/ice.png"}}
{"command":"vfx.spawn","args":{"path":"effects/ice.vfx.json","position":[0,0,0]}}
```

`vfx.create` accepts exactly one of `vfx` or `preset`. Explicit documents retain the previous file
write behavior; the preset route refuses an existing destination with `already_exists`.
`vfx.edit {path,new:true,preset}` creates a shared draft without writing the effect file.
Numeric parameter definitions, defaults, ranges and total system budgets survive both routes.

`vfx.apply_preset {id,preset}` attaches the six bundled presets as a `VFXSystem`, removing direct
ParticleEmitter/Trail components on that owner. It is one scene undo step. Other child entities remain.
Bundled presets require `mode:"replace"`; `merge` is rejected before installation. The original nine
presets keep their direct-component replace/merge behavior. A direct preset replacement removes an
existing VFXSystem on the owner, avoiding two effects there at once.

## Installation and ownership

The release manifest `engine/app/vfx-presets/files.cmake` embeds the same six assets and 13 dependency
files used by the acceptance sample. CMake normalizes JSON newlines and snapshots binary bytes;
changing an embedded asset rebuilds the generated source. No runtime sample-file lookup occurs.
`tools/make_acceptance.py` still regenerates the proven VFXLab definitions, meshes and materials.

References are remapped into `assets/vfx-presets/<bundle-hash>/`, retaining relative source paths
inside that directory. Only reachable dependencies are installed, with transitive material/shader
references. Identical files are reused. All destination conflicts are checked before installation;
modified files produce `preset_conflict`, never an overwrite. A new bundle uses a new hash directory.
The hash is a version namespace, not a security signature.

Dependencies belong to the project and are included by normal packaging. Scene undo and draft discard
retain these files, just like other authored assets. A failed disk write may leave unused dependencies,
but does not publish a partially validated effect or change scene/document history. To customize the
bundled linked asset, create a named copy with `vfx.create` and edit it with `vfx.set`/Effects; edits to
shared installed dependencies intentionally prevent later automatic reuse until restored.

## Release limits

- CPU simulation, fixed 60 Hz; no GPU compute backend, particle physics collision, terrain sampling,
  soft particles or screen refraction/distortion.
- Rain/snow expire at a floor relative to their captured spawn frame, using fixed lifetime and velocity.
  There is no height-kill channel or terrain collision. Moving the owner retains world-space particles.
- Expression source is the serialized authority. 1024 bytes/channel, depth 32, 64 instructions/channel,
  256/emitter, 512/asset and 1,000,000 reserved expression instructions/step; over-budget authoring fails.
  Request simulation cost is bounded separately. Values saturate to finite +/-65504; NaN becomes zero.
- Value nodes are a bounded view of expressions, not a free execution graph. Conversion can be refused
  for unsupported/deep source while preserving the document. Simulation order remains fixed.
- Static mesh particles and shader/custom channels are supported. Arbitrary skeletal mesh particles,
  GPU sorting and GPU simulation remain future work. Transparent renderer comparisons are approximate;
  the measured tolerances and six-effect hashes are in [the acceptance report](VFX_PHASE5B_PLUS.md).

## Verification

Fresh-project preset contact sheets reproduce all six phase 5b+ software hashes unchanged, including
parameterized effects and material shader dependencies. Tests also cover destination collisions,
identical dependency reuse, user files at original sample paths, create argument exclusivity,
shared draft save/discard/undo/redo, linked placement and scene undo with installed files retained.
The native editor test selects healing aura from the Korean popup, saves and places it in a fresh scene.
The screenshot comes from the full test process; its warning count includes earlier intentional test warnings.
The real CLI created all six in `oe new` output and recorded check/preview/analyze in
[VFX_PHASE5D_RESULTS.json](VFX_PHASE5D_RESULTS.json); only preview output paths are normalized to
project-relative form in that report. Windows Release passes all 303 tests; Node/WASM passes all 247.
Web and Android arm64-v8a/x86_64 players rebuilt. Runtime README files record byte counts and SHA256.
No new C++ warnings; the existing unused ProcessFixtureDir warning remains in the Wasm test build.

## Implementation status (work log)

- [x] Embed six proven asset definitions/dependencies; expose 15 discoverable presets.
- [x] Preset asset creation, shared Effects drafts and undoable linked scene placement.
- [x] Fresh-project parity, parameter preservation, collision refusal and document tests.
- [x] User documentation, release limits and real CLI/editor verification.
- [x] Windows 303 tests and Node/WASM 247 tests; Web/Android player rebuilds.
- [ ] Android device, browser GPU and Linux/EGL execution.
