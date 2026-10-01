# glTF skeletal animation

Animation poses use fixed simulation time and the same model-space joint palette
in the software and GPU renderers. Models remain immutable cached assets; each
entity owns its playback state through a reflected Animator component.

## Implementation status (work log)

P1 is implemented in PR #15, pending review. P2 starts independently from the
latest main so the features remain separate PRs; it does not depend on save data.

- [x] P2.1: Validated glTF node hierarchy, joints/inverse bind matrices,
      JOINTS_0/WEIGHTS_0 and named TRS animation channels; asset.info clip metadata;
      compact synthetic loader tests plus the existing Fox asset.
      Windows Release build and all 63 tests pass, including malformed joints,
      times, weights, duplicate channels, palette limits, explicit inverse binds,
      normalized integer rotations/weights and rigid animated child nodes.
- [x] P2.2: Animator reflection/API/Lua playback; deterministic fixed-step clock,
      quaternion/TRS pose evaluation and LINEAR/STEP/CUBICSPLINE interpolation.
      animation.play/state and Lua animation.play/self:play share playback controls.
      Windows Release build and all 65 tests pass, covering known interpolation
      values, inverse binds, parent ordering, serialization/undo, seeking, pause,
      endpoints, reverse loops, Lua errors and identical poses after replay.
- [x] P2.3: Shared RenderScene palette; software vertex skinning and GPU vertex
      shader skinning for scene, shadow and selection passes; regenerated shaders.
      Windows/D3D11 build and all 66 tests pass. AnimatedSkinRenderers compares
      Survey/Walk/Run in smooth and flat shading, checks changed pose hashes,
      thread determinism, GPU cache stability and selection outlines. Walking
      software/GPU screenshots inspected; poses and shadows agree. GLSL ES 3.0
      and desktop GLSL generated alongside HLSL5; web/Android execution remains P2.4.
- [ ] P2.4: Fox Survey/Walk/Run demo, image/hash and GPU comparison tests, docs and
      API refresh, web/Android prebuilt runtimes and available platform checks.

## Model contract

Each model retains its node hierarchy and default local TRS (or matrix). Skin
joints reference nodes plus inverse bind matrices; four influences are normalized
per vertex. Static vertices retain their baked node transforms; animated rigid
nodes use the same palette with one influence. The palette limit is 64 entries
per model, including rigid animated nodes; models beyond it report a load error.
Zero-weight vertices remain untransformed. Morph-target animations and additional
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
selection passes. Web/Android runtime rebuilds and platform verification remain
part of P2.4.

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
