# Camera post-processing and shader materials

## Implementation status (work log)

### Shader graph vertex offset and normal outputs (2026-10-04)

- [x] Add the optional graph outputs `offset` (vertex displacement added to the
      world position, evaluated per vertex) and `normal` (per-fragment world-space
      lighting normal) in both renderers; the node limit rises from 32 to 48.
      Displaced surfaces also move in the shadow map, the depth-of-field depth
      pass and the selection outline. ShaderGraphVertexOffset covers compile
      rules, the reference offset and software/D3D11 parity.
- [x] Add the built-in mesh `plane64` (64x64 cells) and `samples/Water`, a rolling
      sea driven by a 47-node graph. WaterSamplePlays passes; Windows passes 182
      tests.
- [ ] Rebuild the prebuilt web runtime and both Android players. Shipped players
      ignore `offset`/`normal` and reject graphs with more than 32 nodes.
- [ ] Verify the vertex-stage evaluator on WebGL2, GLES3 and Linux/EGL; only
      D3D11 and the software renderer are checked.

### Depth of field and HD-2D sprites (2026-10-04)

- [x] Add `Sprite.billboard` (`none`/`upright`/`camera`) and `Sprite.castShadows`;
      alpha-test every `mask` surface with a base texture or shader graph in the
      shadow pass of both renderers. SpriteBillboards and SpriteCutoutShadows pass.
      Later `Sprite.billboard` was removed from the engine: billboards moved to the
      project script `samples/HD2D/scripts/billboards.lua` (tests:
      `SpriteBillboardScript`, `HD2DSamplePlays`, `RemovedComponentsStillLoad`).
- [x] Add PostProcess depth of field (`dofRadius`, `dofFocus`, `dofRange`,
      `dofFalloff`) in both renderers: two separable gather passes before bloom,
      software depth buffer or a GPU R32F depth pass. CameraDepthOfField covers
      a focused cube untouched and a far cube soft, focus swap, worker-thread
      determinism, unchanged depth/IDs, unaffected UI, serialization and
      undo/redo and an orthographic camera. Software/D3D11 mean channel
      difference is 0.62 of 255 with RGBA8 and 0.34 with Reinhard HDR.
      Windows passes 179 tests.
- [x] Add `samples/HD2D` ("Lantern Road") using billboards, sprite shadows and
      depth of field; key 1 toggles all camera effects, key 2 only depth of field.
- [ ] Rebuild the prebuilt web runtime (`build_web.bat`, Emscripten SDK) and both
      Android players (`build_android.bat`, NDK). Shipped players do not yet have
      sprite shadows or depth of field.
- [ ] Verify execution of the new `scene_depth` and `dof` shaders on WebGL2,
      GLES3 and Linux/EGL; only D3D11 and the software renderer are checked.

### P6 completion pass (2026-10-03)

- [x] Verify varying-alpha shadows against independent half-width geometry;
      verify standard/graph material occlusion in both selection passes.
      ShaderMaterialVaryingAlphaAndOcclusion passes on software/D3D11;
      reference/GPU shadow mean channel difference is 0.0109 of 255.
- [x] Add ShaderLab with animated UV bands, HDR emission, procedural alpha and
      ground shadows. ShaderLabSampleControlsAndPackaging verifies fixed time,
      presets, Lua errors and package extraction/render identity. Windows passes
      137 tests; Node/WASM passes 125. CPU/D3D11 CLI images and packaged WebGL2 neutral/bloom images
      were inspected; on-screen preset buttons also work without a keyboard.
- [x] Recheck the neutral white capture discrepancy: a packaged white-cube
      fixture shows the emitter in both the browser screenshot and exported
      canvas PNG. The diagnostic page requests preserveDrawingBuffer for export;
      without it, an asynchronous toDataURL can capture a cleared drawing buffer.
      This does not establish the cause of the old screenshot, but no rendering
      failure reproduces in the current player. No engine workaround was added.
- [x] Confirm the committed web runtime already includes graphs from networking
      M8; source/asset-only sample edits do not require another player build.
- [ ] Rebuild both Android ABI players. No NDK or Android SDK is present on this
      host; run build_android.bat with NDK r28c or newer before shipping graphs
      or the integrated networking APIs on Android.
- [ ] Android hardware and full Linux/EGL effect execution remain unavailable.
      Run ShaderLab neutral/bloom on those targets and compare masked shadows.

