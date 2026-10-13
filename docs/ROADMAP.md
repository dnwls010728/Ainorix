# OwnEngine feature roadmap

The order in which features are built. Stages are sorted by dependency, so an earlier stage is the base of a later
one. Every feature is exposed through the command API and, wherever possible, made checkable by an AI itself
(screenshots, query commands).

## How one feature is added

```
1. Design   : components / commands (names, argument schema, error codes + hint)
2. Code     : Components.h / Systems.cpp / Commands.cpp
3. Test     : a case in tests/tests.cpp
4. Check    : oe render ... --out shot.png -> look at the image
5. Docs     : oe api --markdown > docs/API.md
6. Editor   : mostly automatic through reflection; edit engine/editor/ only when a dedicated UI is needed
```

---

## Stage 1 — Game logic: scripting ✅ done

Game logic has to be written and changed without rebuilding C++ before any later feature is useful.

- Lua 5.4 built in (source vendored, nothing to install)
- `Script` component: `onStart`, `onUpdate(dt)`, `onCollision(other)` callbacks
- Engine API for scripts: find/create/delete entities, read/write components, input, log
- Hot reload when a file changes
- Commands: `script.eval` (run on the spot), `script.reload`
- Script errors appear in the log and in API errors as file:line + message

**Done when**: the old `Rotator` / `PlayerController` can be written identically as Lua scripts

## Stage 2 — Collision and physics ✅ done (Jolt Physics)

- `Collider` component: box, sphere (optional trigger)
- `RigidBody` component: velocity, gravity, mass, fixed or not
- Collision detection with simple response, trigger enter/exit events -> script callbacks
- Commands: `physics.raycast`, `physics.overlap`
- Editor: collider wireframes

**Done when**: the player is stopped by walls and touching a coin raises an event

Added ✅ **2D physics (Box2D 3.1)**: `Collider2D` (box, circle, capsule, polygon, edge/loop, one-way, 16 collision
layers) / `RigidBody2D` / `CharacterBody2D` (platformer and top-down, slopes, moving platforms, pushing). Events and
queries are merged with the Jolt world. Tileset files + autotiling (16/47 patterns) + per-tile collision (one-way,
slopes), editor tile brush — [2D.md](2D.md)

## Stage 3 — Gameplay building blocks ✅ done

- **Prefabs**: save a group of entities to a file and instantiate it, field overrides (`prefab.create`, `prefab.instantiate`)
- **Scene changes**: load another scene from a script, data that survives between scenes
- **In-game UI**: text, images, buttons (screen coordinates) — for score and HUD
- **Audio**: sound effects, looping music, volume (WAV first)
- **Timers / events**: delayed calls, messages between entities

**Done when**: the sample "coin collecting game" is complete — movement, collision, score UI, sound, next scene on clear

## Stage 4 — Assets and rendering quality ✅ done

- **Asset management**: the `assets/` folder, listing and query commands (`asset.list`, `asset.info`)
- **Mesh import**: glTF (.glb), FBX (.fbx)
- **Textures**: PNG decoder, UV mapping, material component (color + texture)
- **Shading**: smooth shading (vertex normals), point lights, simple shadows
- **Camera**: orthographic projection, follow component
- **Debug drawing**: lines, boxes and spheres from scripts and the API

**Done when**: an external glTF character model is shown with its texture

## Stage 5 — Editor features (native editor ✅)

`oe editor` opens the **native editor** (Dear ImGui docking + ImGuizmo, in the engine process, GPU-textured viewports).
The web editor was removed. Details: [EDITOR.md](EDITOR.md)

- ~~Viewport gizmo: move / rotate / scale~~ ✅ (Q/W/E/R, local/world, snapping)
- ~~Hierarchy: drag to re-parent, multi-select~~ ✅ (duplicate Ctrl+D, clipboard copy/paste Ctrl+C/Ctrl+V — P7)
- ~~Asset browser: asset list, drag into the scene~~ ✅ (+ import by dropping files on the window)
- ~~Script editor (code editor + error marks)~~ ✅ (syntax highlighting, line numbers, find/replace, checks while typing)
- ~~Showing AI work: highlight entities an agent changed~~ ✅ (notices + hierarchy marks, history panel — P7)
- ~~Prefab edit mode~~ ✅ (P7: `prefab.edit/save/close/state`)
- ~~Native file dialogs, asset thumbnails~~ ✅ (P7; dialogs on Windows only) — several windows (ImGui multi-viewport) are not implemented
- macOS native editor: AppKit window (event mode) + Metal `GpuDevice` — the editor code runs unchanged ([PLATFORMS.md](PLATFORMS.md))

## Stage 6 — Advanced features

