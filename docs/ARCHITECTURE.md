# Architecture

```
            ┌───────────── front-ends (all call the same registry) ─────────────┐
            │  oe CLI (exec/script/render)   Web editor (HTTP)   MCP (stdio)     │
            └───────────────────────────────┬────────────────────────────────────┘
                                            │ Engine::Call(name, args) -> {ok,result|error}
┌───────────────────────────────────────────▼──────────────────────────────────────┐
│ Engine (engine/app)                                                               │
│  Scene · fixed-step simulation · undo/redo · revision counter · main-thread jobs  │
├──────────────┬─────────────────────┬──────────────────────┬──────────────────────┤
│ CommandRegistry (api)              │ Scene + reflection   │ IRenderer            │
│  schema-validated commands         │  (scene)             │  SoftwareRenderer    │
│                                    │  components, systems │  GpuRenderer (sokol) │
├──────────────┴─────────────────────┴──────────────────────┴──────────────────────┤
│ core: Json · Math · Log · Image · FileSystem      platform: Win32 | Web | Null     │
└──────────────────────────────────────────────────────────────────────────────────┘
```

## Design principles for AI-friendliness

1. **One API, many transports.** A command is registered once (name, summary, JSON schema, handler) and is automatically available as a CLI call, an HTTP endpoint and an MCP tool. The human editor uses the same API, so there is no hidden functionality an agent cannot reach.
2. **Self-describing.** `api.list` returns JSON schemas for every command; `component.types` returns schemas, docs and defaults for every component. Both are generated from the reflection data, so they cannot drift from the code.
3. **Errors that teach.** Every failure returns a stable `code`, a `message` and usually a `hint` (valid field names, allowed enum values, the command's usage line, the closest command names).
4. **Verifiable.** `render.screenshot` returns a PNG (as an MCP image for agents); `render.pick` maps pixels to entities; frame hashes are deterministic, so a test can assert "this scene still renders the same".
5. **Deterministic simulation.** Fixed 1/60 s steps, ordered component iteration (`std::map` by entity id), no wall-clock dependence in `sim.step`. `input.key` injects the same input a keyboard would.
6. **Safe to experiment.** Every scene edit is undoable; `sim.stop` restores the pre-simulation scene; file access is sandboxed to the project directory; the HTTP server binds to 127.0.0.1 and rejects non-local `Host` headers and non-JSON posts (CSRF / DNS-rebinding protection).
7. **Diff-friendly data.** Scenes and projects are ordered, pretty-printed JSON — agents can read and edit them directly and reviewers can diff them.
8. **Observable.** `log.get` streams structured log entries; `sim.state` exposes a `revision` counter so tools (and the editor) refresh only when something changed.

## Threading

The engine is single-threaded (the GPU device lives on the main thread too). The HTTP server, each editor viewport stream (WebSocket; it JPEG-encodes frames off the main thread) and the MCP reader run on their own threads and hand work to the main thread through `Engine::PostCall` / `PostJob`; the main loop drains that queue every iteration (`tools/oe/main.cpp: MainLoop`). The CLI and tests call `Engine::Call` directly.

## Scene model

- Entity: id, name, optional parent. Components live in one `ComponentPool<T>` per type.
- Components are plain structs with a static `Reflect()` listing their fields. Supported field types: float, int, bool, string (optionally an enum), vec3, color, entity id.
- World transforms walk the parent chain (`Scene::WorldMatrix`).

## Rendering

Both renderers consume the same scene description (`render/RenderScene.h`, UI quads from `render/UI.h`). `SoftwareRenderer` is the deterministic reference: clipping, culling, depth testing, textured smooth/flat Lambert shading, shadow map, an entity-id buffer for picking, editor grid and selection outline; its `RenderTarget` is presented by a window, encoded to PNG, or hashed. `GpuRenderer` draws the same thing through sokol_gfx (D3D11 / WebGL2 / GLES3) with MSAA, filtered shadows and mipmaps, straight into the window's swapchain or offscreen with readback (editor viewport, GPU screenshots). The platform supplies its `GpuDevice`. See [RENDERING.md](RENDERING.md).

## Roadmap

- Scripting for gameplay (candidate: Lua or WebAssembly modules so scripts are sandboxed and hot-reloadable by agents).
- Materials with custom shaders and post-processing effects on the GPU renderer (the composite pass is the hook); more sokol_gfx backends (Metal, Vulkan, WebGPU).
- TTF fonts for the UI (non-ASCII text), UI layout containers. (Physics, audio, UI, prefabs: done — see [PHYSICS.md](PHYSICS.md), [GAMEPLAY.md](GAMEPLAY.md).)
- Prefabs and multi-scene projects; asset pipeline with content hashes.
- Android/Apple/console platform layers (Web and Windows are done; see [PLATFORMS.md](PLATFORMS.md)).
