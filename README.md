# OwnEngine

**English** | [한국어](README.ko.md) | [日本語](README.ja.md)

A small C++17 game engine built so that AI agents can drive and verify it as easily as people can.
Everything the editor does is a command in one registry, and the same commands are reachable from the
CLI, HTTP and MCP — an agent and a person can work in the same session at the same time.

![Native editor - Showcase sample](docs/images/native-editor.png)

| Game view (`oe render samples/Showcase`) | |
|---|---|
| ![Showcase render](docs/images/showcase.png) | A glTF fox (player controls, follow camera), procedural textures, shadows and point lights, drawn by the deterministic software renderer. The game window, the web build and the editor views draw the same scene with the GPU renderer. |

## Why it is different

- **One API for everything.** Scene edits, simulation, input, screenshots, assets, packaging helpers: each is a
  command with a JSON schema, answering `{"ok":true,"result":..}` or `{"ok":false,"error":{code,message,hint}}`.
  The editor changes the scene only through these commands, so undo, agents and people never get out of sync.
- **Verifiable without a window.** The simulation is fixed-step and deterministic, and the software renderer
  gives the same frame hash for the same scene and inputs on every platform. An agent can step the game,
  inject input, take a screenshot and read it back.
- **Nothing to install.** Visual Studio 2022 is the only requirement on Windows. Lua, Jolt Physics, Box2D,
  sokol_gfx, stb, cgltf and Dear ImGui are vendored; the web and Android player runtimes are prebuilt in
  `runtime/`.
- **Engine or project.** Behaviour a Lua script can express lives in the project (templates and samples), not in
  engine components. The engine keeps what scripts cannot do and what both renderers and the deterministic core
  must agree on.

## Quick start

```bat
build.bat                                   :: Release build -> build\bin\oe.exe, build\bin\oe_tests.exe
build\bin\oe_tests.exe                      :: unit tests, exit code 0 = pass

build\bin\oe.exe new MyGame                 :: create a project (a two-level coin collecting game)
build\bin\oe.exe editor MyGame              :: editor; also serves the API on http://127.0.0.1:7777
build\bin\oe.exe run MyGame                 :: play in a native window (WASD / Space)
```

Windows needs Visual Studio 2022 with "Desktop development with C++"; `build.bat` finds it and uses its bundled
CMake and Ninja. The first build compiles Jolt and takes a few minutes. On Linux, `cmake -S . -B build -G Ninja`
builds the headless tools (CLI, MCP, tests, GPU screenshots through EGL when `libegl-dev`/`libgles-dev` are present).

## Command line

| Command | What it does |
|---|---|
| `oe new <dir> [--name N]` | Create a project from `templates/default` |
| `oe editor <project> [--lang en\|ko\|ja]` | Native editor; `--screenshot f.png` renders it headless |
| `oe run <project>` | Play in a window |
| `oe render <project> --out shot.png [--frames 60] [--renderer gpu]` | Simulate, then save a screenshot |
| `oe exec <project> <command> [json] [--save]` | Run one command, print JSON |
| `oe script <project> [--save]` | Run `{"command":..,"args":..}` lines from stdin |
| `oe mcp <project> [--port 7777]` / `oe mcp --connect 7777` | MCP server on stdio / attach to a running editor |
| `oe import <project> <file>` | Copy a model, texture, sound or font into the project |
| `oe package <project> [--web\|--android]` | Standalone game: `Name.exe` + data, a static web folder, or a signed APK / App Bundle |
| `oe serve <dir>` / `oe serve-game <project>` | Serve a web build locally / run a headless dedicated server |
| `oe api --markdown` | Print the command reference ([docs/API.md](docs/API.md)) |

```bat
oe exec MyGame scene.summary
oe exec MyGame entity.create "{\"name\":\"Box\",\"components\":{\"MeshRenderer\":{\"color\":\"#ff8800\"}}}" --save
oe render MyGame --frames 120 --out shot.png
```

## Working with AI agents

