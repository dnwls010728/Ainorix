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
│                                    │  components, systems │  color/depth/id bufs │
├──────────────┴─────────────────────┴──────────────────────┴──────────────────────┤
│ core: Json · Math · Log · Image(PNG) · FileSystem          platform: Win32 | Null  │
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

The engine is single-threaded. The HTTP server and the MCP reader run on their own threads and hand work to the main thread through `Engine::PostCall` / `PostJob`; the main loop drains that queue every iteration (`tools/oe/main.cpp: MainLoop`). The CLI and tests call `Engine::Call` directly.

## Scene model

- Entity: id, name, optional parent. Components live in one `ComponentPool<T>` per type.
- Components are plain structs with a static `Reflect()` listing their fields. Supported field types: float, int, bool, string (optionally an enum), vec3, color, entity id.
- World transforms walk the parent chain (`Scene::WorldMatrix`).

## Rendering

`SoftwareRenderer` does near-plane clipping, back-face culling, depth testing and flat Lambert shading from directional lights, writes an entity-id buffer for picking, and optionally draws the editor grid and a selection outline. Output is a `RenderTarget` that can be presented by a window, encoded to PNG, or hashed.

## Roadmap

- Scripting for gameplay (candidate: Lua or WebAssembly modules so scripts are sandboxed and hot-reloadable by agents).
- Hardware renderers (D3D12/Vulkan/Metal/WebGPU) behind `IRenderer`, textured/smooth-shaded meshes, glTF import.
- Physics/collision component, audio, UI.
- Prefabs and multi-scene projects; asset pipeline with content hashes.
- Web/Android/Apple/console platform layers (see [PLATFORMS.md](PLATFORMS.md)).
