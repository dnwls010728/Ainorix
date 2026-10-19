# Camera post-processing and shader materials

`PostProcess` on the active camera applies screen-space effects (exposure and tone mapping, bloom, vignette, depth
of field, SSR, FXAA, post graphs) in both renderers, and `.shader.json` surface graphs give materials programmable color, emission,
vertex offset and normals. Open items are listed under Status at the end.

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

`vignetteColor` (default black) is what the edges blend toward: the factor is
`mix(1, vignetteColor, vignette * falloff)` per channel, so black gives the plain
darkening above and a warm brown (`[0.62, 0.42, 0.28]`, `games/Kongal`) a sepia,
painted edge, like the color parameter of Unity's URP Vignette. Test:
`PostProcessVignetteColor`.

## Exposure and HDR tone mapping

PostProcess.exposure is a brightness multiplier (0..32, default 1).
PostProcess.toneMapping is `none` (default) or `reinhard`. Reinhard compresses
each exposed channel as `c / (1 + c)`; it does not automatically meter the scene.
Zero exposure makes scene color black while leaving UI and selection visible.

```json
{"command":"component.set","args":{"id":"Camera","type":"PostProcess","values":{"exposure":0.25,"toneMapping":"reinhard"}}}
```

An exposure other than 1, enabled tone mapping, nonzero bloom or SSR selects an HDR scene buffer:
float32 RGB in software and RGBA16F on the GPU. Lighting and emissive channels
remain above 1 through opaque draws and alpha blending, bounded to the largest
finite float16 value (65504) before blending. Exposure is applied after scene
rendering, depth of field and bloom addition, followed by tone mapping, clamping
to 0..1, vignette and then overlays. The full order is: optional SSAO depth prepass,
scene (SSAO affects ambient lighting here), screen-space reflection, depth of field,
bloom, exposure/tone mapping, vignette, FXAA, selection outline and UI.
Only the final display image is quantized to RGBA8. With exposure 0.25, emissive
RGB (8,4,2) becomes (2,1,0.5), or (2/3,1/2,1/3) with Reinhard enabled; clipping
the scene to RGBA8 first would lose that distinction.

