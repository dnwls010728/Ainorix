#pragma once
#include <memory>
#include <string>
#include <vector>

#include "platform/Platform.h"
#include "render/Renderer.h"
#include "scene/Systems.h"

namespace oe {

class Engine;

// Native editor: Dear ImGui (docking) panels drawn with sokol_gfx in the
// engine process. The Scene and Game views are GpuRenderer textures, so
// there is no streaming or encoding between the engine and what people see.
//
// Every edit goes through Engine::Call (the same command API the CLI, HTTP
// and MCP use), so undo, revisions and an attached agent (`oe mcp --connect`)
// see exactly what the human does. Read-only views (world matrices for the
// gizmo, entity markers) read the scene directly.
//
// Requires the GPU renderer (Engine::EnableGpu). Works with a window
// (RunNativeEditor) or headless: Update() + DrawToImage() render the whole
// editor offscreen for screenshots and tests.
class NativeEditor {
public:
    struct Options {
        std::string layoutFile;  // ImGui layout + editor preferences; empty = not persisted
        std::string serverInfo;  // shown in the status bar, e.g. "API http://127.0.0.1:7777"
    };

    NativeEditor(Engine& engine, Window* window, Options options);
    ~NativeEditor();
    NativeEditor(const NativeEditor&) = delete;
    NativeEditor& operator=(const NativeEditor&) = delete;

    // Sets up Dear ImGui on the engine's GPU device. Fails without one.
    bool Init(std::string* error);

    // One editor frame: applies window events, advances the simulation by
    // `dt` of real time, refreshes data from the command API and lays out
    // the panels (Scene/Game views are rendered here). Sizes are in pixels.
    void Update(const std::vector<WindowEvent>& events, int width, int height, float dpiScale, double dt);
    // Draws the frame laid out by Update into the window and presents it.
    bool DrawToWindow();
    // Draws the frame laid out by Update offscreen and reads it back.
    bool DrawToImage(RenderTarget& out);

    // True once the user closed the window (after the save prompt).
    bool QuitRequested() const;
    // Passed to Window::PumpEvents: carries the game's mouse lock to the
    // platform and brings back relative mouse motion (Game view mouse look).
    InputState& WindowInput();

    // Automation (tests, `oe editor --screenshot`).
    void Select(EntityId id);
    EntityId Selected() const;
    // Keyboard focus for the Game view (keys and mouse go to the game while playing).
    void FocusGameView(bool focus);
    bool GameViewFocused() const;
    // Last error or notice shown to the user (toast), for tests.
    std::string LastNotice() const;

    struct Impl;  // engine/editor/EditorInternal.h

private:
    std::unique_ptr<Impl> impl_;
};

// Runs the editor in `window` until the user closes it. The caller keeps
// serving the HTTP/MCP API; posted jobs run inside this loop.
int RunNativeEditor(Engine& engine, Window& window, const NativeEditor::Options& options);

}  // namespace oe
