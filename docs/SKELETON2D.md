# 2D skeletal animation (Skeleton2D)

Cut-out characters for 2D games: a character is drawn from separate part images (head, torso, upper arm, ...)
attached to a tree of bones. Clips turn and move the bones; parts follow rigidly or bend as weighted meshes.
This is roadmap item T12 ([ROADMAP.md](ROADMAP.md)).

The feature is built **for agents first**. An agent (Claude Code, Codex, a team agent in the editor) should be able
to take one character illustration, cut it into parts, rig it, animate it and check the result without a human
touching the editor, and every step ends in a check that returns numbers and hints instead of a picture alone. The
editor uses the same commands, so a human can fix what the agent did and the agent can continue from there.

The engine does no machine learning (no new dependencies, deterministic output): images come from an image
generator the agent drives (Codex image generation, see `games/RPG/tools/` for the existing practice), and the
engine supplies deterministic tools — coordinate grids, alpha outlines, triangulation, automatic weights,
pose renders and measurements — that turn the agent's judgement into a valid rig.

## Concepts

- **Skeleton asset** (`*.skel2d.json`): bones, slots, attachments, skins and clips. Shared by every entity that uses it.
- **Bone**: a 2D transform (x, y, rotation in degrees, scaleX, scaleY) relative to its parent bone, in world units
  (+Y up, like the rest of the 2D engine). `length` is only for display and checks.
- **Slot**: a place in the draw order (back to front, the order of `slots`) bound to a bone, showing at most one
  attachment at a time. Swapping the attachment (open/closed hand, blink) is a slot key in a clip.
- **Attachment**: what a slot shows.
  - `region`: a whole image (or a rectangle of one) placed relative to the slot's bone in the setup pose. Rigid.
  - `mesh`: an image with vertices (image pixels) and triangles; each vertex is weighted to up to four bones, so the
    image bends at joints (elbow, knee, belly). Without weights it moves rigidly with the slot's bone.
- **Skin**: a set of attachments by slot (`default` plus outfits). The entity picks one.
- **Clip**: keyed bone rotation/translation/scale, slot attachment and color keys, draw order keys and events.
- **Setup pose**: the pose the images were placed in; clips key bone values *relative to* it, like most 2D
  skeletal tools, so a clip can be reused by characters whose setup pose differs slightly.

## Using it

```json
"Skeleton2D": {"asset": "characters/hero.skel2d.json", "clip": "walk"}
```

The entity's Transform places the skeleton's root. `Skeleton2D.layer`/`order` sort it among sprites like a Sprite.
Other fields: `skin`, `speed`, `playing`, `mix` (seconds the bones fade from the previous clip when `clip` changes),
`color`, `opacity`, `flipX`/`flipY` (mirror around the root), `alphaCutoff` (0 = soft edges blended in slot order,
for painted art; above 0 = cut out with depth, for pixel art), `lit`, `visible`; `time` and `finished` are runtime.
Clip events reach scripts as `onAnimationEvent(name, clip)`, plus `finished` when a non-looping clip ends, like
sprite and model clips ([ANIMATION.md](ANIMATION.md)).

```lua
skeleton2d.play(self.id, "attack", {mix = 0.05})
local hand = skeleton2d.bone(self.id, "hand_r")   -- {position = {x,y,z}, rotation = degrees}
if skeleton2d.state(self.id).finished then skeleton2d.play(self.id, "idle") end
```

## Commands

| Command | What it does |
|---|---|
| `skeleton2d.create {path, values, overwrite?}` | Writes a new asset after validating it; returns `stats` and `warnings` |
| `skeleton2d.set {path, values, clip?, remove?}` | Replaces top-level fields, or patches/creates/removes one clip; validated before writing |
| `skeleton2d.get {path}` | The file plus `stats` (bones, slots, attachments, meshes, vertices, skins, clips with durations) |
| `skeleton2d.check {path \| values}` | `valid`, the first `error` with its place (`skins.default.arm.upper.weights[3]: unknown bone 'elbw'`) and a `hint`, or `stats` and `warnings` |
| `skeleton2d.state {id, bones?}` | Clip, time, fade, shown attachments, a warning for an unknown clip (with the clip names) and every bone's world position and rotation |

