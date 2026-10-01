# OwnEngine

[한국어](README.md) | **English** | [日本語](README.ja.md)

A C++17 game engine designed so that AI agents can drive and verify it as easily as people can. The editor for people and the interfaces for AI (CLI, HTTP, MCP) share **one command API**.

![Native editor - Showcase sample](docs/images/native-editor.png)

| Game view (`oe render samples/Showcase`) | |
|---|---|
| ![Showcase render](docs/images/showcase.png) | A glTF fox character (player controls + follow camera), procedural textures, shadows and point lights. This is the deterministic software renderer's output (`oe render`); the game window, the web build and the editor views draw the same scene with the GPU renderer. |

- Nothing to install (only MSVC + CMake; Lua, Jolt Physics, Box2D, sokol_gfx, stb, cgltf, Dear ImGui and ImGuizmo are vendored). The web runtime is prebuilt in `runtime/web/`, so web builds need no Emscripten
- Windows (native window, `Name.exe` packaging), Web (WebAssembly + WebGL2, `oe package --web`), Android (APK, `oe package --android` - [docs/ANDROID.md](docs/ANDROID.md)) and headless. iOS / macOS / consoles only need a platform layer - [docs/PLATFORMS.md](docs/PLATFORMS.md)
- Two renderers draw the same scene: the **GPU renderer** (sokol_gfx - D3D11 on Windows, WebGL2 on the web, GLES3 on Linux; 4x MSAA, filtered shadows, mipmaps, one shader file `engine/render/shaders/Shaders.glsl`) for the game window, web and editor views, and the **software renderer** (multithreaded, deterministic) for screenshot hashes, tests and picking
- Rendering: glTF models, PNG/JPEG textures, smooth/flat shading, point lights, shadows, orthographic/follow cameras, debug drawing - [docs/RENDERING.md](docs/RENDERING.md)
- Materials / PBR / transparency: metallic-roughness (GGX) shading, normal/AO/emissive maps, transparency (sorted back to front), mask, double-sided, full glTF materials, `.mat.json` material files (`material.create` / `material.set`, hot reload) - [docs/RENDERING.md](docs/RENDERING.md)
- 3D physics with Jolt and 2D physics with Box2D (rigid bodies, triggers, character controllers, one-way platforms, polygons, collision layers, deterministic simulation) - [docs/PHYSICS.md](docs/PHYSICS.md)
- Game UI: TrueType fonts (any language, add font files), anchors, stretch and parent-child layout, layouts (vertical/horizontal/grid, fit to content), rich text, wrapping, outlines and shadows, rounded and bordered panels, button states (hover/pressed/disabled), images (9-slice, fill bars), sliders/progress bars, clipping, canvas scaling - [docs/UI.md](docs/UI.md)
- 2D games: sprites and sprite sheet animation (pixel art, transparent cut-out), tilemaps written as text (tileset files, autotiling with 16/47 patterns, random variants, per-tile collision: solid, one-way, slopes), Box2D physics, bounded follow camera, 2D editor view with a tile brush - [docs/2D.md](docs/2D.md)
- Gameplay building blocks: prefabs, scene changes + game data, messages/timers, in-game UI, audio (deterministic mixer, generated sound effects) - [docs/GAMEPLAY.md](docs/GAMEPLAY.md)
- Lua 5.4 scripting (sandboxed, hot reload, errors with file:line) - [docs/SCRIPTING.md](docs/SCRIPTING.md)
- Deterministic simulation and software renderer: same inputs give the same frame hash, so an AI can use it as a test oracle

## Build

```bat
build.bat
build\bin\oe_tests.exe
```

Visual Studio 2022 ("Desktop development with C++") is all you need; its bundled CMake and Ninja are used automatically.

