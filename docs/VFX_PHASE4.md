# VFX phase 4: implementation and verification

Materials, bounded death emitters, static mesh particles, two-anchor ribbons and opt-in batching now
extend the shared VFX document and deterministic simulation. The full authoring contract is in
[VFX.md](VFX.md#materials-mesh-particles-ribbons-and-death-chains-phase-4). Structure node editing is
phase 4b and is not included here; its canvas extraction/layout sidecar preparation remains pending.

To inspect the sample, open `samples/VFXLab` and load `scenes/slash.scene.json`, or open
`effects/sword_slash.vfx.json` in Effects. The existing Shaders workspace edits
`materials/slash.shader.json`: lifetime-driven UV scroll, a moving dissolve threshold and stable random
phase. The effect uses a mesh crescent, blade sweep and sparks. The sample's Lua script repeats the
sweep and owns its movement; the engine does not add sword gameplay behavior.

## Verification

- Windows Release: warning-free build; 280 tests, zero failures. Five phase-4 tests cover material
  inputs and batching, two-anchor geometry, death-chain depth/budget/native restores, the sword preset
  and real sample material/Lua/preview behavior. `EditorTranslations` and existing Effects tests pass.
- `ParticleRenderers` legacy hashes stay 2D `eeb1529ca41029c9`, 3D `4ea73d9e1b71d479`.
- D3D11/software mean RGB channel differences (0..255): material age/random/speed test 0.731;
  sword material/ribbon/mesh contact sheet 0.130. Ribbon-only comparison stays below 1.5.
  Software batching agrees across thread counts; unbatched material particles share the static quad,
  including the 1000-particle resource regression.
- A single 1000-quad measurement: 1000 versus 1 draw, gathering 0.288 versus 1.109 ms
  (plain versus batch). These are local CPU samples, not a cross-device performance claim. CPU mesh
  generation costs more; batching trades that work for fewer GPU submissions. Transparent emitters
  sort as groups when batching is enabled.
- Real CLI shader/check/preview/analyze workflows and sample Lua stepping passed. Four 256px-frame
  software/D3D11 previews were inspected: `e6ad38f20f4af9fa` / `456466786934b581`.
  A Korean 1400x950 Effects screenshot and the running sample's D3D11 scene image were inspected.
- Web and Android prebuilt players rebuilt with Emscripten 6.0.10 and Android NDK r28c; both Android
  ABIs (`arm64-v8a`, `x86_64`) are refreshed. The portable core also runs under Node/WASM:
  230 tests, zero failures. Artifact sizes and hashes are recorded in the runtime README files.
- Generated GPU shaders include HLSL5, GLSL ES 3.0 and GLSL430. `sokol-shdc` regeneration succeeded;
  only D3D11 shader execution was tested. Browser WebGL2, Android devices and Linux/EGL remain
  unverified. No particle refraction/distortion or soft-particle depth fade is implemented.

## Implementation status (work log)

- [x] Particle/trail material binding and dedicated age/random/speed inputs; no graph uniform slots used.
- [x] Opt-in emitter mesh batching with RGBA/data streams; shared meshes when batching is disabled.
- [x] Asset-local death targets, deterministic end-of-step dispatch, depth 4 and shared capacity limits.
- [x] Two-anchor ribbon sampling (including tip-only movement), finite trail duration and anchor hints.
- [x] Static mesh particles with captured world orientation, mesh budgets and crescent built-in.
- [x] Sword slash preset, scrolling/dissolving sample material, scene/Lua and asset regeneration tool.
- [x] Windows/D3D11 and Node/WASM tests, visual checks, API/reference docs and runtime rebuilds.
- [ ] Phase 4b: shared node canvas, `.oe/` layout sidecar decision and Effects structure nodes/wires.
- [ ] Browser WebGL2, Android device and Linux/EGL execution of these effects.
- [ ] Optional later: scene-color distortion/refraction and depth-faded soft particles.
