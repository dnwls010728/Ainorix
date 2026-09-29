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

## Editor viewport

The web editor opens a WebSocket (`/api/stream`, localhost origins only) and receives each viewport frame as a JPEG rendered by the display renderer, at device-pixel resolution up to 1920 px wide with the GPU (1280 with software). One frame is in flight at a time, so a slow machine lowers the frame rate instead of queueing stale frames. Without the stream it falls back to polling `GET /api/frame.png`. Picking always uses the software renderer's entity-id buffer.

## Assets

Project files are referenced by project-relative paths. Conventional folders: `assets/models/`, `assets/textures/`, `sounds/`, `scripts/`, `prefabs/`, `scenes/`.

| Command | Use |
|---|---|
| `oe import <project> <file> [--to path]` | Copy an external model/texture/sound into the project (CLI only — the API stays sandboxed to the project) and print its `asset.info` |
| `asset.list {kind?}` | Files by kind: model, texture, audio, script, prefab, scene |
| `asset.info {path}` | Model: vertices, triangles, submeshes/materials, textures, bounds, size **and a scale hint**. Texture: size. Sound: length |
| `asset.generate_texture {path, pattern, size?, cells?, color1?, color2?}` | Procedural PNG: checker, grid, bricks, gradient, noise |
| `asset.reload` | Drop cached models/textures |
| `render.meshes` | Values accepted by `MeshRenderer.mesh` |

Models and textures are cached and **hot-reloaded** when their files change (checked twice a second while playing and before every `sim.step`).

Formats: **glTF 2.0** (`.glb`, `.gltf` with embedded, data-URI or external buffers/images) via cgltf; images **PNG, JPEG, BMP, TGA** via stb_image. Each glTF primitive becomes a submesh with its base color factor and base color texture; node transforms are baked in. Skinned models are shown in their bind pose (skeletal animation is on the roadmap).

## MeshRenderer

| Field | Meaning |
|---|---|
| `mesh` | `cube`, `sphere`, `plane`, `pyramid` or a model path. A missing or broken model renders as an **unlit magenta cube** (and a log warning), so problems are visible in screenshots |
| `color` | Tint multiplied with the material / texture |
| `texture` | Image overriding the model's base color texture (UVs: built-in meshes have 0..1 per face) |
| `shading` | `smooth` (interpolated vertex normals) or `flat` (faceted) |
| `unlit` | Ignore lights and shadows |
| `castShadows` | Contribute to the directional shadow map |

## Lights and shadows

- `DirectionalLight`: color, intensity, ambient, `shadows`, `shadowStrength`. The first directional light casts shadows: an orthographic shadow map fitted to the scene (software 1024², GPU 2048² with hardware comparison filtering), 3×3 PCF, back faces rendered to avoid acne.
- `PointLight`: color, intensity, `range` (quadratic falloff to zero at the range).
- Lighting is per pixel (Lambert). Colors are treated as display values (no gamma/HDR yet).

## Cameras

- `Camera.projection`: `perspective` (`fov`) or `orthographic` (`orthoSize` = half the visible height in meters, for 2D/isometric views).
- `CameraFollow {target, offset, lookOffset, smoothing}`: after physics each frame the entity moves to target + offset and looks at target + lookOffset. `smoothing` is a catch-up rate per second (0 = snap).

## Debug drawing

Lines visible in every view and screenshot, for marking points, paths and areas while debugging:

- API: `debug.draw {lines:[{a,b,color}], boxes:[{center,size,color}], spheres:[{center,radius,color}], seconds?}` (default: until `debug.clear`).
- Lua: `draw.line(a, b, color?, seconds?)`, `draw.box(center, size, color?, seconds?)`, `draw.sphere(center, radius, color?, seconds?)` — default lifetime is one simulated frame, so call them every `onUpdate`.

Collider wireframes are separate: `render.screenshot {colliders:true}` (see PHYSICS.md).

## Performance

GPU: the game window renders at the window's resolution (`project.json` `window.renderScale` 0.25–1 renders the 3D image smaller and upscales it, UI stays sharp) and presents with vsync.

Software: the main pass and the shadow pass are split into horizontal bands rendered on worker threads (one band per core, up to 16). Each pixel belongs to exactly one band and sees triangles in the same order, so the result does not depend on the number of threads (`SetMaxRenderThreads(1)` is used in tests to prove it). The Showcase sample renders at 1280×720 in roughly 14 ms on an 8-core desktop.

## Sample

`samples/Showcase`: the Khronos "Fox" glTF (CC0 model, CC-BY rig — see `assets/models/CREDITS.md`) as a player-controlled character with a follow camera, procedural textures, a shadowing sun and two colored point lights.
