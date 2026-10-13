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
- [x] **R5 — Ambient occlusion (SSAO).** A `PostProcess` effect darkening the ambient term in creases and
  contacts, from the depth buffer, in both renderers.
  Implemented: `PostProcess.ssao/ssaoRadius/ssaoBias`, 32 fixed samples, ambient-only shading.
  352 Windows tests and WebGL2 package checked; web and both Android runtimes rebuilt.
  Remaining: Android physical-device rendering; Linux/EGL; real browser GPU and minimum-capability vertex path.
- [x] **R6 — Planar reflection.** A mirror or water plane that shows what moves (characters), which probes
  cannot: the scene drawn again from the mirrored camera, used by one flat surface.
  Implemented: `PlanarReflection`, nearest visible surface per view, dynamic captures with shared oblique clipping.
  354 Windows tests and WebGL2 animated package checked; web and both Android runtimes rebuilt.
  Remaining: Android physical devices; Linux/EGL; real browser GPUs and existing minimum-device vertex path.
- [x] **R7 — Decals.** A box that projects a texture onto the surfaces inside it (bullet marks, stains, floor
  markings), with its own draw order and fade.
  Implemented: `Decal`, 16 visible oriented boxes, ordered base-color projection, alpha/edge/normal fades.
  357 Windows tests and WebGL2 texture package checked; web and both Android runtimes rebuilt.
  Remaining: Android physical devices; Linux/EGL; real browser GPUs and existing minimum-device vertex path.
- [x] **R8 — Particles colliding with the world.** A VFX emitter option: particles stop, bounce or die on
  static colliders (rain on the ground, sparks), deterministic.
  Implemented: birth-captured stop/bounce/die and coefficients, one static-only center sweep per moving particle/step.
  360 Windows tests and WebGL2 3D demo checked; web and both Android runtimes rebuilt.
  Remaining: Android physical devices; Linux/EGL; browser 2D collision, real GPUs and existing minimum-device vertex path.
- [x] **R9 — Screen-space reflection.** A `PostProcess` effect for glossy floors; falls back to the probe or
  sky where the ray leaves the screen.
  Implemented: camera SSR controls, material-aware fixed-step tracing and probe/sky fallback in both renderers.
  361 Windows tests and WebGL2 demo/effect transitions checked; web and both Android runtimes rebuilt.
  Remaining: physical Android, Linux/EGL, real browser GPUs and existing minimum-device skin/vertex-graph execution.

Later, in no fixed order: motion blur, chromatic aberration, temporal anti-aliasing; volumetric light and fog;
refraction, water, caustics; clear coat.

### A. Animation workflow — before T

Animations play, but a person cannot manage them in the editor, in 2D or 3D. Today: `SpriteAnimation.clips` is a
JSON text box in the Inspector (frame numbers counted by hand on the sheet); `Animator` is a clip name picker and
number fields (`time`, `speed`, `loop`, `blend`); nothing plays while the scene is stopped; there is no
timeline; neither component can call a script at a moment of a clip (scripts compare `time` / `frame` in
`onUpdate` and miss it when a frame is skipped); which states a character has and how it moves between them
exists only as `animation.play` calls in Lua; 2D clips live inside each entity, so every prefab and scene that
uses the character carries its own copy. This list comes before T because it serves both 2D and 3D and is a gap
users meet at once; A1 and A2 depend on nothing, A4 is designed together with T1/T2.

Rules for every item here:

- One design for 2D and 3D wherever the idea is the same (events, the panel, controllers): a script sees the same
  callback and the same commands for a `SpriteAnimation` and an `Animator`.
- A command first (`Engine::Call`), then the editor on top of it, so agents can author and check the same data
  (`animation.state`, `oe editor --screenshot`).
- Playback stays on simulated time and in entity id order; events are part of the deterministic step (rollback
  replays them). A preview in the editor never changes the scene or its revision.
- Old scenes keep working: `SpriteAnimation.clips` inside an entity and a model's own clips stay valid.
- The work log goes into [ANIMATION.md](ANIMATION.md) (2D parts also noted in [2D.md](2D.md)); editor strings
  through `Tr()` with ko/ja rows.

- [x] **A1 — Animation events.** A clip can name events at moments of its timeline: a frame of a sprite clip, a
  time in seconds of a model clip. The entity's scripts get `onAnimationEvent(name, clip)`, and the end of a
  clip that does not loop is an event too (instead of polling `finished`). Decide and document: where a model
  clip's events are stored (beside `<model>.clips.json`; a model file cannot hold them), the order (entity id,
  then time; every event passed in one step fires, also across a loop boundary and when `speed` is negative),
  what a clip that is fading out during `Animator.blend` does (suggested: it fires nothing), and the place in
  the frame order (after the animation systems advance, before physics). `animation.events` lists them.
  Implemented: shared `onAnimationEvent(name, clip)`, `animation.events/set_events`, model `.events.json` sidecars and inline sprite markers.
  Windows tests, software/D3D11/editor demo and WebGL2 callbacks checked; web and both Android players rebuilt.
  Remaining: physical Android, Linux/EGL and real browser GPUs/mobile performance.