Validation is strict on purpose: unknown fields are errors (a typo such as `rotaton` is reported, not ignored), so an
agent finds its mistakes in one round trip. `warnings` list what is valid but probably unintended: images that do not
load, rectangles or mesh vertices outside their image, meshes without weights, bones nothing uses, attachments no
slot or clip ever shows, clips without duration. File writes are not part of scene undo (like the other asset commands).

## Asset format

```json
{
  "pixelsPerUnit": 64,
  "pixelArt": false,
  "bones": [
    {"name": "root"},
    {"name": "hip", "parent": "root", "y": 1.0},
    {"name": "torso", "parent": "hip", "length": 0.6},
    {"name": "thigh_l", "parent": "hip", "x": 0.09, "rotation": 0, "scaleX": 1, "scaleY": 1}
  ],
  "slots": [
    {"name": "torso", "bone": "torso", "attachment": "torso", "color": "#ffffff", "blend": "alpha"},
    {"name": "thigh_l", "bone": "thigh_l", "attachment": "upper"}
  ],
  "skins": {
    "default": {
      "thigh_l": {"upper": {"image": "parts/thigh.png", "pivot": [0.5, 1], "width": 0.16, "height": 0.5}},
      "torso": {"torso": {"image": "parts/torso.png", "pivot": [0.5, 0],
        "vertices": [0, 0, 64, 0, 0, 32, 64, 32, 0, 64, 64, 64],
        "triangles": [0, 2, 3, 0, 3, 1, 2, 4, 5, 2, 5, 3],
        "weights": [[["torso", 1]], [["torso", 1]], [["hip", 0.5], ["torso", 0.5]], [["hip", 0.5], ["torso", 0.5]], [["hip", 1]], [["hip", 1]]]}}
    },
    "armor": {"torso": {"torso": {"image": "parts/torso_armor.png", "pivot": [0.5, 0]}}}
  },
  "clips": {
    "walk": {
      "duration": 0.8,
      "loop": true,
      "bones": {
        "hip": {"translate": [[0, 0, 0, "smooth"], [0.2, 0, 0.05, "smooth"], [0.4, 0, 0]]},
        "thigh_l": {"rotate": [[0, 28, "smooth"], [0.4, -28, "smooth"], [0.8, 28]]}
      },
      "slots": {"torso": {"attachment": [[0.5, "torso"]], "color": [[0, "#ffffff", 1], [0.4, "#ff8080", 0.5]]}},
      "drawOrder": [[0.4, ["thigh_l", "torso"]]],
      "events": [{"time": 0.0, "name": "step"}, {"time": 0.4, "name": "step"}]
    }
  }
}
```

- **bones**: parents are listed before their children; only the first bone (the root) has no parent. `x`, `y`,
  `rotation` (degrees, counter-clockwise), `scaleX`, `scaleY` are the setup pose relative to the parent, in world
  units. At most 256 bones.
- **slots**: draw order, back to front (at most 256). `attachment` is what the slot shows in the setup pose (empty =
  nothing), `color` (`"#rrggbb"`, `"#rrggbbaa"` or `[r, g, b, a]`) tints it, `blend: add` draws it additively.
- **skins**: skin -> slot -> attachment name -> attachment. `default` always exists; `Skeleton2D.skin` picks another
  whose attachments replace the default ones of the same slot and name. An attachment places a project image:
  `rect` `[x, y, w, h]` (pixels, from the top-left; default the whole image), `pivot` `[x, y]` inside it (0..1,
  `[0, 0]` = bottom-left), and `x`, `y`, `rotation`, `scaleX`, `scaleY` = where the pivot sits in the slot bone's
  space in the setup pose. `width`/`height` give its world size (default pixels / `pixelsPerUnit`).
  With `vertices` (rectangle pixels, x right, y down), `triangles` (index triples) and optional `weights` (per vertex
  1..4 `[bone, weight]` pairs, normalized) it is a mesh that bends; without weights a mesh moves with its slot's bone.
  `filled: true` marks a picture repainted where front parts hid it (set by `skeleton2d.variant_import`; the analysis
  then does not report it as revealed). At most 1024 attachments, 4096 vertices each.
