# OwnEngine — guide for AI agents

OwnEngine is a small C++17 game engine designed to be driven and verified by AI agents as easily as by humans. Everything the editor can do is a command in one registry, reachable from the CLI, HTTP and MCP.

## Build & test (Windows)

```bash
build.bat            # Release build -> build/bin/oe.exe, build/bin/oe_tests.exe
build/bin/oe_tests   # unit tests, exit code 0 = pass
```

`build.bat` finds Visual Studio 2022 via vswhere and uses its bundled CMake + Ninja. No dependencies to install (Lua 5.4, Jolt Physics 5.6, Box2D 3.1, sokol_gfx, stb_image, stb_truetype, cgltf, Dear ImGui, ImGuizmo and ImGuiColorTextEdit are vendored in `third_party/`; the first build compiles Jolt and takes a few minutes).

Android: `oe package <project> --android [--install]` writes a signed APK (debug key unless `--keystore`; oe writes the manifest/resources itself, signing needs SDK build-tools `apksigner` + Java) from `liboe_player.so` built by `build_android.bat` / `./build_android.sh` (Android NDK) -> `build/bin/android/<abi>/` and `runtime/android/<abi>/`. Status and remaining device testing: [docs/ANDROID.md](docs/ANDROID.md).

Web runtime: `oe package --web` uses the prebuilt `runtime/web/oe_player.js` + `.wasm` (committed; no Emscripten needed). After changing engine C++ that the player uses, rebuild it: install the Emscripten SDK, `set EMSDK=C:\path\to\emsdk`, run `build_web.bat` (Linux/macOS: `./build_web.sh`) -> `build/bin/web/` and `runtime/web/`; commit the refreshed runtime and update `runtime/web/README.md`. On Linux, `cmake -S . -B build -G Ninja` builds the headless `null` platform (GPU screenshots through EGL when `libegl-dev`/`libgles-dev` are installed).

## Driving the engine

