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
- [ ] P6.3: Optional FXAA and sample controls/demonstration; verify off/on output,
      resolution changes and separation from UI/selection overlays.
- [ ] P6.4: Custom shader materials through commands/assets, validation and
      portable backend shader generation. Document the software reference
      behavior and demonstrate a material in a sample.
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
