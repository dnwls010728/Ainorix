# Particle effects and trails (VFX)

## Implementation status (work log)

P1, P2 and P3.1 were integrated into main directly at the user's request.
P3.2 and P3.3 continue from that combined main.

- [x] P3.1: Reflected ParticleEmitter, bounded deterministic fixed-step simulation,
      explicit state/burst/clear commands, Lua burst helpers and simulation tests.
      Windows Release build and all 64 tests pass. ParticleSimulationAndCommands
      covers startup burst, capacity/drop/shrink, expiry, paused emission,
      world-space spawn positions, runtime serialization exclusion, fractional
      rate, API errors and Lua/global/instance bursts. ParticleRandomStreamIsDeterministic
      checks repeated seeds, changed seeds and cone/speed constraints.
- [x] P3.2: Shared 2D/3D billboard draw items, lifetime size/color interpolation,
      texture sheet frames and transparent ordering in both renderers; visual,
      hash, thread-determinism and software/GPU comparison tests. Windows Release
      passes 74 tests. ParticleBillboardDrawItems checks camera orientation,
      interpolation, local/world positions, sheet UVs, sorting and expiry.
      ParticleRenderers covers solid/textured quads, 2D/3D seeded frame hashes,
      single/multi-thread equality and D3D11 comparison (mean channel difference
      0.118/0.180 of 255; fully covered interiors differ by less than 2 of 255).
      Software/GPU PNGs were inspected. Both Android ABI players rebuilt;
      browser execution is covered below; Android device execution remains unverified.
      Node/WASM passes 70 tests, including the same fixed particle frame hashes
      as Windows (2D eeb1529ca41029c9; 3D 4ea73d9e1b71d479). GPU tests skip
      in Node because there is no browser canvas. CLI script screenshots also
      verified a world-space colored burst in software and D3D11.
- [x] P3.3: Platformer/Dungeon coin/hit effects, API/docs/three READMEs, web and
      Android runtimes rebuilt and available platform verification. Windows passes
      75 tests and Node/WASM passes 71. SampleParticleEffects drives real coin
      trigger collection in both samples, enemy/player hit callbacks, visible
      orthographic particles, cleanup and repeatable scene/frame output.
      Native PNGs were inspected. Packaged Platformer ran with WebGL2 and no
      console errors; an ignored copy used a helper to move coins onto the player,
      exercising the actual collection callback and displaying its particle burst.
      The helper is not part of either sample. P3.2 runtimes are current because
      this milestone changes only Lua, documentation and tests.
- [ ] Hardware follow-up: run packaged samples on an Android device and verify
      coin/hit bursts and expiry; run the rendering suite on Linux/EGL. No Android
      device or Linux execution was available for this feature.
- [x] V1: ParticleEmitter for effects work: birth shapes (point/sphere/box/circle,
      shapeEdge, radialSpeed), rotation and spin, align velocity + stretch, flipbooks
      (frames, frameRate, randomFrame), sizeOverLife/colorOverLife/opacityOverLife
      curves, turbulence, delay/duration cycles, pixelArt switch, `finished` in
      particles.state and Lua particles.state. Emitters that do not use a new field
      keep their random sequence and frame hashes (ParticleRenderers hashes unchanged).
      Tests: ParticleShapesRotationAndTiming, ParticleCurvesFlipbookAndAlignment.
- [x] V2: Trail component (camera-facing ribbon, width/color/opacity fade, optional
      texture, additive blend), trail.state/trail.clear, Lua trail.clear. Both renderers
      draw the same mesh; TrailRibbon measures a mean channel difference of 0.106 of
      255 between software and D3D11.
- [x] V3: samples/VFXLab (campfire, portal, fireworks, orb trail) with generated
      textures; SampleVfxLab runs it twice and compares frame hashes. Software and
      D3D11 PNGs at frames 132 and 200 were inspected. Windows passes 187 tests.
- [ ] Rebuild runtime/web and runtime/android: engine C++ used by the player changed
      and this host has no Emscripten SDK or Android NDK. Until then packaged web and
      Android games ignore the V1/V2 fields and Trail. Also run the suite under
      Node/WASM to confirm the new tests and the turbulence field match Windows.
- [ ] Not done, candidates for later: sub-emitters (spawn on particle death), mesh
      particles, lit or soft (depth-faded) particles, particle collision, an editor
      preview that simulates the selected emitter while the game is stopped.

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
step. loop:false disables continuous rate emission; explicit particles.burst
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
  (`particles.burst` answers `invalid_emitter` naming the curve).
- **Turbulence**: `turbulence` (m/s^2) pushes particles with a fixed value-noise field
  sampled at their position; `turbulenceScale` is cells per meter. The field uses
  integer hashes and float lerps only (no libm). It is a live setting.
- **Timing**: `delay` holds back the initial burst and the rate. `duration` (0 = none)
  ends the rate after that many seconds; with `loop` the cycle repeats and fires the
  burst again every `duration` seconds. `particles.state` reports `finished` once a
  non-looping emitter has nothing left to emit and no live particles.
- **Sampling**: `pixelArt: false` filters the texture smoothly (soft glows, smoke).

A layered effect is a parent entity with one emitter per layer (flash, sparks, smoke
with a `delay`); save it as a prefab and spawn it with `prefab.instantiate`. See
`samples/VFXLab`.

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

- particles.state {id, particles?}: count, emitted, playing, loop, maxParticles and
  finished. particles:true includes age/lifetime, position/velocity, rotation and
  worldSpace snapshots. Lua `particles.state(id)` returns the same table.
- particles.burst {id, count}: immediate emission (0..10000), returning accepted.
  Explicit bursts work while playing:false and do not re-arm the startup burst.
- particles.clear {id, restart?}: clear particles, counters, fractional births, the
  delay/duration clock and PRNG state. restart:true re-arms the startup burst for the next playing step;
  otherwise it stays disarmed. Configured emitter settings are preserved.
- Lua particles.burst(id, count), or self:burst(count) on an attached script,
  returns accepted births and raises an error for missing emitters/bad counts.

Burst/clear change transient simulation state, like audio playback, and do not
participate in scene undo. Reflected emitter settings serialize normally and
component edits participate in undo. Particle coordinate snapshots, colors,
sizes, opacity and gravity are captured at birth; world-space velocities include
the spawn transform's scale/rotation.

## Sample gameplay effects

Both samples expose scripts/effects.lua as require("scripts.effects").spawn(position,
kind), with kind "coin" for gold sparkles or "hit" for red sparks. Coin collection,
enemy hits and player damage call it; Platformer bonus blocks also produce sparkles.
Each effect is a separate world-space emitter, so destroying the coin or enemy
does not remove the burst. There are 12 coin particles or 16 hit particles, with
0.5-second lifetimes and a 16-particle cap. The attached effects script destroys
the effect entity after 0.65 seconds, avoiding an accumulation of empty emitters.

## Rendering contract

Both renderers consume the same camera-facing unit quads from GatherRenderItems
and the same transparent BuildDrawList. Each live particle uses a shared static
quad, so the GPU mesh cache does not grow with births. Dimensions selects planar
or cone emission; both modes face the current camera, including orthographic
and editor views. Particle quads are unlit and do not cast shadows.

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
