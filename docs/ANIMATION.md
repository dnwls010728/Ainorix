# Animation playback and events

Animation poses use fixed simulation time and the same model-space joint palette
in the software and GPU renderers. Models remain immutable cached assets; each
entity owns its playback state through a reflected Animator component.

Skinned and rigid glTF animation plays in both renderers, including GPU shadow and selection passes; the
web and Android players include it.

## Model contract

Each model retains its node hierarchy and default local TRS (or matrix). Skin
joints reference nodes plus inverse bind matrices; four influences are normalized
per vertex. Static vertices retain their baked node transforms; animated rigid
nodes use the same palette with one influence. The palette limit is 1024 entries
per model, including rigid animated nodes; models beyond it report a load error. One GPU draw addresses
64 of them: a model with more is cut when it loads (`Mesh::SplitSkinPalettes`) into ranges that each use at
most 64 joints, every range with its own list of joints (`Submesh::palette`), and the vertices two ranges would
share are duplicated. `asset.info` then lists more submeshes than the file has materials. The software renderer
and everything that reads bones (`animation.bones`, IK, springs, attachments) use the whole palette.
Zero-weight vertices remain untransformed. FBX models (`.fbx`) fill the same structures: their
curves are resampled into LINEAR keys at load time (see RENDERING.md, FBX models). Morph-target animations (keys on blend
shape weights; the shapes themselves load, see [Blend shapes](#blend-shapes)) and additional
JOINTS_1/WEIGHTS_1 influence sets are outside this stage and must be reported.

Animation channels store times and values in source order. LINEAR rotations use
shortest-path quaternion slerp; STEP holds the previous key. CUBICSPLINE uses
glTF Hermite tangents scaled by the key interval, then normalizes quaternions.
Each clip exposes a stable name, duration and channel count through asset.info.

The loader follows the [glTF 2.0 specification](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html).
Rotation channels accept FLOAT or normalized signed/unsigned bytes/shorts;
translation and scale use FLOAT. Skin weights accept FLOAT or normalized unsigned
bytes/shorts. Missing inverse bind matrices use identity. Duplicate clip names
receive stable numeric suffixes; unnamed clips use their source index.

Skin deformation is connected to both renderers, including GPU shadow and
selection passes. Web/Android prebuilts include it; physical-device and Linux
execution limitations are recorded above.

## Rendering contract

RenderScene evaluates one palette per entity without modifying cached models.
Both renderers blend four joint matrices using the normalized vertex weights,
then transform positions/tangents with that matrix and normals with its inverse
transpose. Zero-weight vertices use identity. Flat normals follow deformed
triangles (GPU fragment derivatives); smooth normals are normalized per vertex.
Pose bounds are recomputed for shadow fitting and transparent draw ordering.
GPU vertex uniforms contain at most 64 matrices per draw, including rigid node bindings: the whole palette of
a small model, or the joints of one draw range of a larger one (vertex joint indices then count within the range).

Shaders were regenerated using the official sokol-tools-bin Windows compiler
(SHA256 BD616287F9EA689D53C6D260E443EE733E61AE1B73A9B37ADC482EAD0364D561)
through tools/shaders/compile_shaders.bat. The compiler is an ignored build tool;
normal builds use the committed generated header without downloading it.

## Bones, attachments, IK and springs

Models keep the names of their nodes (glTF node names, FBX model names). `animation.bones {id}` lists them for an
entity's model in its current pose: `bones: [{name, parent, position, rotation, modelPosition, vertices, vertexMin, vertexMax}]` (world space, Euler
degrees; `modelPosition` is the `Transform.position` a child entity needs to stand on the bone, e.g. a new goal;
`vertices` = how many vertices the bone is the strongest influence of, with their bounds in the default pose in model
space (only in the whole list, not with `bone`), which tells a skirt, coat or hair bone from a body bone and shows how far the mesh hangs below a last bone), plus
how the entity's `BoneIK`, `BoneAttachment` and `SpringBone` children resolve (`goals: [{id, name, bone, found, distance}]`,
`attachments: [{id, name, bone, found}]`, `springs: [{id, name, bone, found, bones, simulating}]`,
`colliders: [{id, name, bone, tail, found}]`). `bone: "Hand_R"` returns one node. Lua: `animation.bone(id, name, {ik = false}?)`
returns `{position, rotation, modelPosition}` (each `{x,y,z}`) or `nil` for an unknown name. Both read the pose that is
drawn: the Animator's clip and time, then IK, then springs. `ik: false` stops after the clip, which is what a script that drives the
goals itself needs to read.

All three are components on a **direct child** of the entity that has the model, so several of each can belong to
one character and each one is an ordinary entity to move, parent, save and select. A child's Transform is in the
model's own units and inherits the model entity's scale: under a model shown with `Transform.scale` 0.01, goal
positions are in the model's large numbers and an attached mesh needs the inverse scale.

**BoneAttachment** `{bone, position, rotation}` holds its entity on a bone: a sword in a hand, a hat, a camera on a
head, a `Trail` on a blade tip. After physics each step (and after every edit while not playing) the engine writes the
entity's `Transform.position` and `rotation` from the posed bone; `position`/`rotation` are offsets in the bone's
axes, `Transform.scale` stays as set. Anything on that entity or below it follows: `MeshRenderer`, lights, colliders,
`VFXSystem`. It runs before `CameraFollow` and trails. An unknown bone leaves the Transform alone
(`attachments[].found` is false).

```json
{"command": "entity.create", "args": {"name": "Sword", "parent": "Hero", "components": {
  "MeshRenderer": {"mesh": "assets/models/sword.glb"},
  "BoneAttachment": {"bone": "Hand_R", "position": [0, 0.05, 0], "rotation": [0, 0, 90]}}}}
```

**BoneIK** `{bone, chain, weight, pole, soften, keepRotation, turn, matchRotation}` is a goal: the bones above `bone` turn so that it reaches the
goal entity's `Transform.position` (the child's local position, so the goal moves with the character; set it every
frame from Lua to follow something in the world). It is applied on top of the clip in both renderers, so a walking
character can still plant a foot or reach for a handle.

