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
- **Mesh import**: glTF (.glb)
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
- Navigation (grid path finding)

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

Stages 1–4 are done (scripting, physics, gameplay building blocks, assets and rendering + samples). Also done: GPU
renderer (D3D11 / WebGL2 / GLES3), web packaging, Android packaging, the UI rework (TrueType fonts including Korean,
layouts), 2D (Box2D physics, tilesets and autotiling, tile brush), PBR materials, the native editor.

**Rule for agents**: start from the **first unchecked item** below. Before starting, read [DESIGN.md](DESIGN.md)
(invariants, definition of done, hand-over §5). For each item, first create an `Implementation status (work log)`
checklist in the feature's doc (create the doc if there is none; example: [ANDROID.md](ANDROID.md)), then commit and
push per milestone and tick its line. One item = one branch/PR. When done, change `[ ]` to `[x]` in this list and
leave a new line for every check that could not be made. If the order has to change, write the reason here.

### P1. Persistent save data — [x]

- Implementation and verification: [SAVE.md](SAVE.md), PR #15. Memory/directory
  slots, Lua/command APIs, player platform storage and template high scores are
  implemented. Windows and WebAssembly tests pass; web reload and Windows
  restart restore saved values. Web/Android prebuilt runtimes are refreshed.
- [ ] Android save/restart on a device; no adb device was connected.
- [ ] Full native Linux build/runtime; null backend syntax check passed.

- **Why**: `game.get/set` lasts only for a play session (`Engine::GameData()`, `engine/app/Engine.h`). It is gone on
  exit, so a shipped game cannot keep progress. Small, independent and relevant to every later platform, so it comes first.
- **Requirements**: script API `save.get(key, default)`, `save.set(key, value)`, `save.delete(key)`, `save.flush()`
  (or names consistent with the existing ones such as `game.save`/`game.load`, documented in `docs/SCRIPTING.md`).
  Values must be JSON serializable. Separate slots (a `slot` name).
- **Where it is stored**: desktop = a JSON file in the user data folder (Windows `%APPDATA%/<game name>/`), written
  to a temporary file and renamed; a broken file is ignored and logged. Web = `localStorage` (through Emscripten JS).
  Android = the app's internal storage, `ANativeActivity::internalDataPath`. Path access gets a function in
  `Platform.h` (`PlatformSaveDirectory()` or similar) and OS code stays in `engine/platform/*`.
- **Determinism**: the simulation must stay deterministic. `oe render`, tests and `oe script`/MCP sessions use a
  **memory store** by default (nothing written to disk); only the player runtime and the `oe` option `--save-dir`
  write to disk.
- **Commands**: `save.state` (read), `save.set`/`save.clear` (modify, for tests). Regenerate `docs/API.md`.
- **Tests**: write -> restart -> read, a corrupted file, slots, no disk access in memory mode, `TemplateGamePlaythrough`
  keeps passing. A saved high score in the template is a good addition (regenerate `samples/Hello` after a template change).
- **What may be impossible to verify**: if web localStorage cannot be seen through `node build-web/bin/oe_tests.js`,
  check it in a browser (Playwright); rebuild the web runtime (`runtime/web/`) only when Emscripten is available.

P1 and P2 are integrated into main directly from their local feature branches.
The previous PR #15 merge commit was replaced at the user's request.

### P2. Skeletal animation (glTF) — [x]

- Implementation and verification: [ANIMATION.md](ANIMATION.md). Windows 67 tests,
  Node/WASM 63 tests, D3D11/software animated comparison and browser WebGL2 checked;
  web and both Android ABI runtimes refreshed.
- [ ] Android physical-device and full Linux/EGL execution remain unverified;
  see the feature work log for the exact follow-up steps.

- **Why**: skinned models showed only their bind pose (`docs/RENDERING.md`, the `node->skin` handling in
  `engine/assets/Assets.cpp`). The largest gap: 3D characters could not move.
- **Requirements**: load glTF `skins` (joints, inverseBindMatrices, JOINTS_0/WEIGHTS_0) and `animations`
  (translation/rotation/scale channels, LINEAR/STEP, CUBICSPLINE if possible). Component
  `Animator {clip, speed, loop, playing, time}` (reflection gives serialization, API and inspector). A script call
  like `entity:play(clip)` and a clip list (`clips` in `asset.info`). Cross-fading is secondary (optional).
- **Where**: pose evaluation in `engine/scene/Systems.cpp` (fixed step, deterministic); the skin matrix palette goes
  into `RenderScene` and is applied **the same way in both renderers** (`SoftwareRenderer`, `GpuRenderer` +
  `Shaders.glsl`). After a shader change regenerate `Shaders.glsl.h` with `tools/shaders/compile_shaders.*` (needs
  sokol-shdc; without it, record that and start with the CPU side). The GPU path skins in the vertex shader; document
  the joint limit (for example 64).
