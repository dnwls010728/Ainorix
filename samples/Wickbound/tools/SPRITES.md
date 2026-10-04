# Wickbound sprite animation work log

Current runtime: seven reviewed Grok animation sets, six original single sprites.
Rejected installed sheets and sidecars have been removed; see `GROK_MOTION.md`.
The initial generation record below is historical.

Provider: sprite-gen GPT component rows (`codex`, ChatGPT subscription). Existing
art is the identity reference. Generation and extraction runs are under
`build/wickbound-sprites/<asset>`; no credentials or provider sessions are sample assets.

## Usage audit

- `keeper_ada`, `keeper_bram`, `keeper_suri`: run-scene Player/Body. The selected
  keeper replaces the texture in `run/game.lua`. `run/player.lua` flips the body
  horizontally, moves the separate lamp and bobs the body. Menu `UIImage` portraits
  need the original single image, independently of runtime animation.
- Seven `enemy_*` textures and two `boss_*` textures: ten enemy prefab Body children.
  Gloom and gloomlet share artwork at different sizes. `run/enemy.lua` flips bodies,
  controls AI and damage; Eyes, Aura and Flash are separate effects.
- `brazier`: Body child of six campfires. `run/brazier.lua` switches the separate
  Fire, FireCore, Light2D and sparks when lit. Unlit and lit states need distinct art.
- Ground images belong to Tilemap; icon and key art belong to UI. Keep them still.
- `assets/fx` are procedural tinted masks, glows, projectiles and pickups. Their
  existing transform, rotation, particle and opacity animation does not need
  character pose sheets.

## Implementation status

- [x] Install sprite-gen and its isolated virtualenv; confirm subscription route.
- [x] Audit texture references, prefab hierarchy and regeneration tool.
- [x] Prepare 13 reference-locked runs: idle/move (four frames each), campfire
  unlit (one frame)/lit (four frames).
- [x] Generate rows, extract with canonical sprite-gen tools and review every
  contact sheet. Export GIF previews; physical foot-contact cycles remain unverified.
- [x] Install 13 grid sheets (101 frames) with manifest-derived clips and metadata.
- [x] Update content generator and Lua state selection; preserve menu portraits.
- [x] Verify pause, idle/movement, enemy stun and campfire ignition in WickboundSample.
- [x] Verify all three keeper selections and both software/D3D11 runtime renders.
- [ ] Independently certify humanoid loop seams and foot-contact cycles; current
  movement rows are explicitly experimental in each keeper's QA note.

Use sprite-gen's virtualenv Python to run `tools/generate_sprites.py`. It prepares
all requests; `--generate` runs generation, and `--only keeper_ada` limits the run.
No atlas is installed automatically before extraction and motion review. Use
`--process` for canonical extraction/composition/previews/inspection; `--refit`
centers the entire silhouette. `--workers 3` processes independent character runs
with two generation calls per run. `--install` requires per-run `qa-notes.md` and
inspection reports with no unresolved findings (single-frame static states may
report no motion), plus `motion-review.json` explicitly accepting every state.
Then rerun `tools/make_content.py`.

The follow-up motion review rejects the installed animation quality and keeps
all 13 revised runs outside the sample. See `MOTION_REVIEW.md` for per-asset
findings and the native-timing comparison page. The verification evidence below
establishes runtime integration only; it is not a motion-quality pass.

## Validation evidence

- `build.bat`: Release build passes, no new warnings.
- `build/bin/oe_tests.exe`: 169 tests, zero failed checks, including WickboundSample.
- `tools/verify_sprites.py`: Ada/Bram/Suri all pass; idle/move clips, frame range,
  all enemy prefab variants, lit campfire, zero Lua errors, software/GPU captures.
- `build/wickbound-sprites/verification/`: six 1280x720 runtime screenshots and
  per-keeper JSON command results. GPU uses D3D11.
- All 13 final sprite-gen inspection reports pass with no errors. The unlit
  campfire reports low motion because its one-frame state is intentionally static;
  no other warnings remain.
- Suri idle was regenerated because initial extraction lost the butterfly
  companions. The generated idle prompt now explicitly retains both companions.
- Installed sprite-gen's Windows directory-publication rename needed a bounded
  retry (20 attempts, 0.1 s apart) for transient PermissionError. The canonical
  extraction algorithm was preserved. This local patch must be reapplied after
  reinstalling until upstream handles this Windows condition.
