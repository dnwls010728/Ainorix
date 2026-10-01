# Particle effects

## Implementation status (work log)

P1, P2 and P3.1 were integrated into main directly at the user's request.
P3.2 continues from that combined main; sample gameplay effects remain P3.3.

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
      browser and Android device execution of this milestone remain unverified.
      Node/WASM passes 70 tests, including the same fixed particle frame hashes
      as Windows (2D eeb1529ca41029c9; 3D 4ea73d9e1b71d479). GPU tests skip
      in Node because there is no browser canvas. CLI script screenshots also
      verified a world-space colored burst in software and D3D11.
- [ ] P3.3: Platformer/Dungeon coin/hit effects, API/docs/three READMEs, web and
      Android runtimes rebuilt, available platform verification and final PR.

## Simulation contract

ParticleEmitter defines rate (particles/second), initial burst, lifetime,
speed, spread (cone half-angle in degrees), gravity, startSize/endSize,
startColor/endColor, texture/frame, local/world space, maxParticles, loop and
playing. Additional direction, dimensions (2 or 3), seed and sheet columns/rows
make orientation, planar emission and reproducible art choices explicit.

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

## API and scripting

- particles.state {id, particles?}: count, emitted, playing, loop and maxParticles.
  particles:true includes age/lifetime, position/velocity and worldSpace snapshots.
- particles.burst {id, count}: immediate emission (0..10000), returning accepted.
  Explicit bursts work while playing:false and do not re-arm the startup burst.
- particles.clear {id, restart?}: clear particles, counters, fractional births and
  PRNG state. restart:true re-arms the startup burst for the next playing step;
  otherwise it stays disarmed. Configured emitter settings are preserved.
- Lua particles.burst(id, count), or self:burst(count) on an attached script,
  returns accepted births and raises an error for missing emitters/bad counts.

Burst/clear change transient simulation state, like audio playback, and do not
participate in scene undo. Reflected emitter settings serialize normally and
component edits participate in undo. Particle coordinate snapshots, colors,
sizes, opacity and gravity are captured at birth; world-space velocities include
the spawn transform's scale/rotation. Sample gameplay verification remains P3.3.

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
