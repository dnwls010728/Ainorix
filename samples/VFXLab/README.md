# VFXLab

Run `oe run samples/VFXLab`. Four effects made only from `ParticleEmitter` and
`Trail` settings, with no effect-specific engine code:

- **Campfire**: flames (`colorOverLife`, `sizeOverLife`, `turbulence`, additive),
  smoke (a 2 x 2 sheet with `randomFrame`, random rotation and spin, an opacity
  curve that fades in and out) and embers (`align: velocity` with `stretch`).
- **Portal**: motes born on a circle's rim (`shape: circle`, `shapeEdge`) that
  drift inward (`radialSpeed` below zero) while they rise.
- **Fireworks**: `loop` with `duration: 2` fires the burst every two seconds; the
  flash, the sparks and a smoke puff with `delay` are three emitters under one
  parent. Space fires them by hand (`scripts/lab.lua`).
- **Orb**: `scripts/orbit.lua` flies a figure-eight; its `Trail` draws the ribbon
  and a world-space emitter leaves sparkles behind.

Capture a frame with `oe render samples/VFXLab --frames 132 --out build/vfx.png`
(add `--renderer gpu` to compare). Regenerate the textures and the scene with
`python tools/make_art.py` and `python tools/make_scene.py` from this folder.
See docs/PARTICLES.md for every field.
