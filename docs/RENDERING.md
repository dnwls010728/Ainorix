# Rendering and assets

Two renderers draw the same scene description (`engine/render/RenderScene.h`: meshes, materials, lights, shadow fit, and UI as quads from `BuildUIQuads`):

| Renderer | Used by | Why |
|---|---|---|
| **Software** (`SoftwareRenderer.cpp`) — the reference | `oe render`, `render.screenshot` (default), `render.pick`, frame hashes, tests | Deterministic, multi-threaded, no GPU: bit-identical images on every machine and for any thread count, so hashes work as test oracles |
| **GPU** (`GpuRenderer.cpp`, sokol_gfx) | Game window (`oe run`, packaged `Name.exe`), web build, editor viewport, `render.screenshot {renderer:"gpu"}` | Real-time: 4× MSAA, 2048² hardware-filtered shadow map, mipmapped anisotropic textures, UI at full window resolution. Pixels differ slightly between GPUs/drivers |

GPU backends: **Direct3D 11** on Windows (hardware, falling back to WARP — the software D3D device built into Windows), **WebGL2** in the browser, **OpenGL ES 3 over EGL** on Linux (headless; Mesa's llvmpipe works without a GPU). Front-ends take `--renderer auto|gpu|software` (`auto` = GPU when available, otherwise software); `engine.info` reports both `renderer` (software) and `displayRenderer`. The test `GpuRendererMatchesSoftware` compares the two (mean channel difference below 1/255 on the Showcase).

## GPU pipeline and shaders

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

Formats: **glTF 2.0** (`.glb`, `.gltf` with embedded, data-URI or external buffers/images) via cgltf; images **PNG, JPEG, BMP, TGA** via stb_image. Each glTF primitive becomes a submesh with its full material (see [Materials](#materials)); static node transforms are baked in. Skinned and rigid animated nodes use a shared model-space palette in both renderers (up to 64 entries per model). Add `Animator` or call `animation.play` to animate TRS channels; `asset.info` lists clips. See [ANIMATION.md](ANIMATION.md) for playback controls and supported interpolation.

## MeshRenderer

| Field | Meaning |
|---|---|
| `mesh` | `cube`, `sphere`, `plane`, `pyramid`, `quad` (1×1 in XY facing +Z) or a model path. A missing or broken model renders as an **unlit magenta cube** (and a log warning), so problems are visible in screenshots |
| `material` | Path to a `.mat.json` applied to every submesh; empty = the model's own glTF materials (default material for built-in meshes). See [Materials](#materials) |
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
oe exec samples/Hello material.create '{"path":"materials/gold.mat.json","values":{"baseColor":"#ffc857","metallic":1,"roughness":0.25}}' --save
oe exec samples/Hello material.set '{"path":"materials/gold.mat.json","values":{"roughness":0.4}}'   # merges into the existing file
oe exec samples/Hello component.set '{"id":"Ring","type":"MeshRenderer","values":{"material":"materials/gold.mat.json"}}' --save
oe exec samples/Hello asset.info '{"path":"materials/gold.mat.json"}'
```

`material.create {path, values?, overwrite?}` writes all fields with defaults plus your `values`; `material.set {path, values}` merges into an existing file. Material files are **hot-reloaded**, also when a texture they use changes.

### How MeshRenderer combines with a material

`MeshRenderer.material` is applied to every submesh. If it is empty, a model uses its own glTF materials and a built-in mesh (`cube`, `sphere`, ...) uses the default material (white, metallic 0, roughness 0.7, opaque). On top of that:

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
- **Shadows**: blended surfaces cast no shadows. `mask` surfaces cast shadows of their full geometry (there is no alpha test in the shadow pass). Double-sided materials put both sides into the shadow map and draw back faces with flipped normals.
- **Picking**: blended surfaces are pickable (`render.pick`, editor click) where they are at least 50% opaque.
- **Limitations**: sorting is per object, so intersecting transparent objects, or one large transparent object around others, can sort wrongly. Split the mesh or use `mask` where that matters.

## Sprites and tilemaps (2D)

`Sprite` and `Tilemap` become the same render items as meshes (a unit quad, or one mesh per tilemap rebuilt when the map changes) with three extra material inputs shared by both renderers: a UV rectangle (sheet frame, flips), an alpha cutoff (texels below it are discarded — no color, depth or pick id; a cutoff of 0 blends the image alpha instead, with `Sprite.opacity` on top) and nearest sampling for pixel art. The GPU shader interpolates UVs with `centroid` so MSAA edge samples never read outside the frame. Sprites/tilemaps are unlit by default and do not cast shadows. See [2D.md](2D.md).

## Lights and shadows

- `DirectionalLight`: color, intensity, ambient, `shadows`, `shadowStrength`. The first directional light casts shadows: an orthographic shadow map fitted to the scene (software 1024², GPU 2048² with hardware comparison filtering), 3×3 PCF, back faces rendered to avoid acne.
- `PointLight`: color, intensity, `range` (quadratic falloff to zero at the range).
- Lighting is per pixel with a physically based model (glTF metallic-roughness): GGX normal distribution, Smith-Schlick geometry, Schlick Fresnel; F0 is 0.04 for dielectrics and the base color for metals, and the diffuse part is `baseColor * (1 - metallic)`. Both renderers use the same math (`Lighting::Shade` in `SoftwareRenderer.cpp`, `mesh_fs` in `Shaders.glsl`).
- Light colors include the factor pi and lighting happens on non-linear (display) colors, clamped, with no tone mapping or HDR yet, so a white diffuse surface lit head-on still shows exactly the light color.
- Ambient = `DirectionalLight.ambient` * (diffuse + environment reflection). There is no skybox or environment map yet: reflections are approximated by a hemisphere that is brighter above (sky) than below (ground). Very smooth metals therefore reflect only this approximation and the highlights of the lights. Ambient occlusion textures darken this ambient term only.
- Emissive is added after lighting (also for unlit materials).

## Cameras

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
the original frame hashes. Bloom, FXAA and custom shader materials remain later P6 milestones;
see [POSTPROCESS.md](POSTPROCESS.md) for controls and verification.

## Debug drawing

Lines visible in every view and screenshot, for marking points, paths and areas while debugging:

- API: `debug.draw {lines:[{a,b,color}], boxes:[{center,size,color}], spheres:[{center,radius,color}], seconds?}` (default: until `debug.clear`).
- Lua: `draw.line(a, b, color?, seconds?)`, `draw.box(center, size, color?, seconds?)`, `draw.sphere(center, radius, color?, seconds?)` — default lifetime is one simulated frame, so call them every `onUpdate`.

Collider wireframes are separate: `render.screenshot {colliders:true}` (see PHYSICS.md).

## Performance

GPU: the game window renders at the window's resolution (`project.json` `window.renderScale` 0.25–1 renders the 3D image smaller and upscales it, UI stays sharp) and presents with vsync.

Software: the main pass and the shadow pass are split into horizontal bands rendered on worker threads (one band per core, up to 16). Triangles use a consistent fill rule (a pixel on a shared edge belongs to exactly one of the two triangles, so there are no gaps or double-drawn pixels). Each pixel belongs to exactly one band and sees triangles in the same order, so the result does not depend on the number of threads (`SetMaxRenderThreads(1)` is used in tests to prove it). The Showcase sample renders at 1280×720 in roughly 14 ms on an 8-core desktop.

## Sample

`samples/Showcase`: the Khronos "Fox" glTF (CC0 model, CC-BY rig — see `assets/models/CREDITS.md`) as a player-controlled character with a follow camera, procedural textures, a shadowing sun and two colored point lights.
