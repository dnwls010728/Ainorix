# Particle effects and trails (VFX)

`VFXSystem` asset emitters and `Trail` are simulated at the fixed step, deterministic, and drawn the same by both renderers.
ParticleEmitter remains an internal settings/runtime value. Its scene component and `particles.*` APIs
were retired ([VFX.md](VFX.md#single-particle-authoring-route)); author `.vfx.json` blocks through Effects or
`vfx.create/set`. This document is the contract and the reference for their fields; open items are at the end.

For presets, strict authoring validation, isolated contact sheets and numeric analysis through `vfx.*`,
see [VFX.md](VFX.md). `vfx.create/set` and Trail `component.add/set` reject bad ranges and curves before editing.
Old ParticleEmitter scene entries are skipped with a replacement hint; runtime guards still reject invalid values.


Static particle collision: asset emitters support `collision: none|stop|bounce|die` with birth-captured
`collisionBounce` and `collisionFriction`. Fixed-step center sweeps use static non-trigger 3D/2D bodies;
isolated previews have no colliders. Full semantics and verification: [PARTICLE_COLLISION.md](PARTICLE_COLLISION.md).

## VFX expressions

`expressions` stores typed Birth/Update source text; present channels replace curves, while Update
position remains a non-accumulating offset and Update velocity precedes forces. See
[VFX.md](VFX.md#expression-contract) for inputs, clocks, size variation and numeric/cost bounds.

## VFX motion channels

ParticleEmitter now captures `positionOffset` (emitter-space meters), `rotation3D` (XYZ degrees),
`scale` (nonnegative XYZ multipliers) and `custom0..3` at birth. Normalized-age vector curves
`positionOverLife`, `velocityOverLife`, `rotationOverLife` and `scaleOverLife` share these channels
between quads and meshes. The position curve offsets separately integrated motion; velocity replaces
velocity before forces each step; rotation adds to birth XYZ rotation inside legacy Z spin; scale
multiplies birth scale, legacy scalar size and meshScale once. `pivot` fixes an unscaled geometry point
at the center before rotation/scaling. Four signed scalar custom curves replace their birth values;
materials use zero-argument `particleCustom0..3` ops through a dedicated attribute stream.

World-space position/velocity curves use the emitter matrix captured at birth; local particles keep
following the owner. Full semantics, examples, limits and hot reload behavior are in
[VFX.md](VFX.md#motion-channels-and-pivot). Native saves/rollback copy the captured and evaluated
state, accepted birth `index`, immutable accepted batch `count` and stable random; JSON only saves
settings. Empty curves and default birth values preserve legacy emission RNG and frame hashes.

## Simulation contract

ParticleEmitter defines rate (particles/second), initial burst, lifetime,
speed, spread (cone half-angle in degrees), gravity, startSize/endSize,
startColor/endColor, texture/frame, local/world space, maxParticles, loop and
playing. Additional direction, dimensions (2 or 3), seed and sheet columns/rows
make orientation, planar emission and reproducible art choices explicit.

`speedVariation`, `lifetimeVariation` and `sizeVariation` (0..1) scale each particle's speed, lifetime and size by a random
factor between `1 - variation` and 1, so a burst spreads out instead of forming a ring; `drag` is the fraction of velocity
lost per second. They draw from the emitter's random stream only when non-zero, so existing emitters keep their sequence.
`blend: "add"` makes the particles brighten what is behind them (sparks, fire) and `layer` is their 2D sorting layer, like
`Sprite.layer`. The world stands still while the game is paused (`game.pause`): particles neither age nor move.

An emitter starts its configured burst once on its first playing simulation
step. loop:false disables continuous rate emission; explicit vfx.burst
still works. playing:false pauses automatic emission; existing particles keep
aging and moving. Newly emitted particles start at age zero; integration of
existing particles happens before emission each fixed step. world-space
particles retain their spawn position/velocity when the emitter moves; local
particles follow its transform. Runtime particles are not serialized or saved.

Each emitter uses a fixed-seed xorshift32 stream mixed with its entity id, not
wall-clock time, pointer order or std::rand. Stable vector order defines particle
ordering. Configured maximum is bounded to 10000 per emitter; excess births are
dropped without deferred catch-up. Commands report the actual accepted count.

## Effects toolkit

These fields are optional; an emitter that leaves them at their defaults behaves and
draws exactly as before. Random draws for a feature happen only when it is used, after
the older draws, so turning one on does not reshuffle the rest.

- **Birth shape**: `shape` point | sphere | box | circle with `shapeSize` (radius in x,
  or box half extents), in emitter coordinates. `shapeEdge` keeps births on the surface
  or rim. A circle lies in XY for `dimensions: 2` and on the ground plane XZ for 3; a
  sphere in 2D is that circle. `radialSpeed` adds speed away from the emitter origin
  (negative pulls inward) to particles born off it.
- **Rotation**: `rotation` (degrees, counter-clockwise on screen) and `spin` (degrees
  per second), each with a `...Variation` of +/- that many degrees.
- **Alignment**: `align: "velocity"` points the billboard's up axis along the travel
  direction on screen; `stretch` adds that many seconds of travel to its length
  (sparks, rain). Rotation still applies on top.
- **Flipbook**: `frames` frames starting at `frame`. `frameRate` 0 plays them once over
  the lifetime, otherwise they loop at that rate. `randomFrame` starts each particle on
  a random frame; with `frameRate` 0 it keeps that frame (a sheet of variants).
- **Curves**: `sizeOverLife`, `colorOverLife`, `opacityOverLife` are key lists
  `[[t, value], ...]` with t ascending in 0..1 and at most 16 keys, linear between
  keys. A non-empty curve replaces the start/end pair (sizeVariation still scales
  sizes). Colors are `[r, g, b]` or `"#rrggbb"`. Curves are live settings: editing one
  changes particles already alive. A malformed curve stops emission
  (`vfx.burst` answers `invalid_emitter` naming the curve).
- **Turbulence**: `turbulence` (m/s^2) pushes particles with a fixed value-noise field
  sampled at their position; `turbulenceScale` is cells per meter. The field uses
  integer hashes and float lerps only (no libm). It is a live setting.
- **Timing**: `delay` holds back the initial burst and the rate. `duration` (0 = none)
  ends the rate after that many seconds; with `loop` the cycle repeats and fires the
  burst again every `duration` seconds. `vfx.state` emitter blocks report `finished` once a
  non-looping emitter has nothing left to emit and no live particles.
- **Sampling**: `pixelArt: false` filters the texture smoothly (soft glows, smoke).

A layered effect is a parent entity with one emitter per layer (flash, sparks, smoke
with a `delay`); in a `.vfx.json` the layers are emitter blocks of one asset.

## Trail

`Trail` draws a ribbon behind an entity. After physics each step it ages the path's
points, drops those older than `lifetime` and adds the entity's world position when it
has moved `minDistance` (at most `maxPoints`; oldest dropped first). `emitting: false`
stops new points and lets the ribbon fade out. The path is runtime state: not saved, and
it waits while the game is paused.

Both renderers draw one mesh built by GatherRenderItems: two vertices per point, turned
across the path and toward the camera, plus the entity's current position as the head.
Width runs from `width` at the entity to `endWidth` at the oldest point. Color and
opacity run from `color`/`opacity` to `endColor`/`endOpacity` by point age; they are
baked into a small cached texture, multiplied with `texture` when set (left edge at the
entity, right edge at the tail, filtered smoothly). Ribbons are unlit, cast no shadow,
blend `alpha` or `add`, and sort like sprites (`layer`, then distance).

- trail.state {id, points?}: count, emitting, maxPoints and optionally each point's
  world position and age, oldest first.
- trail.clear {id}: forget the path, so a teleport does not draw a ribbon across the
  level. Lua `trail.clear(id)`. Runtime only, no undo.

## API and scripting

- `vfx.state {id,particles?}` returns system playback/params/count/budget and named emitter blocks.
  Each block reports count, emitted, playing, loop, maxParticles and finished. With particles:true,
  blocks include age/lifetime, position/velocity, birth inputs, evaluated motion/custom and worldSpace.
  Lua: `vfx.state(id, {particles=true})`.
- `vfx.burst {id,count?,emitter?}` emits 0..10000 births, capped by each emitter and the system budget.
  Explicit bursts work while paused and do not rearm startup births. Lua: `vfx.burst(id,countOrOptions)`.
- `vfx.clear {id,restart?}` empties particles and trails. Omitted restart retains clocks/RNG.
  Supplied restart resets counters, fractional births, delay/duration clocks and RNG; true rearms
  startup births, false consumes them. Settings/playback remain intact. Lua: `vfx.clear(id,{restart=true})`.

Runtime controls do not participate in scene undo. Asset settings serialize in `.vfx.json`; author
with `vfx.set` or the Effects document history. VFXSystem component edits participate in scene undo.
Particle coordinate snapshots, colors, sizes, opacity and gravity are captured at birth;
world-space velocities include the spawn transform's scale/rotation.

## Rendering contract

Both renderers consume the same camera-facing unit quads from GatherRenderItems
and the same transparent BuildDrawList. Each live particle uses a shared static
quad, so the GPU mesh cache does not grow with births. Dimensions selects planar
or cone emission; both modes face the current camera, including orthographic
and editor views. Particle quads are unlit by default; material overrides can enable lighting. They do not cast shadows.

Size, RGB tint and opacity interpolate linearly between their birth snapshots
over age/lifetime. Size is the square billboard width/height in world meters;
emitter scale affects local centers and world-space spawn velocity, but does not
stretch billboards. Zero size/opacity and invalid positions are omitted.

Texture/frame/columns/rows are live emitter settings; they apply to all existing
particles. Frames wrap in row-major order, with nearest sampling and a small UV
inset to avoid neighbouring-frame bleed. Empty texture gives a solid quad;
missing textures give magenta quads. Texture alpha multiplies particle opacity.
Opaque geometry precedes particles; blended draws sort back to front by bounds
center distance. Entity id breaks ties, then stable birth order within an emitter.

## Extended effect rendering

Phase 4 adds emitter/trail materials, static mesh particles, optional emitter batching, normalized age/
random/speed graph inputs, two-anchor blade ribbons and bounded asset death chains. See the complete
field, ordering and budget contract in [VFX.md](VFX.md#materials-mesh-particles-ribbons-and-death-chains).
Legacy defaults retain the original particle hashes and draw ordering.

## Status

- [ ] Android device and Linux/EGL execution of particle and trail effects.
- [ ] Candidates: soft/depth-faded particles, particle collision.