| Method | How |
|---|---|
| MCP (Claude Code and others) | `oe mcp <project>`: every command is a tool and screenshots come back as images. [.mcp.json](.mcp.json) connects `samples/Hello` once the engine is built |
| Attach to a person's editor | `oe mcp --connect 7777`, or without MCP `oe exec --connect 7777 <command> [json]` and `oe script --connect 7777`; the edits show up live in the editor |
| CLI and scripts | `oe exec`, `oe script` (JSON in and out, exit code 1 on failure) |
| HTTP | `POST http://127.0.0.1:7777/api/call` with `{"command": "...", "args": {...}}` |

The working guide for agents is [AGENTS.md](AGENTS.md) (the same text as [CLAUDE.md](CLAUDE.md)); the rules every
contributor follows are in [docs/DESIGN.md](docs/DESIGN.md).

**Agent team.** Inside the editor you can keep several agents as a team and give them work in a chat panel
(`@name`, `@all`, hand-offs between agents). The engine does not call model APIs: it runs the coding-agent CLIs
installed on the PC (Claude Code, Codex) as child processes. Windows only, in progress —
[docs/TEAM.md](docs/TEAM.md).

Agent usage: `team.usage` reads agent/day token totals and per-turn cost. The Team panel shows today in UTC; Claude profiles can set `maxBudgetUsd` (0 disables it).

Tasks run beside chat: `team.delegate/report/task/tasks` start fresh workers with bounded deadlines and evidence reports. `canCall` defaults to none; the lead may call everyone. A successful worker without a tool report is done but unverified.

Use `team.reset {id}` (or Reset Conversation in the roster) to start fresh for one idle agent while keeping chat and usage. Optional `team.settings {rotateAboveInputTokens: N}` rotates the next turn at the input-size threshold; 0 disables it by default. Fresh context can lose cache savings.

## Editor

`oe editor` opens a docking editor (Dear ImGui + ImGuizmo, drawn on the GPU in the engine process): Hierarchy,
Inspector generated from reflection, Scene view with move/rotate/scale gizmos and a 2D view with a tile brush, Game
view that hands keyboard and mouse to the game, Assets with thumbnails and drag-and-drop import, a Lua code editor
with live error marks, Console with a command line, History, Network and Team panels. One drag is one undo step,
prefabs are edited in isolation, and the interface is available in English, Korean and Japanese.
Assets offers Grid/List views, filename/path search and kind filters with counts. Shallow panels use compact rows so filenames stay visible.
The interface uses 17 px text, stronger selection/focus contrast, labeled X/Y/Z fields that adapt to panel width, and Console severity labels with wrapping filters. View > Interface size adjusts the scale.
Script tabs have a close/close others/close all context menu with unsaved-change protection. Lua completion suggests API members and file identifiers while typing (Ctrl+Space requests suggestions; Tab/Enter inserts them).

The Shader Graph workspace edits surface nodes and uniforms, previews the surface, creates shader-linked materials, and applies them to selected meshes. Open shader/material assets from Assets or View > Shader Graph ([guide](docs/SHADER_EDITOR.md)).

Details and shortcuts: [docs/EDITOR.md](docs/EDITOR.md).

## Features