- ~~glTF skeletal animation~~ ✅ (P2, [ANIMATION.md](ANIMATION.md)) — transform keyframe animation is not implemented
- ~~Particle system~~ ✅ (P3, [PARTICLES.md](PARTICLES.md))
- ~~Hardware renderer~~ ✅ `GpuRenderer` on sokol_gfx (Windows D3D11, Web WebGL2, Linux GLES3) — same interface as the software renderer
- GPU effects: custom shader materials, post-processing (tone mapping, bloom, FXAA, vignette) — implemented; Android device verification remains (P6, [POSTPROCESS.md](POSTPROCESS.md))
- ~~Navigation~~ ✅ navigation mesh for 3D scenes ([NAVIGATION.md](NAVIGATION.md)) — grid path finding over tilemaps (P8) is not implemented

## Stage 7 — More platforms

After the features are reasonably complete. Cheapest first:

1. ~~Web (Emscripten)~~ ✅ WebGL2 + WebAudio, `oe package --web`
2. macOS -> iOS (Metal) — plan and work log: [APPLE.md](APPLE.md) (without a Mac: GitHub Actions macOS runners + rcodesign)
3. ~~Android (NDK, touch input)~~ ✅ NativeActivity + GLES3 + AAudio, `oe package --android` (APK written and signed) — physical-device verification remains, [ANDROID.md](ANDROID.md)
4. Linux desktop
5. Consoles (Switch / PlayStation) — once the SDKs are available

How to port: [PLATFORMS.md](PLATFORMS.md).

---

## Next tasks (by priority, unimplemented first)

Stages 1–4 are done (scripting, physics, gameplay building blocks, assets and rendering). Also done: GPU
renderer (D3D11 / WebGL2 / GLES3), web packaging, Android packaging, the UI rework (TrueType fonts including Korean,
layouts), 2D (Box2D physics, tilesets and autotiling, tile brush), PBR materials, the native editor.

**Rule for agents**: start from the **first unchecked item** below. Before starting, read [DESIGN.md](DESIGN.md)
(invariants, definition of done, hand-over §5). For each item, first create an `Implementation status (work log)`
checklist in the feature's doc (create the doc if there is none; example: [ANDROID.md](ANDROID.md)), then commit and
push per milestone and tick its line. One item = one branch/PR. When done, change `[ ]` to `[x]` in this list and
leave a new line for every check that could not be made. If the order has to change, write the reason here.

### Done

| Item | Where | Remaining checks |
|---|---|---|
| P1 Persistent save data | [SAVE.md](SAVE.md) | Android save/restart on a device; native Linux run |
| P2 Skeletal animation (glTF) | [ANIMATION.md](ANIMATION.md) | Android device, Linux/EGL |
| P3 Particle system (now asset-based VFX) | [PARTICLES.md](PARTICLES.md), [VFX.md](VFX.md) | Android device, Linux/EGL |
| P4 Gamepad input | [INPUT.md](INPUT.md) | Real controllers on Windows, web and Android |
| P5 Web multi-touch | [TOUCH.md](TOUCH.md) | Real mobile browsers |
| P6 GPU effects: shader graphs, tone mapping, bloom, DOF, FXAA, fog, grading | [POSTPROCESS.md](POSTPROCESS.md) | WebGL2/GLES3/EGL execution of the newer shaders |
| P7 Editor improvements | [EDITOR.md](EDITOR.md) | Native file chooser by hand; Linux editor |
| R1 Environment: `Skybox`, image-based lighting, `ReflectionProbe` | [ENVIRONMENT.md](ENVIRONMENT.md) | Android device |
| R2 Spotlight: soft cone, both renderers, editor cone guide | [LIGHTING.md](LIGHTING.md) | Android device; Linux/EGL; existing minimum-capability vertex shader path |
| R3 Cascaded directional shadows: stable slices, blended boundaries | [LIGHTING.md](LIGHTING.md) | Android device; Linux/EGL; existing minimum-capability vertex shader path |
| R4 Point/spot shadows: nearest two lights, cube/cone maps | [LIGHTING.md](LIGHTING.md) | Android device; Linux/EGL; existing minimum-capability vertex shader path |

Each feature doc lists its own open items under **Status**.

### R. Rendering baseline — before P8

Order changed: these come before P8–P10 because they are what any 3D game expects from a renderer, while P8–P10
wait for a game that needs them. R1 (environment) is done and is the model for the others: read
[ENVIRONMENT.md](ENVIRONMENT.md) and its commits (`93da244`..`1ead30b`) to see what one item contains — the
component, both renderers, the editor (Create menu entry, icon, gizmo, `Tr()` rows), a test, the docs, the rebuilt
web and Android runtimes.

Rules for every item here:

- Both renderers get the same math (`GpuRendererMatchesSoftware`); render the same scene with `oe render` and
  `oe render --renderer gpu`, compare the PNGs and report the difference.
- A scene that does not use the feature keeps its software frame hash, and old scene files still load.
- Limits are fixed numbers stated in the doc (lights per frame, map sizes); the shaders must stay inside the
  WebGL2 / GLES3 minimums (uniform count, texture units — the mesh program already uses bindings 0–10).
- Leave a small project under `build/` that shows the feature and say how to open it.
- The work log of R2–R4 goes into a new `docs/LIGHTING.md`; R5 and R9 into [POSTPROCESS.md](POSTPROCESS.md);
  the others into a doc of their own.