- `chain: 2` (default) is an arm or a leg: an exact two-bone solve for upper bone, lower bone and `bone` as the end.
  `pole` is the model-space direction the elbow or knee points to (`[0,0,1]` for knees that bend forward on a model
  facing +Z); `[0,0,0]` keeps the side the limb bends to in the model's default pose, so a clip that straightens the
  limb for a moment cannot flip the joint (a model whose default pose is straight, such as T-pose arms, needs a pole).
  `soften` (0..0.5, default 0) is the share of the limb length over which it eases into full extension: with 0.1 a
  goal near or beyond reach no longer snaps the limb straight in one frame, and the bone stops slightly short of it.
- `chain: 1` aims the parent bone at the goal (a head or turret looking at something, with the end node in front of it).
- `chain: 3..8` bends a spine, tail or tentacle with a fixed number of cyclic-coordinate-descent passes (deterministic).
- `weight` blends from the animated pose (0) to the goal (1); animate it from Lua (`tween.to`) to reach in and out.
- `matchRotation` also turns the bone to the goal entity's `Transform.rotation`.
- `keepRotation`: the bone keeps the orientation the clip gave it while the limb above it is solved; otherwise it
  turns with the limb. A goal that only corrects a clip (foot placement) needs it, with `pole` left at zero so the
  knee keeps the clip's side: without them a running clip gets twisted ankles even on flat ground. Better still,
  set `weight` from how much correction the ground asks for, so flat ground plays the clip untouched.
- `turn` (Euler degrees, model axes, scaled by `weight`) turns the bone on top of the orientation the pose gives it,
  without replacing it: a foot tilted to the slope under it. Foot placement from a script: read where the clip
  puts the foot (`animation.bone(id, foot, {ik = false})`), cast `physics.raycast(origin, down, distance, body)` with
  the character's own body as the entity to ignore (a ray that starts inside its capsule hits the capsule first
  otherwise, and the feet only adapt while they are outside it), move the goal by the height found and set `turn`
  from the hit normal.

Bone lengths never change: a goal out of reach leaves the limb stretched towards it and `goals[].distance` reports
the shortfall. Goals of one model run in entity id order, each on the result of the one before. Check a setup with
`animation.bones` (distance 0 = reached) and `render.screenshot`.

```json
{"command": "entity.create", "args": {"name": "LeftFootGoal", "parent": "Hero", "components": {
  "Transform": {"position": [0.15, 0.2, 0.1]},
  "BoneIK": {"bone": "Foot_L", "chain": 2, "pole": [0, 0, 1]}}}}
```

**SpringBone** `{bone, stiffness, damping, gravity, weight, radius, endLength, limit, air, collide}` gives a branch of the skeleton secondary motion: a tail, ears,
hair strands, a cape strip, an antenna. `bone` and every bone below it lag behind when the character starts, stops,
turns or lands, swing, and settle back into the animated pose. One component per branch (a tail and two ears are three
child entities); a bone with several bones below it swings by the first, the others ride along and swing below.
A `bone` ending in `*` takes every branch whose bones start with that text: `"Bone_Skirt*"` swings all strips of a
skirt, `"Bone_Hair*"` every strand, with one component.

- Simulated once per fixed step after physics, while playing, on top of the clip and IK: each bone's end is a point
  with inertia in world space (Verlet integration), pulled towards where the pose puts it under its already swung
  parent, then put back at the bone's length. Deterministic; the state is part of the scene, so rollback and
  `sim.stop` restore it. A stopped scene and `ik: false` show the pose without the swing.
- `stiffness` is the pull back to the pose (20 loose hair or cloth, 100 a tail, 400+ short stiff parts; at 3600 the
  bone follows the pose exactly). `damping` is how fast a swing dies out, measured against the pose, so a character
  moving at a steady speed drags nothing (3 a long wobble, 10 one soft overshoot, 25+ no swinging back).
- `gravity` is a world-space acceleration in m/s^2: `[0,-9.8,0]` lets the chain sag, another direction is a steady
  wind. Zero keeps the authored pose at rest. `weight` blends from the pose (0) to the swing (1).
- A clip that already moves these bones in a rhythm (the tail wag of a walk) is exaggerated by a soft spring, more so
  down a long chain; raise `stiffness` and `damping` together until it reads right, or use the spring on bones the
  clips leave alone.
- An entity that jumps more than about two meters in one step (respawn, scene cut) restarts the swing at rest.
- `endLength` (meters): the last bone of a branch has nothing below it and stays rigid on the bone above. With
  `endLength` above 0 it swings (and collides) too, as if it had a bone that far along it: set it to the length of
  mesh that hangs below the last bone (the hem of a skirt, hair tips; see `vertexMin` of that bone in `animation.bones`).
- **SpringCollider** `{bone, tail, radius, offset}`, a child of the model like the springs, is a body part the
  swinging bones do not pass through: a sphere at `bone`, or a capsule from `bone` to `tail` (thigh to calf, calf to
  foot). It follows the clip + IK pose. Every SpringBone of the model is kept `radius` (its own) away from every
  collider. A collider that moves into a bone carries it along (a rising thigh lifts the skirt in front of it instead
  of parting it), and a bone resting on a collider stays where it lies (friction), so it does not creep around the
  limb. Limits: only the bone ends collide, not the mesh between two strips, so use radii a little larger than the
  limb; a collider inside the resting cloth (a torso under a coat, a head under a fringe) pushes it out of shape;
  there is no collision with the ground or with physics bodies. `BoneAttachment` children on a
  swinging bone follow it. Check with `animation.bones` (`springs[].bones` = how many bones swing, 0 for a bone with
  nothing below it; `simulating` once a step has run) and `render.screenshot` after `sim.step`.

```json
{"command": "entity.create", "args": {"name": "TailSpring", "parent": "Fox", "components": {
  "SpringBone": {"bone": "b_Tail01_012", "stiffness": 600, "damping": 24}}}}
{"command": "entity.create", "args": {"name": "SkirtSpring", "parent": "Hero", "components": {
  "SpringBone": {"bone": "Bone_Skirt*", "stiffness": 500, "damping": 24, "endLength": 0.08}}}}
{"command": "entity.create", "args": {"name": "ThighL", "parent": "Hero", "components": {
  "SpringCollider": {"bone": "Bip001LThigh", "tail": "Bip001LCalf", "radius": 0.075}}}}
```

