# OwnEngine

**English** | [한국어](README.ko.md) | [日本語](README.ja.md)

A small C++17 game engine built so that AI agents can drive and verify it as easily as people can.
Everything the editor does is a command in one registry, and the same commands are reachable from the
CLI, HTTP and MCP — an agent and a person can work in the same session at the same time.

![Native editor - games/RPG](docs/images/native-editor.png)

| Game view (`oe render games/RPG --renderer gpu --frames 460`) | |
|---|---|
| ![HD-2D village](docs/images/rpg.png) | `games/RPG`, an HD-2D village: pixel-art billboards in a generated voxel world with modelled houses, swaying grass and trees, a day/night cycle, fog, bloom, colour grading and depth of field. The software renderer draws the same scene deterministically for tests and hashes. |

## Why it is different

- **One API for everything.** Scene edits, simulation, input, screenshots, assets, packaging helpers: each is a
  command with a JSON schema, answering `{"ok":true,"result":..}` or `{"ok":false,"error":{code,message,hint}}`.
  The editor changes the scene only through these commands, so undo, agents and people never get out of sync.
- **Verifiable without a window.** The simulation is fixed-step and deterministic, and the software renderer
  gives the same frame hash for the same scene and inputs on every platform. An agent can step the game,
  inject input, take a screenshot and read it back.
- **Nothing to install.** Visual Studio 2022 is the only requirement on Windows. Lua, Jolt Physics, Box2D,
  sokol_gfx, stb, cgltf, ufbx and Dear ImGui are vendored; the web and Android player runtimes are prebuilt in
  `runtime/`.
- **Engine or project.** Behaviour a Lua script can express lives in the project (templates and games), not in
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
| `oe package <project> [--web\|--android]` | Standalone game: `Name.exe` + `game.pak` (packed, per-game key, scripts as stripped bytecode; [PACKAGING](docs/PACKAGING.md)), a static web folder, or a signed APK / App Bundle |
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
| MCP (Claude Code and others) | `oe mcp <project>`: every command is a tool and screenshots come back as images. [.mcp.json](.mcp.json) connects `games/RPG` once the engine is built |
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
| Rendering | Two renderers for one scene: GPU (sokol_gfx: D3D11, WebGL2, GLES3; MSAA, filtered shadows) and a deterministic multithreaded software renderer. glTF and FBX models, PBR metallic-roughness materials, transparency, point, spot and directional lights, cascaded and local-light shadows | [RENDERING](docs/RENDERING.md) |
| Post-processing and shaders | Exposure with Reinhard/ACES tone mapping, bloom, vignette, FXAA, ambient-only SSAO, glossy screen-space reflections with probe/sky fallback, depth of field, distance fog, colour grading; surface shader graphs (`.shader.json`) with color, emission, vertex offset and normal outputs, also on sprites | [POSTPROCESS](docs/POSTPROCESS.md) |
| Planar reflections | Live mirror/water reflection of moving scenery on a flat mesh; one visible surface per view, shared clipping in both renderers | [PLANAR_REFLECTION](docs/PLANAR_REFLECTION.md) |
| Box decals | Projected stains, impact marks and markings; ordered base-color blending with box/normal fades, 16 visible projectors | [DECALS](docs/DECALS.md) |
| Particle collision | VFX particles stop, bounce or die on static 3D/2D colliders; fixed-step center sweeps, captured bounce/friction and death chains | [PARTICLE_COLLISION](docs/PARTICLE_COLLISION.md) |
| Animation | glTF/FBX skeletal animation (TRS, skins, up to 64 joints per draw) in both renderers, `Animator`, Lua playback; shared sprite/model timeline events and `onAnimationEvent`, author/query commands; Animation panel with isolated playback/scrubbing and event editing | [ANIMATION](docs/ANIMATION.md) |
| Particles | Deterministic emitters (in `.vfx.json` assets) with birth shapes, lifetime curves, flipbooks, turbulence, velocity alignment; `Trail` ribbons | [PARTICLES](docs/PARTICLES.md) |
| VFX authoring | Reusable `.vfx.json` assets and linked VFXSystem, named parameters, Lua control, hot reload and total budgets; Effects panel with grouped fields, curves/gradients, isolated playback, document undo and a reference node graph; 15 presets (including sword slash and six expression effects), particle materials/mesh batching, two-anchor ribbons and bounded death chains, validation and `vfx.*` preview/analysis; axis motion curves, pivots, custom material channels and typed expressions with value nodes | [VFX](docs/VFX.md) |
| Navigation | Navigation mesh baked from static colliders or models (Recast/Detour): paths, nearest point, walk raycast, Lua agents | [NAVIGATION](docs/NAVIGATION.md) |
| Physics | 3D with Jolt and 2D with Box2D: rigid bodies, triggers, character controllers, one-way platforms, collision layers, raycasts; deterministic | [PHYSICS](docs/PHYSICS.md) |
| 2D | Sprites and sheet animation, text tilemaps with tileset files, autotiling and per-tile collision, 2D lights and darkness, HD-2D sprites in a lit 3D world | [2D](docs/2D.md) |
| UI | Text (TrueType, rich text, auto-shrink), panels, buttons (toggles, radio groups, icons), images, sliders, text fields, drop-downs, on-screen joystick, layouts, scroll views, entrance motion, keyboard/gamepad focus; every element takes 9-slice images instead of colors | [UI](docs/UI.md) |
| Gameplay | Prefabs, scene changes and game data, messages and timers, pause, daily date, audio (deterministic 48 kHz mixer, generated sound effects), host bridge for ads and purchases | [GAMEPLAY](docs/GAMEPLAY.md) |
| Scripting | Sandboxed Lua 5.4 per play session, hot reload, errors with file and line, static checks, tweens | [SCRIPTING](docs/SCRIPTING.md) |
| Input | Keyboard, mouse, multi-touch, gamepads (six axes, shared dead zone), all injectable through the API | [INPUT](docs/INPUT.md), [TOUCH](docs/TOUCH.md) |
| Save data | Slots with `save.get/set/delete/flush`; memory-only in tool sessions unless `--save-dir` is given | [SAVE](docs/SAVE.md) |
| Networking | Opt-in: lobbies and RPC, lockstep, rollback, authoritative replication with prediction and interpolation; UDP, TCP, WebSocket; dedicated server | [NETWORK](docs/NETWORK.md) |

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