- [x] **R2 — Spotlight.** A cone light: color, intensity, range, inner and outer angle (soft edge), aimed by
  the entity's rotation. A new component or more fields on `PointLight` — decide from the code. Keep the light
  data ready for a shadow (R4) but do not build the shadow here. Editor: cone gizmo.
  Implemented: `SpotLight`, eight spots per frame; Windows tests and WebGL2 demo checked, web/Android runtimes rebuilt.
  Remaining: Android physical-device rendering; Linux/EGL rendering; existing skin/vertex-graph path on minimum-capability devices.
- [x] **R3 — Cascaded shadows.** The sun's shadow in several maps by distance (`DirectionalLight` fields for
  the count and the far distance) so a large outdoor scene keeps sharp shadows near the camera. Blend between
  cascades; stable under camera motion (texel snapping).
  Implemented: `DirectionalLight.shadowCascades` (1..4) + `shadowDistance`; four 1024-square maps in a 2048-square atlas.
  Windows tests and WebGL2 demo checked; web and both Android runtimes rebuilt.
  Remaining: Android physical-device rendering; Linux/EGL rendering; existing minimum-capability vertex shader path.
- [x] **R4 — Point and spot light shadows.** `shadows` on those lights; a fixed budget of shadowed lights per
  frame, chosen like the probes (in view, nearest first).
  Implemented: opt-in `shadows` on both components, nearest two local lights, 512-square cube/cone maps in a 2048-square atlas.
  351 Windows tests and WebGL2 package checked; web and both Android runtimes rebuilt.
  Remaining: Android physical-device rendering; Linux/EGL rendering; existing minimum-capability vertex shader path.
- [ ] **R5 — Ambient occlusion (SSAO).** A `PostProcess` effect darkening the ambient term in creases and
  contacts, from the depth buffer, in both renderers.
- [ ] **R6 — Planar reflection.** A mirror or water plane that shows what moves (characters), which probes
  cannot: the scene drawn again from the mirrored camera, used by one flat surface.
- [ ] **R7 — Decals.** A box that projects a texture onto the surfaces inside it (bullet marks, stains, floor
  markings), with its own draw order and fade.
- [ ] **R8 — Particles colliding with the world.** A VFX emitter option: particles stop, bounce or die on
  static colliders (rain on the ground, sparks), deterministic.
- [ ] **R9 — Screen-space reflection.** A `PostProcess` effect for glossy floors; falls back to the probe or
  sky where the ray leaves the screen.

Later, in no fixed order: motion blur, chromatic aberration, temporal anti-aliasing; volumetric light and fog;
refraction, water, caustics; clear coat.

### P8. 2D navigation (grid path finding) — [ ]

- The 3D navigation mesh exists ([NAVIGATION.md](NAVIGATION.md)); this is its 2D counterpart.
- A* over the solid cells of a `Tilemap` (command `tilemap.path {from,to}` + Lua `tilemap.path`). Deterministic
  tie-break (cell index order). Use it for enemy AI in 2D games.

### P9. Build without developer tools (`OE_ENABLE_DEVTOOLS`) — [ ]

- A CMake option that leaves the HTTP server and MCP out of release builds (see `docs/PLATFORMS.md`). Needed before a
  console or store release. Decide the default for the player target first and document the size and security difference.

### P10. Engine candidates waiting for a concrete caller — [ ]

Kept from earlier pilots; start one only when a game needs it, with its own work log.

- Shader graphs: raise the 48-node limit (64 fits the WebGL2 uniform minimum by estimate; verify on devices) or
  generate GLSL per graph at packaging time ([POSTPROCESS.md](POSTPROCESS.md#portable-surface-shader-graphs)).
- PostProcess: chromatic aberration, film grain seeded by the frame number, screen-space tilt-shift.
- UI: `ui.fitText` (box fitting / auto-shrink with painted bounds), an owner-bound widget subtree show/hide
  contract, Japanese-capable default font fallback.
- Lua: further `oe.entities` conveniences (setPosition-style handles) once local/world semantics are specified.
- Native snapshots: allocator optimisation of `SnapshotHeap` (measured +15..27 % simulation cost) keeping
  transactional rollback.
- Host bridge: real ads and store purchases (Android `PlatformHostCall`/`PlatformHostPoll`, web provider SDK).

### Platform track (can run alongside feature work)

- **macOS -> iOS**: follow the work log in [APPLE.md](APPLE.md). The first item is the macOS CI workflow. (Writing
  platform code after the platform functions of P1 and P4 are settled avoids touching it twice.)
- **Linux desktop**: headless GLES already works. Only an X11/Wayland (or SDL3) window, input and audio are missing
  ([PLATFORMS.md](PLATFORMS.md) checklist).
- **Waiting for verification (implemented)**: Android physical devices (multi-touch, speaker/headphones, 16 KB page
  devices) — the unchecked lines of [ANDROID.md](ANDROID.md). Investigate the black screen with the emulator's SwiftShader.