The portable P6 implementation and available desktop/browser verification are
finished. P6 remains unchecked in ROADMAP until Android player refresh and the
platform checks above are completed. P7 can proceed independently: it changes
editor tooling and does not depend on those hardware checks.

P6 started from integrated main. At the user's request, the completed P3-P5
branches and P6 vignette/HDR milestones are combined through PRs #18-#21.
The remaining P6 milestones continue on a new branch from the updated main;
merging the completed work does not mark the full P6 feature complete.
Combined verification passes 81 Windows tests and 77 Node/WASM tests. The web
focus-reset merge preserves both touch and gamepad cleanup. Player runtimes
are rebuilt from combined source 6d76ac9; device follow-ups remain open.
The combined WebGL2 player also rechecks two simultaneous touch controls,
secondary move/release with primary mouse held, and clearing all fingers/keys
on window blur. This verifies the merged touch/gamepad focus-reset code.

- [x] P6.1: Reflected PostProcess camera component and optional vignette in both
      renderers. Preserve default frame hashes, picking/depth, UI and selection
      overlays. Test API/serialization/undo and compare software/D3D11 output;
      regenerate shaders/API and refresh player runtimes.
      Windows Release passes 73 tests and Node/WASM passes 69. CameraPostProcessing
      covers neutral-frame identity, changed edge color, unaffected center/opaque
      UI/depth/IDs/selection outline, worker-thread determinism, serialization,
      undo/redo and resetting effects when switching cameras. Software/D3D11
      mean channel difference is 0.0122 of 255 on the comparison scene.
      Real Showcase CLI screenshots were inspected: off f41d641b55beb483,
      software vignette cac0b279d7c910b7, D3D11 c516b61d9ed5c27e.
      Shaders regenerated with sokol-shdc; web and both Android ABI players
      rebuilt. WebGL2 vignette is additionally verified in the P6.2b browser
      fixture; Android GPU execution remains pending.
- [x] P6.2: Exposure/tone mapping and bloom, including required color-buffer
      precision, documented color-space behavior and available backend checks.
      - [x] P6.2a: Preserve HDR lighting and emissive values through scene blending;
            add exposure and optional Reinhard tone mapping in both renderers.
            Windows passes 74 tests and Node/WASM passes 70. HdrCameraToneMapping
            checks emissive values above 1, alpha blending before mapping,
            neutral/HDR target transitions, GPU interior colors, UI separation,
            worker determinism, API serialization and undo/redo. CLI screenshots
            inspected: off f729ad6fed6de3b7, software 28784ac8a8dd0ddc,
            D3D11 959887780e35a43c. Shader/API regeneration completed; web and
            both Android ABI players rebuilt from 4650059. Browser WebGL2
            execution confirms Reinhard color, switching back to neutral HDR-off
            output, black scene at zero exposure with a readable white HUD,
            and restoring Reinhard after zero exposure. Android device and
            Linux/EGL execution remain unchecked.
      - [x] P6.2b: Extract and blur highlights before tone mapping for optional bloom.
            Use a bounded separable tent filter in scene pixels with identical
            CPU/GPU extraction and edge clamping. Default strength zero preserves
            existing hashes. Check bright/dim thresholds, hue, HUD/outline,
            worker determinism, resize/toggle, D3D11 and packaged WebGL2 output;
            regenerate shaders/API and refresh web/Android players.
            Windows passes 82 tests; Node/WASM passes 78. CameraBloom covers
            visible colored halos, below-threshold rejection, opaque UI/outline
            separation, unchanged depth/IDs, worker determinism, serialization,
            undo/redo, zero exposure, constant edge energy and GPU resize/toggle.
            D3D11 mean channel difference stays below 0.60 of 255 for radii
            1/8/32 at 128x72 and 97x55. CLI PNGs inspected: off b5ae0f043da78cdc,
            software 789f48d2d9240334, D3D11 7d9d678c81fcfdac. Shaders/API
            regenerated. Web and both Android ABI players rebuilt from 6c34dc6.
            The packaged WebGL2 fixture shows bloom on/off, correct hue and
            unchanged HUD, recreates targets when canvas size changes from
            640x360 to 320x180 and back, and switches to vignette/neutral modes.
            This also closes the WebGL2 vignette check from P6.1. Android device
            and full Linux/EGL execution remain unchecked.
