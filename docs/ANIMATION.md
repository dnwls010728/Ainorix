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
- [ ] P2.2: Animator reflection/API/Lua playback; deterministic fixed-step clock,
      quaternion/TRS pose evaluation and LINEAR/STEP/CUBICSPLINE interpolation.
- [ ] P2.3: Shared RenderScene palette; software vertex skinning and GPU vertex
      shader skinning for scene, shadow and selection passes; regenerated shaders.
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

Playback and skin deformation are not connected yet (P2.2/P2.3). This loader
milestone preserves the existing rendered appearance. Web/Android runtime
rebuilds and platform verification remain part of P2.4.
