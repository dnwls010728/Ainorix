# Editor

OwnEngine has two editors on the same command API:

| | Native editor (default) | Web editor |
|---|---|---|
| Start | `oe editor <project>` | `oe editor <project> --web` (also served by the native editor's port) |
| Where | Windows today; any platform with a window + GPU backend | any browser, also remote / headless machines |
| Viewport | GpuRenderer textures in the same process (no streaming) | GPU/software frames streamed over WebSocket (JPEG) |
| UI | Dear ImGui (docking) + ImGuizmo, drawn with sokol_gfx | HTML/CSS/JS in `editor/` |

![Native editor](images/native-editor.png)

Both only use public commands (`Engine::Call`), so an agent attached with `oe mcp --connect 7777` sees and can do exactly what the person in the editor does. The native editor keeps the HTTP server running (`--port`, default 7777): the web editor, `oe mcp --connect` and `POST /api/call` work while it is open, and every edit that arrives through the API shows up as a notice (`API: component.set Player MeshRenderer`) plus a fading dot on the entity in the Hierarchy.

When no native window or GPU is available (Linux `null` platform, `--renderer software`), `oe editor` falls back to the web editor.

## Panels

- **Hierarchy** - tree of entities (search by name, or by component with 3+ letters). Click, Ctrl+click (toggle), Shift+click (range). Drag an entity onto another to parent it, onto empty space to move it to the root; drop a prefab from Assets to instantiate it. Right-click: rename, duplicate, delete, create child, save as prefab. Double-click frames the entity.
- **Inspector** - generated from `component.types`: sliders for ranged numbers, drag fields for vectors (one drag = one undo step via `component.set {merge}`), color pickers, enum combos, entity pickers (also accept entities dragged from the Hierarchy), multi-line JSON fields. Asset fields (mesh, texture, material, script, prefab, clip, font) have a `...` picker and accept assets dragged from the Assets panel. Right-click a component header: remove, reset to defaults, copy as JSON.
- **Scene** - free camera, grid, selection outline, collider wireframes, markers for cameras / lights / sounds, and the transform gizmo. Click selects (software-renderer picking, identical to `render.pick`); drop assets to place them (models, prefabs, sprites from images, sounds; textures / materials / scripts onto the entity under the cursor).
- **Game** - the scene's active camera with in-game UI, letterboxed to Free / 16:9 / 16:10 / 4:3. While playing, click it to give the game the keyboard and mouse (green frame); click elsewhere to take them back. Games that lock the mouse (`input.lockMouse`) get raw relative motion; Esc releases it.
- **Assets** - project files by kind. Double-click: open a scene, edit a script, instantiate a prefab. Drag into the Scene, Hierarchy or Inspector. Files dropped on the window from Explorer are imported (same rules as `oe import`).
- **Console** - the engine log with level/text filters, and a command line: `command.name {"json": "args"}` (Tab completes command names, Up/Down browse history).
- **Scripts** - Lua editor tabs; Ctrl+S (or Save) writes through `script.write`, which hot-reloads the script, also while the game runs. Errors for the file are listed below it.

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
| | F, double-click | frame selection |
| | F2, Del, Ctrl+D, Esc | rename, delete, duplicate, deselect |
| Anywhere | Ctrl+S, Ctrl+Z, Ctrl+Y (Ctrl+Shift+Z), Ctrl+N | save, undo, redo, new scene |
| Anywhere | Ctrl+P | play / stop (stop restores the edit-time scene) |

Leaving with unsaved changes (closing the window, opening or creating a scene) asks to save first. Saving is disabled while a play session runs, because Stop restores the scene.

## Languages

The interface is available in English, Korean (한국어) and Japanese (日本語). The editor starts in the OS language (`PlatformUserLanguage`), then in the choice made under View > Language (saved in `.oe/editor.ini`); `oe editor --lang en|ko|ja` overrides both. API names stay English in every language: commands, component types and fields, and the entity names the Create menu gives (scripts refer to them).

For text, the editor merges system fonts into its UI font: Korean (Malgun Gothic on Windows, Apple SD Gothic Neo on macOS, Noto CJK / Nanum on Linux) and Japanese (Yu Gothic / Meiryo / MS Gothic on Windows, Hiragino on macOS, Noto CJK / IPA Gothic on Linux), the interface language's first. IME input (Korean, Japanese) arrives through `WM_CHAR`, with the IME window placed by Dear ImGui.

Translations live in `engine/editor/EditorText.cpp`: UI strings are written in English in the code as `Tr("...")` (format strings keep their `%` arguments) and windows / popups use `TrId("...")` so their docking IDs do not change with the language. Add a catalog row for every new string; the `EditorTranslations` test scans `engine/editor/*.cpp` and fails for a `Tr` string without Korean and Japanese text or with different `%` arguments.

## For agents: looking at the editor

The native editor can be rendered headless (needs a GPU backend: D3D11/WARP on Windows, EGL on Linux with `libegl-dev libgles-dev`):

```bash
oe editor samples/Hello --screenshot build/editor.png                 # default layout, 1600x900
oe editor samples/Hello --screenshot build/editor.png --select Player # inspector + gizmo on Player
oe editor samples/Hello --screenshot build/editor.png --play --frames 60  # Game view after 1 s of play
oe editor samples/Hello --screenshot build/editor.png --lang ja       # Japanese interface (default for screenshots: en)
```

Tests drive it the same way: `NativeEditor::Update(events, w, h, dpi, dt)` takes window events (keys, mouse, text), `DrawToImage` reads the frame back (see `NativeEditorHeadless` in `tests/tests.cpp`).

## Code

`engine/editor/` (library `oe_editor`, linked by `oe` and `oe_tests`, never by games):

- `Editor.h` - `NativeEditor` (Init / Update / DrawToWindow / DrawToImage) and `RunNativeEditor` (main loop: posted API jobs, window events, frame, present).
- `Editor.cpp` - window events -> Dear ImGui, command helpers, menus, toolbar, status bar, default dock layout, save prompts, preferences (`[OwnEngine][Editor]` in the .ini).
- `EditorPanels.cpp` - Hierarchy, Inspector, Assets, Console, Scripts.
- `EditorViewports.cpp` - Scene view (camera, picking, gizmo, markers, drops) and Game view (input forwarding, mouse lock).
- `EditorText.h/.cpp` - interface languages and the translation catalog (`Tr`, `TrId`).
- `EditorMath.h` - header-only math (Transform decomposition, editor camera, screen rays) with unit tests.
- `SokolImGui.cpp` - `sokol_imgui.h` implementation for the engine's graphics API.

Platform requirements (`engine/platform/Platform.h`): `Window::SetEventMode` / `TakeEvents` (full keyboard, text, all mouse buttons, wheel, focus, close, dropped files), `SetCursor`, `DpiScale`, `Maximize`, and `PlatformEnableHighDpi`. Win32 implements them; a macOS port needs them in its AppKit window plus a Metal `GpuDevice` (see [PLATFORMS.md](PLATFORMS.md)).

Adding a panel: a method on `NativeEditor::Impl` (declared in `EditorInternal.h`), called from `NativeEditor::Update`, docked in `DockLayout`, with a toggle in the View menu. Read data with `Call("...")` when the revision changes (`Refresh`), write with commands so undo, the API and the web editor stay in sync.