- [x] **A2 — Animation panel.** View > Animation: the clips of the selected entity (2D or 3D), play / pause /
  step, a timeline with a scrubber, and the pose or frame shown in the Scene view while the game is stopped (the
  way the Effects panel previews). Event marks on the timeline are added, moved and named there (A1).
  Implemented: isolated animation.preview/preview_state/preview_close, editor.animation, stopped Scene/Game poses and event authoring.
  373 Windows tests and final legacy-layout test passed; en/ko/ja screenshots checked, web/Android players rebuilt.
  Preview IK defaults off for the target model only (ik opt-in); Chisa Walk screenshots and authored-state isolation checked.
  Remaining: Linux/EGL editor and physical Android/real browser GPU runtime checks.
- [x] **A3 — Clips as assets.** 2D: a clip set in its own file (`x.anim.json`: sheet or atlas, clips, frames,
  events) that `SpriteAnimation` references, so prefabs and scenes share one definition. 3D:
  `<model>.clips.json` also carries events, a loop range and display names. `animation.create/get/set`,
  `asset.info`, hot reload.
  Implemented: shared sheet/rectangle-atlas sprite definitions, model sidecar metadata and per-instance playback.
  377 Windows tests passed; en/ko/ja editor screenshots, software/D3D11 parity and packaged WebGL2 callbacks checked.
  Web and both Android players rebuilt; physical Android, Linux/EGL and real browser GPU checks remain open.
- [x] **A4 — Sprite clip editor.** In the Animation panel for 2D: the sheet shown as its frames, click or drag
  frames into a clip, reorder them, a duration per frame (per-frame time replaces one `fps` per clip where it is
  set), loop on/off. Replaces the JSON text box. Reads the T1 atlas when that exists; the T2 Aseprite import fills
  the same data (tags become clips, frame durations are kept).
  Implemented: sheet/rectangle-atlas click/drag frame authoring, sequence reorder, durations and loop editing; inline undo/shared files.
  381 Windows tests passed, including actual native UI input; localized screenshots and WebGL2 variable-duration callbacks checked.
  Web and both Android players rebuilt. Remaining: T1/T2 extended atlases/import, physical Android, Linux/EGL and real browser GPUs.
- [x] **A5 — Animation controller (state machine).** An asset with states (a clip each), transitions (a
  condition on parameters, a blend time) and parameters (`speed`, `grounded`, a trigger); scripts set
  parameters and no longer call `animation.play` for every change; the same controller drives a sprite or a
  model. A graph view in the editor (the canvas of the Effects / Shader Graph editors). Start as an official Lua
  module (`oe.animator`) that reads the asset and calls `animation.play`: a state machine is behaviour a script
  can express (DESIGN.md §2). Move evaluation into the engine only when blend trees (several clips mixed by a
  parameter inside the pose) are needed, which a script cannot do.
  Implemented: validated .controller.json assets/API, official oe.animator, shared native state graph and typed transition authoring.
  385 Windows tests passed, including native text/click/drag authoring and sprite/model rollback; localized graphs and packaged WebGL2 transitions checked.
  Web and both Android players rebuilt. Remaining: sprite crossfade/blend trees, live controller reload, physical Android, Linux/EGL and real browser GPUs.

### T. 2D baseline — after R

What a general 2D game expects and the engine does not have yet. What exists is in [2D.md](2D.md): grid sprite
sheets, `SpriteAnimation` clips, sorting layers, `Tilemap` with tileset files, autotiles and collision shapes, Box2D
physics, `Darkness2D` + `Light2D`, the tile brush. Most gaps are in the art pipeline (atlases, files from other
tools), sorting and lighting. Start after the R and A lists unless a game needs an item sooner; T1 and T2 belong together and
come first because nearly every 2D game's art passes through them and the editor tools (A4, T16) build on them.

Rules for every item here:

- Both renderers draw the same picture; a scene that does not use the feature keeps its software frame hash, and
  old scene, tileset and animation files still load.
- Everything a hand can do in the editor is a command first (`Engine::Call`), so agents can author it and check
  it with `oe render` / `asset.info`.