Choosing values for long chains (hair, coat tails): a walk moves the head and hips about twice per second, and every
bone of a chain adds its own swing to that of the bone above. With `damping` below `2 * sqrt(stiffness)` each bone
overshoots and the swing grows down the chain until the hair stands out sideways, even when the character stands
still in an idle clip. At or above it the chain follows late and settles without ringing: stiffness 300-500 with
damping 40-55 reads as hair and cloth on a walking character. `air` (0..1) is the share of the damping that works against the
air instead of against the pose: at 0 a damped bone moves with the character, above 0 it trails behind while the
character moves and falls back when it stops, which is what makes hair and cloth stream in a run (0.02 with the
values above: hair tips about 0.2 m behind at a walk, 0.3 m at a run). A slowly changing `gravity` of a few tenths
of a m/s^2 set from a script reads as a breeze. `limit` (degrees) caps how far a bone leaves its
animated direction when the character starts, stops or jumps (25-40); colliders may push further. `collide: false`
keeps a spring out of the colliders (long hair behind legs whose colliders are there for the skirt).

In the editor: select the model and the Inspector lists its bones under MeshRenderer (search, a paperclip button to
attach a new entity, a pin button to add an IK goal on the bone, drop a Hierarchy entity on a bone to hold it there,
**Add Spring** in a bone's right-click menu);
`bone` and `Animator.clip` fields have a picker and turn red for a name the model does not have; the Scene view's
**Bones** toggle draws the skeleton of the selected model. See [EDITOR.md](EDITOR.md).

**Foot placement** is a project script built from these pieces: each step read where the clip alone puts a foot
(`animation.bone(id, foot, {ik = false})`), cast `physics.raycast` down at that spot, and set the foot's goal to the
ground height plus the height the clip gives the foot (`modelPosition.y` times the model's scale); move the body to
the lowest foot so legs bend on high ground instead of stretching. Three things keep it from popping: advance the
Animator from the same script (`playing = false`, set `time`), because scripts run before the engine steps animators and
a clip read one frame early lags the pose that is drawn; smooth each foot's ground height over a few frames; and never
ask a foot to reach further down than the clip does while the body is still on its way down (plus `soften`).

Not there: joint angle limits, twist-bone distribution, blending between clips, and goals or attachments on entities
that are not direct children of the model's entity.

## Blend shapes

Faces of game characters often have no bones: eyes, brows and mouth move through **blend shapes** (FBX; glTF calls
them morph targets), named sets of vertex offsets. A model's shapes load with it and `asset.info` lists their
names in `blendShapes`. `MeshRenderer.blendShapes` holds the weights:

```json
{"command": "component.set", "args": {"id": "Hero", "type": "MeshRenderer",
  "values": {"blendShapes": {"E_Close": 1, "M_Smile_L": 0.6, "M_Smile_R": 0.6}}}}
```

- A weight of 1 applies the shape as authored, 0.5 half of it; several shapes add up; weights below 0 or above 1
  extrapolate. Names the model does not have are ignored; a missing name weighs 0. Setting the field replaces the
  whole object.
- Shapes are added to positions and normals **before** skinning, so they work together with clips, `BoneIK` and
  `SpringBone`. Tangents, bounds, physics and picking keep the undeformed model.
- From Lua, set them every frame: `self:set("MeshRenderer", {blendShapes = {E_Close = blink}})`. Blinking,
  lip movement and glances are project scripts, not engine components.
- Shapes with the same name in several parts of a model (face, lashes, brows) are one shape. FBX: the full-weight
  target of each blend channel (in-between targets are not used); glTF: `POSITION` and `NORMAL` of each target,
  named by the mesh's `extras.targetNames` (`target0`, ... without it). A target that moves nothing is dropped.
- A child layer with `parentPose` and no weights of its own takes its parent's (an outline shell follows the face).
- Cost: both renderers draw a copy of the model with the weights applied, made on the CPU **when the weights
  change** and kept until they change again (the GPU renderer uploads its vertices once per change). A face that
  moves every frame re-uploads the whole model every frame: round weights from scripts to steps (0.05) so idle
  frames reuse the copy. No shader work is involved, so the web and Android players behave the same.
- Not supported: clips that key shape weights (a script or tween drives them), tangent offsets, in-between shapes.
- The editor's Inspector lists them under **Blend Shapes** in the MeshRenderer, a slider per shape.

Verified by `BlendShapes` (glTF targets in both renderers, weights, undo, `parentPose`, vertices duplicated between
draw ranges).

## Clips from other files and retargeting

A character file often has no animation of its own, or the animation was made for another rig.

**Clip files.** A file named `<model file>.clips.json` next to a model lists files whose clips are added to it when
it loads, their channels matched to the model's bones **by name**:

```json
// assets/models/Hero.fbx.clips.json   (paths are relative to the model file)
{"clips": ["../anims/hero_clips.gltf"]}
```

The model then has those clips like its own: `asset.info` lists them, `Animator.clip` / `animation.play` play them.
A listed file needs no mesh (a glTF with nodes and animations only loads as a model with clips). Channels of bones the
model does not have are dropped; a clip whose name the model already has is skipped with a warning.

**Retargeting.** `animation.retarget` moves clips to a skeleton with other bone names, proportions and rest pose (a
mocap library for a T-pose rig onto a character modelled in an A-pose) and writes them as such a clip file:

```json
{"command": "animation.retarget", "args": {
  "source": "assets/anims/mocap.glb", "target": "assets/models/Hero.fbx", "out": "assets/anims/hero_clips.gltf",
  "hips": "Bip001Pelvis", "clips": {"Idle": "idle", "Walk": "walk", "Run": "run"},
  "bones": {"Bip001Pelvis": "mixamorig:Hips", "Bip001Spine": "mixamorig:Spine", "Bip001Head": "mixamorig:Head",
            "Bip001LUpperArm": "mixamorig:LeftArm", "Bip001LForearm": "mixamorig:LeftForeArm", "Bip001LHand": "mixamorig:LeftHand",
            "Bip001LThigh": "mixamorig:LeftUpLeg", "Bip001LCalf": "mixamorig:LeftLeg", "Bip001LFoot": "mixamorig:LeftFoot"}}}
```

- `animation.retarget_map {source, target}` guesses `bones`, `hips` and `ends` from the bone names of two humanoid rigs
  (Mixamo, 3ds Max Biped, Unreal, Unity / VRM styles) and lists the body parts it did not find in both (`unmatched`):
  a starting point to check and complete. The editor's Retarget Animation window uses it.
- `bones` maps target bone -> source bone for the body (hips, spine, neck, head, shoulders, arms, hands, legs, feet,
  toes). Bones left out keep their rest pose: fingers, hair, cloth, twist and helper bones. Names come from
  `animation.bones` of an entity showing each model.
- How it works: each mapped bone takes the turn its source bone has made from the source's rest pose, in world space.
  Before that the target's rest pose is laid over the source's: every bone is turned so that it points along its
  source bone (direction = towards its only mapped bone below; hands, feet and the head take the turn of the bone
  above them), which is what takes an A-pose character to a T-pose source. A turn around the vertical aligns models
  that face different ways (`yaw` in the result). `hips` also takes the source's movement of that bone (the bob and
  sway of the body), scaled by the ratio of the two hip heights (`scale`).
- `ends` lists target bones that are not laid along their source bone but keep their rest orientation relative to the
  limb above: **feet and toes**. Rigs disagree on where the toe bone sits (under the ankle in 3ds Max Biped, at the
  ball of the foot in Mixamo) and heels tip the foot bone, so laying it along the source's tilts the shoes in every
  clip (toes up when standing, pointed down when running).
- The result: `clips` (name, duration, keys), `bones` (how many were mapped), `missing` (names of the map or clips
  that do not exist) and `attached` (the `.clips.json` it was listed in; `attach: false` skips that). Keys are linear
  at `rate` per second (default 30).
- Limits: bone lengths are the target's, so hands and feet end up where the target's proportions put them (feet may
  slide a little or hover: correct with `BoneIK` goals from a script); no finger retargeting unless mapped; clips that
  travel keep their travel on the hips (use in-place clips for characters moved by physics).

`Animator.blend` (seconds) fades a change of `clip` from the pose that was shown, while the old clip keeps playing,
instead of jumping (0.2 suits idle/walk/run). It also works when a script drives `time` itself with `playing: false`.

Verified by `AnimationRetargetAndClipFiles`.

## Playback controls

Add Animator to an entity with a glTF MeshRenderer. Its reflected fields are
clip (exact name; empty uses the default pose), speed (default 1; negative plays
backwards), loop (default true), playing (default true) and time (seconds).
Direct component edits preserve time, including when changing clips. Set time
to seek; rendering samples clamp to each channel's first/last key. Paused
Animators hold their pose and time. Missing clips or models do not advance time.

animation.play {id, clip, speed?, loop?, restart?} validates the model and clip
before editing and adds Animator if needed. It keeps existing speed/loop values
when omitted, resumes playback and restarts by default (time zero, or clip end
for negative speed). restart:false keeps the current time. A non-looping clip
clamps at its end (or beginning when reversing) and sets playing:false; a loop
wraps time into the clip duration. A zero-duration non-looping clip stops at zero.

animation.state {id, pose?} returns clip, speed, loop, playing, time, duration and
validClip. pose:true includes jointMatrices, an array of column-major 16-number
matrices in model space, including inverse bind transforms. This allows pose
checks without a GPU. Invalid/empty clips return the model's default pose.

Lua uses animation.play(id, clip, {speed=1, loop=true, restart=true}), or
self:play(clip, options) in attached scripts. The returned table is playback
state. Use scene.set(id, "Animator", {playing=false}) to pause and
scene.set(id, "Animator", {time=0.5}) to seek; self:set works as usual.

## Timeline events (A1)

Both `Animator` and `SpriteAnimation` call the attached script's
`onAnimationEvent(name, clip)` after built-in systems advance, before physics. A callback therefore sees
the new animation time/frame and can change a body or spawn an effect in that step. Scripts that do not
define the callback behave as before. The outgoing clip in an `Animator.blend` never emits events;
only the destination clip does, even while its visual weight is small.

### Authoring and queries

Legacy model markers belong to `<model file>.events.json`, beside `<model file>.clips.json`;
A3 also supports markers in the clips sidecar metadata. They refer to
the final clip names after external clips have been added; the model/clip source files remain untouched:

```json
{"clips": {"Walk": [{"time": 0, "name": "leftFoot"}, {"time": 0.35, "name": "rightFoot"}]}}
```

All instances of that model share these markers. The model cache watches sidecar creation, changes and
deletion. An invalid sidecar produces an asset error with the validation reason; it is not silently skipped.
Imported/retargeted clips use the target model's event sidecar, not the source model's markers.

Sprite markers stay inside the existing inline clip definition. `frame` is a **zero-based index into
the clip's frames array**, not a tile number in the sheet:

```json
"SpriteAnimation": {"clip": "walk", "clips": {
  "walk": {"frames": [6, 7, 8, 9], "fps": 8, "events": [
    {"frame": 0, "name": "leftFoot"}, {"frame": 2, "name": "rightFoot"}]}}}
```

- `animation.events {id, clip?}` lists sorted markers, their times in seconds and their source, plus
  `completion: "finished"`. Sprite results also contain the sequence frame indices. Omitted `clip` uses
  the component's current clip. Listing is read-only.
- `animation.set_events {id, clip?, events}` replaces the markers; `[]` clears them. Sprite changes
  use normal scene undo/save. Model changes write the shared sidecar immediately and **are not undoable**.
  Set model markers before recording a simulation; resource hashes include the sidecar.
- `animation.play` and `animation.state` support sprites as well as models. When both components exist,
  commands address the sprite. Sprite playback requires a `SpriteAnimation` with a nonempty clip and a
  `Sprite`; model playback retains the existing add-Animator behavior. `loop` overrides the sprite clip's
  setting. Reverse sprite restart starts on the last sequence frame. `restart:false` keeps time.
- Lua `animation.events(id, clip?)` reads the same report. `animation.play` and `self:play` use the same
  command for either kind. Runtime marker authoring is unnecessary; gameplay callbacks are ordinary script methods:

```lua
function Hero:onAnimationEvent(name, clip)
    if name == "rightFoot" then
        -- Play a sound, spawn a VFX asset or activate an attack hit box here.
    elseif name == "finished" then
        self:play("idle")
    end
end
```

### Ordering, completion and bounds

Markers are dispatched in entity id order, then in the order encountered during that step. All crossed
markers fire, including several loops in one step. Reverse playback traverses times in descending order;
markers at the same time retain their authored order in either direction. At a model loop seam,
forward playback emits duration markers before zero markers, reverse playback zero before duration.
The first advance includes markers exactly at the starting position, but not the other side of a seam.
Subsequent intervals exclude their starting point and include their destination. If an entity has both
playback components, events are merged by elapsed step time; ties put model events before sprite events.

`finished` is a reserved event name, sent once when a non-looping clip reaches its endpoint, after any
authored marker there. It also occurs on the first advance of a zero-duration non-looping clip. A model
stops advancing (`playing:false`); a sprite holds its frame with `finished:true`. To preserve existing
sprite timing, its non-looping endpoint is the **start of the last frame**, `(frameCount-1)/fps`;
its loop period is `frameCount/fps`. A singleton non-looping sprite thus completes on its first advance.

Pause, zero speed and `game.pause` emit nothing. Direct seeking changes the next interval's starting point
and does not synthesize events from the skipped interval. Restart rearms start/completion events; resume
does not duplicate them. A direct sprite clip change retains its existing restart-at-zero behavior;
use `animation.play` to restart reverse playback at the last frame. Direct model clip changes preserve
time, as before. Manually setting time while `playing:false` drives the pose without emitting events.

The event list is captured before callbacks run. Changing clips in a callback takes effect on the next
step; queued crossings keep their original clip/name. Destroyed entities are skipped. A callback error
faults its script through the existing Lua error/budget mechanism and prevents later calls to that instance.
Native snapshots copy playback's internal start state and Lua data, so rollback replays callbacks;
external effects need the same replay discipline as other gameplay callbacks.

Each clip accepts at most 256 markers. Names are 1..128 bytes and may not be `finished`; sprite indices
must be integers within the sequence, model times finite and within `[0,duration]`. Unknown marker fields
are rejected. A playback component crossing more than 4096 authored markers in one step returns
`animation_event_limit` before changing that component's time. Such a failed step is not transactional:
earlier scripts/components may already have advanced. Lower speed or marker count before resuming.
There is no callback truncation; completion is an additional notification. Clips without markers add no
loop-enumeration work. Old clips/scenes need no new fields.

Verified by `AnimationEventsSpritesAndReplay`, `AnimationEventsModelsAndBlend`,
`AnimationEventsBoundariesAndLimits` and `AnimationEventsCallbackMutationAndFaults`.
The `build/AnimationEvents` project displays 2D and 3D footfall
events in its HUD; run it with `build\bin\oe.exe editor build\AnimationEvents`, then Play.

## Animation panel and isolated preview (A2)

Open **View > Animation** and select a sprite with `SpriteAnimation`, or a model with animation clips.
The panel docks beside Assets and remembers its visibility in `.oe/editor.ini`; existing layouts gain
the tab without moving other windows. A model does not need an authored `Animator` to preview it.

Choose a clip, Play/Pause Preview, Step Preview (1/60 s), Restart Preview, Loop Preview or a playback
speed (negative reverses). Drag the time slider or an empty part of the timeline to seek and pause.
The Scene and stopped Game views show the sampled sprite frame/model pose, including bone attachments
and the skeleton overlay. **Apply IK is off by default.** Only the preview target's eligible BoneIK goals
(direct children with a Transform, exactly as Pose.cpp collects them) have their weight set to zero in the
copy. Other models' goals and nested goals keep their authored values. Enable Apply IK to use the stored
targets; no gameplay script moves them during preview. When eligible goals exist and IK is off, the panel
explains that gameplay scripts normally move these targets. Other entities do not advance. No script, timer, physics, audio or animation
event callback runs. The preview never changes scene fields, revision, dirty state or scene undo and is
never saved in a scene/prefab. BoneAttachment follows the resulting preview pose, including when IK is
excluded. SpringBone is not simulated in preview; it has no dynamic response to the scrubbed clip.
Closing the panel or starting the game restores authored playback.
Deleting the target or replacing the scene also closes it. Edits/undo and model hot reload refresh the
preview; a missing/invalid clip closes it safely. A collapsed/background tab can keep playing while open.

Event triangles show the A1 markers. Click a marker to select it, then drag it to move it; sprite markers
snap to sequence-frame indices, model markers use seconds. A drag preserves marker identity when it
passes another marker, and one sprite drag is one undo group. Add Event inserts `event` at the preview
cursor. The table edits names and frame/time positions with **Enter** and removes markers. These are
real authored changes, through `animation.set_events`, separate from pose preview. Inline sprite markers save
with the scene and support normal undo. Linked sprites and models write their shared animation/sidecar file immediately
and are not undoable; the panel states this next to Add Event. Invalid edits return the A1 validation hint.
Use the panel scrollbar or enlarge the bottom dock when several event rows are below the fold.

Commands work without an editor for headless authoring/verification:

- `animation.preview {id?, clip?, time?, playing?, speed?, loop?, ik?, frames?}` opens/controls one isolated
  preview. `id` is required on first opening; later calls may omit it. The default clip is the authored
  current clip if valid, otherwise the first available clip. A new target/clip resets time to zero.
  `time` clamps to its timeline; `frames` advances 0..3600 fixed 1/60 s steps **even when paused**.
  The `playing` flag tells an open editor to auto-advance at 60 Hz (real-time catch-up capped to 0.25 s
  per UI frame); standalone callers advance explicitly. `loop`/`speed` affect only preview.
  `ik` defaults to false on a new target and is retained across seeks/clip changes on the same target;
  true opts the target model into its stored BoneIK goals. Authored weights, undo and revision never change.
  Sprite non-looping endpoints retain A1's start-of-last-frame timing.
- `animation.preview_state` returns active/id/type/clip/time/playing/speed/loop, duration, available
  `clips`, sorted `events`, and sprite `fps`/`frameCount`. Preview pose sampling clamps the last sprite
  frame when seeking exactly to the loop-period endpoint. State also includes `ik` and `ikGoals` (the
  eligible goal count for the target model; zero for sprites).
  `animation.preview_close` discards the copy.
- `render.screenshot {animationPreview:true, ...}` renders that copy with its camera/attachments in
  either renderer. The default screenshot still renders the authored scene.
- `editor.animation {open?:true}` opens/focuses the panel in an attached editor; `open:false` closes
  it and preview. Without an editor it returns `editor_unavailable`; preview commands still work.
  `editor.state` includes `animationPanel` and the full `animation` preview report. Use `editor.select`
  before opening to point the panel at the desired entity.
- Model `animation.events/set_events` accept an explicit `clip` on a MeshRenderer without an Animator.
  `animation.set_events` accepts an optional sprite undo `merge` key for repeated drag edits.

```json
{"command":"animation.preview","args":{"id":"Fox","clip":"Walk","time":0.35}}
{"command":"render.screenshot","args":{"animationPreview":true,"path":"preview.png"}}
{"command":"animation.preview","args":{"frames":1}}
{"command":"animation.preview_close","args":{}}
```

Screenshot automation: `build\bin\oe.exe editor build\AnimationEvents --screenshot build\animation.png
--select Fox --animation --animation-time 0.35` (one command line). `--animation` opens the panel;
`--animation-time` sets its initial seek time. `--lang ko` / `--lang ja` select localized labels.
`--animation-clip Walk` chooses the preview clip without editing the scene; `--frame-selection` fits
the Scene camera to `--select` for close-up comparisons. These flags affect screenshots only.
The same `build/AnimationEvents` demo contains Fox and Hero for model/sprite previews. A2 does not add
controller evaluation in the isolated preview. A3/A4 provide reusable clips/frame authoring; A5 provides Lua controllers and their graph below.

Verified by `AnimationPreviewIsolationAndLifecycle`, `AnimationPreviewModelPoseAndMarkersWithoutAnimator`
and `NativeEditorAnimationPanel`; the editor case drives play/pause/step/restart, scrubbing, marker dragging,
name edits and removal through window events.

**Known stopped-scene behavior outside preview:** editing an authored `Animator.time` in the normal
Scene view still applies saved BoneIK targets. If gameplay normally updates those targets in scripts,
they remain fixed while stopped and can pin the feet. This is deliberately unchanged; use Animation
preview with Apply IK off to inspect the unmodified clip. Preview does not reproduce script-driven foot
placement or SpringBone dynamics from play.

## Shared clip assets (A3)

`SpriteAnimation.asset` names a reusable `.anim.json` file. Its clips take precedence over the
legacy inline `clips` object; leaving `asset` empty preserves existing scenes. Instances in scenes
and prefabs share the definition, while each keeps its own clip/time/speed/playing/finished state.
Texture paths are relative to the project root, not the animation file.

```json
{
  "sheet": {"texture": "assets/sprites/hero.png", "columns": 6, "rows": 2},
  "clips": {
    "walk": {
      "displayName": "Walking",
      "frames": [6, 7, 8, 9, 10, 11], "fps": 6, "loop": true,
      "events": [{"frame": 0, "name": "leftFoot"}, {"frame": 3, "name": "rightFoot"}]
    }
  }
}
```

Link it with `SpriteAnimation {asset:"animations/hero.anim.json", clip:"walk"}` plus a `Sprite`.
The asset supplies the rendered texture/grid without rewriting authored Sprite fields. Tint,
flip, pivot, material, sorting and size remain instance properties. A stopped scene samples the
linked sprite at its authored time; isolated preview still changes only its scene copy.

Instead of `sheet`, use exactly one `atlas` with a texture and pixel rectangles, indexed by clip frames:
`{"atlas":{"texture":"assets/hero.png","frames":[[0,0,24,32],[24,0,20,32]]},"clips":{"walk":{"frames":[0,1],"fps":8}}}`.
Rectangles are `[x,y,width,height]` from the image's top-left. Both renderers sample texel centers,
honor flips and use each rectangle's dimensions for pixelsPerUnit sizing. Rotated/trimmed atlas
entries, source-canvas offsets, per-frame pivots are not supported by A3; A4 adds per-frame duration below;
T1 extended atlas support remains separate work. A rectangle outside the loaded image renders an error tint.

Clip fps defaults to 8 (range 0.001..10000), loop to true, displayName to the stable clip key.
Clip keys and labels contain 1..128 bytes. Definitions allow 1..256 clips, 1..65536 sequence frames
per clip or atlas, and sheet columns/rows in 1..4096. Frames are integer indices in the sheet/atlas.
Markers retain A1's sequence-index semantics and 256-marker limit. Unknown fields and invalid types,
indices, fps or markers are rejected before writing. Missing images render the existing missing-image tint.
`animation.play {loop:...}` sets a linked sprite's per-instance `loopOverride` (-1 = asset default,
0 = once, 1 = repeat); it never rewrites the shared clip. Inline sprites retain their old loop edits.
Changing the asset or clip restarts playback; editing an existing asset keeps the instance time.

