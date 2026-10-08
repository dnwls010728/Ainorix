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