- **Verification**: `GpuRendererMatchesSoftware` passes with a skinned model, the frame hash at a given time is fixed,
  a walking sample is checked with `render.screenshot`. The fox of `samples/Showcase` gets Survey/Walk/Run as the demo.
- **Care**: keep test scenes small (skinned vertex count × software renderer). Do not break the determinism of the
  multithreaded renderer.

### P3. Particle system — [x]

P3's simulation milestone is integrated into main alongside P1 and P2. Shared
billboards and Platformer/Dungeon coin/hit effects are implemented in the follow-up
branch. Windows passes 75 tests, Node/WASM passes 71, and D3D11/software images
and packaged WebGL2 coin effects were inspected. Web/Android runtimes are refreshed.
Android hardware and Linux/EGL follow-ups remain in [PARTICLES.md](PARTICLES.md).

- **Why**: there were no particles at all. Without effects (explosions, dust, coin sparkle) a game feels flat.
- **Requirements**: component `ParticleEmitter {rate, burst, lifetime, speed, spread, gravity, startSize/endSize,
  startColor/endColor, texture/frame, space(local|world), maxParticles, loop, playing}`. Both 2D (sprite billboards)
  and 3D. Script `emitter:burst(n)`.
- **Determinism (the core)**: random numbers come from an own PRNG seeded with the entity id + a fixed seed, per
  simulation frame. No `std::rand`, time or pointer order. Same scene + same input = same frame hash.
- **Where**: simulation in `Systems.cpp`; `BuildDrawList` makes billboard quads so both renderers draw them the same
  (following the transparency sorting rule). Play/stop preview in the editor is secondary.
- **Verification**: a hash test with `render.screenshot --frames N`, a particle limit test, coin and hit effects in
  `samples/Platformer` / `Dungeon`.

### P4. General gamepad input — [x]

- Implementation status: [INPUT.md](INPUT.md). Portable axes, shared dead zone,
  API injection and Lua reads are implemented. Windows/web/Android physical
  adapters, Platformer/Dungeon controls and native editor Game-view forwarding
  are implemented. Windows 75 and Node/WASM 71 tests pass; CLI sample renders
  and a D3D11 editor screenshot were inspected. Prebuilt players are refreshed.
- [ ] Real-controller verification on Windows, web and Android, and a full
  Linux runtime check; exact follow-ups are recorded in [INPUT.md](INPUT.md).

- **Why**: only Android had gamepads (`ANDROID.md`: D-pad/left stick = arrow keys); desktop and web had none, and
  there were no analog axes.
- **Requirements**: add axes (`axes`: `LeftX/LeftY/RightX/RightY/LT/RT`) and button names
  (`GamepadA/B/X/Y/LB/RB/Start/Back/DPadUp...`) to `InputState`, Lua `input.axis(name)`, buttons also through the
  existing `input.key`. API `input.axis {name, value}` for test injection (like `input.touch`). The dead zone is
  applied by the engine (default 0.15, documented).
- **Platforms**: Windows = XInput (loaded dynamically, ignored when missing), web = Gamepad API
  (`navigator.getGamepads`), Android adds axes to its existing mapping. OS code goes into platform files; engine code
  stays portable.
- **Verification**: input injection tests, a compile check with `PlatformNull`. Real controllers need a device; when
  that is impossible, leave it in the checklist.

### P5. Web multi-touch `input.touches` — [x]

- Implementation and verification: [TOUCH.md](TOUCH.md). All changed web touch
  points reach Lua/UI with stable IDs, press order and one-step began flags;
  first-finger mouse compatibility, cancellation and window-focus reset are
  covered. Windows 73 and Node/WASM 69 tests pass. Synthetic DOM events through
  a packaged WebGL2 player verified simultaneous controls, move/end/cancel/blur
  and CSS-coordinate normalization. The prebuilt web player is refreshed.
- [ ] Real mobile-browser multi-touch, outside-canvas drags and app switching.

- `OnTouch` in `engine/platform/web/PlatformWeb.cpp` mapped only the first finger to the mouse. Every
  `EmscriptenTouchPoint` goes into `InputState.touches` (with `began`; the first finger keeps acting as the mouse).
  Needs a web runtime rebuild (when the Emscripten SDK is available); otherwise change the source and leave it
  unverified. A small task that fits between P1 and P4.

### P6. GPU effects (shader materials, post-processing) — [ ]

P3 rendering, P4, P5 and the completed P6 vignette/HDR milestones are integrated
at the user's request through PRs #18-#21. P6 remains incomplete: custom shader materials
continue on a new branch from the updated main.
Combined Windows 81 and Node/WASM 77 tests pass; prebuilt players are refreshed
from the combined source, preserving all four PRs' features.

