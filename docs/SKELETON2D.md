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
  At most 1024 attachments, 4096 vertices each.
- **clips**: each timeline is a list of keys `[time, value..., curve]` with increasing times; `curve` (the way to the
  next key) is `linear` (default), `step`, `smooth`, `easeIn` or `easeOut`. Bone `rotate` `[t, degrees]` is added to
  the setup rotation, `translate` `[t, x, y]` to the setup position, `scale` `[t, sx, sy]` multiplies the setup scale;
  before the first key the first key holds, after the last the last. Slot `attachment` `[t, name | null]` and
  `drawOrder` `[t, [every slot, back to front]]` switch at their keys (the setup value holds before the first one);
  slot `color` `[t, color, alpha?]` fades. `duration` defaults to the last key; `loop` defaults to true. `events`
  `{time, name}` reach `onAnimationEvent`. Rotations interpolate as numbers (350 -> 10 turns back through 180).

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
- [ ] **M2 — Agent eyes.** `image.grid {path, rect, step, out}` (zoomed coordinate grid over an image region),
  `vision.calibrate` (marks at known pixels; the agent's answers are scored before it rigs), `skeleton2d.overlay`
  (bones, slots, mesh edges and weights drawn over the setup pose or a clip frame).
- [ ] **M3 — Build from a draft.** A semantic draft (`*.skel2d-draft.json`: body regions as polygons/ellipses and
  joints as points, from templates `humanoid-front`, `humanoid-side`, `quadruped`) and `skeleton2d.build`: cut part
  images out of one illustration (or take separate part images), outline them from alpha, triangulate, weight
  automatically by distance to bone segments, write the asset. `skeleton2d.draft_template {kind}`.
- [ ] **M4 — Review.** `skeleton2d.poses {out, poses|clip, times}` (contact sheet + zoomed crops) and
  `skeleton2d.analyze` (flipped and stretched triangles, gaps between parts at joints, holes: pixels inside the
  rest silhouette that become transparent when a joint bends, loop seams, foot sliding of planted bones) with hints.
- [ ] **M5 — Generated fills.** `skeleton2d.variant_requests` (mask + prompt files for hidden areas the analysis found
  and for extra attachments: closed eyes, hand poses), `skeleton2d.variant_import` (rejects changes outside the mask,
  corrects small shifts/tints, registers attachments). Sending a picture to an image generator needs the user's consent.
- [ ] **M6 — Motion.** `skeleton2d.motion {recipe}` writes clips from parameters (idle breathing, walk, run, jump,
  attack, hit, death) onto the bones the draft named; `SpringBone2D`-style secondary motion and two-bone IK for feet.
- [ ] **M7 — Agent guide.** `docs/SKELETON2D_AGENT.md` (the step-by-step workflow with a check per step), a
  "Rigger" profile in the default team, and one full run from a generated illustration to an animated character.
- [ ] **M8 — Editor.** Skeleton window: bones, attachments, weights painting, clip timeline, all through the commands.
- [ ] Optional: DragonBones JSON import (MIT). Spine JSON import only after its licence terms are checked.