A model's existing `<model>.clips.json` source list can also carry `metadata` keyed by stable clip name:

```json
{
  "clips": ["../anims/hero_clips.gltf"],
  "metadata": {
    "Walk": {
      "displayName": "Walking",
      "loopRange": [0.1, 0.7],
      "events": [{"time": 0.1, "name": "leftFoot"}, {"time": 0.4, "name": "rightFoot"}]
    }
  }
}
```

`clips` and `metadata` are optional. Source paths remain relative to the model. Metadata applies to
embedded and external clips after matching external channels by bone name. Events use original clip
seconds. `loopRange` must satisfy `0 <= start < end <= duration` and remain nonempty as float playback
values. It changes only repeating playback: start/reverse restart use its start/end, all advances
wrap within it, and only markers in the closed range repeat. End/start seam ordering remains A1's
ordering in either direction. Decimal marker times at a range boundary use the same float seam in
playback calculations; authored/query marker times stay unchanged. Non-looping playback still traverses the full original clip and emits
`finished` once. Preview seeks can inspect the full clip; repeating advances use the configured range.
Labels appear in preview and the model clip picker; commands and callbacks still use the stable key.
`asset.info` model clips expose displayName, loopRange and events; `animation.state/preview_state`
include the label/range. The preview state also identifies a linked sprite's asset.