- Implementation status: [POSTPROCESS.md](POSTPROCESS.md). The first milestone
  adds a reflected camera PostProcess component and optional vignette in both
  renderers. Windows 73 and Node/WASM 69 tests pass; software/D3D11 comparison
  and Showcase off/on screenshots checked. Shaders and players are refreshed.
  Exposure/HDR Reinhard tone mapping is also implemented: Windows 74 and
  Node/WASM 70 tests pass, including preservation of bright emissive values
  through alpha blending and a software/D3D11 comparison. WebGL2 HDR transitions
  and zero exposure/HUD separation are checked; web and both Android ABI
  runtimes are refreshed. Bloom is implemented in the P6 follow-up: Windows 82
  and Node/WASM 78 tests pass, with deterministic hue-preserving halos and
  D3D11 comparison. WebGL2 bloom/vignette switching and resizing are verified;
  web and both Android ABI runtimes are refreshed. Optional directional FXAA
  and Showcase 1-6 effect presets are implemented. Windows 84 and Node/WASM
  80 tests pass; CLI images and packaged WebGL2 FXAA/All/Off transitions,
  sharp HUD and resize/restore are inspected. PR #22 adds portable shader graphs,
  strict material binding/hot reload and CPU/GPU execution, including graph alpha
  in shadow/selection passes. Windows 88 and Node/WASM 84 tests pass, covering
  instruction families, texture/time inputs, HDR emission and alpha blending.
  P6 remains unchecked until the follow-ups below are completed.

Completion pass on 2026-10-03: Windows 137 and Node/WASM 125 tests pass.
ShaderLab demonstrates animated emission, procedural masks and ground shadows;
CPU/D3D11 and packaged WebGL2 neutral/bloom output were inspected.

- [x] Varying-alpha shadow masks and mixed-material selection occlusion.
- [x] Procedural material sample, fixed-time/preset/package tests and WebGL2 execution.
- [x] Web player has shader graphs and is refreshed with P7 source.
- [x] Current neutral white fixture agrees in browser capture and exported canvas.
- [ ] Rebuild both Android ABI players: this host has no Android SDK/NDK.
- [ ] Android hardware and full Linux/EGL effect execution.
- [x] P7 implementation completed independently; remaining P6 platform checks do not block editor work.

P7 starts before the final P6 platform checks because its editor-only tooling
has no dependency on Android runtime refresh or those devices. P6 remains unchecked
until those explicit follow-ups are finished; see POSTPROCESS.md for evidence.

- The composite pass (`GpuRenderer`) is the hook. Start with component/camera settings that switch tone mapping,
  bloom, FXAA and vignette on and off. **The software renderer is the reference**, so effects must not change hashes:
  keep them off by default, or implement them in both renderers (the rule in `CLAUDE.md`). Custom shader materials
  depend heavily on sokol-shdc and come in the second half of this item.

### P7. Editor improvements — [x]

- Implementation and verification: [EDITOR.md](EDITOR.md). Command-backed clipboard, history timeline, isolated prefab source editing, lazy asset previews and Windows native file dialogs implemented. Windows 141 tests pass; localized D3D11 screenshots inspected. Web runtime rebuilt; Android runtime rebuild and real native-dialog/hardware checks remain explicitly tracked.
- P6 completion/sample verification is in separate PR #24; Android/Linux checks remain pending.

- Hierarchy copy/paste (clipboard), a history (undo) panel, prefab edit mode, asset thumbnails, native file dialogs.
  Edits go through `Impl::Call` (commands) only; UI strings use `Tr("...")` with Korean and Japanese rows in
  `EditorText.cpp`; check with `oe editor --screenshot` and add a `NativeEditorHeadless` test ([EDITOR.md](EDITOR.md)).

### P8. Navigation (grid path finding) — [ ]

- A* over the solid cells of a `Tilemap` (command `tilemap.path {from,to}` + Lua `nav.path`). Deterministic
  tie-break (cell index order). Use it for the slime AI of the 2D samples.

### P9. Build without developer tools (`OE_ENABLE_DEVTOOLS`) — [ ]

- A CMake option that leaves the HTTP server and MCP out of release builds (see `docs/PLATFORMS.md`). Needed before a
  console or store release. Decide the default for the player target first and document the size and security difference.

### Platform track (can run alongside feature work)

- **macOS -> iOS**: follow the work log in [APPLE.md](APPLE.md). The first item is the macOS CI workflow. (Writing
  platform code after the platform functions of P1 and P4 are settled avoids touching it twice.)
- **Linux desktop**: headless GLES already works. Only an X11/Wayland (or SDL3) window, input and audio are missing
  ([PLATFORMS.md](PLATFORMS.md) checklist).
- **Waiting for verification (implemented)**: Android physical devices (multi-touch, speaker/headphones, 16 KB page
  devices) — the unchecked lines of [ANDROID.md](ANDROID.md). Investigate the black screen with the emulator's SwiftShader.
