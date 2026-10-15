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

## Files

- `chars/hero.skel2d.json` — the skeleton (bones, slots, meshes with weights, clips, the scarf spring).
- `scripts/hero.lua` — keys to clips; one-shot clips play to their end (`skeleton2d.state(id).finished`).
- `scenes/main.scene.json` — camera, ground, the character, help text.