- [x] P6.3: Optional FXAA and sample controls/demonstration; verify off/on output,
      resolution changes and separation from UI/selection overlays.
      - [x] P6.3a: CPU/GPU directional FXAA on post-processed RGBA8 scene color,
            neutral-frame identity, diagonal edges, flat regions, UI/outline,
            worker determinism and resize/toggle checks. Refresh shaders/API.
            Windows passes 83 tests; Node/WASM passes 79. CameraFxaa covers API/serialization/undo,
            diagonal smoothing, flat-color identity, unchanged depth/IDs and
            opaque UI/selection outlines, worker determinism and GPU target
            resize/toggle. D3D11 mean channel difference is below 0.32 of 255.
            Real CLI images inspected: off 457ffd44e88c5760, software
            c86ccc033505d8dc, D3D11 c21a168cde9909e4. Shaders/API regenerated.
            Web and both Android ABI players rebuilt from 0ea84f2.
            Packaged WebGL2 verification and sample controls remain in P6.3b.
      - [x] P6.3b: Sample effect controls, real CLI/WebGL2 verification and
            refreshed player runtimes.
            Showcase camera presets: 1 Off, 2 Tone, 3 Bloom, 4 Vignette,
            5 FXAA, 6 All. Complete settings are applied through Lua scene
            commands, preserving neutral startup and restoring Off after All.
            Windows passes 84 tests; Node/WASM passes 80. The sample script
            passes script.check with zero errors/warnings. CLI PNGs inspected:
            Off 07a6a4d1eba5c930, All 4d4ae159d978eb9b. Packaged WebGL2
            execution verifies FXAA/All/Off, sharp HUD, 640x360 to 320x180 and
            back, and matching live canvas/exported frame appearance. Player
            binaries were refreshed from 0ea84f2; sample-only changes do not
            require another engine rebuild. Device follow-ups remain open.
- [x] P6.4: Custom shader materials through commands/assets, validation and
      portable backend shader generation. Document the software reference
      behavior and demonstrate a material in a sample.
      - [x] P6.4a: Validate a project-authored surface shader graph, compile
            bounded vector instructions and evaluate a CPU reference. Expose
            graph creation/checking through commands with precise diagnostics.
            shader.create/shader.check and asset.list kind shader are available.
            Windows passes 85 tests; Node/WASM passes 81. Real CLI create/check
            and asset.list commands succeed. ShaderGraphValidationAndReference covers
            procedural UV stripes, uniform overrides, emissive output,
            invalid references/cycles/types/outputs/swizzles, path containment,
            overwrite rejection, invalid-write preservation, division by zero
            finite saturation, texture/swizzle, fixed-time sine and instruction limits.
            Material/GPU integration remains P6.4b. Player refresh is deferred
            to P6.4c; shipped players still use source 0ea84f2.
      - [x] P6.4b: Load graph assets from materials, apply per-material uniforms,
            integrate fragment evaluation in both renderers, generate portable
            backend shaders with sokol-shdc and preserve default materials.
            - [x] Main fragment pass: cached graph assets, strict material
                  shader/shaderUniforms fields, dependency hot reload, CPU/GPU
                  vector instruction execution and fixed simulation time in
                  CLI/API/player/editor views. Shaders regenerated.
                  Windows passes 86 tests; Node/WASM passes 82.
                  ShaderMaterialRendering verifies red/blue UV stripes,
                  worker determinism, unchanged opaque depth/IDs, uniform
                  edits, rejection without overwrite, graph hot reload,
                  asset.info and exact standard-material restoration.
                  Software/D3D11 mean channel difference is 0.5606 of 255.
                  CLI images inspected: software c67b46e7be2ac459,
                  D3D11 66d23d1087649f31.
            - [x] Verify all instruction families with GPU comparisons,
                  textured/HDR/time-driven surfaces and alpha behavior;
                  extend graph alpha to shadow/selection passes as needed.
                  - [x] Graph alpha in CPU shadow depth and GPU shadow/selection
                        passes. Shared shader blocks keep auxiliary evaluation
                        identical to main shading. Procedural masks remain
                        enabled without a base texture; graph programs are
                        retained even when their input opacity is zero.
                        Windows passes 87 tests; Node/WASM passes 83.
                        ShaderMaterialAlphaAndShadow
                        verifies that alpha zero matches an absent object in
                        color, CPU depth/IDs and GPU selection output; alpha one
                        restores the object, outline and a visible ground
                        shadow in both renderers. Disabling castShadows removes
                        that ground shadow. CPU/D3D11 mean difference is 1.2367.
                        CLI masked stripes/selection PNGs inspected: software
                        7004187b216e5ce6, D3D11 e492f9db4088e1b0.
                  - [x] ShaderMaterialInstructionFamilies covers all arithmetic
                        operations with independent numeric expectations and
                        software/D3D11 image comparisons. UV, world position,
                        normal, base color, explicit texture sampling and fixed
                        time inputs are compared, including a four-color texture
                        and changes in time. Graph base/emissive values above 1
                        and alpha blending are checked before exposure/Reinhard;
                        graph alpha replaces even zero input opacity and keeps
                        sub-0.5 fragments out of picking. Worst CPU/D3D11 mean
                        difference is 1.4719 of 255. Windows passes 88 tests;
                        Node/WASM passes 84 tests.
                  - [x] Varying-alpha shadow masks and selection occlusion with
                        mixed materials; packaged WebGL checks are P6.4c.
                  - [x] Recover D3D compiler diagnostics with D3DCompile using
                        the generated HLSL and engine compiler flags. Replacing
                        indexed vector division with explicit channel expressions
                        removes X3550 from both programs. The auxiliary program
                        compiles without warnings; the main program retains the
                        existing X3570 shadow-comparison derivative warning.
      - [x] P6.4c: Sample procedural material, CPU/GPU image comparison,
            hot reload/packaging, WebGL execution, docs and refreshed players.
            Done in the completion pass at the top of this log (ShaderLab);
            the Android player refresh is tracked there as an open line.
