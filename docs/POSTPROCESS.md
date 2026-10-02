# Camera post-processing and shader materials

## Implementation status (work log)

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
- [ ] P6.4: Custom shader materials through commands/assets, validation and
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
      - [ ] P6.4b: Load graph assets from materials, apply per-material uniforms,
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
            - [ ] Verify all instruction families with GPU comparisons,
                  textured/HDR/time-driven surfaces and alpha behavior;
                  extend graph alpha to shadow/selection passes as needed.
      - [ ] P6.4c: Sample procedural material, CPU/GPU image comparison,
            hot reload/packaging, WebGL execution, docs and refreshed players.
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
rendering and bloom addition, followed by tone mapping, clamping to 0..1,
vignette and then overlays.
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

## Portable surface shader graphs (P6.4 in progress)

A project authors a *.shader.json graph rather than platform-specific HLSL or
GLSL. The graph is a programmable fragment surface calculation, not a list of
built-in visual presets. Ordered four-vector instructions form an acyclic
program; the CPU reference evaluates the same instructions that the generated
GPU evaluator will execute. Existing sokol-shdc generation remains the backend
compiler. Material binding and main fragment execution are implemented on CPU
and GPU; alpha in auxiliary passes and full instruction-family verification
remain open.

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
in -65504..65504. Up to 32 nodes and 8 named uniform defaults are supported.
Per-material overrides must name declared uniforms and preserve unspecified
values. Node outputs saturate to -65504..65504; NaN becomes zero. Division by
zero returns zero for that channel. This gives the GPU a finite bounded program
and keeps the CPU reference independent of platform compiler behavior.

Inputs: uv, world position, world normal, fixed simulation time (broadcast),
baseColor RGBA, and texture(args UV) for the material's base texture. Missing
textures sample white. The integration milestone will supply these interpolated
fragment inputs; the reference evaluator already accepts them.

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
validated before disk replacement. Shipped players still predate this binding
milestone; refresh and packaged execution remain part of P6.4c.
