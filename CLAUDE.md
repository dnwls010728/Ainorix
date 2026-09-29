# OwnEngine — guide for AI agents

OwnEngine is a small C++17 game engine designed to be driven and verified by AI agents as easily as by humans. Everything the editor can do is a command in one registry, reachable from the CLI, HTTP and MCP.

## Build & test (Windows)

```bash
build.bat            # Release build -> build/bin/oe.exe, build/bin/oe_tests.exe
build/bin/oe_tests   # unit tests, exit code 0 = pass
```

`build.bat` finds Visual Studio 2022 via vswhere and uses its bundled CMake + Ninja. No third-party dependencies.

## Driving the engine

| Goal | Command |
|---|---|
| Overview of a scene | `oe exec samples/Hello scene.summary` |
| Field schemas | `oe exec samples/Hello component.types` |
| Edit + save | `oe exec samples/Hello component.set '{"id":"Player","type":"MeshRenderer","values":{"color":"#ff0000"}}' --save` |
| Many edits | pipe `{"command":..,"args":..}` lines into `oe script samples/Hello --save` |
| See the result | `oe render samples/Hello --out build/shot.png` then read the PNG |
| Simulate first | `oe render samples/Hello --frames 120 --out build/shot.png` |
| Live session | `oe mcp samples/Hello --port 7777` (MCP on stdio + web editor for the human) |
| Attach to a human's editor | `oe mcp --connect 7777` |

All commands print JSON `{"ok":true,"result":...}` or `{"ok":false,"error":{"code","message","hint"}}`. Read the `hint` — it says how to fix the call. Entities can be referenced by id or by unique name. Full reference: [docs/API.md](docs/API.md) (regenerate with `oe api --markdown > docs/API.md`).

## Code map

- `engine/core` — Json (ordered, diff-friendly), Math, Log (ring buffer, stderr only), Image (PNG encoder), FileSystem.
- `engine/scene` — reflection (`Reflect.h`), built-in components (`Components.h`), `Scene` (entities + component pools, JSON I/O), behaviour systems (`Systems.cpp`).
- `engine/render` — `IRenderer` interface, deterministic `SoftwareRenderer` (color + depth + entity-id buffers), built-in meshes.
- `engine/api` — command registry + all built-in commands (`Commands.cpp`), HTTP server, editor routes, MCP server.
- `engine/app` — `Engine` (scene, fixed-step sim, undo, main-thread job queue), project templates.
- `engine/platform` — `Platform.h` interface; `win32/` and `null/` (headless) implementations.
- `editor/` — web editor (vanilla JS), served by `oe editor`. Uses only the public API.
- `tools/oe/main.cpp` — CLI front-end. `tests/tests.cpp` — self tests.

## Conventions

- stdout is reserved for machine-readable output; logs go to stderr (`OE_LOG_*`).
- Simulation is fixed-step 1/60 s and deterministic: same scene + same inputs = same frame hash.
- New component: struct with `kTypeName`, `kDoc`, `Reflect()` in `Components.h`, register in `Components.cpp`. Serialization, API, schemas and the editor inspector follow automatically. Behaviour goes in `Systems.cpp`.
- New command: `Register(...)` in `Commands.cpp` with a `Params()` schema; mark `mutates=true` if it edits the scene (gives undo + revision bump). Throw `ApiError(code, message, hint)` for caller errors.
- New platform: implement `engine/platform/Platform.h` and add it to `CMakeLists.txt`; see [docs/PLATFORMS.md](docs/PLATFORMS.md).
- Engine code outside `engine/platform/*` must stay portable C++17 (no OS headers).
- Keep `docs/API.md` regenerated when commands or components change, and add a test in `tests/tests.cpp` for new behaviour.