- [ ] Hardware follow-up: Android device and full Linux/EGL effect execution.
- [ ] Browser capture follow-up: the in-app whole-page capture omits the small
      pure-white emitter in the neutral-mode fixture, although scene resolve,
      composite, UI and 30 consecutive frame-end WebGL readbacks all report
      RGBA (255,255,255,255) at its center with GL error 0. Bloom/tone/vignette
      images show it normally. CSS filter is none and blend mode normal.
      Compare exported canvas images and another browser capture path before
      attributing this discrepancy to rendering; no engine failure is proven.

## Initial contract

Attach PostProcess to a Camera entity. Only the active game camera selects its
settings; free/editor Scene cameras start with effects disabled. All effects
default off so existing screenshots and scene files retain their behavior.
The scene color is processed before UI and the selection outline, keeping
controls and editor feedback readable. Depth and entity IDs remain unchanged.

Vignette strength 0..1 darkens the image
edges; radius is measured in normalized screen coordinates (center 0, midpoint
of an edge 1, corner sqrt(2)). Softness is the transition width, with a minimum
of 0.01 to avoid a degenerate smooth transition. Subsequent milestones add the
other effects and custom materials; P6 is not complete until those are verified.

## Vignette controls

Use component.add/component.set on the camera (and the same scene.set calls in
Lua); reflection supplies serialization, undo and the editor inspector.

```json
{"command":"component.add","args":{"id":"Camera","type":"PostProcess","values":{"vignette":0.8,"vignetteRadius":0.3,"vignetteSoftness":0.5}}}
```

Defaults are vignette 0, radius 0.75 and softness 0.5. Reflected ranges are
0..1, 0..1.5 and 0.01..2 respectively. Direct RenderView settings are normalized
at rendering time; non-finite values fall back to safe defaults. The vignette
multiplies scene display-color channels by a cubic smoothstep falloff before
overlays, after exposure and tone mapping when enabled.

## Exposure and HDR tone mapping

PostProcess.exposure is a brightness multiplier (0..32, default 1).
PostProcess.toneMapping is `none` (default) or `reinhard`. Reinhard compresses
each exposed channel as `c / (1 + c)`; it does not automatically meter the scene.
Zero exposure makes scene color black while leaving UI and selection visible.

```json
{"command":"component.set","args":{"id":"Camera","type":"PostProcess","values":{"exposure":0.25,"toneMapping":"reinhard"}}}
```