By default colors keep the engine's numeric RGB convention: textures and
material colors are used directly, without sRGB decode or encode. A project with
`"rendering": {"colorSpace": "linear"}` lights in linear space instead: it always
uses the float scene buffer, and the composite encodes to sRGB right after tone
mapping (before grading, vignette, FXAA and the post graph); see
[RENDERING.md](RENDERING.md#color-space). CPU/GPU precision and MSAA can cause small differences; deterministic
hashes use the software renderer. Neutral settings retain the original RGBA8
path and its hashes, including vignette-only scenes.

GPU HDR requires renderable, blendable, linearly filterable RGBA16F targets
(WebGL2 requires the corresponding float color-buffer support). Unsupported
devices report an error when HDR is requested; use software rendering or neutral
settings. HDR disables MSAA if that format does not support multisampling.
Switching effects or output size recreates the correct scene targets; output,
UI and selection-mask formats remain RGBA8.

## Screen-space reflections (SSR)

On the active camera, enable `PostProcess.ssr` for glossy opaque floors and other smooth surfaces:

```json
{"command":"component.set","args":{"id":"Camera","type":"PostProcess","values":{"ssr":1,"ssrDistance":15,"ssrThickness":0.2}}}
```

`ssr` is strength (0..1, default 0), `ssrDistance` is maximum ray length in world meters
(0.1..1000, default 30), and `ssrThickness` is view-depth hit tolerance in meters (0.001..10,
default 0.2). Direct render settings normalize ranges and nonfinite values. Zero strength
allocates no SSR framebuffers and preserves existing screenshots. Reflection-probe and planar
captures disable camera effects, preventing recursive reflections.

Both renderers retain each opaque pixel's final lighting normal, roughness, Fresnel/AO weight
and reflected ambient fallback after material maps, shader graphs and decals. A fixed 64-step
world-space reflected ray is projected into opaque/cut-out depth, with five bisections at the
first crossing. Perspective and orthographic cameras are supported. Successful hits sample the
original scene color; only reflected ambient is replaced, leaving direct lighting and emission
intact. A missing or rejected hit keeps the existing local probe, sky or flat-ambient reflection.
There is no reflected image history or recursive sampling of the SSR result.

Reflection strength fades quadratically to zero at roughness 0.6, over the last 5% at screen
edges, and toward the maximum ray length. This is a glossy reflection approximation, without a
roughness blur or temporal filtering. It cannot show hidden, offscreen or transparent geometry;
large steps can miss thin objects and depth discontinuities can leave gaps. Reduce ray length
for small rooms; increasing thickness fills gaps but can admit false intersections. Unlit and
transparent materials do not receive SSR. Alpha-mask holes write no depth; transparent foreground
coverage attenuates the underlying correction. A selected planar receiver keeps its planar
reflection instead of also receiving SSR. Fog attenuates the correction with the receiver.

SSR runs before DOF, bloom, tone mapping, grading and FXAA; UI and selection are applied later.
Software depth and picking IDs remain unchanged. GPU SSR uses the existing R32F opaque depth
prepass and three RGBA16F scene attachments (normal/roughness, weight, fallback), then a separate
fullscreen shader with five textures. The mesh shader gains no uniforms or texture bindings;
it still fits the WebGL2/GLES3 minimum of 224 fragment uniform vectors including 16 samplers.
Four simultaneous color attachments fit their minimum draw-buffer limit. Float color-buffer
support is required; unsupported GPU formats report an error with the software-renderer fallback.
With SSR and DOF together, DOF uses its separate depth pass instead of adding a fifth MRT.

Demonstration: `oe editor build/SSR`, or `oe render build/SSR --renderer gpu --out build/r9.png`.
The camera Inspector exposes the same three reflected fields; Lua uses `scene.set` as usual.

## Screen-space ambient occlusion (SSAO)

Add `PostProcess` to the active camera and set `ssao` to enable contact and crease shading:

```json
{"ssao": 0.8, "ssaoRadius": 1.0, "ssaoBias": 0.02}
```

`ssao` is strength (0..1, default 0), `ssaoRadius` is the sampling radius in world meters
(0.01..10, default 1), and `ssaoBias` is the self-occlusion bias in meters (0..1, default 0.02).
Nonfinite inputs fall back to those defaults. Zero strength skips the depth prepass and all samples;
old scenes retain their software hashes. Reflection-probe captures do not include camera SSAO.

Both renderers first capture opaque and alpha-mask geometry, including skins and shader vertex
displacement. They reconstruct neighboring world positions from depth and use the surface's geometric
normal with 32 fixed spiral samples. Perspective and orthographic cameras use the same world radius;
the projected radius is limited to 64 scene pixels per axis. The distance falloff and bias reject
distant geometry and coplanar self-occlusion. Samples outside the screen or on the background contribute
no occlusion. No random seed, temporal history or renderer-specific sample pattern is involved.

SSAO multiplies only the ambient lighting, including sky/probe diffuse and reflected ambient. It
combines with material occlusion textures. Directional, point and spot direct lighting, emission, fog,
unlit surfaces, blended/additive surfaces, background, UI and selection outlines keep their usual
behavior. Unlit opaque objects can occlude their neighbors; transparent objects neither occlude nor
receive SSAO. Depth and entity IDs remain unchanged. The editor's free Scene camera keeps effects off;
the Game view uses the active camera settings. Generic Inspector controls and component reflection
provide the same fields through CLI, HTTP and MCP.

The GPU uses an additional single-sample R32F depth-color target before lighting; unsupported devices
report an error when SSAO is requested. WebGL2/GLES3 needs float color-buffer support, as for depth of
field. SSAO coexists with MSAA, DOF, HDR, bloom and FXAA. Its depth texture uses binding 13: the mesh
fragment program uses 16 of the GLES3 minimum 16 texture units with R6 planar reflections and R7 decals and 208 vec4 uniform vectors, or 224
including a conservative vector per sampler, at the minimum 224. No vertex-stage resources were
added. The existing skin/vertex-graph uniform requirements still need minimum-device verification.

This is a bounded screen-space approximation: offscreen and hidden occluders are absent, silhouettes
can show sampling bands, large radii lose detail at the 64-pixel cap, and single-sample depth can differ
slightly from MSAA edges. Reduce radius/strength or increase bias for a cleaner result. It does not
replace shadow maps or baked/global illumination. A demonstration is left at `build/SSAO`:
`build/bin/oe editor build/SSAO`; set Camera's `ssao` to zero to compare.

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

Order: optional SSAO depth prepass, scene, depth of field, bloom, exposure/tone mapping, vignette, FXAA,
then selection outline and UI (UI is never blurred). The effect works on the
RGBA8 scene buffer or the HDR buffer, whichever the other settings select; it
does not select HDR by itself. Depth and entity IDs (picking) are unchanged, and
`dofRadius` 0 skips the passes and keeps previous frame hashes. Perspective and
orthographic cameras both work (same view depth).

Depth comes from opaque and cut-out surfaces. Blended surfaces (soft-edged
sprites, particles, anything with opacity below 1) write no depth and are blurred
like whatever is behind them; pixels with nothing drawn count as the far plane.

The software renderer uses its depth buffer directly. The GPU renderer's scene
pass writes each opaque or cut-out pixel's view depth into a second RGBA16F color
target (multisampled like the scene and resolved; blended pipelines mask it off),
then runs the two blur passes in the scene format (`mesh_fs` output `frag_depth`
and shader `dof` in `engine/render/shaders/Shaders.glsl`; helpers `DofViewDepth`,
`DofCoc` and `DofWeight` in `engine/render/PostProcess.h`). Resolved MSAA edge
pixels average the depths of both sides. Where multisampled RGBA16F targets are
not renderable it falls back to one extra single-sample geometry pass that writes
the depth buffer value into an R32F target (shader `scene_depth`); MSAA edge
pixels of a focused object can then carry the background's depth and smear
slightly. Without renderable R32F targets either, an error tells the caller to
use the software renderer.

Verified by CameraDepthOfField. The prebuilt web and Android players include it;
WebGL2/GLES3/EGL execution of the new shaders is unverified. `games/RPG` uses it with a long lens.

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

## Post graphs

`PostProcess.shader` names a `*.shader.json` with `"stage": "post"`: a graph that gives each pixel its color,
by default running last on the display image (after tone mapping, grading, vignette and FXAA), the way Unreal post process
materials, Unity URP full screen passes or a three.js `ShaderPass` do. `PostProcess.shaderUniforms` overrides
its uniform defaults (`{name: number or [x,y,z,w]}`; scripts may change it every frame). Empty = none.

```json
{"format": "ownengine.shader", "stage": "post", "uniforms": {"gain": 1},
 "nodes": [{"op": "uv"}, {"op": "sceneColor", "args": [0]}, {"op": "constant", "value": 1},
           {"op": "subtract", "args": [2, 1]}, {"op": "uniform", "name": "gain"}, {"op": "multiply", "args": [3, 4]}],
 "color": 5}
```

A post graph has only the `color` output and up to 128 nodes, as a surface graph. Its optional `injection` field chooses when it runs:

- `"afterToneMapping"` (default): reads the quantized display image and clamps output rgb to 0..1, preserving existing graphs.
- `"beforeToneMapping"`: reads the rendered scene after depth of field, before bloom, exposure, tone mapping,
  sRGB encoding, grading, vignette and FXAA. `sceneColor` and `sceneBlur` read scene-referred values, which may
  exceed 1: linear HDR light in a linear project, plain color numbers in a gamma project. Output rgb is finite
  and saturated to 0..65504, replacing scene color for the remaining stack. Bloom extracts highlights from this
  output, so a graph can create or suppress bloom. Only post graphs accept `injection`.

Its inputs (the image is scene color for `beforeToneMapping`, display color otherwise):

| Operation | Args | Behavior |
|---|---:|---|
| uv | 0 | Screen position 0..1 of the pixel center, (0,0) at the top left |
| texel | 0 | (1/width, 1/height, width, height) of the display image |
| time | 0 | Fixed simulation time |
| sceneColor | 1 | The image at uv (xy), bilinear with clamped edges, alpha 1; through FXAA only for `afterToneMapping` when `fxaa` is on |
| sceneBlur | 2 | 5x5 gaussian of the image around uv (a.xy), samples b.x pixels apart, weights exp(-(i*i + j*j) * 0.35) |

Surface inputs (position, normal, baseColor, texture, particle*) are rejected, as are post inputs in a surface
graph; `noise`, `fbm`, `pow`, `length` and `smoothstep` (in the operation table below) work in both stages.
Pixel-sized effects scale with `texel` (multiply uv by its zw for pixel coordinates).

For `afterToneMapping`, both renderers read the same quantized RGBA8 display image (the one FXAA reads): the GPU
writes it to the FXAA source target first. For `beforeToneMapping`, the GPU runs a full-screen graph pass into
a separate RGBA16F scene target before bloom; the software renderer evaluates from an immutable float scene
image into its HDR buffer at the same point. Like surface graphs, each post graph gets its own
compiled program: `GraphPostBody` (GraphSource.cpp) replaces the body of `PostGraph` in the generated composite
shader with straight-line code that reads its inputs through `PostUniform`/`PostTime`/`PostTexel`/`PostScene`/
`PostBlur`, compiled on first use (in the background with async shaders) and cached per graph. Until it is ready,
or if it fails to compile, the composite interpreter runs the graph from its instructions as uniforms; it holds
64 nodes, so a longer graph is skipped for those frames. `render.screenshot` reports which path ran in
`stats.postGraph` (`compiled`, `interpreted`, `skipped`, or `software`). The software renderer evaluates the graph
with `EvaluateShaderGraph`. A missing graph, a surface graph or a bad uniform is skipped with one log warning per
problem; `shader.check` validates it. Test: `PostProcessGraph` (a 100-node graph runs compiled and matches the CPU).

Helper functions that compiled graphs call never write their parameters: SPIRV-Cross turns a written parameter
into `inout`, and `GraphFbm(oeg5.xyz)` would then change the register it was given (found by `PostProcessGraph`
before `GraphFbm`/`GraphHash` copied their inputs).

Test: `PostProcessGraphInjection` (validation, the unchanged default, HDR scene values in and out, bloom of the
graph's output, gamma and linear display conversion, compiled and interpreted GPU paths against the CPU).

`games/Kongal/shaders/watercolor.shader.json` (64 nodes) is the watercolor pass of the three.js prototype:
edges that wander with fbm, bleed toward `sceneBlur`, pigment pooling where the color differs from the blur,
blotchy wash density (`pow`), granulation and paper tooth.

## Fog, color grading and ACES

Distance fog and color grading live on the same `PostProcess` component; all neutral defaults leave
existing frames unchanged (the effects are skipped, not computed with neutral values).

- `fog` (0..1, 0 = off), `fogColor`, `fogStart`, `fogEnd`: each lit or unlit surface is mixed toward
  `fogColor` by `fog * clamp((distance - fogStart) / (fogEnd - fogStart), 0, 1)`, where distance is
  measured from the camera position to the surface point. It is applied per surface after lighting and
  emission (`FogAmount` in `PostProcess.h`, `mesh_fs` in `Shaders.glsl`), so blended surfaces fog
  correctly behind each other; additive surfaces (glows, `blend: add`) fade toward black instead of
  adding fog color. The background clear color is not fogged; set it to `fogColor` for a horizon that
  melts into the fog. A material can opt out with `"fog": false` (sky domes, mountain backdrops).
- `saturation` (0 = grey, 1 = unchanged), `contrast` (around mid grey 0.5) and split toning
  `shadowTint` / `highlightTint` (multipliers blended by luminance with a smoothstep up to 0.9) run
  after tone mapping and before the vignette, on display color (`Grade` in `PostProcess.h`,
  `composite_fs`).
- `toneMapping: "aces"` is the Narkowicz fit of the ACES filmic curve
  (`x(2.51x + 0.03) / (x(2.43x + 0.59) + 0.14)`) after `exposure`; it needs the HDR target like Reinhard.

Verified by FogGradingHemisphereAndSpriteEmission (software checks per pixel, D3D11 mean difference
0.58 against the software renderer). `games/RPG` drives all of them per time of day from Lua.

## Portable surface shader graphs

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
in -65504..65504. Up to 128 nodes and 8 named uniform defaults are supported.
Per-material overrides must name declared uniforms and preserve unspecified
values. Node outputs saturate to -65504..65504; NaN becomes zero. Division by
zero returns zero for that channel. This gives the GPU a finite bounded program
and keeps the CPU reference independent of platform compiler behavior.

The GPU renderer compiles each graph into its own program the first time a
material uses it (`engine/render/GraphSource.cpp`): it takes the mesh and surface
shaders sokol-shdc generated from `Shaders.glsl` (HLSL for D3D11, GLSL ES 3.00 for
WebGL2/GLES3) and replaces the bodies of `SurfaceGraph` and `VertexOffset` with the
graph as straight-line code, one variable per node, with the same saturation and
division rules. Lighting code therefore stays in one place. The generated code
reads its inputs only through the `Graph*`/`VGraph*` helper functions in
`Shaders.glsl`; keep those names when editing it. A graph whose program does not
compile logs a warning and is interpreted by the general shader instead (the
loops in `SurfaceGraph`/`VertexOffset`), which gives the same picture more slowly;
the same happens while a program compiles in the background (the editor). The
interpreter holds graphs of up to 48 nodes (`ShaderGraph::kMaxInterpretedSurfaceNodes`;
a larger one draws as its plain material until its program is ready). It reads the
graph from uniforms with four packed instructions per vec4 (`GraphCode`: opcode +
128 * (a + 48 * (b + 48 * c)), exact in a highp float), which keeps `mesh_fs` inside
the GLES3/WebGL2 minimum of 224 fragment uniform vectors with room for new material
parameters (test `SpotLightRendering` checks the budget). `GpuRenderer::Settings::compileGraphs`
(`Engine::SetGpuCompileGraphs(false)`) keeps every graph on the interpreter to check
it; test `ShaderGraphInterpreterAndLargeGraphs`.
The first frame that uses a graph pays its shader compile.

Three more optional outputs sit next to `color` and `emissive` (the third, `light`, is described in
[Light stage](#light-stage-custom-lighting)):

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
depth of field and the editor selection outline. The displacement is
render-only: physics, `physics.raycast` picking and `boundsMin`/`boundsMax` do not
know about it. Shared vertices need a mesh with enough vertices, so use `plane64`
(see [RENDERING.md](RENDERING.md)) rather than the 4-vertex `plane`.

The shadow map is fitted to the scene bounds without running the vertex stage on
the CPU (as Unreal's `Max World Position Offset Displacement` or Godot's
`extra_cull_margin`): each displaced item's bounds are widened by a margin. The
optional `"maxOffset": <number>` (world units, 0..65504, only with `offset`)
declares the largest distance the graph moves a vertex; without it the margin is
1 world unit. A graph that moves vertices further than its margin can have its
shadow cut where it reaches past every other object of the scene, so declare
`maxOffset` for large displacements. `shader.create`, `shader.check` and
`asset.info` report a declared `maxOffset`.
The same margin drives the GPU renderer's frustum culling: a vertex moved further
than the margin can be culled while its moved position is on screen. For
displacements that only go one way use `"offsetBounds": {"min": [x,y,z], "max":
[x,y,z]}` instead (per world axis, min <= max, not together with `maxOffset`):
boxes grow by exactly that range. A world bent down away from the camera (offset
`-k * |(p - camera).xz|^2` on y) declares `{"min": [0, -k * R^2, 0], "max": [0, 0, 0]}`
for the farthest on-screen distance R; with `maxOffset` its boxes would grow by
hundreds of metres sideways too and nothing would be culled. Ainorix Online
showed the problem: far objects popped in and out (36-45 draw calls instead of
165-212 for the same views). Tested next to the `maxOffset` checks (parsing and bounds).
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

Five uniform names are reserved: a graph that declares `cameraPosition` receives the
rendering view's world position (w = 1), one that declares `lightDirection` receives the
unit vector toward the first directional light (w = 0), and one that declares `playing`
receives 1 while the game is played (a play session exists: running or paused, in the editor,
`oe run`, a packaged game, after `sim.play`/`sim.step`) and 0 while its scene is edited (the
editor's Scene and stopped Game view, `oe render` without `--frames`, effect previews), every
frame and in both renderers. Multiply by `playing` what would be in the way while editing: a
world that bends away from the camera is flat until Play is pressed.
Two more give graphs the screen, as Unity's `_ScreenParams` or Godot's `VIEWPORT_SIZE` do: `screenSize`
receives (width, height, 1/width, 1/height) of the render target in pixels, and `pixelScale` the world size
of one pixel: at view distance d it is `d * pixelScale.x + pixelScale.y` (perspective: x = 2 tan(fov/2) /
height, y = 0; orthographic: x = 0, y = 2 * orthoSize / height). Multiplying an `offset` by that size keeps
it a fixed number of pixels on screen (the pixel outlines of [RENDERING.md](RENDERING.md#outlines) do).
Declared defaults and material overrides for these names are replaced, so view- and
light-dependent graphs follow a moving camera or sun without material edits
They still count toward the eight uniforms. Graphs without these
names are unaffected. Prebuilt web/Android players older than this change leave the
declared defaults in place.

Inputs: uv, world position, world normal, fixed simulation time (broadcast),
baseColor RGBA, and texture(args UV) for the material's base texture. Missing
textures sample white. Both renderers supply these interpolated fragment inputs
to the shared graph contract. In the fragment stage `baseColor` is the surface
color *with* the base texture already applied (material base color x tint x
vertex color x texture sample at the mesh uv); use it alone for a textured
surface, and `texture` only to sample the image at other coordinates. Multiplying
the two squares the texture (a yellow texture turns orange). In the vertex stage
`baseColor` has no texture (see `offset` above).

Operations and argument counts:

| Operation | Args | Behavior |
|---|---:|---|
| constant / uniform | 0 | value scalar/vector, or name declared uniform |
| uv / position / normal / time / baseColor | 0 | Surface input |
| particleAge / particleRandom / particleSpeed | 0 | Normalized age, stable birth random and world speed; zero outside effects |
| particleCustom0 / particleCustom1 / particleCustom2 / particleCustom3 | 0 | Four per-particle signed custom channels; scalar replicated to RGBA, no graph uniform slots. Available in color/emissive/normal and vertex offset; defaults zero outside VFX. |
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
| pow | 2 | a to the power b per channel; 0 where a <= 0 (also for b = 0) |
| length | 1 | Four-channel length, broadcast |
| smoothstep | 3 | Smooth 0..1 ramp of c between edges a and b per channel; equal edges act as step(a, c) |
| noise | 1 | 3D value noise of a.xyz in 0..1 (cells of size 1; the hash13 of the three.js prototype), broadcast |
| fbm | 1 | Four octaves of noise (lacunarity 2.03, gain 0.5, about 0..0.94), broadcast |
| uv / texel / time / sceneColor / sceneBlur | | Post stage inputs, see [Post graphs](#post-graphs) |
| ambient | 0 | Surface fragments: the scene's diffuse ambient light where the normal points (image environment, sky colors or the flat/hemisphere ambient), rgb |
| lightVector / lightColor / lightAttenuation | 0 | Light stage only: unit vector toward the light, its color times intensity, and how much arrives (distance falloff, spot cone, shadow); see [Light stage](#light-stage-custom-lighting) |
| viewDirection | 0 | Surface, every stage: unit vector from the surface toward the camera (xyz, w = 0), like Unity's View Direction; an orthographic camera gives its axis everywhere. Rim light: `pow(1 - dot(normal, viewDirection), k)` |
| objectPosition | 0 | Surface, every stage: the position in the mesh's own space (xyz, w = 1), like Unity's Position (Object): the rest pose of a skinned mesh, before `offset`, so a pattern over it stays on the surface while the object moves or animates. A graph that reads it is drawn unbatched by the GPU renderer (its sprite/mesh batches are in world space) |

### Light stage (custom lighting)

A lit material's graph can shade the surface itself, like Godot's `light()` function or a custom lighting
function in Unity's Shader Graph: give it a `light` output. The engine then shows `color` (the ambient part,
times ambient occlusion) plus, for every light it shades with (each directional light, the point lights and spot
lights in range), the graph's `light` output for that light. The PBR model, image-based reflections and
reflection probes are skipped for that surface; `emissive` still adds.

```json
{"format": "ownengine.shader",
 "nodes": [{"op": "baseColor"}, {"op": "ambient"}, {"op": "multiply", "args": [0, 1]},
           {"op": "normal"}, {"op": "lightVector"}, {"op": "dot", "args": [3, 4]}, {"op": "constant", "value": 0},
           {"op": "max", "args": [5, 6]}, {"op": "lightColor"}, {"op": "lightAttenuation"},
           {"op": "multiply", "args": [7, 9]}, {"op": "multiply", "args": [8, 10]}, {"op": "multiply", "args": [0, 11]}],
 "color": 2, "light": 12}
```

That is Lambert; a toon material bands `dot(normal, lightVector)` instead (`games/Kongal/tools/make_toon.cjs`), and
shadows cut the band through `lightAttenuation` (the sun's shadow map, local shadows of point and spot lights).

- `lightVector`, `lightColor` and `lightAttenuation` exist only in the light stage: `color`, `emissive`, `normal`
  and `offset` must not depend on them (`shader.create`/`shader.check` name the node). Everything else, `ambient`
  and the surface inputs included, may feed both. `baseColor` is the surface color before the graph.
- The light output is added as it is (negative channels count as zero), once per light; `unlit` materials and
  post graphs have no light stage.
- GPU: only the compiled graph program has the light stage (`GraphLightBody` replaces `LightGraph` in mesh_fs). An
  interpreter there would sit inside the light loop, and the D3D compiler unrolls that into minutes of compile
  for every program. While the program compiles in the background, or when it failed (logged), the GPU shades
  the graph's `color` with the standard lighting instead (`PutGraph` sends no light output). The software
  renderer evaluates it with `EvaluateShaderLight` (`Lighting::ShadeGraph`).

Test: `MaterialLightGraph` (Lambert with a cast shadow, the ambient input and a point light, GPU against CPU).

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
packaged graph execution is verified.

## Status

Implemented in both renderers: post graphs, vignette, exposure with Reinhard/ACES tone mapping, bloom, ambient-only SSAO, screen-space reflections, depth of field,
FXAA, distance fog, colour grading and surface shader graphs (colour, emission, normal and vertex offset
outputs, `MeshRenderer.shaderUniforms`, reserved `cameraPosition`/`lightDirection`). The committed web and
Android players include all of them.

- [ ] Execution on WebGL2, Android GLES3 and Linux/EGL of the newer shaders (depth of field, graph vertex
      stage, fog and grading); only D3D11 and the software renderer are verified.

R5 SSAO: 352 Windows tests pass. CameraSsaoContactsAndExclusions verifies ambient-only shading,
perspective/orthographic contacts, unlit and transparent exclusions, fully masked occluders, unchanged
depth/IDs, serialization, nonfinite settings, thread-count determinism and HDR/bloom/DOF target changes.
The 800x500 demo's software/D3D11 mean RGB difference is 0.160/255. Showcase retains its prior hash
72ab07fb213fd42b with SSAO off. CLI renders and the editor Inspector screenshot were visually checked.
Web and both Android players were rebuilt; headless Edge/SwiftShader compiled and linked the mesh
program and rendered the packaged demonstration without WebGL errors. Both Android ELF files retain
16KB LOAD alignment.

- [ ] SSAO rendering on an Android physical device, Linux/EGL and a real browser GPU.
- [ ] Existing skin/vertex-graph execution on minimum-capability WebGL2/GLES3 devices.

R9 SSR: 361 Windows tests pass; after the final constructor-order and format-capability cleanup,
14 camera tests and GpuRendererMatchesSoftware pass again. Perspective/orthographic hits, probe/missing-ray
fallback, rough/unlit exclusion, masked/transparent depth exclusion, unchanged depth/IDs/UI, nonfinite
settings and thread-count determinism are covered. The 800x500 software/D3D11 demonstration's mean RGB
difference is 0.146/255. Showcase retains hash 72ab07fb213fd42b with SSR off. CLI and Inspector screenshots
were visually checked. Web/Android players were rebuilt from d428219; headless Edge/SwiftShader compiled
and linked the mesh/SSR programs and rendered the packaged demo, including SSR/SSAO/DOF/bloom target
transitions without WebGL errors. Both Android ELF files retain 16KB LOAD alignment.

- [ ] SSR on a physical Android device, Linux/EGL and a real browser GPU, including performance.
- [ ] Existing skinned/vertex-graph rendering on minimum-capability WebGL2/GLES3 devices.
