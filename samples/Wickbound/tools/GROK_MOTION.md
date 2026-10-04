# Grok motion correction

The user authorized switching sprite-gen from GPT rows to Grok after rejecting
the first animation set. Generation used the user's Grok subscription login,
not an API-key fallback. The original RGBA art was composited on a flat chroma
plate; sprite-gen owns canvas placement, video generation, frame extraction,
cycle detection, alpha cleanup and native-timing strip/WebP/GIF export.

## Applied

- Ada: 64-frame idle, 16-frame alternating walk.
- Bram: 64-frame idle, 19-frame shuffle.
- Brute: 64-frame idle, revised 31-frame quadruped walk.
- Shade, Wisp and Eclipse: 64-frame hover loops shared by idle/move.
- King: 64-frame idle, 18-frame shuffle.

`tools/install_grok.py` packs reviewed strip cells in sequence without redrawing,
reordering or independently scaling poses. A 320px square cell accommodates the
motion extent; world sprite sizes compensate for the larger cell. The fixed
standing-height baseline preserves the entity anchor. Each clip retains the
strip sidecar's duration. Keeper selection updates texture, grid, clips and size
together, since the generated cycles have different frame counts.

Review notes and strip hashes are stored alongside the installed assets.
Brute's first candidate failed periodicity and was rejected. Its regenerated
clip/loop passed; a later cache-only rerun overwrote set.report.json with a prompt
cache mismatch because the species-specific generator overrides the stock
prompt. The accepted evidence is the preserved revised clip and passed loop
report, with its hash. Do not treat that rerun as another successful generation.

## Rejected candidates and single-sprite fallback

- Suri: generated walking is smoother, but frequent blinking and a reported
  3.05% sideways head displacement remain. The correction ran out of quota.
- Gloom: generated a ground ellipse; rejected despite passing numeric loop QA.
- Skitter: repeated blinking/support quality still needs revision.
- Stalker: 8.81% one-step head displacement; rejected.
- Leech: movement failed the periodicity gate. Regeneration was refused.
- Brazier: a flame-only layer over the static bowl is planned to guarantee a
  fixed bowl. Grok refused flame-image generation before an output was produced.

These six assets now use the original single PNGs without SpriteAnimation at the
user's request. Their installed sheets/manifests/QA sidecars have been removed;
generation evidence stays in build. Suri selection removes the default player's
animation component and resets its frame/grid. Static enemies skip clip changes.
Campfire ignition leaves its bowl static while enabling the existing fire effects.
`tools/use_static_sprites.py` reproduces the fallback and metadata regeneration.
No failed Grok candidate was installed. Grok returned HTTP 403 `personal-team-blocked:spending-limit` with
"You have run out of credits or need a Grok subscription." Additional generation
needs restored media access/quota; signing in alone does not resolve this code.
No paid API route was substituted.

## Quality limits

The applied loops visibly improve temporal continuity over independent GPT
poses; they are not a physical foot-contact certification. Bram/Brute retain
small reported head steps (1.17% / 0.93%). Ada's first exported loop recorded a
small jump that was not interpolated before RIFE was installed. These are
documented limitations, not hidden motion passes.

## Verification and reproduction

- Release build succeeds.
- `tools/verify_sprites.py` verifies Ada/Bram/Suri selection, manifest frame
  membership, enemy lineup, campfire ignition, zero Lua errors and software/GPU
  rendering. Original six-asset limitations remain independent of these checks.
- Full unit test result: 169 tests, zero failed checks
  (`build/wickbound-grok-tests.log`).
- Browser preview: `build/wickbound-grok-review.html` via the local build server.
- Candidates, clips and canonical reports: `build/wickbound-grok/`.
- Installed first-pass backup: `build/wickbound-grok/installed-before-grok/`.
- Reproduce one new attempt with sprite-gen's virtualenv Python and
  `tools/grok_motion.py --only <name> --variant <new-name> --states idle,walk`.
  Custom-prompt runs require a new variant when regenerating; cached stock-prompt
  comparison does not accept the custom species prompt.
- Record a visual verdict, then run `tools/install_grok.py --only <accepted names>`
  and `tools/make_content.py`. `tools/review_grok.py` records the seven named
  inspected candidates only; it must not serve as an automatic quality classifier.

No engine player code changed, so prebuilt web/Android runtimes do not need a
rebuild for this asset/Lua correction.
