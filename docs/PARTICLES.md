# Particle effects

## Implementation status (work log)

P1 and P2 are implemented in PRs #15 and #16, pending review. P3 starts from
latest main independently because emission does not depend on saves or skeletal
animation. Its changes remain in one feature PR.

- [x] P3.1: Reflected ParticleEmitter, bounded deterministic fixed-step simulation,
      explicit state/burst/clear commands, Lua burst helpers and simulation tests.
      Windows Release build and all 64 tests pass. ParticleSimulationAndCommands
      covers startup burst, capacity/drop/shrink, expiry, paused emission,
      world-space spawn positions, runtime serialization exclusion, fractional
      rate, API errors and Lua/global/instance bursts. ParticleRandomStreamIsDeterministic
      checks repeated seeds, changed seeds and cone/speed constraints.
- [ ] P3.2: Shared 2D/3D billboard draw items, lifetime size/color interpolation,
      texture sheet frames and transparent ordering in both renderers; visual,
      hash, thread-determinism and software/GPU comparison tests.
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
the spawn transform's scale/rotation. Billboard rendering remains P3.2; runtime
rebuilds and visual/sample verification remain P3.3.