Legacy `<model>.events.json` remains supported. Explicit `metadata[clip].events`, including an empty
array, overrides that clip's legacy markers. Other legacy entries still apply. `animation.set_events`
writes the linked sprite asset, or a model's clips sidecar when it has a `metadata` field; models
without that field keep writing the legacy events sidecar. Source lists and unrelated metadata survive.
The panel labels shared writes as immediate and outside undo. Inline sprite events still use scene undo.

- `animation.create {path, values, overwrite?:false}` validates and writes a complete definition;
  existing files require overwrite. `.clips.json` needs its associated model and valid source files.
- `animation.get {path}` returns `{path, values}` for a valid definition.
- `animation.set {path, values, clip?}` merges fields at the root, or into one sprite clip/model
  metadata entry. Nested objects and arrays replace their previous values. A root metadata replacement
  can remove entries; replacing the entire definition uses create with overwrite.
- File writes leave scene JSON, revision and undo unchanged. They are blocked during recorded or
  network matches. The normal scene save does not duplicate shared definitions into entities.
- `asset.info` recognizes both file types as kind `animation`; Assets groups them as Animation.
  Cached `.anim.json`, model clips/events sidecars and external clip source files reload on timestamp
  change, creation or deletion. External `.gltf` binary buffers are watched too. Textures keep their
  existing independent reload. Malformed definitions cache an error and recover when repaired;
  missing/invalid preview clips close the isolated preview safely.


