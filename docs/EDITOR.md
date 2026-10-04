# Editor

## Implementation status (work log): P7

- [x] P7.1: Command-backed hierarchy clipboard with subtree/reference remapping,
      validation, atomic paste and native editor Ctrl+C/Ctrl+V.
- [x] P7.2: Named undo/redo history, history.list/history.go and History panel.
- [x] P7.3: Isolated prefab edit/save/close commands and editor workflow with
      save/discard prompts and restoration of the original scene/history.
- [x] P7.4: Asset thumbnail previews with lazy caching and hot reload.
- [x] P7.5: Native Windows open/save/import dialogs behind Platform.h, headless
      fallback, project containment and cancel/error handling.
- [x] P7.6: Windows tests, user-driven editor events, localized screenshots,
      API/docs refresh and runtime rebuild or explicit platform follow-ups.

Windows: 141 tests, zero failed checks. Node/WASM: 128 tests, zero failed checks
(native/GPU tests skip). Added API tests cover atomic clipboard validation,
merged history/cursor behavior, prefab isolation/reference remapping, bounded
preview pixels/framing/hot reload and project containment. Window-event tests
exercise Ctrl+C/V/Z/S, history focus, prefab save/discard and chooser result paths.
English/Korean/Japanese D3D11 CLI screenshots were inspected; the History panel
has a separate event-driven screenshot. Web player rebuilt from this source.

- [ ] Exercise the actual Windows chooser UI interactively (automated tests use
      a Window adapter for selected/cancelled/error/unavailable results).
- [ ] Rebuild Android arm64-v8a/x86_64 players; NDK/SDK unavailable on this host.
      Verify on Android hardware and Linux/EGL; neither environment is available.

P7 proceeds independently of the remaining P6 Android/Linux hardware checks.
It starts from updated main; P6 verification/sample changes have a separate branch.

`oe editor <project>` opens the editor: Dear ImGui (docking) + ImGuizmo panels drawn with sokol_gfx in the engine process. The Scene and Game views are GpuRenderer textures, so nothing is streamed or encoded. It needs a window and a GPU backend (Windows today; a macOS/Linux desktop platform layer is enough, see [PLATFORMS.md](PLATFORMS.md)).

![Editor](images/native-editor.png)

The editor only uses public commands (`Engine::Call`), so an agent attached with `oe mcp --connect 7777` sees and can do exactly what the person in the editor does. The editor keeps the HTTP API running (`--port`, default 7777): `oe mcp --connect` and `POST /api/call` work while it is open, and every edit that arrives through the API shows up as a notice (`API: component.set Player MeshRenderer`) plus a fading dot on the entity in the Hierarchy.