| Area | What is there | Doc |
|---|---|---|
| Rendering | Two renderers for one scene: GPU (sokol_gfx: D3D11, WebGL2, GLES3; MSAA, filtered shadows) and a deterministic multithreaded software renderer. glTF models, PBR metallic-roughness materials, transparency, point and directional lights, shadows | [RENDERING](docs/RENDERING.md) |
| Post-processing and shaders | Exposure and tone mapping, bloom, vignette, FXAA, depth of field; surface shader graphs (`.shader.json`) with color, emission, vertex offset and normal outputs | [POSTPROCESS](docs/POSTPROCESS.md), [TOON](docs/TOON.md) |
| Animation | glTF skeletal animation (TRS, skins, up to 64 joints) in both renderers, `Animator`, Lua playback | [ANIMATION](docs/ANIMATION.md) |
| Particles | Deterministic emitters with birth shapes, lifetime curves, flipbooks, turbulence, velocity alignment; `Trail` ribbons | [PARTICLES](docs/PARTICLES.md) |
| VFX authoring | Reusable `.vfx.json` assets and linked VFXSystem, named parameters, Lua control, hot reload and total budgets; eight presets, strict validation and isolated preview/analysis through `vfx.*` | [VFX](docs/VFX.md) |
| Physics | 3D with Jolt and 2D with Box2D: rigid bodies, triggers, character controllers, one-way platforms, collision layers, raycasts; deterministic | [PHYSICS](docs/PHYSICS.md) |
| 2D | Sprites and sheet animation, text tilemaps with tileset files, autotiling and per-tile collision, 2D lights and darkness, HD-2D sprites in a lit 3D world | [2D](docs/2D.md) |
| UI | Text (TrueType, rich text, auto-shrink), panels, buttons (toggles, radio groups, icons), images, sliders, text fields, drop-downs, on-screen joystick, layouts, scroll views, entrance motion, keyboard/gamepad focus; every element takes 9-slice images instead of colors | [UI](docs/UI.md) |
| Gameplay | Prefabs, scene changes and game data, messages and timers, pause, daily date, audio (deterministic 48 kHz mixer, generated sound effects), host bridge for ads and purchases | [GAMEPLAY](docs/GAMEPLAY.md) |
| Scripting | Sandboxed Lua 5.4 per play session, hot reload, errors with file and line, static checks, tweens | [SCRIPTING](docs/SCRIPTING.md) |
| Input | Keyboard, mouse, multi-touch, gamepads (six axes, shared dead zone), all injectable through the API | [INPUT](docs/INPUT.md), [TOUCH](docs/TOUCH.md) |
| Save data | Slots with `save.get/set/delete/flush`; memory-only in tool sessions unless `--save-dir` is given | [SAVE](docs/SAVE.md) |
| Networking | Opt-in: lobbies and RPC, lockstep, rollback, authoritative replication with prediction and interpolation; UDP, TCP, WebSocket; dedicated server | [NETWORK](docs/NETWORK.md), [NETWORK_SAMPLES](docs/NETWORK_SAMPLES.md) |

## Platforms

| Platform | Status | Notes |
|---|---|---|
| Windows (x64) | Working | Native window, Direct3D 11, editor, `oe package` → `Name.exe` |
| Web (WebAssembly) | Working | WebGL2; prebuilt runtime in `runtime/web/`, so `oe package --web` needs no Emscripten |
| Android | Working on the emulator; checks on physical devices remain | APK / App Bundle written and signed by `oe` (no Gradle) — [docs/ANDROID.md](docs/ANDROID.md) |
| Linux, headless | Working | `null` platform: CLI, MCP, tests, screenshots |
| macOS, iOS | Planned | [docs/APPLE.md](docs/APPLE.md) |
| Linux desktop, consoles | Planned | [docs/PLATFORMS.md](docs/PLATFORMS.md) |

After changing engine C++ that the player uses, rebuild the runtimes: `build_web.bat` (needs the Emscripten SDK,
`set EMSDK=...`) and `build_android.bat` (needs the Android NDK), then commit `runtime/`.

## Samples and games

Run any of them with `oe run <path>` or open it with `oe editor <path>`.

