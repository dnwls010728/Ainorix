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

## Content migration (6.2)

Migrated 27 placed emitters across VFXLab, HD2D, Wickbound, DongleZoo and OnlineRPG, plus the dynamic
HD2D crystal burst and both coin/hit effects in Dungeon/Platformer. Entity ids and transforms stay
unchanged. Five content generators now write reusable emitter assets; regenerating cannot restore the
retired scene component. Generated assets use deterministic settings hashes as filenames and expose
color, speed, size and playing parameters for gameplay control. Existing grouped campfire and all six
acceptance presets remain available. No sample is deleted; Trail remains a direct component here.

32 isolated old/new contact sheets reproduce the same hashes and numeric peak/finish/bounds results,
recorded in [VFX_PHASE6_CONTENT_RESULTS.json](VFX_PHASE6_CONTENT_RESULTS.json). The proof enables
paused emitters and requests a 12/16-particle burst for normally manual-only effects; the shipped
settings/defaults remain unchanged. Gameplay callers use vfx.set/burst/spawn; one-shot Dungeon,
Platformer and crystal owners now disappear when their particles finish rather than on a later timer.
All 303 Windows tests pass after migration, including Dungeon/Platformer, Wickbound, DongleZoo,
VFXLab and OnlineRPG gameplay coverage. Pure content changes require packaging, not a player rebuild.

## Unified core and authoring (6.3–6.5)

Emitter regression fixtures now create real assets and VFXSystem owners through the public commands;
EmitterSettings is test-only shorthand, never a scene component. VfxEmitterType independently reflects
internal ParticleEmitter values for asset schemas, strict validation and Effects controls. Scene
registration, standalone stepping/gather, particles.* commands, the Lua particles table and self:burst
are removed. Stale scene entries retain the owner/other components and emit a VFXSystem migration hint.
Trail remains registered. No inline VFX definitions or deleted samples were introduced.

All 15 presets now place linked assets. Original presets use the sum of their emitter caps as their
explicit system budget. Their merge mode writes a separate asset and preserves unspecified authored
fields; parameterized assets refuse merge to avoid defaults silently overriding the preset patch.
Installed assets survive scene undo. Use vfx.set for named-block edits of parameterized effects.

vfx.state {particles:true} reports detailed snapshots inside named emitter blocks. vfx.clear leaves
clocks/RNG unchanged when restart is omitted; supplied restart resets clocks/counters/RNG, rearming
startup births only when true. Lua accepts matching option tables. vfx.from_entity packs linked assets
and direct trails, renaming parameters, expression references and death links, retaining relative
matrices and authored overrides as defaults. Checks validate emitter values without fake scene pools.

API/particle/scripting/editor docs, the three READMEs and identical AGENTS.md/CLAUDE.md now describe the
single asset authoring route. DESIGN invariant 1.8 permits explicitly approved pre-release consolidation
with shipped migration and a removal hint. Korean D3D11 Effects screenshot inspected.

Windows Release: 305 tests, zero failed checks. The two new retirement/packing tests cover stale-file
hints, removed commands/Lua, parameter/expression/death remapping, relative matrices and runtime options.
Final 2D `eeb1529ca41029c9` and 3D `4ea73d9e1b71d479` hashes are unchanged across repeated runs.

## Implementation status (work log)

- [x] 6.1: run the old-path parity gate and capture final particle reference hashes/seed contract.
- [x] 6.2: migrate shipped content, generators and gameplay effect scripts; compare preview/analyze.
- [x] 6.3: move fixtures and regression tests to assets; establish stable reference hashes.
- [x] 6.4: retire registration, scene stepping/gather and standalone commands/Lua; retain reflection.
- [x] 6.5: revise particle/API/editor/agent documentation and pre-release compatibility rule.
- [ ] 6.6: full Windows/Node suites and Web/Android player rebuilds.
- [ ] Android device, browser GPU and Linux/EGL execution.