## Example game

| Project | What it shows |
|---|---|
| `games/RPG` | An HD-2D village ported from a three.js prototype: a generated voxel terrain, modelled half-timbered houses, billboards, swaying grass and trees, sky, clouds, moon and mountains, water, particles, a day/night cycle and an orbit camera. All pictures were made with image generation. Design notes: [docs/RPG.md](docs/RPG.md) |

Run it with `oe run games/RPG` or open it with `oe editor games/RPG`. `oe new <dir>` creates a small starting game
from `templates/default`.

## Repository layout

```
engine/core       Json, Math, Log, PNG, FileSystem, Zip
engine/scene      reflection, components, Scene, behaviour systems
engine/script     Lua script host and static checks
engine/physics    Jolt (PhysicsWorld) and Box2D (Physics2D) wrappers
engine/nav        Navigation mesh (Recast/Detour): bake from the scene, path queries
engine/audio      mixer, WAV, sound effect generator
engine/net        transports, channels, sessions, lockstep/rollback, authority
engine/assets     asset cache, glTF, FBX and image loading
engine/render     IRenderer, software and GPU renderers, shaders, materials, UI, fonts
engine/api        command registry, HTTP server, MCP server
engine/app        Engine (simulation, undo, jobs), packaging, dedicated server
engine/editor     native editor (never linked into games)
engine/team       agent team (never linked into games)
engine/platform   Platform.h + win32, web, android, null implementations
tools/oe          CLI          tools/player   game runtime          tools/shaders  shader regeneration
tests/            self tests (also run as WebAssembly) and their fixture projects
templates/        what `oe new` copies          games/   the example game
runtime/          prebuilt web and Android player runtimes
third_party/      Lua 5.4.8, Jolt 5.6.0, Box2D 3.1.1, Recast/Detour 1.6.0, sokol, stb, cgltf, ufbx, Dear ImGui, ImGuizmo, ImGuiColorTextEdit, Roboto
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
| Feature docs | The table under [Features](#features): each starts with what the feature is and how to use it, and ends with its open items |
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
