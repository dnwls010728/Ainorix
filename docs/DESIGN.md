# Design rules and working agreement

How code is written and changed in OwnEngine, for every contributor — human or AI agent (Claude,
Codex, Gemini, Copilot, Cursor, ...). The goal: whoever picks up the next task keeps the same
architecture, style and quality bar, so the codebase reads as if one careful person wrote it.

Start every task by reading `AGENTS.md` (the same text as `CLAUDE.md`: build, commands, code map,
conventions), this file, and the doc of the area you touch (`docs/*.md`). When this file and a
task conflict, follow the task but say which rule you broke and why (PR description).

## 1. Invariants — never break these

1. **One API for everything.** Every capability is a command in the registry
   (`engine/api/Commands.cpp`), reachable from CLI, HTTP, MCP and the editor. The editor and tools
   change the scene only through `Engine::Call`; no feature is editor-only or agent-only. A new
   feature that changes the scene or game state ships with its command (and its Lua binding when
   gameplay code needs it).
2. **Deterministic simulation.** Fixed 1/60 s steps; same scene + same inputs = same frame hash on
   every platform. Iterate entities in id order, never by pointer or hash order; no wall-clock time,
   randomness only from deterministic sources (Lua `math.random` is seeded per session; tile variants use stable hashes), no `-ffast-math`, no FMA contraction
   (`-ffp-contract=off`). Input is injectable through the API (`input.key`, `input.mouse`,
   `input.touch`, ...) so every behaviour can be tested headless.
3. **The software renderer is the reference.** Hashes, tests and picking use `SoftwareRenderer`.
   Lighting/material changes go into both renderers (`GpuRendererMatchesSoftware` compares them).
4. **Portability boundary.** Everything outside `engine/platform/*` is portable C++17 with no OS
   headers. Graphics only through sokol_gfx; only platform files define `SOKOL_IMPL`. New OS
   features go behind `Platform.h` (or a platform-private header such as `android/AndroidApp.h`).
5. **No new dependencies.** Nothing to install for a build except the compiler toolchain. Third-party
   code is vendored, unmodified, in `third_party/` with its version noted in the code map; behaviour
   changes go into wrapper files (see `third_party/lua_oe`). Prefer writing a small, tested piece of
   code over adding a library (examples: `core/Zip.cpp`, the APK/AAB writers in `app/AndroidPackage.cpp`).
6. **Machine-readable I/O.** stdout carries JSON only (`{"ok":true,"result":..}` /
   `{"ok":false,"error":{code,message,hint}}`); logs go to stderr via `OE_LOG_*`. Errors that a
   caller can fix are `ApiError(code, message, hint)` — the hint says *how* to fix it.
7. **Safe by default.** File access from commands stays inside the project (`Engine::ResolvePath`);
   the HTTP server binds to 127.0.0.1; data read from files (scenes, paks, zips) is validated
   (sizes, offsets, paths — see `ExtractGamePak`).
8. **Data stays diff-friendly.** Scenes, prefabs, tilesets and project files are ordered, pretty
   JSON; default values are not written. Old files must keep loading.

## 2. Where code goes

| Change | Put it in | Also update |
|---|---|---|
| New component | struct with `kTypeName`, `kDoc`, `Reflect()` in `scene/Components.h`, registered in `Components.cpp`; behaviour in `Systems.cpp` | field docs in `Reflect()`, `docs/API.md` (regenerate), area doc |
| New command | `Register(...)` with a `Params()` schema in `api/Commands.cpp`; `mutates=true` if it edits the scene | `docs/API.md` (regenerate), test |
| New Lua function | `L_*` in `script/ScriptHost.cpp`, registered in `ScriptHost::Open()`; throwing code in `Guard(L, ...)` | `docs/SCRIPTING.md`, test |
| Editor UI | `engine/editor/`, scene changes only via `Impl::Call` | `Tr("...")` text + Korean/Japanese rows in `EditorText.cpp`, `docs/EDITOR.md`, screenshot check |
| Rendering | both renderers (`render/SoftwareRenderer.cpp`, `render/GpuRenderer.cpp`, `shaders/Shaders.glsl` + regenerated `.h`) | `docs/RENDERING.md` |
| Platform / OS feature | `engine/platform/<name>/` behind `Platform.h` | `docs/PLATFORMS.md`, `CMakeLists.txt` |
| CLI-only tooling (packaging, process launching) | `tools/oe/main.cpp`; portable logic it needs (formats, encoders) in `engine/app` or `engine/core` so tests reach it | `oe help` text, README command list |
| Default project | `templates/default/`, then regenerate `samples/Hello` | `TemplateGamePlaythrough` test |

