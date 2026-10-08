# Camera post-processing and shader materials

`PostProcess` on the active camera applies screen-space effects (exposure and tone mapping, bloom, vignette, depth
of field, FXAA) in both renderers, and `.shader.json` surface graphs give materials programmable color, emission,
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

The shadow map is fitted to the scene bounds without running the vertex stage on
the CPU (as Unreal's `Max World Position Offset Displacement` or Godot's
`extra_cull_margin`): each displaced item's bounds are widened by a margin. The
optional `"maxOffset": <number>` (world units, 0..65504, only with `offset`)
declares the largest distance the graph moves a vertex; without it the margin is
1 world unit. A graph that moves vertices further than its margin can have its
shadow cut where it reaches past every other object of the scene, so declare
`maxOffset` for large displacements. `shader.create`, `shader.check` and
`asset.info` report a declared `maxOffset`.
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
They still count toward the eight uniforms. Graphs without these
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

Implemented in both renderers: vignette, exposure with Reinhard/ACES tone mapping, bloom, depth of field,
FXAA, distance fog, colour grading and surface shader graphs (colour, emission, normal and vertex offset
outputs, `MeshRenderer.shaderUniforms`, reserved `cameraPosition`/`lightDirection`). The committed web and
Android players include all of them.

- [ ] Execution on WebGL2, Android GLES3 and Linux/EGL of the newer shaders (depth of field, graph vertex
      stage, fog and grading); only D3D11 and the software renderer are verified.