| Project | What it shows |
|---|---|
| `samples/Hello` | The `oe new` template: a two-level coin game |
| `samples/Showcase` | Rendering: textured glTF fox with animation, shadows, point lights, post-processing presets |
| `samples/Platformer`, `samples/Dungeon` | 2D side-scroller and top-down action on Box2D: tilemaps, autotiling, sprite animation |
| `samples/Wickbound` | A complete 10-minute survivor roguelite: prefabs with scripts, 2D lighting, menus and dialogs, save data |
| `samples/FPS` | First-person shooter: mouse look, hitscan, moving targets |
| `samples/HD2D`, `samples/Water` | Pixel-art sprites in a lit 3D world with depth of field; a rolling sea from a shader graph |
| `samples/ShaderLab`, `samples/WuwaToon`, `samples/VFXLab` | Surface shader graphs, a toon shading study, particle and trail effects |
| `samples/NetCoop`, `NetDuel`, `NetArena`, `NetChase` | Lockstep co-op, rollback versus, authoritative arena and third-person race, with in-game lobbies |
| `games/OnlineRPG` | An online RPG prototype in the HD-2D look with a login server and channel servers players enter at any time: ground items, pick-up, bag, drop, chat and speech bubbles — design document: [docs/ONLINERPG.md](docs/ONLINERPG.md) |
| `games/DongleZoo` | A shippable portrait merge puzzle for phones: shop, missions, ads and purchases through the host bridge — design document: [docs/DONGLEZOO.md](docs/DONGLEZOO.md) |

## Repository layout

```
engine/core       Json, Math, Log, PNG, FileSystem, Zip
engine/scene      reflection, components, Scene, behaviour systems
engine/script     Lua script host and static checks
engine/physics    Jolt (PhysicsWorld) and Box2D (Physics2D) wrappers
engine/audio      mixer, WAV, sound effect generator
engine/net        transports, channels, sessions, lockstep/rollback, authority
engine/assets     asset cache, glTF and image loading
engine/render     IRenderer, software and GPU renderers, shaders, materials, UI, fonts
engine/api        command registry, HTTP server, MCP server
engine/app        Engine (simulation, undo, jobs), packaging, dedicated server
engine/editor     native editor (never linked into games)
engine/team       agent team (never linked into games)
engine/platform   Platform.h + win32, web, android, null implementations
tools/oe          CLI          tools/player   game runtime          tools/shaders  shader regeneration
tests/            self tests (also run as WebAssembly)
templates/        what `oe new` copies          samples/, games/   see above
runtime/          prebuilt web and Android player runtimes
third_party/      Lua 5.4.8, Jolt 5.6.0, Box2D 3.1.1, sokol, stb, cgltf, Dear ImGui, ImGuizmo, ImGuiColorTextEdit, Roboto
```

A longer map of the code is in [AGENTS.md](AGENTS.md); the structure is drawn in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Documentation

| Read this | For |
|---|---|
| [AGENTS.md](AGENTS.md) / [CLAUDE.md](CLAUDE.md) | Build, driving the engine, code map, conventions (start here, human or agent) |
| [docs/DESIGN.md](docs/DESIGN.md) | Invariants, where code goes, code style, definition of done, hand-over between sessions |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | How the layers fit together |
| [docs/API.md](docs/API.md) | Every command and component, generated by `oe api --markdown` |
| Feature docs | The table under [Features](#features): each starts with what the feature is and how to use it, and ends with a work log of what is done and what is not verified |
| [docs/PLATFORMS.md](docs/PLATFORMS.md), [docs/ANDROID.md](docs/ANDROID.md), [docs/APPLE.md](docs/APPLE.md) | Platform status and porting |
| [docs/ROADMAP.md](docs/ROADMAP.md) | Feature roadmap and the prioritized task list |
| [docs/NETWORK_NOTES.md](docs/NETWORK_NOTES.md), [docs/TEAM_LOG.md](docs/TEAM_LOG.md) | Work logs and implementation notes that outgrew their feature doc |

Documentation is written in English; this README is also kept in [Korean](README.ko.md) and
[Japanese](README.ja.md).

## Contributing

Read [docs/DESIGN.md](docs/DESIGN.md) first. In short: every feature ships with its command, a test in
`tests/tests.cpp` and its doc; simulation stays deterministic; lighting changes go into both renderers; no new
dependencies; and a change is reported with what was and was not verified.

## License

MIT — see [LICENSE](LICENSE). Third-party code in `third_party/` keeps its own licenses (MIT, zlib, public domain,
Apache 2.0 for the Roboto font).