## Sprite clip authoring (A4)

Select a Sprite + SpriteAnimation and open View > Animation. **Sprite Frames** switches from the
preview timeline to the frame editor; **Preview Timeline** returns to playback/event editing.
The Inspector's former `SpriteAnimation.clips` JSON box is now **Edit Sprite Frames**. An empty
inline clip set opens the authoring view so a first clip can be created. Choose a unique key and
**Create Sprite Clip**; existing keys are rejected rather than replaced. Clip selection remains
preview-only; set the authored SpriteAnimation.clip or call animation.play to use it in play.

Source Frames shows sheet cells or A3 rectangle-atlas entries at their aspect ratio. Click one to
append it, or drag it onto a sequence frame to insert before that frame. Drop onto **Drop Frames
Here** to append. Drag a sequence frame onto another to move it to that final index, or select it
and use **Move Frame Left/Right**. **Remove Frame** retains at least one frame. Source/sequence
pages contain 64 entries; sequence paging selects a frame in the next/previous page. Repeated
source indices are allowed and retain their separate duration/event identity. Selecting a sequence
frame seeks the isolated Scene/Game preview; append/reorder keeps the edited frame visible. Missing images use
numbered buttons. For large sequences, use paging or the authoring API.

**Frame Seconds** commits with Enter and enables explicit per-frame timing. **Loop Clip** changes
its authored loop setting; **Loop Preview** remains the isolated playback switch. FPS is the fallback
rate; **Use FPS Timing** removes explicit durations. Structural operations move each duration and
event with its sequence frame. Inserting shifts event indices; removing drops markers on the removed
frame and shifts those after it. A direct replacement of frames through values must also supply
appropriate durations/events; the engine cannot infer identity from repeated source indices.

