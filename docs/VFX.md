# Headless VFX authoring

Create, tune and verify existing `ParticleEmitter` and `Trail` effects through the same command API
used by CLI, HTTP and MCP. No play session or editor window is needed. The simulation and rendering
contract remains in [PARTICLES.md](PARTICLES.md); the larger implementation plan is [VFX_DESIGN.md](VFX_DESIGN.md).

## Agent recipe

Start with `vfx.schema` for reflected field names, types, defaults, ranges, options and fixed-order
module groups. `vfx.presets` lists eight texture-independent starting points with intent sentences:
`fire`, `smoke`, `sparks`, `hit_burst`, `portal_ring`, `fireworks`, `trail_swing`, `trail_projectile`.
Filter by an exact `tag`, such as `burst` or `trail`. These settings are derived from VFXLab; solid
quads keep them portable. Import a soft glow/smoke texture for the desired final appearance.

Feed these JSON lines to `oe script <project>` (add `--save` to persist authoring changes):

```json
{"command":"entity.create","args":{"name":"Impact","components":{"Transform":{}}}}
{"command":"vfx.apply_preset","args":{"id":"Impact","preset":"hit_burst"}}
{"command":"component.set","args":{"id":"Impact","type":"ParticleEmitter","values":{"burst":24,"maxParticles":48,"lifetime":0.3,"startColor":"#33aaff"}}}
{"command":"vfx.check","args":{"id":"Impact"}}
{"command":"vfx.preview","args":{"id":"Impact","times":[0.1,0.2,0.4,0.6],"size":256,"out":"previews/impact.png"}}
{"command":"vfx.analyze","args":{"id":"Impact","duration":1,"render":true}}
```

Expect `peakCount:24`, `budgetRatio:0.5`, and completion shortly after 0.3 seconds. Inspect the PNG,
then patch the fields and repeat. A count of zero at time zero is expected: startup emission happens
on the first fixed step. Software PNG/hash output is identical with one or multiple render threads.

## Commands and validation

| Command | Behavior |
|---|---|
| `vfx.schema {}` | Reflection-derived fields, seven module groups, limits and the planned phase-2 parameter syntax |
| `vfx.presets {tag?}` | Preset names, intent, tags and emitter/trail blocks |
| `vfx.apply_preset {id,preset,mode?}` | One undoable edit on the named entity; default `replace` resets only the affected component, including transient state. `merge` patches preset fields and preserves unspecified settings/state. Other components and children remain intact. Adds Transform when needed. |
| `vfx.check {id}` | Validate the entity and its descendants; return `ok:true` plus actionable `warnings`. Invalid settings return an API error. |
| `vfx.preview {id,times?,size?,renderer?,camera?,motion?,out?,inline?}` | Restart an isolated copy and return a contact sheet, actual sampled times, per-tile counts/bounds/hashes and the sheet hash. PNG path when `out` is supplied; otherwise `png_base64`. |
| `vfx.analyze {id,duration?,step?,render?,camera?,motion?}` | Restart an isolated copy; return completion time, particle peak/time, point peak, capacity ratio, sampled counts/bounds and a metrics hash. Optional software-frame mean luminance. |

`component.add` and `component.set` strictly validate ParticleEmitter/Trail authoring edits before
reflection can clamp values. Unknown fields return `unknown_field` with a nearest-name suggestion;
invalid numeric ranges return `invalid_range`; malformed or negative-size/out-of-range-opacity curves
return `invalid_curve`. Edits fail before changing any fields. Other component types retain their
existing validation. Legacy scene loading remains unchanged, including runtime guards for invalid
legacy particle settings. Empty curves use the existing start/end values.

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

## Implementation status (work log)

- [x] Phase 1: six commands, eight portable presets, strict component authoring validation and bounded
      isolated preview/analysis. Windows Release build is warning-free; all 254 tests pass, including
      nine VFX tests and unchanged ParticleRenderers hashes. The actual VFXLab CLI preset/patch/check/
      preview/analyze loop passed twice with identical hashes: campfire software d597a3c59617fb55,
      D3D11 3f0b16a1e8477484, analysis 53e15b08e391bab0 (peak 62, nine samples), moving orb/trail
      559f75cedb007766. Software, GPU and trail contact sheets were inspected. API reference and
      English/Korean/Japanese READMEs are updated; AGENTS.md and CLAUDE.md match.
- [ ] Phase 2: reusable assets, VFXSystem, runtime control, Lua, hot reload and budgets.
- [ ] Phase 3: Effects panel and visual authoring widgets.
- [ ] Platform follow-up: Linux/EGL and Android device verification. Web/Android prebuilt runtimes still
      require the previously tracked particle V1/V2 rebuild; no SDK is available on this host.
