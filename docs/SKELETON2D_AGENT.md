# Rigging a 2D character: the agent's steps

From one character picture to an animated `Skeleton2D` that holds up in motion. Every step ends with a **check**;
go on only when it passes, and report the numbers. The commands are ownengine MCP tools (`skeleton2d_build`) or
`oe exec <project> skeleton2d.build '{...}'`. This guide is built into the engine: `skeleton2d.guide` returns it in
any project. The reference for every command and file format is the engine's docs/SKELETON2D.md.

## 0. Before you start

- You read pictures: the PNGs the commands write (under `.oe/vision/`) are for you to look at. Never estimate a
  coordinate from a whole picture; zoom with `image.grid`.
- `vision.calibrate {}` -> look at its `out` -> read every dot with `image.grid {path: out, rect, step: 5}` ->
  `vision.calibrate {seed, answers: {"A": [x, y], ...}}`.
  **Check:** `passed`. If you cannot pass it in two tries, tell the person your rig may be off and go on carefully.
- Pictures you need from an image generator (step 1, step 6) leave the machine: the person agrees first. In a team,
  you list what you need and the art agent generates it.

## 1. The picture

One figure on a transparent background, the whole body in frame, limbs apart: front view in an A-pose (arms a
little away from the body, legs apart) or side view facing right. Prompt that works: "full-body 2D game character,
strict front view (or: side view facing right), relaxed A-pose with a clear gap between arms and torso and between
the legs, clean cel-shaded style, transparent background, no ground shadow, no text".
**Check:** `asset.info {path}` (size); look at it: nothing cut off, limbs apart, background transparent.

## 2. Draft

- `skeleton2d.draft_template {kind: "humanoid-front" | "humanoid-side", image}`: first guesses, with `notes` saying
  where each joint goes (`_l`/`_r` = the character's left/right; in a side view `_front`/`_back`).
- For each joint: `image.grid {path: image, rect: [x-60, y-60, 120, 120], step: 10, marks: [[x, y, "elbow_l"]]}`,
  look, move it to the middle of the joint, look again.
- `skeleton2d.draft_template {kind, image, joints: {...all corrected...}, out: "art/<name>.skel2d-draft.json"}` fits
  the part regions to your joints.
- `skeleton2d.build {draft, out: "chars/<name>.skel2d.json", dryRun: true}` and look at its `draftImage`: magenta =
  no part took those pixels. Add parts for what the template does not know (hair strands, a scarf, a tail, a weapon:
  a polygon around it, `bones` from the body part it hangs on to a bone of its own) or grow regions; neighbours
  should overlap a little at the joints.
- Where limbs lie over each other or over the body (a side view's legs, an arm hanging over the tunic), the
  template's bands also take what is behind them, and that picture then moves with the limb. Trace those parts along
  their outline instead (zoom with `image.grid`, a polygon of 10-20 points), ending at the joint where the next part
  begins (a leg at the ankle when the foot is its own part).
- A part nothing shows (the far arm hidden behind the body) gets no part: delete it from the draft, keep its bones.
- Things that should swing (a ponytail, a bag) are rigid parts on a bone of their own (`bone`, not `bones`); step 7
  gives them springs.
**Check:** `coverage.uncoveredPercent` below 1, no warnings left that you did not mean.

## 3. Build

`skeleton2d.build {draft, out}`, then `skeleton2d.overlay {path: out}`.
**Check:** the overlay looks exactly like the picture; every bone sits on its joint and points along its limb (a
bone points along its local +X).

## 4. Bend it

`skeleton2d.analyze {path: out}` (stress poses: every joint bent both ways) and look at its `image`.
**Check:** no `gap`, `flipped` or high `stretched`. For each: the hint says what to change in the draft (regions
overlapping past the joint, a part weighted to both bones, a smaller `cell`); rebuild and analyze again.
`revealed` problems are expected here (picture hidden under a front part); step 6 paints them.

## 5. Clips

- `skeleton2d.motion {path, recipe}` for each clip the game needs: idle, walk, run, jump, attack, hit, death
  (`params` to tune; `bones: {role: name}` when your bones are named otherwise). Clips no recipe gives: keys with
  `skeleton2d.set {path, clip, values}` (rotate keys add degrees to the setup pose).
- `skeleton2d.poses {path, clip}` and `skeleton2d.analyze {path, clip}` for each.
**Check:** no high problems; no `ground` problem (feet through the floor); for a side walk/run note
`ground.plantedSpeed` - the game moves the character at minus that speed (times the entity's scale) so the feet do
not slide. A mesh that squashes at a deep bend: lower that recipe's numbers (`knee`, `elbow`, `strength`) and
generate the clip again.

## 6. Paint what was hidden

`skeleton2d.analyze {path, masks: true}` lists `revealed` slots. `skeleton2d.variant_requests {path, slots: [...],
grow: 8}` writes one request folder each (`edit.png` with the area in magenta, `context.png`, `prompt.txt`). The
image generator gets the request's images in order with its prompt and the answer is saved as the request's
`result`. Then `skeleton2d.variant_import {request}`: it refuses an answer that changed what had to stay or left
magenta. Generators often redraw the whole picture; on a refusal ask once more, starting the prompt with "This is an
inpainting edit, not a new drawing: the composition, position and scale must stay exactly the same; only replace
the magenta pixels". An answer just over the limit that looks right to you can be taken with a larger `tolerance`.
- A part that was mostly hidden (a far leg behind the near one) cannot be pieced together: ask for it whole with
  `redraw: true` (its visible pixels mark where it is; parts hanging below it, like its foot, are not forbidden).
- Look at every import in motion (`skeleton2d.poses {clip, focus}`): a repaint that did not line up shows as slivers
  or doubled edges; redraw it.
**Check:** `skeleton2d.analyze` no longer lists those slots as `revealed`; look at `skeleton2d.poses {focus}` around
the joint you fixed. A later `skeleton2d.build` keeps a repaint when its part is cut exactly as before; a part you
re-cut needs its request again.

## 7. Life

- Hair, scarves, tails, ears: `skeleton2d.set {path, values: {springs: [{bone, stiffness, damping, inertia, gravity,
  limit}]}}`.
- Feet that must stay planted, hands on things: `ik` constraints toward target bones the clips key.
- In the scene: `Skeleton2D {asset, clip}`; scripts call `skeleton2d.play(id, clip, {mix})`, read
  `skeleton2d.state(id).finished`, and get `onAnimationEvent(name, clip)` (step, takeoff, land, hit, dead).
**Check:** play it (`sim.play`, `input.key`, `sim.step`) and look at `render.screenshot`; `skeleton2d.state {id}`
shows the clip, time and spring swings; `script.errors` is empty.

## Report

Coverage after step 2, analyze before/after for the stress poses and each clip, the clips with their durations,
plantedSpeed for walks and runs, the requests sent and imported, and the pictures you looked at (paths).