The web runtime (for `oe package --web`) is prebuilt in `runtime/web/`. To bring engine C++ changes to the web build, install the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html), `set EMSDK=C:\path\to\emsdk` and run `build_web.bat` (Linux/macOS: `./build_web.sh`); the results land in `build\bin\web\` and `runtime\web\` - commit them together.

## Usage

```bat
build\bin\oe.exe new MyGame                 :: create a project (a two-level coin collecting sample game)
build\bin\oe.exe editor MyGame              :: editor (the API for agents is also served on http://127.0.0.1:7777)
build\bin\oe.exe editor MyGame --lang ja    :: editor language (en / ko / ja; default: the OS language)
build\bin\oe.exe run MyGame                 :: play in a native window (WASD / Space)
build\bin\oe.exe render MyGame --out shot.png --frames 60
build\bin\oe.exe exec MyGame scene.summary
build\bin\oe.exe exec MyGame entity.create "{\"name\":\"Box\",\"components\":{\"MeshRenderer\":{\"color\":\"#ff8800\"}}}" --save
build\bin\oe.exe import MyGame model.glb     :: copy an external model/texture/sound into the project
build\bin\oe.exe package MyGame             :: standalone game in dist\MyGame\ (MyGame.exe + game\)
build\bin\oe.exe package MyGame --web       :: web build in dist\MyGame-web\ (index.html + wasm, any static host)
build\bin\oe.exe serve dist\MyGame-web      :: run the web build locally (http://127.0.0.1:8080)
build\bin\oe.exe package MyGame --android   :: Android APK dist\MyGame-android\MyGame.apk (no NDK; needs SDK build-tools + Java; --install puts it on the phone)
build\bin\oe.exe api --markdown             :: print the command reference
```

## Editor

`oe editor` opens the **editor** (Dear ImGui docking + ImGuizmo, drawn on the GPU in the engine process). It uses only the command API, so an agent (`oe mcp --connect 7777`) works in the same session as the person at the same time. Without a window (e.g. headless Linux), `oe editor MyGame --screenshot shot.png` renders the editor to an image. Details: [docs/EDITOR.md](docs/EDITOR.md)

- Docking panels: Hierarchy, Inspector, Scene, Game, Assets, Console, Scripts - the layout is saved per project (`.oe/editor.ini`), View > Reset Layout
- Scene view: right-drag + WASD/QE to fly, middle mouse to pan, Alt + left drag to orbit, wheel to zoom, click to select, **move / rotate / scale gizmo** (Q/W/E/R, local/world, snapping), camera and light icons, collider display, 2D view, tile painting (Tiles panel), place assets by dragging them in
- Game view: click it to hand keyboard and mouse to the game (games that lock the mouse get raw mouse motion; Esc releases it), fixed aspect ratios (16:9, ...)
- Hierarchy: multi-select (Ctrl/Shift), drag to re-parent, context menu (rename, duplicate, delete, create child, save as prefab)
- Inspector: generated from reflection, one drag = one undo step, asset fields with a picker + drag and drop
- Assets: double-click to open a scene / edit a script / place a prefab; drop files from Explorer on the window to import them
- Scripts: Lua code editor - syntax highlighting, line numbers, find / replace / replace all (Ctrl+F), syntax errors and global-variable mistakes marked while you type (line markers + underline + problem list), runtime errors marked, Ctrl+S saves and hot-reloads
- Script params in the Inspector: the values a script reads are found automatically and shown as typed fields (numbers, checkboxes, choices, vectors, colors, scene pickers), defaults dimmed, Reset buttons, warnings for keys the script does not read
- Console: log filters + command line (Tab completion)
- Changes an AI makes through the API show up live as notices and marks in the Hierarchy; unsaved changes are confirmed before closing or switching scenes
- Interface language: English, 한국어 (Korean), 日本語 (Japanese) - picked from the OS language, View > Language or `--lang en|ko|ja`; Korean/Japanese input and display (system fonts merged automatically, IME), high-DPI aware, adjustable interface size
- Shortcuts: Ctrl+S save, Ctrl+Z/Y undo/redo, Ctrl+D duplicate, Del delete, F frame, F2 rename, Ctrl+P play/stop
- Editor screenshots for agents: `oe editor MyGame --screenshot shot.png [--select Player] [--play --frames 60] [--lang ko]`


## AI integration

| Method | Command |
|---|---|
| MCP (Claude Code etc.) | `oe mcp <project> [--port 7777]` - every command is an MCP tool, screenshots come back as images. With `--port` a person watches the same session in the editor |
| Attach to a running editor | `oe mcp --connect 7777` |
| CLI / scripts | `oe exec`, `oe script` (JSON in and out, exit code 1 on failure) |
| HTTP | `POST /api/call {"command": "...", "args": {...}}` |

This repository's [.mcp.json](.mcp.json) connects the `samples/Hello` project to Claude Code as an MCP server (build first). The working guide for agents is in [CLAUDE.md](CLAUDE.md) / [AGENTS.md](AGENTS.md), the full API in [docs/API.md](docs/API.md) the architecture in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and the design rules every contributor (human or AI agent) follows in [docs/DESIGN.md](docs/DESIGN.md).

## Layout

```
engine/core      Json, Math, Log, PNG, FileSystem
engine/scene     reflection, components, Scene, systems
engine/script    Lua script host (ScriptHost)
engine/physics   physics (Jolt wrapper PhysicsWorld, Box2D wrapper Physics2D)
engine/audio     audio mixer, WAV, sound effect generator
engine/assets    asset management, glTF/image loading
engine/render    IRenderer, software rasterizer, GPU renderer (sokol_gfx) + shaders, meshes, game UI
engine/api       command registry, HTTP server, editor routes, MCP server
engine/app       Engine (simulation, undo, job queue), project templates
engine/editor    native editor (Dear ImGui panels, gizmo, Scene/Game views, translations)
engine/platform  Platform.h + win32 (D3D11) / web (WebGL2) / android (GLES3) / null (EGL) implementations
tools/oe         CLI
tools/player     game runtime (Name.exe / web wasm / Android .so) + web page template
tools/shaders    shader regeneration scripts (sokol-shdc)
tests/           self tests
third_party/lua  Lua 5.4.8 (MIT)
third_party/jolt Jolt Physics 5.6.0 (MIT)
third_party/box2d Box2D 3.1.1 (MIT)
third_party/stb, cgltf  image decoders/encoders, glTF decoder (PD/MIT, MIT)
third_party/sokol  sokol_gfx + sokol_imgui (zlib) - D3D11 / WebGL2 / GLES3 abstraction
third_party/imgui, imguizmo, imguicolortextedit  Dear ImGui 1.92.9b docking, ImGuizmo, ImGuiColorTextEdit (MIT) - native editor only
templates/       project templates for `oe new`
samples/Hello    sample project (generated from the template)
samples/Showcase rendering sample (glTF fox character, textures, shadows, point lights)
samples/Dungeon  2D top-down action (Box2D, autotiled tileset, pushable crates, bolts, slimes)
samples/Platformer 2D side-scroller (text tilemap, sprite animation, enemies, ? blocks, parallax)
samples/FPS      first-person shooter test game (mouse look, hitscan pistol, reload, moving targets, results screen)
```