- **clips**: each timeline is a list of keys `[time, value..., curve]` with increasing times; `curve` (the way to the
  next key) is `linear` (default), `step`, `smooth`, `easeIn` or `easeOut`. Bone `rotate` `[t, degrees]` is added to
  the setup rotation, `translate` `[t, x, y]` to the setup position, `scale` `[t, sx, sy]` multiplies the setup scale;
  before the first key the first key holds, after the last the last. Slot `attachment` `[t, name | null]` and
  `drawOrder` `[t, [every slot, back to front]]` switch at their keys (the setup value holds before the first one);
  slot `color` `[t, color, alpha?]` fades. `duration` defaults to the last key; `loop` defaults to true. `events`
  `{time, name}` reach `onAnimationEvent`. Rotations interpolate as numbers (350 -> 10 turns back through 180).

### Springs and IK

```json
"springs": [{"bone": "scarf", "stiffness": 40, "damping": 4, "inertia": 1.2, "gravity": 2, "limit": 50}],
"ik": [{"name": "leg_l", "bones": ["thigh_l", "shin_l"], "target": "foot_goal_l", "bend": "negative", "mix": 1}]
```

- **springs** give a bone secondary motion (hair, a scarf, a tail, ears): while the game runs, the Skeleton2D
  component swings it away from its clip angle as a damped spring - thrown by the acceleration of the bone's origin
  (the parent's motion and the entity's own movement; `inertia` scales it, 0 = none), pulled by `gravity` at its
  tip, back toward the clip by `stiffness` (1/s²), slowed by `damping` (1/s), never more than `limit` degrees.
  Deterministic (fixed steps); a mirrored (flipX) skeleton swings the mirrored way. `skeleton2d.state` reports each
  spring's current swing. The root cannot have one.
- **ik** turns one bone, or a parent and its child (thigh + shin, upper arm + forearm), after the clip so the end
  reaches the origin of the `target` bone: a bone of its own (under the root or the hip, never under the chain) that
  clips key like any other - a planted foot, a hand on a ledge. The end of a chain is the last bone's tip
  (`length`). `bend` picks the side the joint folds to (positive = counter-clockwise), `mix` blends from the clip's
  pose. Out of reach, the chain points straight at the target. IK is part of every pose (drawing, overlay,
  analysis).

## Motion recipes

`skeleton2d.motion {path, recipe, name?, view?, params?, bones?, dryRun?}` writes a clip from a recipe:

| recipe | Loop | What moves | params (defaults) |
|---|---|---|---|
| `idle` | yes | breathing: hip bob, chest, head, arms | `duration` 2.4, `breath` 0.006 (of the figure) |
| `walk` | yes | side: contact / passing / contact / swing per leg, the far leg half a cycle later, feet kept flat, body lowered at the contacts so the feet stay on the ground, arms opposite; front: knees lift in turn, body bobs and sways | `duration` 1, `stride` 24, `knee` 40, `arm` 16, `bob` 0.012, `lean` 3 |
| `run` | yes | like the side walk, bigger, with a flight phase, bent arms, a lean | `duration` 0.65, `stride` 38, `knee` 80, `arm` 32, `bob` 0.025, `lean` 10 |
| `jump` | no | crouch, takeoff, tuck, land; events `takeoff`, `land` | `duration` 1, `lift` 0.25 (0 when physics moves the body) |
| `attack` | no | wind-up, strike, hold, recover with one arm, the body turning into it; event `hit` | `duration` 0.6, `hand` (l/r, front/back) |
| `hit` | no | thrown back and recovering | `duration` 0.4 |
| `death` | no | collapses backward; event `dead` | `duration` 1.2 |

