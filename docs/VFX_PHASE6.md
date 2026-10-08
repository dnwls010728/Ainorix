# VFX phase 6: one scene particle component

Particle authoring moves to asset-backed VFXSystem components. ParticleEmitter remains an internal
settings/runtime value, but ceases to be a registered scene component. Trail retirement is deferred
as a separate decision. All samples are migrated; none are deleted. Inline VFX definitions are not
introduced: Effects and agents continue to author the same `.vfx.json` documents.

This is an explicitly requested pre-release breaking change. Stale component files receive a removal
hint. Standalone particles commands and Lua bindings are removed after sample/test migration.

## Parity gate (6.1)

The unmodified old path passes all 303 Windows Release tests, including VfxSystemMatchesEmitter.
That test compares accepted births, RNG state, particle position/velocity and rendered frames for 40
steps against a single-emitter asset. The final old-path reference hashes are 2D
`eeb1529ca41029c9` and 3D `4ea73d9e1b71d479` (ParticleRenderers).
`VfxSeedMix(index) = uint32(index) * 0x27d4eb2d`; index 0 is exactly zero, matching the old standalone
step. Entity ids are retained in migrated scenes/prefabs so the owner contribution to RNG stays stable.

## Implementation status (work log)

- [x] 6.1: run the old-path parity gate and capture final particle reference hashes/seed contract.
- [ ] 6.2: migrate shipped content, generators and gameplay effect scripts; compare preview/analyze.
- [ ] 6.3: move fixtures and regression tests to assets; establish stable reference hashes.
- [ ] 6.4: retire registration, scene stepping/gather and standalone commands/Lua; retain reflection.
- [ ] 6.5: revise particle/API/editor/agent documentation and pre-release compatibility rule.
- [ ] 6.6: full Windows/Node suites and Web/Android player rebuilds.
- [ ] Android device, browser GPU and Linux/EGL execution.
