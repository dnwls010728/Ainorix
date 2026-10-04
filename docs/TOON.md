# WuwaToon shader test project

## Implementation status

- [x] Author bounded toon surface graphs and material presets from public technical references.
- [x] Generate original character/test geometry and a comparison scene.
- [x] Add repeatable controls for material variants, outlines and turntable motion.
- [x] Verify graph math, controls, deterministic rendering and packaging through commands.
- [x] Inspect software/D3D11/editor and packaged WebGL2 images; record limitations.

This is an OwnEngine shader experiment inspired by public Wuthering Waves
community rendering references, not an official Kuro Games shader or asset pack.
Reference provenance, implemented approximations and missing game-specific
features are documented below and in the sample README.

## Verification (2026-10-03)

- Windows Release build: no new compiler warnings; 137 tests, zero failed checks.
- Node/WASM: 125 tests, zero failed checks (native/GPU/socket checks skip where
  unsupported). Both new sample tests pass on Windows and Wasm.
- `WuwaToonGraphBandsRimAndSpecular` checks independent numeric values below and
  above diffuse, rim and specular thresholds, with preserved alpha and limits.
- `WuwaToonSampleControlsAndPackaging` verifies keyboard/mouse/touch actions,
  held mouse and primary-touch compatibility without duplicate toggles, real
  surface-pixel changes apart from UI labels, deterministic reset, no Lua errors
  and package extraction with the exact same initial frame hash.
- CLI shader/script validation succeeds with zero script diagnostics.
  Software/D3D11 1280x720 images and a D3D11 native editor screenshot were inspected.
  Mean absolute RGB channel difference is 0.1772 on a 0..255 scale (including UI
  and the scene). Matching simulation frame 1: software `c756da397697d46c`,
  D3D11 `ffd9a2afde50a539`.
- A packaged WebGL2 player visibly switched Toon/PBR/Normals and outlines,
  with the original geometry/materials loaded from game.pak.
- Regenerating all 37 generated assets preserves every file's SHA-256 hash.
  Project content uses existing graph operations; no engine code or API schema
  changes and no player-runtime rebuild is needed for this asset-only sample.
- [ ] Android device and full Linux/EGL execution; unavailable here. Android's
  committed player lacks graph support and must first be rebuilt as tracked in
  POSTPROCESS.md. Physical mobile touch is not verified; injected compatibility
  inputs are covered by the tests.

## Using the project

Run `oe editor samples/WuwaToon`, press Play and focus the Game view, or run
`oe run samples/WuwaToon`. Keys 1/2/3 select Toon/PBR/Normals; O toggles outlines,
P starts/pauses the turntable and R restores the initial pose. Mouse/touch buttons
perform the same actions. Package it with `oe package samples/WuwaToon --web`.

The committed project has original procedural character GLBs, inverted normal
hulls for silhouettes, nine part-specific material pairs, a 28-node/eight-uniform
toon graph and a normal-display graph. Three reference spheres keep their fixed
materials while character modes change. `tools/make_lab.py` regenerates assets
with Python's standard library; Python is not needed to open or package the sample.

The graph implements a hard diffuse threshold, multiplicative shadow tint,
view-angle rim and quantized highlight. The studio direction and camera position
are explicit uniforms; this is not automatic scene-light integration. The
standard ground receives geometric cast shadows, while toon materials use unlit
surface color and do not receive shadow-map darkening. Camera or light edits need
matching uniforms. The highlight half-vector is a fixed studio approximation.

Face SDF masks, sampled ramps/control maps, depth-buffer rims, anisotropic hair,
stencil hair/face layering and original-game tone mapping are outside this graph
experiment. Materials can be edited/hot-reloaded through existing material APIs.

## Toon graph in a game (NetChase)

`samples/NetChase` uses the same technique on a moving third-person camera. Its
`materials/toon.shader.json` (29 nodes) declares the reserved `cameraPosition` and
`lightDirection` uniforms (see POSTPROCESS.md), so the rim and the highlight half-vector
are computed from the real view and scene light instead of fixed studio values. The
player prefab is the mannequin scaled to 1.8 m with inverted-hull outlines; the coat is
the root mesh and takes the replicated player colour as its tint. Pillars and the
crystal share `materials/prop-toon.mat.json`. `tools/make_art.py` regenerates the models,
graph, materials and prefab. Covered by `ShaderGraphSceneUniforms` and
`NetChaseThirdPersonSample`; software and D3D11 screenshots were inspected. The limits
above still apply (no cast-shadow darkening on toon surfaces, no face SDF or ramps).

## Reference provenance

Reviewed the public community
[HoyoToon Wuthering Waves common shader](https://github.com/Hoyotoon/HoyoToon/blob/d9e5ca2f312bf16fba89dee67d32c08b482dcda4/Shaders/Wuthering%20Waves/Include/HoyoToonWutheringWaves-common.hlsl)
at revision `d9e5ca2f312bf16fba89dee67d32c08b482dcda4`, 2026-10-03.
It organizes shadow/ramp, specular and rim effects separately; its depth rim and
face-mask paths exceed the current graph inputs. The OwnEngine graph independently
uses elementary dot, step, mix and vector arithmetic to study the broad effects.
No third-party shader code or original-game models/textures are included.
See its [license](https://github.com/Hoyotoon/HoyoToon/blob/d9e5ca2f312bf16fba89dee67d32c08b482dcda4/LICENSE)
and the sample [README](../samples/WuwaToon/README.md) for attribution and limits.