Keep one responsibility per file and follow the existing split (e.g. only `PhysicsWorld.cpp` knows
Jolt, only `Physics2D.cpp` knows Box2D). Before adding a helper, search for an existing one
(`core/FileSystem.h`, `core/Json.h`, `core/Image.h` (`Fnv1a64`, PNG), `core/Zip.h`, `render/UI.h`).

## 3. Code style

Match the surrounding code; when in doubt copy the nearest similar code.

- **C++17**, namespace `oe`; file-local helpers in an anonymous namespace. `#pragma once`.
  Includes: own header first, then `<std>` headers, then `"area/Header.h"` project headers.
- **Naming**: types and functions `PascalCase` (`LayoutUI`, `ExtractGamePak`); variables and
  component fields `camelCase`; private members end with `_` (`uiHovered_`); constants
  `kPascalCase`; Lua bindings `L_Name`; commands `area.verb` (`input.touch`, `tilemap.paint`);
  CLI flags `--kebab-case`; JSON keys `camelCase`.
- **Formatting**: 4-space indent, braces on the same line, aim for lines of at most ~140 columns.
- **Comments** explain *why* or the contract (units, ranges, ownership, ordering), not what the next
  line does. Every public header declaration and every component field has a short doc.
  Platform/format code names the spec it follows (e.g. the protobuf message/field numbers it writes).
- **Errors**: `ApiError` for caller mistakes (commands); `bool Fn(..., std::string* error)` for
  engine operations that can fail (loading, writing); no exceptions across the C API of Lua
  (use `Guard`). Never swallow an error silently — log it or return it.
- **Memory/ownership**: values and `std::unique_ptr`; raw pointers only as non-owning references.
- **Warnings**: builds stay warning-free with MSVC `/W4` and clang/gcc `-Wall -Wextra`; new code
  should also be clean under `-Wconversion` (explicit `static_cast`s).
- **Text**: code, comments, docs and commit messages in English. User-facing editor strings go
  through `Tr()`; the READMEs exist in Korean (`README.md`), English and Japanese — change all three.

## 4. Definition of done

A change is finished when all of these hold (copy the list into the PR description):

- [ ] Builds on the platforms it touches; no new warnings.
- [ ] `oe_tests` passes, and a test in `tests/tests.cpp` covers the new behaviour (drive it through
      `Engine::Call` like an agent would; use `TempProject(...)`, clean up files it writes).
- [ ] Verified like a user: `oe render` / `render.screenshot` and read the PNG for visual changes,
      `oe editor --screenshot` for editor UI, the real CLI command for tooling, an external validator
      where one exists (e.g. `apksigner verify`, `aapt dump`, `bundletool validate` for Android output).
- [ ] Docs updated: the area doc, `docs/API.md` regenerated (`oe api --markdown > docs/API.md`),
      `AGENTS.md` + `CLAUDE.md` (identical) when commands, layout or conventions change, READMEs
      for user-visible features.
- [ ] Prebuilt runtimes: if engine C++ used by the player changed, rebuild `runtime/web/` (and
      `runtime/android/` once it exists) or record in the PR that it still needs a rebuild.
- [ ] The PR description states what was **not** verified (no device, no GPU, no SDK, ...).

## 5. Working on long tasks (handoff between sessions and agents)

Large features are built so that another agent can continue at any point:

1. Before coding, add an **Implementation status (work log)** checklist to the feature's doc
   (example: `docs/ANDROID.md`): one line per milestone, what it contains, and what is unverified.
2. Commit and push after every milestone and tick its line in the same commit. Never leave
   finished work only in a working tree.
3. When something cannot be done or checked in the current environment, leave an unchecked line
   that says exactly what is missing and how to do it.
4. The next agent starts from the first unchecked line and reads the commits since the log changed.

## 6. Git and pull requests

- Branch from the latest `main`. One topic per PR; split unrelated work. After a PR is merged,
  start the follow-up from the new `main` and open a new PR — never stack on merged history.
- Commit messages: a short imperative summary line (≤ 72 chars, e.g. "Multi-touch: input.touches,
  input.touch and UIButton.key"), a blank line, then *what and why* wrapped at ~72 columns. Each
  commit builds and passes the tests.
- PR description: summary, per-feature sections, how it was verified, what was not verified,
  follow-ups. Use `.github/pull_request_template.md`.
- Do not commit build outputs (`build/`, `dist/`), local paths, credentials or keystores. The
  only committed binaries are the prebuilt runtimes in `runtime/` and sample assets.

## 7. Reporting

Say what you did and did not check, with the evidence (test names, commands, tool output).
"Implemented but not run on a device" is acceptable; claiming it works without checking is not.
If a rule here turns out to be wrong or missing, fix this file in the same PR and say so.
