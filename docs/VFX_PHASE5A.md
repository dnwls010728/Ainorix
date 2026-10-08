# VFX phase 5a: motion channels and curves

Phase 5a provides data-driven transforms inside VFX assets. It adds no expression language or value
nodes: those remain 5b and 5c, with all six acceptance effects proved as text assets before 5c starts.
A curve-only ice pillar demonstrates ground-anchored growth, hold and shader-driven disappearance.

## Use

Open `samples/VFXLab/effects/ice_pillar_curves.vfx.json` in Effects, choose the Curves module and
select X/Y/Z on Axis Scale Curve. Alternatively load `scenes/channels.scene.json` and play.
The asset grows only Y, using cube pivot `[0,-0.5,0]`, then dissolves through `particleCustom3`.
Its frost block rises with a position curve. No gameplay script drives either transform.
This is a channel proof, not the complete phase 5b+ ice-pillar acceptance preset (fragments remain).

## Contract

- Birth captures offset position, velocity, XYZ rotation, axis scale and four custom values alongside
  existing size, color, opacity and stable random. Runtime `index` is the emitter's accepted birth order;
  `count` is that call's accepted batch size, captured immutably. Clearing resets the birth index.
- Existing particles step before new births. Each update advances age, optionally replaces velocity
  from its curve, applies gravity/turbulence/drag, integrates a separate position, then evaluates
  offset position, rotation, scale and custom curves, expires particles and performs emission.
- Position curve is an additive displacement from separately integrated motion. It never modifies
  that integration base. A removed curve removes its displacement on the next step. Velocity curve
  is a prescribed velocity in emitter meters/second, replaced before forces each step; gravity does
  not accumulate across steps with this curve enabled. Empty velocity curves keep normal integration.
- World motion curves transform by the emitter matrix captured at birth; moving/rotating the owner
  later leaves that frame intact. Local particles use local values and follow their current owner.
  Rotation is extra XYZ Euler degrees inside legacy Z rotation/spin, in billboard or mesh axes.
- Axis scale is birth `scale` multiplied by `scaleOverLife`; scalar lifetime size and meshScale apply
  once. Geometry translates by negative `pivot` before scaling/rotation, fixing that point at its
  particle center. Default transforms bypass the new matrix path and preserve legacy frame hashes.
- Vector curves are `[[age,[x,y,z]],...]`, at most 16 keys, nondecreasing age within 0..1, linear
  interpolation and endpoint clamp. Position/velocity/rotation are signed, scale is nonnegative.
  Birth vectors and custom values/curves are finite within -65504..65504. New fields default to zero,
  unit scale and empty curves. Existing scalar size/color/opacity rules remain unchanged.
- `custom0..3` are captured values replaced by their signed scalar lifetime curves. Dedicated vertex
  attributes carry them to materials; shared unbatched meshes use dedicated per-draw inputs as with
  phase 4 particle attributes. They consume no authored graph uniform slots. `particleCustom0..3`
  are zero-argument graph operations available in both vertex and fragment stages. Ordinary meshes
  and ribbons default to zero. Batching preserves one custom vector for every particle's vertices.
- Native snapshots and rollback clone captured/evaluated values, integration base, accepted counters,
  random stream and clocks. Scene JSON excludes transient state. Hot reload preserves live birth
  values while replacing curves; a rejected asset edit leaves the valid runtime unchanged.
- `vfx.schema.channels`, reflected docs and `particles.state {id,particles:true}` expose the contract.
  Effects fields and graph modules edit the shared draft; vector axes share one set of age keys.
  Moving/adding a key resamples its other axes; removing a key removes it across axes.

## Verification

- Four command/core tests cover non-accumulation, prescribed velocity, captured world frames, strict
  vector/custom validation, pivots, axis scale, mesh/billboard behavior, custom vertex/fragment inputs,
  all eight graph uniform slots, batching, hot reload, deterministic isolated previews and native restore.
- NativeEditorEffectsVectorCurves drives axis selection and a key drag through real window events,
  checks independent axes and shared undo/redo. Existing authoring tests scroll controls into view
  because the Curves module now contains more channels. English vector and Korean Effects screenshots
  were inspected, as were software/D3D11 previews.
- Pivot/custom material comparison: mean absolute RGB difference 0.057 of 255 for individual and batched
  D3D11 versus software. Individual versus batched software mean remains below 0.1.
- Actual VFXLab CLI `vfx.check`, repeated software preview, D3D11 preview and three-second analysis pass.
  At 192 pixels/tile, times 0.1/0.5/1.5/2.3 seconds, software hash `e30ceb2cf250dedc`, D3D11 hash
  `ec288b31319abb02`. Analysis hash `adb8b14663758357`, peak 33/48 (0.6875), finish 2.43333333 seconds.
  The full-height pillar keeps X/Z width 0.7 meters and its base at Y=0; frost bounds may extend below Y=0.
- Full suite and runtime results are recorded in the work log below. Android device, Linux/EGL and
  packaged browser WebGL2 execution remain unverified. Node tests cover the portable core, not a GPU.

The 128-pixel channel proof has software hash `2620de22f4505b16`, asserted by the same test in
Windows and Node/WASM. Scene load and 40 fixed steps are also checked; the 640x360 scene screenshot
hash is `bf948add5e2fb45b`.

## Follow-ups

5b supplies bounded expressions, deterministic math, named snippets, located errors and numeric costs.
5b+ must prove all acceptance effects before 5c value nodes. Height-based kill/collision is deferred;
finite lifetimes can end rain/snow but no automatic ground-height kill is promised by this milestone.

## Implementation status (work log)

- [x] Serialized channel contract, birth-state capture and fixed-order evaluation.
- [x] Position/velocity/rotation/scale/custom curves, pivot and strict schema/command validation.
- [x] Software/GPU material attributes and batching; shared transform/bounds gathering.
- [x] Effects vector/signed curve authoring and channel proof asset/scene.
- [x] Windows Release build and all 287 native tests, zero failed checks; no new native warnings.
- [x] Node/WASM all 234 tests, zero failed checks, including the shared 128-pixel proof hash.
      The pre-existing unused ProcessFixtureDir warning remains in the portable test build.
- [x] Web player and Android arm64-v8a/x86_64 players refreshed; hashes in runtime README files.
- [ ] Browser GPU, Android device and Linux/EGL execution.