```json
"walk": {
  "frames": [0, 1, 2], "fps": 8, "durations": [0.1, 0.2, 0.3], "loop": false,
  "events": [{"frame": 1, "name": "foot"}]
}
```

`durations` is optional, in seconds, and has exactly one numeric value per sequence frame. Values
are finite and in 0.0001..3600, with total duration at most 3600 seconds. When present it overrides
fps for playback, event positions, preview and both renderers. Frame starts are cumulative (0, 0.1,
0.3 above); the non-looping last frame holds for its duration, then finished fires at the total (0.6).
Without durations, legacy fps clips retain their start-of-last-frame non-looping endpoint. This
preserves existing scenes. Forward/reverse crossings, multi-loop notifications and snapshot replay
retain A1 contracts. Markers still store sequence indices, not source-sheet frame numbers.
Preview reports variableTiming, frameTimes and effective frameDurations; scrubbing and marker
add/drag use those positions. Springs and script-driven IK retain the A2 preview limits.

- `animation.get_clip {id, clip?, offset?:0, limit?:64, sourceOnly?:false}` returns clip values,
  asset, image metadata, texture dimensions, source frameCount and a bounded page of `{index, rect}`
  pixel rectangles. limit is 1..128; sourceOnly omits clip values for lightweight atlas requests.
  A missing inline key returns null values so a new clip can be authored.
- `animation.set_clip {id, clip, values?, operation?, index?, to?, frame?, duration?, merge?}` creates
  or patches named clips. values merges clip fields. Operations: create (requires a new key), append,
  insert, move (to is the final index), remove, duration (edits one frame) and uniform (removes durations).
  Failed validation leaves scene/files unchanged. Inline edits use normal undo/save and merge groups;
  linked clips update their shared .anim.json immediately without scene revision/undo. Other shared
  clips and sheet/atlas data remain intact. Recording/network-match guards also cover these edits.
- `editor.animation {frames:true}` opens the frame view; false returns to preview. editor.state
  includes animationFrames. Screenshot automation adds `--animation-frames` to `--animation`.

The editor consumes A3 sheet/rectangle-atlas data. T1 extended atlases and T2 Aseprite import remain
separate roadmap work; rotated/trimmed entries, source-canvas offsets and per-frame pivots are still
unsupported. T2 can write the same durations arrays when it is implemented.


## Animation controllers (A5)

A controller is a project-relative `.controller.json` asset. `require("oe.animator")`
loads the validated asset and evaluates its state machine in Lua; it calls `animation.play`
only on entry or transition. The engine supplies asset validation/loading and playback, not
state-machine evaluation. SpriteAnimation and MeshRenderer use the same format/module.

`animation.controller_create {path, values, overwrite?}` creates a file;
`animation.controller_get {path, id?}` reads it;
`animation.controller_check {path, id?}` validates it; optional id verifies **every** state clip
without adding Animator or changing playback. `animation.controller_set {path, values}` replaces
the complete definition after validation. Writes are immediate shared file edits without scene
revision/undo and are rejected during recording or active matches. Invalid writes preserve the file.
`asset.info` reports kind `controller` and the validated values.

