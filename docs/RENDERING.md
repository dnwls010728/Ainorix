# Rendering and assets

Two renderers draw the same scene description (`engine/render/RenderScene.h`: meshes, materials, lights, shadow fit, and UI as quads from `BuildUIQuads`):

| Renderer | Used by | Why |
|---|---|---|
| **Software** (`SoftwareRenderer.cpp`) — the reference | `oe render`, `render.screenshot` (default), `render.pick`, frame hashes, tests | Deterministic, multi-threaded, no GPU: bit-identical images on every machine and for any thread count, so hashes work as test oracles |
| **GPU** (`GpuRenderer.cpp`, sokol_gfx) | Game window (`oe run`, packaged `Name.exe`), web build, editor viewport, `render.screenshot {renderer:"gpu"}` | Real-time: 4× MSAA, 2048² hardware-filtered shadow map, mipmapped anisotropic textures, UI at full window resolution. Pixels differ slightly between GPUs/drivers |

GPU backends: **Direct3D 11** on Windows (hardware, falling back to WARP — the software D3D device built into Windows), **WebGL2** in the browser, **OpenGL ES 3 over EGL** on Linux (headless; Mesa's llvmpipe works without a GPU). Front-ends take `--renderer auto|gpu|software` (`auto` = GPU when available, otherwise software); `engine.info` reports both `renderer` (software) and `displayRenderer`. The test `GpuRendererMatchesSoftware` compares the two (mean channel difference below 1/255 on a lit test scene with a skinned model).

GPU texture/mesh caches follow their CPU owners. Scene rendering evicts expired owners, and the native
editor also calls `GpuRenderer::CollectGarbage` between frames so an Effects/Shader workspace continues
releasing old preview images while Scene/Game are hidden. Resource-pool failures include their names
in Release logs: `sg item 159` is `VIEW_POOL_EXHAUSTED`, the sokol view pool limit, not a particle-field
validation error. Pool sizes are fixed; preview lifetimes must stay bounded rather than increasing them.

D3D11 shader compiler diagnostics are logged with their actual text, including in Release builds.
Sokol emits `D3D11_SHADER_COMPILATION_OUTPUT` (item 40) first as a marker and then sends the diagnostic
at info level; the engine preserves the text as a warning instead of dropping it. Keep the shaders
free of them: a compare/implicit-gradient sample inside a loop with a runtime exit (X3570) or a write
to `vec[i]` inside a loop (X3550) makes the compiler unroll the loop, which bloats the program and
slows every compile. The directional-light loop therefore takes the shadow lookup computed before it,
and `GraphFinite` clamps the four channels without a loop.

### Shader compiling and the cache

On D3D11 the generated header holds HLSL source, compiled at run time. The device compiles it
itself (`GpuDevice::CompileShader`, d3dcompiler_47 with the flags sokol uses), so GpuRenderer can:

- compile the ten built-in programs in parallel when it starts;
- keep compiled programs in `GpuRenderer::Settings::shaderCacheDir`, one `<hash>.bin` per stage keyed
  by compiler, target, entry point and full source (an edited shader or graph gets a new file; the
  folder is trimmed to the newest 400 files on start);
- with `asyncShaders`, compile shader graph programs on worker threads; until a program is ready its
  materials draw through the interpreter in the standard program (the same image, slower shader).

The editor uses `<project>/.oe/shader-cache` (async in the window, synchronous for
`--screenshot`); games and tests compile on first use without a cache. A game that cannot afford
that first use in the middle of play (a skill effect: a stalled frame per new graph) loads ahead with
`game.preload(paths)` / `asset.preload {paths}`: an effect's, material's or graph's programs and
scene pipelines are built, textures and meshes uploaded, sounds decoded, at once or when the GPU
renderer is enabled (`Engine::Preload`, `GpuRenderer::Preload`; test `AssetPreload`). GL backends (web, Android,
EGL) have no `CompileShader`, so sokol_gfx compiles their GLSL as before. Measured on `games/RPG`
(seven graphs): the first editor frame came 3.7 s after loading the scene before; now 0.65 s on a
cold cache and 0.29 s on a warm one.

### Effect vertex streams

Effect meshes can carry optional RGBA vertex colors and normalized particle age/random/world speed.
Both renderers interpolate them with perspective-correct weights; absent colors are white and absent
particle inputs are zero. Unbatched effects supply the same inputs through a dedicated per-draw value
and keep shared GPU meshes rather than creating a GPU buffer per particle. `RenderItem.materialOverride` binds emitter/trail materials like mesh
materials. Legacy billboards keep the shared-quad path; batching is opt-in because transparent emitter
groups cannot interleave their particles with other draws. See [VFX.md](VFX.md) for limits and shader inputs.

## GPU pipeline and shaders

ParticleEmitter uses shared camera-facing quads in both renderers, with lifetime
size/color/opacity interpolation, texture sheet frames and stable transparent
ordering. Both 2D and 3D emission modes render without lighting or cast shadows.
See [PARTICLES.md](PARTICLES.md) for the rendering and simulation contract.
Billboards can rotate, follow their velocity and play flipbooks; `Trail` adds a camera-facing
ribbon mesh rebuilt each frame. Both are unlit draw items shared by the two renderers (docs/PARTICLES.md).

Frame: **shadow pass** (depth only, first directional light) → **scene pass** (MSAA; meshes, then grid/collider/debug lines) → **selection mask** (editor outline) → **composite pass** into the window or an offscreen image (+ UI quads). Screen-space effects (tone mapping, bloom, color grading, FXAA, …) belong in the composite pass or in extra passes between the scene and composite passes.

Shaders live in one file, `engine/render/shaders/Shaders.glsl` (sokol-shdc annotated GLSL). `tools/shaders/compile_shaders.sh` / `.bat` regenerates `Shaders.glsl.h` (HLSL for D3D11, GLSL ES 3.0 for WebGL2/GLES3, desktop GLSL); the generated header is checked in, so building the engine needs no shader tools. Get `sokol-shdc` from https://github.com/floooh/sokol-tools-bin (`bin/<os>/`). Conventions: matrices from `core/Math.h` are column-major with OpenGL clip space (vertex shaders use `@hlsl_options fixup_clipspace`); offscreen images are sampled with `gl_FragCoord / target size`, which has the same orientation on every backend. Lighting math matches the software renderer; keep the two in sync when changing it.

GPU limits: 4 directional lights and 16 point lights per frame (the software renderer has no limit). Meshes and textures are uploaded once and dropped when the asset manager releases them (hot reload).

## Editor views

The editor's Scene and Game views are drawn by the GPU renderer into textures (`GpuRenderer::RenderToTexture`) that the editor UI shows directly - no readback or encoding. Picking always uses the software renderer's entity-id buffer (`render.pick`).

## Assets

Project files are referenced by project-relative paths. Conventional folders: `assets/models/`, `assets/textures/`, `sounds/`, `scripts/`, `prefabs/`, `scenes/`.

| Command | Use |
|---|---|
| `oe import <project> <file> [--to path]` | Copy an external model/texture/sound into the project (CLI only — the API stays sandboxed to the project) and print its `asset.info` |
| `asset.list {kind?}` | Files by kind: model, texture, material, font, audio, script, prefab, scene |
| `asset.info {path}` | Model: vertices, triangles, submeshes (each with a `material` index), a `materials` array, bounds, size **and a scale hint**, joint count and animation `clips` (name, duration in seconds, channel count). Material: its values + alphaMode. Texture: size. Sound: length |
| `asset.generate_texture {path, pattern, size?, cells?, color1?, color2?}` | Procedural PNG: checker, grid, bricks, gradient, noise |
| `asset.reload` | Drop cached models/textures |
| `render.meshes` | Values accepted by `MeshRenderer.mesh` |

Models and textures are cached and **hot-reloaded** when their files change (checked twice a second while playing and before every `sim.step`).

Formats: **glTF 2.0** (`.glb`, `.gltf` with embedded, data-URI or external buffers/images) via cgltf and **FBX** (`.fbx`, binary or ASCII; see [FBX models](#fbx-models)) via ufbx; images **PNG, JPEG, BMP, TGA** via stb_image. Each glTF primitive becomes a submesh with its full material (see [Materials](#materials)); static node transforms are baked in. `COLOR_0` vertex colors (RGB or RGBA) multiply the material color in both renderers and reach shader graphs through `baseColor`. Skinned and rigid animated nodes use a shared model-space palette in both renderers (up to 1024 entries per model; a model with more than 64 is cut at load time into draw ranges of at most 64 joints each, see [ANIMATION.md](ANIMATION.md#model-contract)). Add `Animator` or call `animation.play` to animate TRS channels; `asset.info` lists clips (a model also takes the clips of the files listed in `<model file>.clips.json`, see ANIMATION.md). See [ANIMATION.md](ANIMATION.md) for playback controls and supported interpolation.

### FBX models

`.fbx` files load directly, like glTF: `oe import <project> hero.fbx`, then `MeshRenderer {mesh: "assets/models/hero.fbx"}`.
The loader (`engine/assets/Fbx.cpp`) fills the same `Mesh` as the glTF loader, so everything after loading is shared.

- **Space**: the file's axes and unit are converted to right-handed, Y up, meters (a model authored in centimeters
  arrives 100 times smaller than its raw numbers; `asset.info` reports the size). V is flipped to the top-left origin.
- **Geometry**: polygons are triangulated and equal corners merged; missing normals are generated. Geometric
  transforms, pivots and scale-inheritance modes are resolved into plain TRS nodes (helper nodes where needed).
- **Skins and animation**: the first skin of a mesh, four strongest influences per vertex. Meshes that bind the same
  bones share palette entries and unused clusters take none; the palette holds up to 1024 entries (game characters with a few hundred bones load). Every
  animation stack becomes a clip of linear TRS keys (Euler curves are resampled at 30 Hz and reduced); clips without
  keys are skipped. Blend shapes load (the full-weight target of each channel) and are weighted by `MeshRenderer.blendShapes`, see [ANIMATION.md](ANIMATION.md#blend-shapes); clips do not key them.
- **Materials**: base color, metallic, roughness, emission, opacity, double-sided and unlit come from the file
  (Lambert/Phong values are converted). A base color image replaces the color. Separate roughness and metalness
  images are not combined: use a `.mat.json` material for those.
- **Textures**: embedded media first; else the file's relative path beside the model, else just its file name in the
  model's folder. Absolute paths stored by the exporter are never opened. Only PNG, JPEG, BMP and TGA decode; other
  formats (PSD, TIFF, DDS) are reported in the log and the material keeps its color.

## MeshRenderer

| Field | Meaning |
|---|---|
| `mesh` | `cube`, `sphere`, `plane`, `plane64` (the same 1×1 plane split into 64×64 cells, for vertex offsets), `pyramid`, `quad` (1×1 in XY facing +Z) or a model path. A missing or broken model renders as an **unlit magenta cube** (and a log warning), so problems are visible in screenshots |
| `material` | Path to a `.mat.json` applied to every submesh; empty = the model's own glTF materials (default material for built-in meshes). See [Materials](#materials) |
| `materials` | Material files per material of a model, in the order `asset.info` lists its `materials` (each has the `name` from the source file): `["materials/face.mat.json", "", ...]`. An empty or missing entry keeps `material`, or the model's own when that is empty. `shaderUniforms` belong to `material` only |
| `blendShapes` | Weights of the model's blend shapes (morph targets), `{"E_Close": 1}`; names from `asset.info`. See [ANIMATION.md](ANIMATION.md#blend-shapes) |
| `parentPose` | Draw this model in the pose of the parent entity, which must show the same model: a second layer of a character (an outline shell, a glow) follows the parent's Animator, `BoneIK` goals and `SpringBone`s |
| `color` | Tint multiplied with the material's base color / texture |
| `texture` | Image overriding the material's base color texture (UVs: built-in meshes have 0..1 per face) |
| `opacity` | 0..1; below 1 the object is drawn transparent (see [Transparency](#transparency)) |
| `shading` | `smooth` (interpolated vertex normals) or `flat` (faceted) |
| `unlit` | Ignore lights and shadows (forces unlit whatever the material says) |
| `castShadows` | Contribute to the directional shadow map |

## Materials

A material describes how a surface looks: the glTF 2.0 **metallic-roughness** model. Materials come from two places: glTF models bring their own (loaded completely, see below), and you can write **material files** (`*.mat.json`, asset kind `material`) and point a `MeshRenderer` at them.

A material file is a JSON object with any of the fields below (missing fields keep their defaults; `"format": "ownengine.material"` is optional). Texture fields are project paths.

| Field | Default | Meaning |
|---|---|---|
| `baseColor` | white | Albedo, `[r,g,b]` 0..1 or `"#rrggbb"` |
| `opacity` | 1 | Alpha 0..1. Below 1 needs `alphaMode: "blend"` (set automatically by `material.create` when you give `opacity` < 1 without an `alphaMode`) |
| `baseTexture` | none | Image multiplied with `baseColor` (its alpha with `opacity`) |
| `metallic` | 0 | 0 = dielectric (plastic, wood, stone), 1 = metal |
| `roughness` | 0.7 | 0 = mirror-smooth, 1 = fully rough |
| `metallicRoughnessTexture` | none | glTF layout: green = roughness, blue = metallic, multiplied with the two factors |
| `normalTexture` | none | Tangent-space normal map, +Y up (OpenGL / glTF convention) |
| `normalScale` | 1 | Strength of the normal map |
| `occlusionTexture` | none | Ambient occlusion (red channel); darkens the ambient light only |
| `occlusionStrength` | 1 | 0..1 |
| `emissive` | black | Light the surface gives off, `[r,g,b]` |
| `emissiveIntensity` | 1 | Multiplier for `emissive` (can exceed 1) |
| `emissiveTexture` | none | Image multiplied with `emissive` |
| `alphaMode` | `opaque` | `opaque`, `mask` (cut out below `alphaCutoff`) or `blend` (transparent) |
| `alphaCutoff` | 0.5 | Threshold for `mask` |
| `doubleSided` | false | Draw back faces too (leaves, cloth, glass panes) |
| `unlit` | false | Ignore lights: color = base (+ emissive) |
| `pixelArt` | false | Nearest-neighbour texture sampling |
| `tiling` | `[1,1]` | UV repeat |
| `offset` | `[0,0]` | UV offset |

The one-line docs for every field are also in the `invalid_material` error hint. Examples:

```json
// materials/gold.mat.json — polished metal
{"baseColor": "#ffc857", "metallic": 1, "roughness": 0.25}

// materials/glass.mat.json — see-through, glossy
{"baseColor": [0.7, 0.9, 1.0], "opacity": 0.35, "alphaMode": "blend", "roughness": 0.05, "doubleSided": true}

// materials/neon.mat.json — glowing sign
{"baseColor": [0.05, 0.05, 0.05], "emissive": "#ff2bd6", "emissiveIntensity": 3}

// materials/floor.mat.json — normal-mapped tiled floor
{"baseTexture": "assets/textures/floor_color.png", "normalTexture": "assets/textures/floor_normal.png",
 "metallicRoughnessTexture": "assets/textures/floor_orm.png", "roughness": 1, "tiling": [4, 4]}
```

Create and edit them with commands (both validate the values; errors are `invalid_material` with a hint listing the field names, `already_exists`, or `invalid_path` unless the path ends in `.mat.json`):

```bash
oe exec <project> material.create '{"path":"materials/gold.mat.json","values":{"baseColor":"#ffc857","metallic":1,"roughness":0.25}}' --save
oe exec <project> material.set '{"path":"materials/gold.mat.json","values":{"roughness":0.4}}'   # merges into the existing file
oe exec <project> component.set '{"id":"Ring","type":"MeshRenderer","values":{"material":"materials/gold.mat.json"}}' --save
oe exec <project> asset.info '{"path":"materials/gold.mat.json"}'
```

`material.create {path, values?, overwrite?}` writes all fields with defaults plus your `values`; `material.set {path, values}` merges into an existing file. Material files are **hot-reloaded**, also when a texture they use changes.

### How MeshRenderer combines with a material

`MeshRenderer.shaderUniforms` sets values for the material's shader graph uniforms on this entity only
(scripts may change them every frame); see [POSTPROCESS.md](POSTPROCESS.md#portable-surface-shader-graphs).

A shader graph can also bend the surface: its optional `offset` output is added to every vertex's world position in the vertex stage (shadows, depth of field and the selection outline follow it; physics and picking do not), and its `normal` output replaces the lighting normal per fragment. Use the `plane64` built-in mesh for a flat surface with enough vertices; see the same POSTPROCESS.md section; `games/RPG` uses `offset` for swaying grass and trees.

`MeshRenderer.material` is applied to every submesh, except those whose material has an entry in `MeshRenderer.materials` (a character with a file per part: face, hair, clothes). If it is empty, a model uses its own glTF materials and a built-in mesh (`cube`, `sphere`, ...) uses the default material (white, metallic 0, roughness 0.7, opaque). On top of that:

| Field | Effect |
|---|---|
| `color` | Tints (multiplies) the material's base color |
| `texture` | Overrides the base texture |
| `opacity` | 0..1; below 1 makes the object transparent whatever the material says |
| `unlit` | Forces unlit |

### glTF materials

glTF models load their full materials: base color factor including alpha, base color / metallic-roughness / normal / occlusion / emissive textures, metallic and roughness factors (glTF default 1 / 1 when a material exists), `alphaMode` and cutoff, `doubleSided`, `KHR_materials_emissive_strength` and `KHR_materials_unlit`. Old specular-glossiness materials fall back to their base color with a warning. Tangents for normal mapping are computed from the UVs. `asset.info` on a model lists each material (base color, opacity, metallic, roughness, alphaMode, which textures it has, doubleSided, unlit); each submesh refers to its material by index.

### Transparency

- **Order**: opaque and `mask` surfaces are drawn first; blended surfaces follow, sorted back to front by the distance from the camera to each object's bounds center (ties by entity id).
- **Depth**: blended surfaces test against depth but do not write it. `mask` surfaces write depth (pixels below `alphaCutoff` are discarded).
- **Shadows**: blended surfaces cast no shadows. `mask` surfaces that have a base texture or a shader graph are alpha-tested in the shadow pass of both renderers, so cut-out materials (glTF `alphaMode: mask` leaves, fences) shadow only where they are drawn. Double-sided materials put both sides into the shadow map and draw back faces with flipped normals.
- **Picking**: blended surfaces are pickable (`render.pick`, editor click) where they are at least 50% opaque. `render.pick` also unprojects the depth buffer: `point` (world position of the surface at the pixel), `normal` (from the neighbouring pixels of the same entity, facing the camera) and `distance`; no `point` where nothing is drawn. The editor's Scene right-click uses it for "this spot".
- **Limitations**: sorting is per object, so intersecting transparent objects, or one large transparent object around others, can sort wrongly. Split the mesh or use `mask` where that matters.

## Sprites and tilemaps (2D)

`Sprite` and `Tilemap` become the same render items as meshes (a unit quad, or one mesh per tilemap rebuilt when the map changes) with three extra material inputs shared by both renderers: a UV rectangle (sheet frame, flips), an alpha cutoff (texels below it are discarded — no color, depth or pick id; a cutoff of 0 blends the image alpha instead, with `Sprite.opacity` on top) and nearest sampling for pixel art. The GPU shader interpolates UVs with `centroid` so MSAA edge samples never read outside the frame. Sprites/tilemaps are unlit by default. Tilemaps never cast shadows; a sprite casts one only with `castShadows` (lit, cut-out), shaped by its image through the shadow-pass alpha test. `Sprite.material` swaps the default sprite material for a `.mat.json` (the sprite's `texture` stays the base image, its cutoff/blend/lit/tint/emission still apply), so a shader graph can sway the quad with `offset` (graph `uv` includes the frame, v = 0 at the image top) or a material can set `fog: false`; a missing material renders magenta. A Sprite is always drawn along its entity rotation; facing the camera is a project script (the billboard loop in `games/RPG/scripts/game.lua`). See [2D.md](2D.md#sprites-in-a-3d-world-hd-2d).

## Lights and shadows

- `DirectionalLight`: color, intensity, ambient, `shadows`, `shadowStrength`, `shadowDistance`. The first directional light casts shadows: an orthographic shadow map fitted to the scene (software 1024², GPU 2048² with hardware comparison filtering), 3×3 PCF, back faces rendered to avoid acne. `shadowDistance` > 0 fits the map to what the camera sees up to that distance instead (a sphere around that part of the view frustum, moved in whole texels so shadows do not shimmer; its depth range reaches back over the scene so casters between the sun and the view still cast; the GPU shadow pass skips casters outside it). Large scenes get sharper shadows; a scene smaller than that part keeps the whole-scene fit. Surfaces beyond the distance are unshadowed.
- `PointLight`: color, intensity, `range` (quadratic falloff to zero at the range), optional `shadows` (six faces).
- `SpotLight`: color, intensity, radial `range`, `innerAngle` and `outerAngle` (half-angles in degrees),
  aimed along rotated local -Z. Smoothstep soft edge; eight per frame, optional `shadows` (one cone map).
  See [LIGHTING.md](LIGHTING.md).
- Local shadows: two in-view/nearest point or spot lights per view, 512-square maps in a 2048-square atlas;
  same face selection, perspective bias and 3x3 PCF in both renderers. Shadow flags default to false.
- Lighting is per pixel with a physically based model (glTF metallic-roughness): GGX normal distribution, Smith-Schlick geometry, Schlick Fresnel; F0 is 0.04 for dielectrics and the base color for metals, and the diffuse part is `baseColor * (1 - metallic)`. Both renderers use the same math (`Lighting::Shade` in `SoftwareRenderer.cpp`, `mesh_fs` in `Shaders.glsl`).
- Light colors include the factor pi and lighting happens on non-linear (display) colors, clamped, with no tone mapping or HDR yet, so a white diffuse surface lit head-on still shows exactly the light color.
- Hemisphere ambient: with `DirectionalLight.hemisphere` the ambient light is `groundAmbient` for faces pointing down, `ambient` for faces pointing up, blended by `normal.y * 0.5 + 0.5` (outdoor scenes: blue sky above, warm bounce below). Off by default (one flat ambient color).
- Environment lighting (`Skybox` component, one per scene; design and limits in [ENVIRONMENT.md](ENVIRONMENT.md)): a sky that lights the scene, is reflected by it and drawn behind it. Either three colors (`sky` zenith, `horizon`, `ground`: an analytic gradient) or an equirectangular panorama (`texture`: `.hdr`, which keeps light brighter than white, or `.png`/`.jpg`; turned by `rotation`), with `intensity` and a `reflections` strength. With one, image-based lighting replaces the flat ambient: diffuse light comes from the whole sky (by the normal), and smooth/metallic surfaces reflect it along the reflection vector, blurrier as they roughen (an image is blurred level by level when it loads, so roughness picks a level) — a chrome ball shows the sky, a wet floor catches the horizon. `background` draws it behind the scene; the three-color sky adds a soft sun disc toward the first directional light (`sun`, `sunSize`). Both renderers run the same math on the same data (`RenderScene.h` / `Environment.h` and `mesh_fs` / `sky_fs` in `Shaders.glsl`; keep them identical).
- Local reflections (`ReflectionProbe` component; [ENVIRONMENT.md](ENVIRONMENT.md)): a picture of the scenery taken from the entity's position, used instead of the sky by the surfaces inside its box (`size`, `offset`; fading out over `blend` meters outside), so a room reflects its own walls and is ambient-lit by them (`ambient`). `boxProjection` keeps the reflection of a wall on the wall as the viewer moves. The picture is taken by the software renderer when it is first needed (so both renderers use the same one) and again after edits while stopped, at play start, or on `probe.bake` / Lua `game.bakeProbes()`; moving bodies, characters, animated models and effects are left out. Up to four probes per frame, blended per pixel. `probe.state {id, out}` writes a probe's picture to look at.
- Without a `Skybox`, ambient stays as before: `DirectionalLight.ambient` * (diffuse + a hemisphere reflection brighter above than below). Very smooth metals then reflect only that approximation and the highlights of the lights. Ambient occlusion textures darken the ambient term only.
- Emissive is added after lighting (also for unlit materials). `Sprite.emission` (lit sprites) turns the sprite's own image into emission at that fraction, so billboards seen against the light at night do not go black.
- Distance fog (`PostProcess.fog`) is applied per surface after emission; materials with `"fog": false` skip it. See [POSTPROCESS.md](POSTPROCESS.md#fog-color-grading-and-aces).

## Cameras

Directional shadows optionally use `DirectionalLight.shadowCascades` (1 by default, 2..4 for cascades)
and `shadowDistance` for the far reach. Cascade slices are texel-snapped and blend over the last 10% of
each interval; four 1024-square maps share a 2048-square GPU atlas. See [LIGHTING.md](LIGHTING.md).

- `Camera.projection`: `perspective` (`fov`) or `orthographic` (`orthoSize` = half the visible height in meters, for 2D/isometric views).
- `CameraFollow {target, offset, lookOffset, smoothing, useBounds, boundsMin, boundsMax}`: after physics each frame the entity moves to target + offset and looks at target + lookOffset. `smoothing` is a catch-up rate per second (0 = snap). With `useBounds` the position is clamped to the box and the rotation is left alone (2D side-scrollers).

## Camera post-processing

`PostProcess` on the active Camera provides optional screen effects. The first
milestone implements `vignette` (strength 0..1), `vignetteRadius` and
`vignetteSoftness` in both software and GPU renderers, before UI and selection
outlines. Defaults leave existing frames unchanged; picking and depth buffers
are unaffected. Free/editor Scene cameras do not inherit game-camera effects.
Exposure and optional Reinhard tone mapping preserve HDR lighting/emissive values
before conversion to display color in both renderers. Neutral settings retain
the original frame hashes. Optional bloom extracts and blurs HDR highlights before
tone mapping in both renderers. Optional depth of field (`dofRadius`, `dofFocus`,
`dofRange`, `dofFalloff`) blurs surfaces by their view depth before bloom. Distance fog, ACES tone mapping and color grading
(saturation, contrast, shadow/highlight tints) are further optional fields. FXAA and custom shader materials remain later P6 milestones;
see [POSTPROCESS.md](POSTPROCESS.md) for controls and verification.

## Debug drawing

Lines visible in every view and screenshot, for marking points, paths and areas while debugging:

- API: `debug.draw {lines:[{a,b,color}], boxes:[{center,size,color}], spheres:[{center,radius,color}], seconds?}` (default: until `debug.clear`).
- Lua: `draw.line(a, b, color?, seconds?)`, `draw.box(center, size, color?, seconds?)`, `draw.sphere(center, radius, color?, seconds?)` — default lifetime is one simulated frame, so call them every `onUpdate`.

Collider wireframes are separate: `render.screenshot {colliders:true}` (see PHYSICS.md).

## Performance

Render interpolation: the simulation runs fixed 1/60 s steps while a window (and the editor's Game view) draws at
the monitor's rate, so each drawn frame puts every Transform between its previous and its current step by the time
left in the accumulator (`Engine::BeginInterpolatedRender` / `EndInterpolatedRender`, display only; the simulated
values are restored right after drawing). Moves longer than 3 m in one step snap instead of streaking. Tools that
step exactly (`sim.step`, `oe render`, screenshots, tests) draw the simulated state, so hashes are unchanged.
Test: `RenderInterpolation`.

GPU: the game window renders at the window's resolution (`project.json` `window.renderScale` 0.25–1 renders the 3D image smaller and upscales it, UI stays sharp) and presents with vsync. Frame-time measurement and the optimization work log: [RENDER_PERF.md](RENDER_PERF.md).

Software: the main pass and the shadow pass are split into horizontal bands rendered on worker threads (one band per core, up to 16). Triangles use a consistent fill rule (a pixel on a shared edge belongs to exactly one of the two triangles, so there are no gaps or double-drawn pixels). Each pixel belongs to exactly one band and sees triangles in the same order, so the result does not depend on the number of threads (`SetMaxRenderThreads(1)` is used in tests to prove it). A small lit scene with a skinned model renders at 1280×720 in roughly 14 ms on an 8-core desktop.

## Example game

`games/RPG`: an HD-2D village with a generated voxel terrain (vertex colours, cloud-shadow graph), modelled houses, swaying grass and trees, a sky dome with clouds and stars, mountain backdrops, stylised water, particles and a day/night cycle driving the sun, hemisphere ambient, fog and colour grading; see [RPG.md](RPG.md).

Free-camera overrides in `render.screenshot` and `render.pick` accept `projection: "orthographic"` with positive `orthoSize` (half the visible height in world units). Omitted projection remains perspective; free cameras use clip distances 0.05 and 1000. The editor uses this override for 2D picking, matching its orthographic Scene view.
