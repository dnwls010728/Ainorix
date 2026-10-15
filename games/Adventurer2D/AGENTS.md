# Adventurer 2D — Skeleton2D sample

A cut-out 2D character made from one generated picture with the agent pipeline of
[docs/SKELETON2D.md](../../docs/SKELETON2D.md), and a scene that plays it. Run it with `oe editor games/Adventurer2D`
and press Play: Left/Right walk, Space jumps, J attacks, H is hit, W (held) waves, K falls, R stands up.

## How it was made

1. `art/hero.png`: Codex image generation (front view, A-pose, transparent background).
2. `vision.calibrate` once; `skeleton2d.draft_template {kind: "humanoid-front", image: "art/hero.png"}` guessed the
   joints; two `image.grid {rect, marks}` looks corrected them; `skeleton2d.draft_template {..., joints}` fitted the part
   regions to the picture; a `scarf` bone and part were added to the draft by hand -> `art/hero.skel2d-draft.json`.
3. `skeleton2d.build {draft: "art/hero.skel2d-draft.json", out: "chars/hero.skel2d.json"}` cut `chars/hero-parts/`.
4. `skeleton2d.analyze` found the torso's cut edges under the arms; `skeleton2d.variant_requests {slots: ["torso"],
   grow: 8}` -> Codex -> `skeleton2d.variant_import` painted them (`chars/hero-parts/torso-filled.png`).
5. Clips: `skeleton2d.motion` for idle, walk, jump, attack, hit, death; `wave` keyed with `skeleton2d.set`.
6. `springs: [{bone: "scarf", ...}]` for the scarf.

Rebuilding from the draft cuts the parts again and drops the torso fill: import it again afterwards
(`.oe/variants/` is not kept in the repository, so repeat step 4).

## The courier (side view)

`art/courier.png`, made the same way by following `skeleton2d.guide` (the M7 run of docs/SKELETON2D.md):

1. `skeleton2d.draft_template {kind: "humanoid-side", image}` left 18.2% uncovered; joints read with two
   `image.grid` looks -> `art/courier-joints.json`, passed back as `joints`.
2. Draft edits the template cannot know (`tools/courier_draft.mjs`, to apply again after a template run): the far
   arm's parts dropped (it is hidden behind the body), `ponytail` and `satchel` parts on bones of their own, the near
   leg, arm and hand traced along their outlines (bands around overlapping limbs carry what is behind them). 0.8%.
3. `skeleton2d.build`, then repaints: torso and satchel (`variant_requests` + Codex, the torso on a second, stricter
   prompt), the near leg under the hand, and the far leg redrawn whole (`redraw: true`) - it was mostly hidden.
4. Clips from `skeleton2d.motion` (run with `knee` 55, `arm` 22, `elbow` 35; death with `strength` 0.65); every clip
   passes `skeleton2d.analyze`; walk `ground.plantedSpeed` -1.17, run -2.93 -> `scripts/courier.lua` moves her at
   those speeds times her scale (0.85).
5. Springs on `ponytail` and `satchel`.

## Files

- `chars/hero.skel2d.json` — the skeleton (bones, slots, meshes with weights, clips, the scarf spring).
- `scripts/hero.lua` — keys to clips; one-shot clips play to their end (`skeleton2d.state(id).finished`).
- `chars/courier.skel2d.json`, `scripts/courier.lua` — the courier and her patrol (Shift: run).
- `scenes/main.scene.json` — camera, ground, the character, help text.
