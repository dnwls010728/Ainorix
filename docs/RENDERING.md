# Rendering and assets

The reference renderer is a deterministic, multi-threaded software rasterizer (`engine/render/SoftwareRenderer.cpp`). It needs no GPU, runs headless, and produces bit-identical images on every machine and for any thread count — so frame hashes work as test oracles.

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

- `DirectionalLight`: color, intensity, ambient, `shadows`, `shadowStrength`. The first directional light casts shadows: a 1024² orthographic shadow map fitted to the scene, 3×3 PCF, back faces rendered to avoid acne.
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

The main pass and the shadow pass are split into horizontal bands rendered on worker threads (one band per core, up to 16). Each pixel belongs to exactly one band and sees triangles in the same order, so the result does not depend on the number of threads (`SetMaxRenderThreads(1)` is used in tests to prove it). The Showcase sample renders at 1280×720 in roughly 14 ms on an 8-core desktop.

## Sample

`samples/Showcase`: the Khronos "Fox" glTF (CC0 model, CC-BY rig — see `assets/models/CREDITS.md`) as a player-controlled character with a follow camera, procedural textures, a shadowing sun and two colored point lights.