| Goal | Command |
|---|---|
| Overview of a scene | `oe exec samples/Hello scene.summary` |
| Field schemas | `oe exec samples/Hello component.types` |
| Edit + save | `oe exec samples/Hello component.set '{"id":"Player","type":"MeshRenderer","values":{"color":"#ff0000"}}' --save` |
| Many edits | pipe `{"command":..,"args":..}` lines into `oe script samples/Hello --save` |
| See the result | `oe render samples/Hello --out build/shot.png` then read the PNG (software renderer: deterministic hash). Add `--renderer gpu` (or `render.screenshot {renderer:"gpu"}`) to see what the game window shows |
| Simulate first | `oe render samples/Hello --frames 120 --out build/shot.png` |
| Live session | The human runs `oe editor samples/Hello` (it serves the API on 7777); you attach with `oe mcp --connect 7777`. Alone: `oe mcp samples/Hello` (MCP on stdio; `--port 7777` also serves the HTTP API) |
| Attach to a human's editor | `oe mcp --connect 7777` (your edits show up in the editor as notices) |
| See the editor itself | `oe editor samples/Hello --screenshot build/editor.png [--select Player] [--play --frames 60]` (the editor rendered headless; needs a GPU backend) — see [docs/EDITOR.md](docs/EDITOR.md) |
| Physics | 3D (Jolt): `Collider` (+ `RigidBody` to move, `isTrigger` for volumes), `CharacterBody` for players. 2D (Box2D): `Collider2D` (box, circle, capsule, polygon, edge; `oneWay`, `layer`), `RigidBody2D`, `CharacterBody2D {mode: platformer\|topdown}`. Check with `render.screenshot {colliders:true}`, `physics.contacts`, `physics.raycast` — see [docs/PHYSICS.md](docs/PHYSICS.md) |
| Prefabs, scenes, UI, audio | `prefab.instantiate`, `game.load_scene`/`game.state`, `UIText`/`UIPanel`/`UIButton`/`UIImage`/`UISlider` + `UILayout` (nest UI under a parent UI entity), `ui.layout` (pixel rects) + `input.click {x,y}` (screenshot pixels), `audio.generate`/`audio.state`/`audio.capture` — see [docs/GAMEPLAY.md](docs/GAMEPLAY.md) |
| UI details | TrueType fonts (`font: "assets/fonts/x.ttf"`, built-in `default`/`pixel`), anchors + stretch, rich text, wrap, outline, 9-slice, fill bars, clipping, layouts, `UICanvas` scaling, `onPointerEnter/Exit`, `onValueChanged` — see [docs/UI.md](docs/UI.md) |
| 2D games | Orthographic camera + `Sprite {texture, frame, columns}` + `SpriteAnimation {clips, clip}` + `Tilemap {tileset: "x.tileset.json", map: [text rows], legend}` (tile rules: frame, variants, autotile sides/blob, collision solid/oneway/slopes) + 2D physics; `tileset.create`, `tilemap.info/paint/fill`, Lua `tilemap.get/set/fill/cellAt` — see [docs/2D.md](docs/2D.md) |
| Models, textures, materials, lights | `oe import <project> <file>` then `asset.info` (gives a scale hint); `MeshRenderer {mesh:"assets/models/x.glb", texture, shading}`; PBR materials: `material.create {path:"materials/x.mat.json", values:{metallic, roughness, emissive, ...}}` then `MeshRenderer {material:"materials/x.mat.json", opacity}`, `PointLight`, `DirectionalLight.shadows`, `CameraFollow`, `debug.draw` — see [docs/RENDERING.md](docs/RENDERING.md) |
| Ship a game | `oe package <project> [--out dist/Name]` -> `dist/Name/Name.exe` (player runtime, no console/editor/API) + `game/` (project files minus AGENTS.md, dotfiles and tools/). `--web` -> `dist/Name-web/` (`index.html`, `oe_player.js/.wasm`, `game.pak`) for any static host; test with `oe serve dist/Name-web`. `--android [--install]` -> `dist/Name-android/Name.apk` ([docs/ANDROID.md](docs/ANDROID.md); `project.json` `android {package, versionCode, versionName, orientation, icon}`). Optional `project.json` `window {width,height,title,renderer:auto\|gpu\|software,renderScale,maxRenderWidth}` |
| Gameplay code | Lua in `<project>/scripts/`, attached via the `Script` component. `script.write`, `script.check` (syntax + global warnings, no run), `script.params`, `script.eval`, `script.errors` — see [docs/SCRIPTING.md](docs/SCRIPTING.md) |

All commands print JSON `{"ok":true,"result":...}` or `{"ok":false,"error":{"code","message","hint"}}`. Read the `hint` — it says how to fix the call. Entities can be referenced by id or by unique name. Full reference: [docs/API.md](docs/API.md) (regenerate with `oe api --markdown > docs/API.md`).

## Code map