An exposure other than 1, enabled tone mapping or nonzero bloom selects an HDR scene buffer:
float32 RGB in software and RGBA16F on the GPU. Lighting and emissive channels
remain above 1 through opaque draws and alpha blending, bounded to the largest
finite float16 value (65504) before blending. Exposure is applied after scene
rendering, depth of field and bloom addition, followed by tone mapping, clamping
to 0..1, vignette and then overlays. The full order is: scene, depth of field,
bloom, exposure/tone mapping, vignette, FXAA, selection outline and UI.
Only the final display image is quantized to RGBA8. With exposure 0.25, emissive
RGB (8,4,2) becomes (2,1,0.5), or (2/3,1/2,1/3) with Reinhard enabled; clipping
the scene to RGBA8 first would lose that distinction.

The color convention remains the engine's existing numeric RGB convention:
textures and material colors are used directly, without additional sRGB decode
or encode. This is an explicit compatibility choice, not a color-managed ACES
pipeline. CPU/GPU precision and MSAA can cause small differences; deterministic
hashes use the software renderer. Neutral settings retain the original RGBA8
path and its hashes, including vignette-only scenes.

GPU HDR requires renderable, blendable, linearly filterable RGBA16F targets
(WebGL2 requires the corresponding float color-buffer support). Unsupported
devices report an error when HDR is requested; use software rendering or neutral
settings. HDR disables MSAA if that format does not support multisampling.
Switching effects or output size recreates the correct scene targets; output,
UI and selection-mask formats remain RGBA8.

## Bloom

PostProcess.bloom controls highlight strength (0..4, default 0). The threshold
is bloomThreshold (0..32, default 1), measured before exposure in the engine's
numeric scene RGB convention. bloomRadius is an integer radius in scene pixels
(1..32, default 8); when renderScale is used this refers to the smaller scene
buffer, not the final window/UI resolution.

```json
{"command":"component.set","args":{"id":"Camera","type":"PostProcess","values":{"bloom":1.5,"bloomThreshold":1,"bloomRadius":24,"exposure":0.25,"toneMapping":"reinhard"}}}
```

For each scene color c, let p be its largest channel. Highlight extraction is
zero when p <= threshold, otherwise c * (p - threshold) / p. This preserves
highlight hue. Extraction is fused into a horizontal tent filter, followed by
a vertical tent filter. Each axis uses weights radius + 1 - abs(offset) for
offsets -radius..radius, normalized by (radius + 1)^2. Sampling clamps to the
nearest border texel, preserving constant highlights at screen edges.

The blurred highlight multiplied by bloom is added to scene color, bounded to
65504, and then exposed/tone mapped. Opaque UI and selection outlines are drawn
afterward and never seed the blur. Software uses float32 intermediates; GPU
uses two full-resolution RGBA16F targets, giving small precision/MSAA differences.
Depth and picking IDs are unchanged. Strength zero skips both passes and extra
buffers; neutral settings preserve old frame hashes. Activating bloom selects
HDR scene blending even when the threshold rejects all highlights.

This bounded two-pass filter has no automatic exposure, mip-chain downsampling
or screen-size-independent radius. Cost and memory grow with scene resolution
and radius; it is intended as an optional, directly testable effect. Increase
radius for a broader halo or lower threshold for dimmer light sources.

## Depth of field

PostProcess controls (all default to disabled):

| Field | Default | Meaning |
|---|---|---|
| `dofRadius` | 0 | Integer 0..16: largest blur radius in scene pixels for surfaces far from the focus; 0 disables the effect. Scene pixels are the scene buffer, like `bloomRadius`, so the look depends on resolution and `renderScale` |
| `dofFocus` | 10 | View depth in meters (distance in front of the camera along its view direction) that is sharp |
| `dofRange` | 2 | Depth on either side of `dofFocus` that stays fully sharp |
| `dofFalloff` | 8 | Further depth over which the blur grows linearly to `dofRadius` (minimum 0.01) |

The blur radius (circle of confusion, coc) of a surface at view depth d is
`dofRadius * clamp((|d - dofFocus| - dofRange) / dofFalloff, 0, 1)`.

```json
{"command":"component.set","args":{"id":"Camera","type":"PostProcess","values":{"dofRadius":6,"dofFocus":12.9,"dofRange":2.2,"dofFalloff":7}}}
```

Two separable gather passes (horizontal, then vertical) run over the scene
color, each over offsets -dofRadius..dofRadius with edge clamping. A sample k
pixels away has weight `clamp(coc - k + 1, 0, 1)`, where coc is the sample's own
blur radius; a sample behind the filtered pixel uses `min(its coc, the filtered
pixel's coc)` instead. A blurred background therefore does not bleed over a sharp
surface in front of it, while a blurred foreground spreads over what is behind it.
The result is the weighted average.

