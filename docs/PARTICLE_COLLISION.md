# Particle collision with static scenery

VFX asset emitters can stop, bounce or die when their particle centers cross static colliders.
Queries run on fixed simulation time through the existing 3D/2D physics backends; rendering needs
no special shader. Collision is disabled by default and captured at birth.

```json
{"collision":"bounce", "collisionBounce":0.8, "collisionFriction":0.2}
```

Set these emitter asset values through `vfx.create/set`, or use **Effects > Fields > Motion**.
`vfx.schema` includes the options, ranges and channel contract; `vfx.state {id,particles:true}`
reports the captured mode/coefficient values, stopped flag and 2D flag. CLI, HTTP, MCP and editor
use the same validation. Lua can spawn/play effects and change exposed parameters through its
existing VFX bindings. No ParticleEmitter scene component or second authoring route is introduced.

## Modes and captured inputs

- `collision`: `none` (default), `stop`, `bounce` or `die`.
- `collisionBounce`: normal velocity retained after a bounce, 0..1, default 0.5.
- `collisionFriction`: tangential velocity lost on contact, 0..1, default 0.

All three and the emitter's 2D/3D choice are captured at birth. Asset hot reload or parameter edits
affect future births; existing particles retain their response. Unknown modes and out-of-range or
nonfinite authoring values are rejected before writing an asset. Native snapshots and recorded
rollback preserve collision state, previous world position, stopped anchor and coefficients.
JSON saves authoring settings only. No extra random draws are consumed.

`stop` pins the center to a world anchor, cancels velocity/gravity integration and ignores prescribed
motion while stopped. It still ages, rotates, changes appearance and expires normally. Local particles
keep that anchor when their owner moves, rather than following it after collision. `bounce` reflects
actual world displacement velocity, scaling its normal/tangential parts by bounce/friction, then
converts it back to local velocity if needed. `die` expires at the contact and enters the existing
bounded `onDeath` burst queue; children are born after every emitter stepped and start at age zero.

Velocity curves/expressions still prescribe velocity before forces each next step, so they can override
a previous bounce. Position curves/expressions and owner motion contribute to the swept trajectory.
The final evaluated position is constrained after those channels, with the non-accumulating offset
kept separate from integrated position. A stopped particle's world anchor takes precedence over motion
channels; appearance channels keep evaluating. Local collision needs an invertible owner transform.

## Static query and fixed bounds

Every moving live colliding particle uses at most **one segment ray query and one response per fixed
1/60-second step**, from the previous evaluated world center to the new one. This catches high-speed
crossings of thin geometry. Births are not integrated/queried until the next step, matching existing
VFX timing. The physics worlds sync once for the batch without advancing physics or game time, after
scripts and before the normal physics step; kinematic bodies a script moved this frame keep their sweep
for that step (test `KinematicCarrySurvivesQueriesBetweenSteps`). Fully disabled collision makes no physics queries.

3D emitters query Jolt static non-sensor bodies only. 2D emitters query Box2D static non-sensor shapes
only (world XY); the backends do not mix. Explicit static rigid bodies, standalone colliders and static
tile collision geometry qualify. Dynamic/kinematic bodies, characters and triggers are ignored.
Layers/one-way shapes follow the geometric ray query, not contact pre-solve/character mover rules.
Moving a static collider updates its transform on the next query batch; stopped particles do not ride it.

There is a fixed **0.001-meter world-space contact skin**. The remaining displacement on an impact step
is discarded; there is no multi-hit corner solver. Particle centers are points, not spheres, so visible
quad/mesh edges can intersect the surface. There is no initial-overlap depenetration: start outside
colliders. Expired particles use normal lifetime cleanup. Authored velocity/position teleports can still
produce abrupt trajectories; extreme density costs one query per active moving particle. Existing caps
remain 10,000 particles per emitter and the VFX system's configured total budget.

Isolated `vfx.preview/analyze` and the Effects preview have **no gameplay colliders**. They simulate
motion/appearance without collision queries, show no contact behavior, and do not run gameplay scripts.
The editor preview explains this when a collision mode is selected (English/Korean/Japanese). Verify
contacts in a real project through `sim.step`, `vfx.state`, or `oe render --frames ...`. The example is
`build/ParticleCollision`: `build/bin/oe editor build/ParticleCollision`, then Play. The left/center/right
emitters show stop/bounce/die over a static floor.

## Status

Implemented for asset emitters, through both static physics backends and the shared deterministic
simulation. 360 Windows tests pass. ParticleCollisionStaticQueriesAndResponses,
ParticleCollisionLocalCaptureAndRestore and ParticleCollisionMotionChannelsAndDeathChain verify
3D/2D domain separation, sensor/kinematic exclusions, high-speed sweeps, stop/bounce/die, restitution/
friction, local nonuniform transforms, pinned anchors under owner movement, captured hot-reload
settings, invalid authoring values, native future replay, position curves and deferred death children.
The 800x500 demo after 120 frames has mean software/D3D11 RGB difference 0.429/255; Showcase retains
hash 72ab07fb213fd42b. CLI images and the Effects editor screenshot were visually checked.

Web and both Android players were rebuilt. Headless Edge/SwiftShader displayed the packaged 3D
stop/bounce/die example without WebGL errors. Both Android ELF libraries retain 16KB LOAD alignment.

- [ ] Particle collision on Android physical devices and Linux/EGL.
- [ ] Browser 2D particle collision, real browser GPUs and device frame-rate measurements.
- [ ] Existing skin/vertex-graph execution on minimum-capability WebGL2/GLES3 devices.