- Animation and tile changes run on simulated time and in id order (deterministic, rollback-safe).
- Importers for other tools' files are written here; no new third-party library without vendoring it.
- Behaviour a Lua script can express is an official Lua module, not a component (see the list at the end).
- The work log goes into [2D.md](2D.md) (a notes file beside it when it outgrows the doc); extend the
  `tests/fixtures/Dungeon` fixture or add a small one, and leave a project under `build/` that shows the feature.

Needed by nearly every 2D game:

- [ ] **T1 — Sprite atlas.** Frames of different sizes in one image, described by a file (`x.atlas.json`): a
  rectangle per frame, trimmed margins with the original size kept, a pivot per frame, frames addressed by name.
  `Sprite.frame` accepts a name; the grid form (`columns`/`rows`) stays. `atlas.create`, `asset.info`. Read the
  TexturePacker JSON layout too.
- [ ] **T2 — Aseprite import.** `oe import` of an Aseprite sheet + JSON export (and, if it stays small, the
  `.aseprite` file itself) writes the atlas and the `SpriteAnimation` clips from its tags, with each frame's duration.
- **T3 — moved.** Animation events are A1 (shared by 2D and 3D); a duration per sprite frame is part of A4.
- [ ] **T4 — Y sort.** A sorting option for top-down games: within a layer, what stands lower on the screen is
  drawn in front (by the entity's pivot, with an offset). Done in the draw-list ordering of both renderers, not by
  scripts writing `order` every frame. Tilemap cells of the same layer take part (a wall hides the feet behind it).
- [ ] **T5 — Tilemap layers and tile attributes.** Several layers in one map (ground, decoration, foreground,
  each with its sorting layer), flipped and rotated cells, animated tiles (water, torches), user data per tile rule
  readable from Lua. The one-character-per-cell text form stays valid as a single layer.
- [ ] **T6 — Tiled and LDtk import.** Read `.tmj`/`.tmx` and `.ldtk` levels into tilesets, layers and entities:
  object layers become entities or markers a level script reads (spawn points, areas). Needs T5.
- [ ] **T7 — 2D path finding.** This is P8 below (A* over tilemap cells); do it here, with layers from T5 in mind.
- [ ] **T8 — Pixel-perfect drawing.** A camera option that renders at the art's resolution and scales by whole
  numbers (letterboxed), and snaps the camera and sprites to the pixel grid so pixel art does not shimmer when it
  moves. `window.renderScale` exists; the snapping and integer scale do not.

Needed by some genres:

- [ ] **T9 — 2D lighting.** `Light2D` today only cuts holes into `Darkness2D`. Colored and additive lights, lights
  blocked by occluders (walls from `Collider2D` or tile collision casting shadows), normal-mapped sprites. Decide
  the limits (lights and occluder edges per frame) so the software renderer stays the reference.
- [ ] **T10 — Sprite draw modes.** Repeated (tiled) sprites, 9-slice in world space, fill amount (health bars,
  radial cooldowns) and masks (draw only inside another sprite's shape). The UI has slicing and fill; sprites do not.
- [ ] **T11 — Shapes and lines.** Filled polygons, thick lines and circles as scene content (spline terrain,
  ropes, beams), not `debug.draw`. `Trail` covers moving ribbons only.
- [ ] **T12 — 2D skeletal animation.** Cut-out characters: bones, weighted mesh deformation, slots, clips. Either
  an importer for an open runtime format (Spine JSON needs its runtime licence checked; DragonBones is open) or the
  engine's own small format reusing `Pose`. Large; start only with a game that needs it.
- [ ] **T13 — 2D physics areas.** Effect regions on a trigger `Collider2D`: wind/force, buoyancy and drag (water),
  local gravity. Also measure the cost of rebuilding tile collision when cells change during play (destructible
  terrain) and rebuild only the changed part if it is too slow — not measured yet.
- [ ] **T14 — Text in the world.** Damage numbers and speech bubbles placed in world coordinates with sorting
  layers. Possible today by converting to screen coordinates for `UIText`; decide whether that is enough.

Good to have:

- [ ] **T15 — Large tilemaps.** Chunks: drawing and collision only for the part near the camera, loading parts of
  a big world.
- [ ] **T16 — Editor tools for 2D.** Sprite sheet slicer (writes the T1 atlas), collision polygon editing on the
  sprite, a tile rule editor instead of hand-written JSON. Each needs its command first. The animation clip
  window is A2/A4.
- [ ] **T17 — Sprite effect presets.** Outline, hit flash, dissolve, screen wipes as ready-made shader graphs
  through `Sprite.material` / `PostProcess`. First check which of them today's graph nodes can already express —
  not verified.

Not engine components (official Lua modules or template scripts instead, because scripts can express them):
camera helpers (dead zone, look-ahead, shake, zoom, room transitions — e.g. `oe.camera2d`) and parallax
backgrounds (endless scrolling wants the repeated sprites of T10).

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