Order: scene, depth of field, bloom, exposure/tone mapping, vignette, FXAA,
then selection outline and UI (UI is never blurred). The effect works on the
RGBA8 scene buffer or the HDR buffer, whichever the other settings select; it
does not select HDR by itself. Depth and entity IDs (picking) are unchanged, and
`dofRadius` 0 skips the passes and keeps previous frame hashes. Perspective and
orthographic cameras both work (same view depth).

Depth comes from opaque and cut-out surfaces. Blended surfaces (soft-edged
sprites, particles, anything with opacity below 1) write no depth and are blurred
like whatever is behind them; pixels with nothing drawn count as the far plane.

The software renderer uses its depth buffer directly. The GPU renderer adds one
single-sample geometry pass that writes the depth buffer value into an R32F
target, then runs the two blur passes in the scene format (shaders `scene_depth`
and `dof` in `engine/render/shaders/Shaders.glsl`; helpers `DofViewDepth`,
`DofCoc` and `DofWeight` in `engine/render/PostProcess.h`). It needs renderable
R32F targets; without them an error tells the caller to use the software
renderer. Because the depth pass is not multisampled, MSAA edge pixels of a
focused object can carry the background's depth and smear slightly.

Verified by CameraDepthOfField (see the work log). The prebuilt web and Android
players do not include depth of field yet, and WebGL2/GLES3/EGL execution of the
new shaders is unverified. `samples/HD2D` uses it with a long lens.

## FXAA controls

PostProcess.fxaa is a boolean, disabled by default. Enable it through
component.set (or Lua scene.set). A compact directional FXAA filter samples
post-processed RGBA8 display scene color with clamped bilinear filtering,
a maximum span of eight scene pixels and luminance contrast thresholds
max(1/32, brightest * 0.125). This is not the NVIDIA FXAA 3.11 quality preset
implementation. It follows the engine's existing RGB convention, without
adding a color-space conversion. Bloom, exposure, tone mapping and vignette
run before FXAA; selection outlines and UI run afterward. GPU rendering uses
an optional RGBA8 intermediate at scene resolution, so reduced-resolution
window rendering keeps HUD and editor outlines at output resolution.

Disabling FXAA avoids its intermediate allocation/pass and preserves previous
frame hashes. Depth and entity IDs are never filtered. Sampling across small
features may soften them; pixel-art cameras should leave it disabled.

## Showcase presets

Run `oe run samples/Showcase`, or package it with `oe package samples/Showcase
--web`. Press 1 for neutral output, 2 for exposure/Reinhard, 3 for bloom,
4 for vignette, 5 for FXAA, or 6 for all effects. The HUD displays the selected
preset; movement and animation controls remain available. Presets replace all
effect settings so switching back to Off cannot retain bloom or HDR targets.
The initial scene keeps all effects neutral. Adjust preset values in
`samples/Showcase/scripts/post_process.lua`; no engine rebuild is needed.

## HD2D sample

`oe run samples/HD2D`: pixel-art sprites turned to the camera by a script in a lit 3D world. Key 1
toggles all camera effects (Reinhard exposure, bloom, vignette, depth of field)
and key 2 toggles only depth of field, to compare with the plain scene.

## Portable surface shader graphs

`samples/WuwaToon` demonstrates a project-authored toon graph with independent
skin/hair/clothing palettes, explicit studio light/camera uniforms, normal/PBR
comparisons and original procedural silhouette geometry. See [TOON.md](TOON.md)
for public reference provenance, runnable controls, verification and limits.

A project authors a *.shader.json graph rather than platform-specific HLSL or
GLSL. The graph is a programmable surface calculation (per-fragment color, emission
and normal, optional per-vertex displacement), not a list of built-in visual presets. Ordered four-vector instructions form an acyclic
program; the CPU reference evaluates the same instructions that the generated
GPU evaluator will execute. Existing sokol-shdc generation remains the backend
compiler. Material binding and main fragment execution are implemented on CPU
and GPU. Graph alpha also reaches shadow/selection passes. Instruction families,
texture/time inputs, HDR emission and blending are verified; varying-alpha
shadows and mixed-material selection verification remain open.