Without a window (Linux `null` platform, CI) the editor can still be rendered to an image for agents and tests: see [For agents](#for-agents-looking-at-the-editor).

## Panels

- **Hierarchy** - tree of entities (search by name, or by component with 3+ letters). The **eye** at the right of a row shows or hides what the entity draws (its UI element, sprite, mesh or tilemap: the component's `visible` field) - the way to look at a hidden screen or dialog while editing. Click, Ctrl+click (toggle), Shift+click (range). Drag an entity onto another to parent it, onto empty space to move it to the root; drop a prefab from Assets to instantiate it. Right-click: rename, duplicate, delete, create child, save as prefab. Double-click frames the entity.
- **Inspector** - generated from `component.types`: sliders for ranged numbers, drag fields for vectors (one drag = one undo step via `component.set {merge}`), color pickers, enum combos, entity pickers (also accept entities dragged from the Hierarchy), multi-line JSON fields. Asset fields (mesh, texture, material, script, prefab, clip, font) have a `...` picker and accept assets dragged from the Assets panel; hovering the picker of a texture, model, material or prefab field shows a preview of the file it points at. Right-click a component header: remove, reset to defaults, copy as JSON. A Script component's **params** show as typed fields read from the script (`script.params`): numbers, checkboxes, choices, vectors, colors, scene/prefab/sound pickers. Values the entity does not set appear dimmed with the script's default; editing one sets it, **Reset** goes back to the default, keys the script never reads are flagged in amber with **Remove**. The tooltip shows the description (the comment on the line that reads it), the default and the line. **+ Add** adds a key, **JSON** switches to the raw text.
- **Scene** - free camera, grid, selection outline, collider wireframes, markers for cameras / lights / sounds, a ring for the reach of every `Light2D`, and the transform gizmo. The **2D** toggle looks along -Z: the grid becomes an XY grid that follows the camera and shows decade levels (0.1, 1, 10, 100 ... units) as in Unity: a level fades in once its cells are 8 pixels wide and is fully drawn at 80 (red X axis, green Y axis), and the gizmo is planar (move and scale on X/Y, rotate around Z). Click selects (software-renderer picking, identical to `render.pick`); drop assets to place them (models, prefabs, sprites from images, sounds; textures / materials / scripts onto the entity under the cursor).
- **Game** - the scene's active camera with in-game UI, letterboxed to Free / 16:9 / 16:10 / 4:3. **While stopped it is the UI editor**: click a UI element to select it (the topmost one under the pointer; Ctrl+click adds), drag it to move it (`x`, `y`), drag one of the eight handles to resize it (`width`, `height`; the opposite edge stays put whatever the anchor), arrow keys nudge by 1 reference pixel (Shift: 10). The outline shows the element's name and size; a drag is one undo step. Elements placed by a `UILayout` keep the position their parent gives them (their size still changes). Text and images sized by their content get explicit sizes once a handle is dragged. While playing, click it to give the game the keyboard, mouse and gamepad (green frame); click elsewhere to take them back. Gamepad axes and named buttons are forwarded through commands, with held input released when focus is lost. Games that lock the mouse (`input.lockMouse`) get raw relative motion; Esc releases it.
- **Network** - available in every project through View > Network, which opens and focuses the tab. Projects without networking show a disabled-state explanation and a project.json example; the panel never enables networking implicitly. Old editor.ini files migrate the new tab beside Inspector without resetting other panels, and new files persist its visibility with NetworkPanel. Enabled projects show Players 1..min(8,maxPlayers) beside Play. Multiple players run independent in-process loopback peers; the Game Player selector chooses the view/input recipient and releases held input on switching. View > Network shows readiness, peer frames/state, RTT/loss/bytes/pending traffic, desync reports and a one-way latency slider (60 Hz frames). `net.spawn_local_peers`, `net.peer_call`, `net.local_peers` and `net.simulate` expose the same tools to agents. Stop restores the edit-time scene and destroys peers. See [NETWORK.md](NETWORK.md).
- **Team** - the project's AI agent team, a tab beside the Hierarchy (View > Team): each agent's picture with a status dot (grey idle, hollow offline, blue queued, pulsing amber thinking, pulsing green working, red error), name, agent CLI and model, what it is doing and for how long, with **Stop** while a turn runs. **Add Agent** or a double-click opens the profile dialog: picture (built-in or an uploaded image), name, description, agent CLI (entries for CLIs that are not installed on this PC are disabled, with the install command as a tooltip), model, access level and instructions. Right-click: Edit, Make Lead, Stop, Delete; drag a row onto another to reorder. Everything goes through the `team.*` commands, so an attached agent can manage the team too. See [TEAM.md](TEAM.md).
- **Team Chat** - a tab beside the Console (View > Team Chat): talk to the team. `@name` gives an agent a turn (Tab completes the mention), `@all` everyone, no mention goes to the lead; Enter sends, Shift+Enter is a new line. A working agent has a live line with what it is doing and **Stop**; its reply shows how many tool steps the turn took (click to list them). Drop files on the panel to attach them to the next message; type `#` to pick a project file from a list with previews. Pictures an agent's turn produced appear under its reply; click one for the viewer (fit / actual size, Import to assets). Edits an agent makes through the API appear as notices and Hierarchy dots in that agent's color.
- **Assets** - project files by kind. Double-click: open a scene, edit a script, instantiate a prefab. Drag into the Scene, Hierarchy or Inspector. Files dropped on the window from Explorer are imported (same rules as `oe import`).
- **Console** - the engine log with level/text filters, and a command line: `command.name {"json": "args"}` (Tab completes command names, Up/Down browse history).
- **Tiles** - palette of the selected Tilemap's tile characters, drawn from its tileset (autotiles show their fully connected frame; tooltips list collision, autotile mode and variants). Picking one turns on **Paint Tiles**: in the Scene view left drag paints, right drag erases, Shift+drag fills a rectangle, Ctrl+click picks the tile under the cursor, middle drag pans; the cell under the cursor is outlined with its column, row. Autotiles connect as you paint and each stroke is one undo step (`tilemap.paint {merge}`). See [2D.md](2D.md#tilemap).
- **Scripts** - Lua code editor tabs ([ImGuiColorTextEdit](https://github.com/goossens/ImGuiColorTextEdit)): syntax highlighting (engine API names such as `scene`, `input`, `self` in their own color), line numbers, bracket matching, auto indent, multiple cursors, undo/redo, **find / replace / replace all** (Ctrl+F; Aa = match case, [] = whole word, Ctrl+G next). While you type, the script is checked with `script.check` (0.35 s after the last key): syntax errors and warnings (a global assigned without `local`, an unknown global) are marked on the line number and underlined, with the message as a tooltip, and listed under the code - click one to jump there. Runtime errors of the running game (`script.errors`) are marked the same way. Ctrl+S (or Save) writes through `script.write`, which hot-reloads the script, also while the game runs.

The theme is tuned for reading (`ApplyStyle` in `Editor.cpp`): 16 px text (Korean/Japanese glyphs merged 6 % larger to match Roboto), hints and secondary text at about 6:1 contrast, fields with a lighter fill and an outline, menu and tab bars clearly darker than the panels, the accent color only for what is selected or active (inspector section bars are neutral), an overline on the visible tab of every dock, guide lines between parents and children in the Hierarchy. View > Interface size scales all of it.

The layout (docking), panel visibility, interface size and language, gizmo/snap settings and the scene camera are saved per project in `<project>/.oe/editor.ini` (a dot folder: not packaged by `oe package`). View > Reset Layout restores the default.

## Controls

| Where | Input | Action |
|---|---|---|
| Scene | Right mouse drag | look around; with WASD / Q E fly, Shift faster, wheel = fly speed |
| Scene | Middle mouse drag | pan |
| Scene | Alt + left drag | orbit |
| Scene | Wheel | zoom |
| Scene / Hierarchy | Q / W / E / R | select / move / rotate / scale gizmo |
| | X | gizmo local / world |
| | Ctrl while dragging a gizmo | toggle snapping (toolbar: Snap, step sizes) |
| | F, double-click | frame selection (a whole map for a Tilemap) |
| Scene, Paint Tiles on | Left / right drag, Shift+drag, Ctrl+click | paint / erase / fill a rectangle / pick a tile |
| | F2, Del, Ctrl+D, Esc | rename, delete, duplicate, deselect |
| Anywhere | Ctrl+S, Ctrl+Z, Ctrl+Y (Ctrl+Shift+Z), Ctrl+N | save, undo, redo, new scene |
| Anywhere | Ctrl+P | play / stop (stop restores the edit-time scene) |

Leaving with unsaved changes (closing the window, opening or creating a scene) asks to save first. Saving is disabled while a play session runs, because Stop restores the scene.

## Clipboard, history and prefab sources

Ctrl+C copies selected hierarchy subtrees; Ctrl+V pastes fresh entities in one undo
step. Nested selections are copied once. Reflected entity fields point to the
new copies; references outside the copied set become null. Script JSON params
remain unchanged. Root transforms retain their local values. Clipboard v1 accepts
at most 10,000 entities and 8 MiB; malformed documents leave the scene untouched.
Agents use `entity.copy {ids:[...]}` and `entity.paste {document:...,parent:...}`.

View > History opens the named undo/redo timeline. Select Initial state or an
entry to move the cursor; a new edit after undo drops the redo tail. Merged drags
remain one entry. `history.list` exposes entries and cursor; `history.go {cursor}`
moves to a position. History changes are disabled during play.

Right-click a prefab asset and choose Edit Prefab. This opens its source in an
isolated scene with its own history; Ctrl+S writes the source atomically.
Close Prefab restores the original scene, dirty state and history. Unsaved
prefab changes require Save, Don't Save or Cancel. Saving requires exactly one
root. Source editing disables play, scene switching and network commands;
instances already placed in another scene are not refreshed automatically.
Equivalent commands: `prefab.edit {path}`, `prefab.save`, `prefab.close {discard}`
and `prefab.state`. Internal entity references survive source save/instantiate.

## Thumbnails and file dialogs

Assets shows 64-pixel previews for textures, models, materials, prefabs and scenes.
View > Asset Thumbnails toggles them. Rendering is lazy (one visible asset per
frame), with a bounded 128-entry cache. Stopped editors poll asset changes every
two seconds; dependency changes and periodic cache expiry refresh previews.
Scenes/prefabs preview geometry without running scripts, audio or physics;
material previews use a sphere and fixed shader time. UI-only assets have no
geometry preview. `asset.preview {path,size,pixels,out}` returns a PNG as base64,
optionally RGBA-packed integer pixels or a project-local PNG file (16..256 px).

File > Open Scene (Ctrl+O), Save Scene As and Import Asset use the Windows native
chooser. Scene open/save paths must stay inside the project and end in
`.scene.json`; imports use `asset.import {source}` with the existing CLI rules.
Cancellation leaves state unchanged; errors are reported. Platforms without
native dialogs keep the path-entry save dialog and Assets/drop-file workflow.

## Languages

The interface is available in English, Korean (한국어) and Japanese (日本語). The editor starts in the OS language (`PlatformUserLanguage`), then in the choice made under View > Language (saved in `.oe/editor.ini`); `oe editor --lang en|ko|ja` overrides both. API names stay English in every language: commands, component types and fields, and the entity names the Create menu gives (scripts refer to them).

For text, the editor merges system fonts into its UI font: Korean (Malgun Gothic on Windows, Apple SD Gothic Neo on macOS, Noto CJK / Nanum on Linux) and Japanese (Yu Gothic / Meiryo / MS Gothic on Windows, Hiragino on macOS, Noto CJK / IPA Gothic on Linux), the interface language's first. IME input (Korean, Japanese) arrives through `WM_CHAR`, with the IME window placed by Dear ImGui.

Translations live in `engine/editor/EditorText.cpp`: UI strings are written in English in the code as `Tr("...")` (format strings keep their `%` arguments) and windows / popups use `TrId("...")` so their docking IDs do not change with the language. Add a catalog row for every new string; the `EditorTranslations` test scans `engine/editor/*.cpp` and fails for a `Tr` string without Korean and Japanese text or with different `%` arguments.

## For agents: driving the open editor

The editor serves the command API on `127.0.0.1:7777` (`--port` changes it). Three ways in, all the same commands and all shown in the editor as notices:

```bash
oe exec --connect 7777 component.set '{"id":"Player","type":"MeshRenderer","values":{"color":"#ff0000"}}'   # one command
oe script --connect 7777 < commands.jsonl      # many: {"command":..,"args":..} per line
oe mcp --connect 7777                           # as MCP tools (stdio)
curl -X POST http://127.0.0.1:7777/api/call -H "Content-Type: application/json" -d '{"command":"scene.summary","args":{}}'
```

`--save` saves the editor's scene after a successful call. `oe exec <project> ...` without `--connect` is a different thing: it opens the project files in its own process, and the open editor does not see what it does until the files are reloaded.

## For agents: looking at the editor

The editor can be rendered headless (needs a GPU backend: D3D11/WARP on Windows, EGL on Linux with `libegl-dev libgles-dev`):

```bash
oe editor samples/Hello --screenshot build/editor.png                 # default layout, 1600x900
oe editor <network-project> --players 3 --play --frames 100 --screenshot build/network-editor.png
oe editor samples/Hello --screenshot build/editor.png --select Player # inspector + gizmo on Player
oe editor samples/Platformer --screenshot build/editor.png --2d --select Player  # 2D Scene view: XY grid + planar gizmo
oe editor samples/Hello --screenshot build/editor.png --play --frames 60  # Game view after 1 s of play
oe editor samples/Wickbound --screenshot build/editor.png --game --select Start  # stopped Game view: UI editing with a selection
oe editor samples/Hello --screenshot build/editor.png --lang ja       # Japanese interface (default for screenshots: en)
oe editor samples/FPS --screenshot build/editor.png --script scripts/target.lua  # code editor with the script's problems
oe editor samples/Hello --screenshot build/editor.png --team --agent new  # Team panel and the agent profile dialog (--agent <id> edits one)
oe editor samples/Hello --screenshot build/editor.png --chat             # Team Chat panel
```

Tests drive it the same way: `NativeEditor::Update(events, w, h, dpi, dt)` takes window events (keys, mouse, text), `DrawToImage` reads the frame back (see `NativeEditorHeadless` and `NativeEditorTilePainting` in `tests/tests.cpp`; `SetTileBrush` and `SceneViewRect` help aim the brush; `NativeEditorUIEditing` selects, moves and resizes UI through `GameViewRect`). The running Game view also forwards the mouse wheel (`input.mouse {wheel}`).

## Code

`engine/editor/` (library `oe_editor`, linked by `oe` and `oe_tests`, never by games):

- `Editor.h` - `NativeEditor` (Init / Update / DrawToWindow / DrawToImage) and `RunNativeEditor` (main loop: posted API jobs, window events, frame, present).
- `Editor.cpp` - window events -> Dear ImGui, command helpers, menus, toolbar, status bar, default dock layout, save prompts, preferences (`[OwnEngine][Editor]` in the .ini).
- `EditorPanels.cpp` - Hierarchy, Inspector (including typed Script params), Assets, Console.
- `EditorScripts.cpp` - Scripts panel: the code editor, live `script.check`, problem markers.
- `EditorViewports.cpp` - Scene view (camera, picking, gizmo, markers, drops) and Game view (input forwarding, mouse lock).
- `EditorNetwork.cpp` - multiplayer preview diagnostics, latency controls and peer selection helpers.
- `EditorTiles.cpp` - Tiles panel and the Scene view tile brush.
- `EditorTeamChat.cpp` - Team Chat panel (messages, live lines, input with mention completion).
- `EditorTeam.cpp` - Team panel and the agent profile dialog; `avatars/*.png` are the built-in profile pictures, embedded at build time.
- `EditorText.h/.cpp` - interface languages and the translation catalog (`Tr`, `TrId`).
- `EditorMath.h` - header-only math (Transform decomposition, editor camera, screen rays) with unit tests.
- `SokolImGui.cpp` - `sokol_imgui.h` implementation for the engine's graphics API.

Platform requirements (`engine/platform/Platform.h`): `Window::SetEventMode` / `TakeEvents` (full keyboard, text, all mouse buttons, wheel, focus, close, dropped files), `SetCursor`, `DpiScale`, `Maximize`, and `PlatformEnableHighDpi`. Win32 implements them; a macOS port needs them in its AppKit window plus a Metal `GpuDevice` (see [PLATFORMS.md](PLATFORMS.md)).

Adding a panel: a method on `NativeEditor::Impl` (declared in `EditorInternal.h`), called from `NativeEditor::Update`, docked in `DockLayout`, with a toggle in the View menu. Read data with `Call("...")` when the revision changes (`Refresh`), write with commands so undo and agents using the API stay in sync.