Every recipe takes `strength` (0..3, scales every move). Bones are found by **role** - the names
`skeleton2d.draft_template` gives (hip, chest, head, upper_arm/forearm/hand, thigh/shin/foot with `_l`/`_r` in a
front view or `_front`/`_back` in a side view) - and `bones: {"hip": "pelvis", ...}` maps them for other rigs;
roles the rig lacks are skipped and listed in `missingRoles`. The view is `side` when the rig has `thigh_front`
(else `front`), or `view`. Directions are read from the rig: which sign of rotation moves a bone's tip forward
(+x: side views face right), outward (away from the body's middle) or up comes from its setup pose, so a recipe
fits rigs whose bones point any way. The result is an ordinary clip (`skeleton2d.set` tunes its keys). `dryRun`
returns it without writing.

With a clip, `skeleton2d.analyze` also checks the feet (`feet`, default `foot_l/r` or `foot_front/back`) against
the ground line of the setup pose - a foot more than 2% of the figure below it is a `ground` problem, as is no foot
touching it in any pose (unless the clip has a `takeoff` event) - and reports `ground.plantedSpeed`: how fast the
foot carrying the body moves along the ground. A walk cycle played in place needs the entity to move at minus that
speed (world units per second) for the feet not to slide; the side `walk` recipe at the defaults on a 2-unit leg
gives -3.26.

## Reading coordinates (agent eyes)

Rigging from a picture needs pixel positions (where the elbow is, where the hair starts). A model looking at a
whole picture guesses them to within a few percent; these commands make the reading exact and checkable. Each
writes a PNG (default under `.oe/vision/`, never packaged) for the agent to read and returns how to map it back.

- `vision.calibrate {seed?, count?, size?}` draws labeled dots at secret positions. Read their centres and send
  them back with the same seed, count and size: `vision.calibrate {seed, answers: {"A": [x, y], ...}}` returns the
  error of each, `mean`, `max` and `passed` (every dot within `tolerance`, default 3 px). An agent runs this once
  before it rigs; if it cannot pass, it tells the user that its automatic rig may be off.
- `image.grid {path, rect?, step?, scale?, marks?, out?}` zooms into `rect` of an image (nearest neighbour, 1..32x,
  about 1024 px wide by default) and draws grid lines every `step` image pixels (every fifth yellow and labeled,
  the others labeled too when there is room), transparent pixels over a checkerboard. Labels are image pixel
  coordinates (x right, y down) of the line at the left/top edge of that pixel. `marks: [[x, y, "label"], ...]` draws
  crosshairs where the agent thinks points are, so a guess is checked in one more look. The reply gives `rect`,
  `scale` and `origin` (where the image starts in the picture).
- `skeleton2d.overlay {path | id, clip?, time?, skin?, weights?, mesh?, labels?, size?, out?}` renders the skeleton
  (setup pose, or a clip at a time; looping clips wrap the time) and draws the rig over it: bones as yellow lines
  along their length with named joints, the parent links, mesh edges and vertices, and with `weights: "bone"` that
  bone's weight per vertex from blue (0) to red (1). The reply lists every bone's `pixel` position in the picture
  and `world` position, and `missingImages`.

A bone points along its local +X: `length` is drawn that way and a child placed at `x: length` sits at its tip. An
upright torso bone therefore has `rotation: 90`, a hanging leg `rotation: -90`.

## Building from a picture

A character starts as one illustration: the whole figure on a transparent background, in a pose that keeps its
limbs apart (front view: arms held a little away from the body, legs apart; side view: facing right). Image
generators make these well (`docs/RPG.md` shows the Codex image generation setup; ask for "transparent background,
A-pose, clear gap between arms and torso, no ground shadow"). The agent then writes a **draft** and the engine
builds the skeleton from it.

```json
{
  "image": "art/hero.png",
  "height": 2,
  "joints": {"hip": [502, 790], "chest": [502, 460], "neck": [502, 270], "elbow_l": [720, 575], "...": [0, 0]},
  "bones": [
    {"name": "hip", "from": "hip", "to": "chest"},
    {"name": "chest", "parent": "hip", "from": "chest", "to": "neck"},
    {"name": "upper_arm_l", "parent": "chest", "from": "shoulder_l", "to": "elbow_l"}
  ],
  "parts": [
    {"name": "torso", "polygon": [[340, 270], [670, 270], [690, 820], [320, 820]], "bones": ["hip", "chest"]},
    {"name": "head", "ellipse": [502, 155, 120, 125], "bone": "head"},
    {"name": "arm_l", "polygon": [[...]], "bones": ["upper_arm_l", "forearm_l"], "cell": 24},
    {"name": "hat", "image": "art/hat.png", "at": [430, 0], "bone": "head"}
  ]
}
```

- `joints` are image pixels (x right, y down). `bones` run `from` -> `to` (joint names or `[x, y]`); a root bone at
  the `origin` (default: the bottom centre of the opaque pixels, or a `root` joint) is added, and `parent` defaults
  to it. Clips turn a bone around its `from`.
- `parts` are listed back to front. Each takes the opaque pixels of the illustration inside its `polygon`,
  `ellipse` `[cx, cy, rx, ry]` or `rect` `[x, y, w, h]` (neighbours should overlap a little at the joints so bending
  opens no gap), or brings its own `image` placed with its top-left at `at`. `bone` makes it rigid; `bones` (1..8)
  makes it a grid mesh (`cell` pixels, default about a tenth of the part) whose vertices are weighted by their
  distance to those bones, so it bends at the joints between them.
- `height` (world units of the figure, default 2) or `pixelsPerUnit` sets the scale; `alphaThreshold` (default 8)
  what counts as opaque; `pixelArt` is copied to the asset.

`skeleton2d.draft_template {kind: humanoid-front|humanoid-side, image, joints?, out?}` writes a complete draft for
the usual figure: 20 joints with a note on where each goes, 18 bones (hip, chest, head, and per side upper arm,
forearm, hand, thigh, shin, foot; `_l`/`_r` are the character's left and right, `_front`/`_back` in a side view)
and 10 parts in draw order. With `image` the joints are first guesses from the figure's bounds; the agent corrects
them with `image.grid` and passes them back as `joints`, and the template fits the regions to them: limb thickness is
measured across each bone in the picture's opaque pixels (the second-thinnest of five runs, so a limb touching the
body does not measure the body) and the band is moved onto the middle of the limb. Things the template does not
know (a scarf, a tail, a weapon) are parts the agent adds.

`skeleton2d.build {draft | values, out, partsDir?, dryRun?, draftImage?}` cuts each part into
`<out>-parts/<name>.png`, writes `out` so the setup pose is exactly the illustration (images stay upright, bones at
their joints) and returns per part its rectangle, pixels, vertices and bones; `coverage` (opaque pixels no part
took, pixels two parts share); warnings with hints (a part that takes nothing, joints and bones nothing uses, bones
without a direction, more than 1% of the figure uncovered); and `draftImage`, a picture of the draft over the
illustration with regions, bones and joints, and magenta where no part took the pixels. `dryRun` only checks and
draws. Rebuilding keeps the clips of `out`; if they key a bone or slot that is gone the build is refused (the hint
says how to fix it) and the file stays as it was. With `values` and `draft` the draft is also saved there.

The loop an agent runs: `vision.calibrate` -> `skeleton2d.draft_template {image}` -> correct joints with
`image.grid {marks}` -> `skeleton2d.draft_template {image, joints}` -> `skeleton2d.build {dryRun}` and read
`draftImage` -> add or grow parts until the magenta is gone -> `skeleton2d.build` -> `skeleton2d.overlay` in the
setup pose and in a test clip.

## Reviewing a rig

A rig that looks right in its setup pose can still fall apart when it moves. Two commands look at it in motion. Both
take `path` or `id` and a set of poses: a `clip` sampled at `samples` (default 8) evenly spaced times or at exact
`times`, every clip with `clip: "*"`, or - without a clip - **stress poses** that bend each joint that moves
something by +-`angle` degrees (default 45), one joint at a time. The poses are drawn by a small rasterizer that
knows for every pixel which slot shows it and from which texel of its image, so the measurements need no GPU.

`skeleton2d.analyze` returns `problems`, the most serious first, each with `kind`, `severity` (`high` fails
`passed`), `subject`, a `message`, the worst `pose` and a `hint`:

| kind | What it means | Typical fix |
|---|---|---|
| `revealed` | Picture that was under another part in the setup pose shows (`hiddenUnder` names it): cut edges, or a copy of the covering part (shared pixels cut into both) | Repaint the area (`masks: true` writes each slot's revealed texels as a white mask at its image's size, under `.oe/vision/revealed/`), or make the parts overlap less |
| `gap` | The figure opens around a joint: the disk around it is less filled than in the setup pose (`drop`) | Grow both regions past the joint, or weight one part to both bones |
| `flipped` | Mesh triangles fold over | Spread the weights near the joint, smaller `cell`, or bend less |
| `stretched` | Triangles stretch beyond 1.6x (2x high) or squash below 0.5x (0.4x high) (`ratio`) | Spread the move over more of the part |
| `seam` | A looping clip ends in another pose than it starts (`bone`) | Key the end like time 0, or `loop: false` |

It also writes `image` (default `.oe/vision/analyze.png`): the setup pose and the three worst poses side by side,
revealed picture tinted magenta, opened joints circled red. On the 1024 x 1536 test adventurer the stress analysis
(32 poses) takes about 0.4 s.

`skeleton2d.poses {..., columns?, cell?, focus?, zoom?, out?}` draws the poses as a labeled contact sheet; `focus:
"bone"` centres every cell on that joint, zoomed (`zoom`, default 3), to compare a shoulder or a knee across a clip.

## Repainting parts

Cut from one illustration, a part has nothing where another part was in front of it: the torso has the arm's
pixels (or nothing) under the arm, and they show when the arm lifts (`revealed` in the analysis). An image
generator can paint what was hidden. The engine does not call one: it writes requests an agent hands to one and
checks what comes back. **Sending the pictures to a generator shares them with that service; ask the user first.**

`skeleton2d.variant_requests {path, kind?, slots?, reach?, grow?, outDir?}`

- `kind: "fill"` (default): for every part (or `slots`), the area of its picture that lay under a part drawn after
  it in the setup pose, within `reach` pixels of the part's own pixels (default 15% of the part), widened by two
  pixels. `grow` first enlarges the part's canvas by that many pixels on every side, for parts cut too tightly to
  continue under their neighbour.
- `kind: "attachment"` with `slot`, `name` and `description`: a new picture for the slot (a fist, closed eyes),
  the whole canvas editable.

Each request is a folder under `outDir` (default `.oe/variants/<skeleton>/<slot>-<fill|name>/`): `edit.png` (the
part with the area to paint in magenta), `source.png`, `mask.png` (transparent = may change), the shared
`context.png` (the whole figure), `prompt.txt` (names the part and every part that hid it) and `request.json`.
Generators draw at a few shapes, so the pictures are padded to the nearest of square, 2:3 and 3:2 (`crop` in the
request says where the part is). The reply lists the requests with the images to send, in order, and the `result`
path to save the answer at.

With Codex CLI (as used for `games/RPG` art):
`codex exec --skip-git-repo-check -c "sandbox_mode='read-only'" -i <folder>/edit.png -i <outDir>/context.png -`
with "Generate exactly one image with your native image generation tool, editing the first attached image ..." and
the prompt on stdin; the picture lands in `~/.codex/generated_images/<session>/`.

`skeleton2d.variant_import {request, result?, maxShift?, tolerance?}` checks the answer before it touches anything:

- it must have the shape of the request's pictures (any size: it is scaled);
- the area that had to stay is matched against the source over shifts of up to `maxShift` pixels (default 8,
  the smallest shift wins in flat areas) and a per-channel tint (0.75..1.33, channels too dark to measure stay at
  1); if it still differs by more than `tolerance` (default 0.08 of full scale) the answer is refused - generators
  often redraw the whole picture, and a slightly different redraw passes while a different picture does not;
- a fill with magenta left in more than a twentieth of the area is refused;
- an answer without transparency has its background keyed out (the colour of its corners).

It then keeps only the masked area of the answer, writes a new picture beside the part's (`<image>-filled.png`,
`<slot>-<name>.png`; the originals stay), regrids a mesh part over it (the old cell size, each vertex weighted like
the nearest old vertices) and updates the asset: a fill replaces the attachment's picture and marks it
`filled: true` (the analysis no longer reports it as revealed), an attachment variant is added to the slot. A
rebuild with `skeleton2d.build` cuts the parts again from the illustration: import the fills again afterwards.

On the test adventurer the torso request (both arms and the head had hidden parts of it) came back from Codex
redrawn as a whole, slightly larger; the import found a (-2, -4) shift, accepted it at 5.7% difference and kept
only the masked area: the sleeves now continue under the arms when they lift. The whole character offered as the
answer instead was refused at 18.1%.

## Rendering

A skeleton is drawn as one skinned mesh through the same path as skinned 3D models: the attachments of the current
skin and slot state are merged into a mesh (cached by that state), each vertex carries up to four bone weights, and
every frame only the bone palette (one matrix per bone) changes. Both renderers already skin on that path (software
`RenderItem::VertexSkinMatrix`, GPU `mesh_vs` joint palette), so the GPU matches the software reference without a
new shader. Slots are separated by a tiny step along +Z so cut-out parts sort by depth in draw order; images of
different files become submeshes with their own material.

## Implementation status (work log)

Milestones, each committed on its own with its tests (docs/DESIGN.md §5). The next agent starts at the first
unchecked line.

- [x] **M1 — Runtime.** `scene/Skeleton2D.{h,cpp}` (strict parse with places in messages, sampling, crossfade,
  `Skeleton2DComponentPose`), `AssetManager::GetSkeleton2D` (hot reload, asset kind `skeleton2d`), `Skeleton2D`
  component stepped in `Systems.cpp` (`UpdateSkeletons2D`: time, fade, events, `finished`), `render/Skeleton2DMesh.cpp`
  (attachments merged into one skinned mesh cached by the shown state; drawn through the 3D skinning path),
  `api/Skeleton2DCommands.cpp`, Lua `skeleton2d.play/state/bone`. Test `Skeleton2DRuntime` (validation errors,
  sampling, events, slot switch, determinism, GPU vs software where a GPU exists, fades, Lua, `skeleton2d.set`).
  Checked by eye: a 12-bone test humanoid (`build/sk2d`, local) in setup, walk, bend and flipped poses, software and
  D3D11 frames alike. Not in M1: `light2D`, `ySort`, masks and pixel-perfect bone snapping for skeletons, per-vertex
  (free-form) deform keys, IK; an animated slot color makes a new mesh each frame (fine for fades, not for hundreds
  of characters). The web and Android runtimes are not rebuilt yet.
- [x] **M2 — Agent eyes.** `render/ImageDraw.{h,cpp}` (lines, circles, rectangles, pixel-font text with outline on an
  `Image`), `api/VisionCommands.cpp` (`image.grid` with marks, `vision.calibrate` with deterministic layouts and
  scoring, `WriteToolImage`), `skeleton2d.overlay` (render + bones, links, mesh, weight heat map, non-overlapping
  labels). Test `Skeleton2DAgentVision` (grid size and errors, calibration determinism, dot centres found in the
  pixels pass and moved ones fail, overlay bone positions). Checked by doing it: calibration seed 7 read through
  `image.grid` zooms and marks, all six dots at 0 px error; overlays of the test humanoid read correctly.
- [x] **M3 — Build from a draft.** `app/Skeleton2DBuild.cpp`: draft parsing (strict, places in messages), part cutting
  (polygon / ellipse / rect regions of one illustration, or separate images), grid meshes inside the opaque pixels,
  distance weights (inverse fourth power, at most four bones, below 2% dropped), bone and attachment placement so the
  setup pose is the illustration, coverage, the draft picture, clip keeping on rebuild; templates `humanoid-front` and
  `humanoid-side` with joints guessed from the bounds and regions fitted to given joints by measured limb widths.
  Commands `skeleton2d.draft_template`, `skeleton2d.build`. Test `Skeleton2DBuildFromDraft`. Run end to end on a
  Codex-generated 1024 x 1536 adventurer (`build/sk2d/art/hero.png`, local): template guesses left 23.8% of the figure
  uncovered; joints corrected through two `image.grid` looks brought it to 2.9%, one added `scarf` part to 0.6%; the
  built rig (20 bones with the root, 11 parts, meshes of 45..90 vertices) bends cleanly at elbows, knees and waist. Seen there for
  M4/M5: the torso shows its cut edge at the armpit when the arm lifts (the hidden area behind the arm was never
  drawn). Not done: a `quadruped` template.
- [x] **M4 — Review.** `app/Skeleton2DReview.cpp`: pose sets (clip samples, all clips, stress poses), a slot/texel
  rasterizer, `skeleton2d.analyze` (revealed with masks, gap, flipped, stretched, seam; worst-pose picture) and
  `skeleton2d.poses` (contact sheet, joint focus). `render/Skeleton2DMesh` now shares the attachment geometry
  (`Skeleton2DPieceGeometry`, `Skeleton2DPieces`, `Skeleton2DPalette`, `Skeleton2DSkinPoint`). Test `Skeleton2DReview`
  (revealed area and its mask, an opened hip, a folded mesh, a loop seam, a still clip passing, contact sheets). On
  the test adventurer it reports the torso's armpit edges, the legs' tops under the tunic hem and the scarf under the
  arm as revealed - the problems seen by eye in M3. Not done: foot sliding (needs planted bones; with M6 motion).
- [x] **M5 — Generated fills.** `app/Skeleton2DVariants.cpp`: `skeleton2d.variant_requests` (fill areas from the setup
  pose's coverage, attachment variants, shape padding, prompts naming the covering parts) and
  `skeleton2d.variant_import` (shape check and scaling, shift and tint matching, refusal, magenta check, background
  keying, regrid with weight transfer, `filled` attachments the analysis skips). `Skeleton2DGridMesh` is shared with
  the build; the review rasterizer moved to `app/Skeleton2DRaster.h`. Test `Skeleton2DVariants` (refused spoiled,
  magenta and square answers; a moved, doubled answer accepted; a keyed attachment variant; a grown canvas keeping its
  place). Run with Codex on the test adventurer's torso (see "Repainting parts"). Not done: the other five requests of
  that rig were not sent; a rebuild drops imported fills (re-import them).
- [x] **M6 — Motion.** `app/Skeleton2DMotion.cpp` (`skeleton2d.motion`: seven recipes, roles, directions from the
  setup pose, side walk with four key poses per leg, flat feet and contact drop), asset `springs` (stepped in
  `Systems.cpp` `StepSkeleton2DSprings`, swing in `Skeleton2DComponentPose`, reported by `skeleton2d.state`) and
  `ik` (`Skeleton2DSolveIk` inside `Skeleton2DSample`), the analysis' `ground` check and `plantedSpeed`. Test
  `Skeleton2DMotionSpringsIk` (walk directions and knee, feet on the ground, planted speed, dry run, one-shot jump,
  errors; a tail spring thrown and settling, the same in a second engine; IK reach, bend sides, out of reach, mix,
  refused constraints). On the generated (front-view) adventurer: idle and walk pass the analysis, jump reports the
  leg mesh squashing at the deep crouch (0.44x), a scarf spring swings 48 degrees at the takeoff and settles.
  Not done: a Lua call to move IK targets or springs at run time (scripts key them through clips only); recipes for
  four-legged rigs; front-view walks are steps in place (a front view cannot show legs swinging forward).
- [ ] **M7 — Agent guide.** `docs/SKELETON2D_AGENT.md` (the step-by-step workflow with a check per step), a
  "Rigger" profile in the default team, and one full run from a generated illustration to an animated character.
- [ ] **M8 — Editor.** Skeleton window: bones, attachments, weights painting, clip timeline, all through the commands.
- [ ] Optional: DragonBones JSON import (MIT). Spine JSON import only after its licence terms are checked.
