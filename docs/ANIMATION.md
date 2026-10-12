# glTF skeletal animation

Animation poses use fixed simulation time and the same model-space joint palette
in the software and GPU renderers. Models remain immutable cached assets; each
entity owns its playback state through a reflected Animator component.

## Status

Skinned and rigid glTF animation plays in both renderers, including GPU shadow and selection passes; the
web and Android players include it.

- [ ] Android device and Linux/EGL execution of animated models (not available so far).

## Model contract

Each model retains its node hierarchy and default local TRS (or matrix). Skin
joints reference nodes plus inverse bind matrices; four influences are normalized
per vertex. Static vertices retain their baked node transforms; animated rigid
nodes use the same palette with one influence. The palette limit is 64 entries
per model, including rigid animated nodes; models beyond it report a load error.
Zero-weight vertices remain untransformed. FBX models (`.fbx`) fill the same structures: their
curves are resampled into LINEAR keys at load time (see RENDERING.md, FBX models). Morph-target animations and additional
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
GPU vertex uniforms contain at most 64 matrices, including rigid node bindings.

Shaders were regenerated using the official sokol-tools-bin Windows compiler
(SHA256 BD616287F9EA689D53C6D260E443EE733E61AE1B73A9B37ADC482EAD0364D561)
through tools/shaders/compile_shaders.bat. The compiler is an ignored build tool;
normal builds use the committed generated header without downloading it.

## Bones, attachments, IK and springs

Models keep the names of their nodes (glTF node names, FBX model names). `animation.bones {id}` lists them for an
entity's model in its current pose: `bones: [{name, parent, position, rotation, modelPosition}]` (world space, Euler
degrees; `modelPosition` is the `Transform.position` a child entity needs to stand on the bone, e.g. a new goal), plus
how the entity's `BoneIK`, `BoneAttachment` and `SpringBone` children resolve (`goals: [{id, name, bone, found, distance}]`,
`attachments: [{id, name, bone, found}]`, `springs: [{id, name, bone, found, bones, simulating}]`). `bone: "Hand_R"` returns one node. Lua: `animation.bone(id, name, {ik = false}?)`
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

**BoneIK** `{bone, chain, weight, pole, soften, matchRotation}` is a goal: the bones above `bone` turn so that it reaches the
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

Bone lengths never change: a goal out of reach leaves the limb stretched towards it and `goals[].distance` reports
the shortfall. Goals of one model run in entity id order, each on the result of the one before. Check a setup with
`animation.bones` (distance 0 = reached) and `render.screenshot`.

```json
{"command": "entity.create", "args": {"name": "LeftFootGoal", "parent": "Hero", "components": {
  "Transform": {"position": [0.15, 0.2, 0.1]},
  "BoneIK": {"bone": "Foot_L", "chain": 2, "pole": [0, 0, 1]}}}}
```

**SpringBone** `{bone, stiffness, damping, gravity, weight}` gives a branch of the skeleton secondary motion: a tail, ears,
hair strands, a cape strip, an antenna. `bone` and every bone below it lag behind when the character starts, stops,
turns or lands, swing, and settle back into the animated pose. One component per branch (a tail and two ears are three
child entities); a bone with several bones below it swings by the first, the others ride along and swing below.

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
- There is no collision: a swinging bone can pass through the body or the ground. `BoneAttachment` children on a
  swinging bone follow it. Check with `animation.bones` (`springs[].bones` = how many bones swing, 0 for a bone with
  nothing below it; `simulating` once a step has run) and `render.screenshot` after `sim.step`.

```json
{"command": "entity.create", "args": {"name": "TailSpring", "parent": "Fox", "components": {
  "SpringBone": {"bone": "b_Tail01_012", "stiffness": 600, "damping": 24}}}}
```

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