```json
{
  "entry": "Idle",
  "parameters": {
    "speed": {"type": "number", "default": 0},
    "grounded": {"type": "boolean", "default": true},
    "jump": {"type": "trigger", "default": false}
  },
  "states": {
    "Idle": {"clip": "idle", "position": [40, 80]},
    "Walk": {"clip": "walk", "speed": 1, "loop": true, "position": [320, 80]},
    "Jump": {"clip": "jump", "loop": false, "position": [320, 280]}
  },
  "transitions": [
    {"from": "*", "to": "Jump", "blend": 0.15,
     "conditions": [{"parameter": "jump", "op": "trigger"},
                    {"parameter": "grounded", "op": "==", "value": true}]},
    {"from": "Idle", "to": "Walk", "blend": 0.2,
     "conditions": [{"parameter": "speed", "op": ">", "value": 0.1}]},
    {"from": "Walk", "to": "Idle",
     "conditions": [{"parameter": "speed", "op": "<=", "value": 0.1}]},
    {"from": "Jump", "to": "Idle", "minTime": 0.5,
     "conditions": [{"parameter": "grounded", "op": "==", "value": true}]}
  ]
}
```

```lua
local Animator = require("oe.animator")
local Player = {}
function Player:onStart()
    self.controller = Animator.new(self.id, "animations/player.controller.json")
end
function Player:onUpdate(dt)
    self.controller:set("speed", math.abs(input.axis("moveX")))
    self.controller:set("grounded", true) -- replace with the project's ground query
    if input.pressed("jump") then self.controller:trigger("jump") end
    self.controller:update(dt)
end
return Player
```

The module provides `new(id, path)`, `controller:set(name, value)`, `get(name)`,
`trigger(name)`, `update(dt)`, and `state()`. State returns a copied parameter table,
current state, elapsed seconds, id and path. Parameters are typed: finite numbers in
[-1e9,1e9], booleans, and false-default triggers. Set/get reject unknown names; set rejects
triggers (use trigger). Construction verifies all clips before starting the entry clip.
Each owner has independent state; store it on the script instance for native rollback.

Transitions run in authored array order, including wildcard origins, with AND conditions.
The first match wins; at most one transition per update. Self transitions are skipped.
Numeric operators: ==, !=, >, >=, <, <=; booleans: ==/!=; triggers: op trigger without value.
Triggers stay pending until consumed by the winning transition; losing/blocked transitions
consume nothing. minTime is elapsed state time in seconds, including this update's dt;
it defaults to zero. Entry and every transition reset this clock and restart the destination
clip. An unchanged state does not restart playback. Failed playback leaves state/trigger/clock
unchanged. dt must be finite, nonnegative and <=3600; the accumulated clock saturates at 3600.
Calling update once in onUpdate keeps it on fixed simulation time; calls elsewhere are the
project's responsibility. Missing/deleted entities or clips produce Lua errors.

A state speed defaults to 1 (range -10000..10000), loop to true. Each transition blend defaults
to zero (0..5 seconds); `animation.play {blend}` forwards it to Animator.blend for a model.
Sprites change immediately, without an image crossfade. Models use the existing outgoing-clip
pose blend and emit events only from the incoming clip. No blend trees, completion/exit-time
conditions or automatic event routing are added; scripts may translate onAnimationEvent into
a trigger. minTime is wall time in fixed updates, not normalized clip progress.

Definitions allow 1..64 states, <=64 parameters, <=256 transitions, 1..16 conditions each and
at most 1 MiB JSON. Names/clip keys have 1..128 bytes; * is reserved as an origin wildcard,
not a state name. Unknown fields/types/operators, missing references, invalid defaults and
nonfinite/out-of-range values are rejected. State position is optional [x,y] in canvas units
(each within +/-100000); it affects authoring layout only. Loaded controllers retain their
validated definition for the owner lifetime. Restart/recreate owners to adopt file changes;
there is no automatic hot reload of running controllers. Use one controller per entity; external
animation.play calls can make playback diverge from the controller state.

In View > Animation, choose **Animation Controller**, enter a path and **Create Controller**,
or double-click an existing controller in Assets. `editor.animation {controller:path}` opens it;
empty controller returns to the timeline. The shared Effects/Shader EditorNodeCanvas supplies
state dragging, wheel zoom, middle/empty-canvas pan and Fit Controller Graph. The inspector
creates/renames/removes states, changes clips/speed/loop/entry, creates typed parameters,
changes defaults, and adds/removes/reorders transitions and AND conditions. Removing a state
removes its incident transitions and chooses another entry if needed; removing a referenced
parameter fails validation until its conditions are removed. File writes use the same commands.
Preview State shows just the selected state's clip in the isolated preview; it does not evaluate
the controller, scripts, physics or springs. Select the target sprite/model first.
`editor.state.animationController` reports the path and `animationControllerState` the selected
node, not live gameplay state. Screenshot example:

```bat
build\bin\oe.exe editor build\AnimationController --screenshot build\controller.png --animation-controller animations/hero.controller.json --select Hero --width 1800 --height 1400 --frames 5
```

## Status

A1 through A5 are implemented. All 385 Windows tests pass. The four A5 tests cover validated
file edits without scene/revision/history/hash changes, malformed definitions and recording guards,
typed parameters, ordered/wildcard/AND transitions, pending triggers, failure atomicity, model
crossfades, shared sprite/model native rollback and changed-resource rejection. Native window
input verifies graph selection, preview, state/transition creation, parameter text entry/addition,
state dragging and switching back to the timeline. English/Korean/Japanese graph screenshots
were inspected; the model/sheet/atlas demo visibly changes Idle/Walk/Run poses and frames.
Its software/D3D11 mean RGB difference is 0.077/255; Showcase retains 72ab07fb213fd42b.
Web and both Android players were rebuilt. Headless Edge WebGL2 (SwiftShader) ran the packaged
controller demo through Walk, Run, Jump and Idle for all three entities without script/WebGL
errors. Both Android ABIs retain 16 KB LOAD alignment. Windows snapshot budget: 6.25 ms worst
in the full suite (16.67 ms limit).

Physical Android, Linux/EGL and real browser/mobile GPUs remain unverified. Sprite crossfades,
blend trees, automatic controller hot reload and controller evaluation inside isolated previews
remain outside A5; ordinary stopped Scene IK and preview SpringBone limits are described above.