- `engine/core` — Json (ordered, diff-friendly), Math, Log (ring buffer, stderr only), Image (PNG encoder, Base64), FileSystem, Zip (reader + aligned writer for APKs).
- `engine/scene` — reflection (`Reflect.h`), built-in components (`Components.h`), `Scene` (entities + component pools, JSON I/O), behaviour systems (`Systems.cpp`, incl. SpriteAnimation), `TileGrid` (Tilemap cells, tile rules + tileset files, autotiling, collision geometry: merged rectangles, outlines, one-way runs, shaped cells).
- `engine/script` — `ScriptHost`: sandboxed Lua state per play session, script instances, bindings (`scene`, `input`, `time`, `log`), hot reload, error capture. `ScriptCheck`: static checks (compile + bytecode walk for globals) and params inference (`script.check`, `script.params`).
- `third_party/lua` — Lua 5.4.8, unmodified (built as C++; `third_party/lua_oe` fixes the hash seed for determinism).
- `engine/physics` — `PhysicsWorld`: the only code that knows Jolt. Mirrors Collider/RigidBody/CharacterBody into Jolt, writes results back, produces sorted collision/trigger events, raycasts, collider wireframes; owns `Physics2D` and merges its events/queries. `Physics2D.cpp`: the only code that knows Box2D (Collider2D/RigidBody2D/CharacterBody2D, tilemap chains, one-way pre-solve, the character mover).
- `third_party/jolt` — Jolt Physics 5.6.0, unmodified; built in cross-platform deterministic mode (options in the top-level `CMakeLists.txt`).
- `third_party/box2d` — Box2D 3.1.1, unmodified (`include/`, `src/`); built as C17 with `-ffp-contract=off` (target `oe_box2d`), scalar on the web.
- `engine/audio` — `AudioSystem` (deterministic 48 kHz mixer ticked by the simulation, capture, AudioSource), WAV decode/encode, procedural sound presets.
- `engine/assets` — `AssetManager` (model/texture/material/font/tileset cache + hot reload, `asset.info`), glTF loading (cgltf) and image decoding (stb_image); `ThirdPartyImpl.cpp` compiles the single-header libraries.
- `third_party/stb`, `third_party/cgltf` — stb_image 2.30, stb_truetype 1.26, cgltf 1.15, unmodified. `third_party/fonts` — Roboto (Apache 2.0), embedded at build time as the UI font `default`.
- `engine/render` — `IRenderer` interface; `RenderScene` (draw list + lights shared by both renderers); deterministic multi-threaded `SoftwareRenderer` (textures, smooth/flat shading, PBR metallic-roughness lighting, transparency, directional + point lights, shadow map, color + depth + entity-id buffers) — the reference for hashes, tests and picking; `GpuRenderer` (sokol_gfx: shadow pass, MSAA scene pass, selection mask, composite + UI; offscreen with readback or into a window) with shaders in `render/shaders/Shaders.glsl`; `GpuDevice.h` is what a platform provides; `Material.cpp` parses/validates `.mat.json` material files (glTF materials load into the same `Material` struct); `BuildDrawList` orders opaque/mask first, blended back to front; built-in meshes; `UI.cpp` lays out UIText/UIPanel/UIButton/UIImage/UISlider (parent rectangles, anchors, UILayout, clipping) and turns them into quads (`BuildUIQuads`: solid, textured, rounded) drawn identically by both renderers, plus hit testing; `Font.cpp` rasterizes TrueType glyphs (stb_truetype) into atlas pages and holds the 5x7 pixel font. See [docs/RENDERING.md](docs/RENDERING.md).
- `engine/api` — command registry + all built-in commands (`Commands.cpp`), HTTP server, the HTTP API routes `/api/call` + `/api/commands` (`ApiService.cpp`), MCP server.
- `engine/app` — `Engine` (scene, fixed-step sim, undo, main-thread job queue, remote-call observer), project templates, `ImportAssetFile`, game.pak write/extract, `AndroidPackage` (binary AndroidManifest.xml, resources.arsc, APK layout).
- `engine/editor` — the editor (`oe editor`, library `oe_editor`, never linked into games): `NativeEditor` + `RunNativeEditor` (`Editor.cpp`: input, menus, toolbar, dock layout, prompts), panels (`EditorPanels.cpp`), Scene/Game views with ImGuizmo gizmo and picking (`EditorViewports.cpp`), Tiles panel + tile brush (`EditorTiles.cpp`), `EditorMath.h`, `SokolImGui.cpp`. Edits only through `Engine::Call`. See [docs/EDITOR.md](docs/EDITOR.md).
- `third_party/imgui`, `third_party/imguizmo`, `third_party/imguicolortextedit` — Dear ImGui 1.92.9b (docking branch), ImGuizmo and ImGuiColorTextEdit (code editor), unmodified; `third_party/sokol/sokol_imgui.h` draws ImGui with sokol_gfx.
- `engine/platform` — `Platform.h` interface (games use `PumpEvents` + `InputState`; tools use the window event mode: `SetEventMode`/`TakeEvents`, cursor, DPI); `win32/` (window, waveOut, `GpuD3D11.cpp`), `web/` (Emscripten canvas/DOM/WebAudio, `GpuWebGL.cpp`), `android/` (NativeActivity, AAudio, `GpuAndroid.cpp` EGL), `null/` (headless; `GpuEgl.cpp` or `GpuNone.cpp`), `gl/` (sokol GLES3 implementation + readback shared by web, Android and EGL).
- `third_party/sokol` — sokol_gfx (graphics API abstraction), unmodified.
- `samples/Showcase` — rendering sample: textured glTF fox (player + follow camera), shadows, point lights.
- `samples/Dungeon` — top-down 2D action on Box2D: tileset file with autotiled walls (blob) and water (sides), floor variants, pushable crates, a concave boulder, bolts on their own collision layer, chasing slimes. `tools/make_art.py` generates its art.
- `samples/Platformer` — 2D side-scroller: text tilemap level with spawn markers, animated sprites, coyote time/jump buffer, slimes, `?` blocks, parallax, results screen. `tools/make_art.py` generates its pixel art.
- `samples/FPS` — first-person shooter test game: mouse look (`input.mouseDelta`, `input.lockMouse`), hitscan pistol via `physics.raycast`, reload, moving/armoured targets, results screen.
- `templates/default/` — what `oe new` copies (two-level coin game: scenes, prefab, Lua scripts, AGENTS.md). `samples/Hello` is generated from it.
- `tools/oe/main.cpp` — CLI front-end. `tools/player/main.cpp` — game runtime shipped by `oe package` (desktop exe, the web module built with Emscripten, `liboe_player.so` + `android_main` built with the NDK); `tools/player/web/index.html` — the web page. `tools/shaders/` — regenerates `Shaders.glsl.h`. `tests/tests.cpp` — self tests (also run as WebAssembly: `node build-web/bin/oe_tests.js`).