Create a graph with shader.create {path, graph, overwrite?}; validate an existing
file with shader.check {path}. Both commands validate every field and identify
the failing node. Invalid creation never replaces an existing valid file.
asset.list {kind:"shader"} lists these files. Paths remain inside the project.

Example graph (place this object in shader.create.graph):

```json
{
  "format": "ownengine.shader",
  "uniforms": {"frequency": 4},
  "nodes": [
    {"op": "uv"},
    {"op": "uniform", "name": "frequency"},
    {"op": "multiply", "args": [0, 1]},
    {"op": "fract", "args": [2]},
    {"op": "constant", "value": 0.5},
    {"op": "step", "args": [4, 3]}
  ],
  "color": 5
}
```

Each node's args contains indices of earlier nodes; cycles and forward references
are rejected. color references the RGBA output, and optional emissive references
an RGB output. All registers are four-vectors. Scalar constants/uniform defaults
broadcast to all channels; vector values must have exactly four finite numbers
in -65504..65504. Up to 48 nodes and 8 named uniform defaults are supported.
Per-material overrides must name declared uniforms and preserve unspecified
values. Node outputs saturate to -65504..65504; NaN becomes zero. Division by
zero returns zero for that channel. This gives the GPU a finite bounded program
and keeps the CPU reference independent of platform compiler behavior.

Two more optional outputs sit next to `color` and `emissive`:

- `"offset": <node index>` displaces vertices. The node's xyz is added to each
  vertex's world position. It is evaluated per vertex, in both renderers, and only
  the instructions up to that node run in the vertex stage. Vertex-stage inputs:
  `position` (world position before the offset), `normal` (world vertex normal),
  `uv` (mesh uv with the sprite frame and material tiling/offset), `time`, and
  `baseColor` (the material's base color and opacity without the texture). A
  `texture` node cannot feed `offset`; `shader.create`/`shader.check` reject it
  ("offset must not depend on a texture node"). In the fragment stage `position`
  is the displaced world position.
- `"normal": <node index>` replaces the world-space lighting normal per fragment.
  The renderer normalizes it; a zero vector keeps the geometric normal; it is
  flipped on back faces of double-sided materials, and a normal map is applied on
  top of it. The `normal` input node still gives the geometric normal.

Displaced surfaces are displaced consistently in the shadow map, the
depth-of-field depth pass and the editor selection outline. The displacement is
render-only: physics, `physics.raycast` picking and `boundsMin`/`boundsMax` do not
know about it. Shared vertices need a mesh with enough vertices, so use `plane64`
(see [RENDERING.md](RENDERING.md)) rather than the 4-vertex `plane`.
`shader.create`, `shader.check` and `asset.info` report `normal` and `offset`
(-1 = absent). Prebuilt web and Android players older than this change ignore both
outputs and reject graphs with more than 32 nodes until they are rebuilt.

Example: a sine wave along world x that moves with time lifts the vertices of a
`plane64` surface (`offset` uses the xyz of its node, so only the y channel is set):

```json
{
  "format": "ownengine.shader",
  "uniforms": {"amplitude": 0.4},
  "nodes": [
    {"op": "position"},
    {"op": "time"},
    {"op": "add", "args": [0, 1]},
    {"op": "sin", "args": [2]},
    {"op": "uniform", "name": "amplitude"},
    {"op": "multiply", "args": [3, 4]},
    {"op": "constant", "value": [0, 1, 0, 0]},
    {"op": "multiply", "args": [5, 6]},
    {"op": "constant", "value": [0.1, 0.4, 0.8, 1]}
  ],
  "color": 8,
  "offset": 7
}
```

Two uniform names are reserved: a graph that declares `cameraPosition` receives the
rendering view's world position (w = 1) and one that declares `lightDirection` receives the
unit vector toward the first directional light (w = 0), every frame and in both renderers.
Declared defaults and material overrides for these two names are replaced, so view- and
light-dependent graphs follow a moving camera or sun without material edits
(`samples/NetChase`). They still count toward the eight uniforms. Graphs without these
names are unaffected. Prebuilt web/Android players older than this change leave the
declared defaults in place.

Inputs: uv, world position, world normal, fixed simulation time (broadcast),
baseColor RGBA, and texture(args UV) for the material's base texture. Missing
textures sample white. Both renderers supply these interpolated fragment inputs
to the shared graph contract.

Operations and argument counts:

| Operation | Args | Behavior |
|---|---:|---|
| constant / uniform | 0 | value scalar/vector, or name declared uniform |
| uv / position / normal / time / baseColor | 0 | Fragment input |
| texture | 1 | Sample base texture using input vector xy |
| add / subtract / multiply / divide | 2 | Component-wise arithmetic |
| min / max | 2 | Component-wise minimum/maximum |
| sin / cos / floor / fract / abs | 1 | Component-wise function |
| clamp | 3 | min(max(a,b),c), including reversed bounds |
| mix | 3 | a*(1-c)+b*c; c is not clamped |
| step | 2 | b<a ? 0 : 1, component-wise |
| dot | 2 | Four-channel dot product, broadcast |
| normalize | 1 | Four-vector length; length <=1e-8 gives zero |
| swizzle | 1 | value contains four channel indices 0..3 |

`MeshRenderer.shaderUniforms` overrides graph uniforms per entity: `{name: number or [x, y, z, w]}`.
Unnamed uniforms keep the material's values, so entities sharing one material file can draw
differently, and a script can change the values every frame with `scene.set(id, "MeshRenderer",
{shaderUniforms = {...}})` without touching the material file. Setting the component field replaces
the whole object. A name the graph does not declare, a malformed value, or uniforms on a material
without a graph render the mesh magenta, like a broken material. The values are resolved when render
items are gathered, so both renderers use them (`MeshRendererShaderUniforms` test); the reserved
`cameraPosition`/`lightDirection` uniforms still win. Prebuilt web and Android players built before
this field need a rebuild to run scenes or scripts that use it. (For 2D light and darkness use
`Light2D`/`Darkness2D`, see [2D.md](2D.md#lights-and-darkness).)

Material files accept shader (a project-relative graph path) and shaderUniforms
(named scalar/four-vector overrides). The graph's color RGBA replaces the
texture-multiplied base color/opacity before ordinary PBR lighting or unlit
output; its emissive RGB adds to the material's existing emissive contribution.
Negative base RGB clamps to zero and alpha clamps to 0..1. Standard materials
keep their previous output when shader is empty. Clearing a graph with
material.set also requires clearing its overrides with shaderUniforms:{}.
Graph texture nodes sample the base image at explicit mip level zero on GPU,
matching the CPU reference; they use the material's pixelArt filtering choice.
UV includes sprite frame transforms and material tiling/offset. Position and
normal are world-space, with geometric normals before normal-map modification.

Graph files and their dependents reload through AssetManager.PollChanges;
shader.create overwrite invalidates the graph/material caches immediately.
Graph validation also appears through asset.info. Invalid material edits are
validated before disk replacement. The web player includes graph binding and
packaged ShaderLab execution is verified. Android player refresh remains open.

## ShaderLab sample

`oe run samples/ShaderLab` demonstrates time-driven cyan UV bands with emission
and an orange procedural alpha mask casting a striped ground shadow. Press 1
for neutral output or 2 for exposure/Reinhard/bloom; the matching on-screen
buttons work with mouse/touch. Edit materials/pulse.shader.json to change the
frequency/tint while the player or editor runs. Dependencies hot reload through
the same asset path covered by ShaderMaterialRendering. Package with
`oe package samples/ShaderLab --web` or the native packaging command. Graph and
material assets are included in game.pak; extraction preserves the reference
frame hash at a fixed simulation time.

## Water sample

`oe run samples/Water` is a rolling sea: one `plane64` scaled 44x44 whose graph
`materials/water.shader.json` (47 nodes) sums four sine waves, one per register
channel. `offset` lifts the vertices, `normal` gives per-pixel slopes so sun glitter
comes from the ordinary PBR lighting, and `color` goes from trough to crest color
plus foam. `scripts/waves.lua` holds the same wave table; `scripts/float.lua` makes
buoys, crates and a boat ride and lean on the surface (it samples at
`time.now() + dt` because the frame is drawn with the time after the step).
`scripts/ocean.lua` eases between the sea states Calm/Swell/Storm (keys 1/2/3) and
sends amplitudes and colors through `MeshRenderer.shaderUniforms` every frame.
`scripts/orbit.lua` orbits the camera (A/D or arrows turn, W/S zoom, Q/E height).
`python tools/make_scene.py` regenerates the scene, graph and material. WaterSamplePlays
checks that a buoy's height equals the graph's vertex stage, the storm preset, the
orbit camera and determinism.