## Conventions

- stdout is reserved for machine-readable output; logs go to stderr (`OE_LOG_*`).
- Simulation is fixed-step 1/60 s and deterministic: same scene + same inputs = same frame hash. Frame order: timers → scripts `onUpdate` → UI pointer (hover/press/drag) → built-in systems → physics → collision/trigger callbacks → `onClick` → pending scene change → audio mix (see docs/GAMEPLAY.md). Iterate entities in id order; never depend on pointer order or wall-clock time.
- New component: struct with `kTypeName`, `kDoc`, `Reflect()` in `Components.h`, register in `Components.cpp`. Serialization, API, schemas and the editor inspector follow automatically. Behaviour goes in `Systems.cpp`.
- New Lua binding: add a `L_*` function in `engine/script/ScriptHost.cpp`, register it in `ScriptHost::Open()`, document it in `docs/SCRIPTING.md`. Wrap bodies that can throw C++ exceptions in `Guard(L, ...)`.
- New command: `Register(...)` in `Commands.cpp` with a `Params()` schema; mark `mutates=true` if it edits the scene (gives undo + revision bump). Throw `ApiError(code, message, hint)` for caller errors.
- Template changes: edit `templates/default/`, then regenerate the sample (`rm -rf samples/Hello && oe new samples/Hello --name Hello`) and keep the `TemplateGamePlaythrough` test passing.
- New platform: implement `engine/platform/Platform.h` and add it to `CMakeLists.txt`; see [docs/PLATFORMS.md](docs/PLATFORMS.md).
- Engine code outside `engine/platform/*` must stay portable C++17 (no OS headers). Graphics API code goes through sokol_gfx; only `engine/platform/*` compiles a backend (`SOKOL_IMPL`).
- Shaders: edit `engine/render/shaders/Shaders.glsl`, run `tools/shaders/compile_shaders.bat` (needs `sokol-shdc`), commit the regenerated `Shaders.glsl.h`. Lighting changes must be made in both renderers (`GpuRendererMatchesSoftware` compares them). New screen-space effects go into the composite pass of `GpuRenderer`.
- Editor: new UI goes in `engine/editor/` and changes the scene only through commands (`Impl::Call`), so undo and agents stay in sync. UI text is English in code wrapped in `Tr("...")` (window/popup names `TrId`), with a Korean + Japanese row in `engine/editor/EditorText.cpp` (`EditorTranslations` test). Check it visually with `oe editor <project> --screenshot`; `NativeEditorHeadless` in `tests/tests.cpp` drives it with window events.
- Keep `docs/API.md` regenerated when commands or components change, and add a test in `tests/tests.cpp` for new behaviour.
