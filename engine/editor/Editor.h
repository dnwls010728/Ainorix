#pragma once
#include <array>
#include <functional>
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
        // Interface language: "en", "ko" or "ja". Empty = the choice saved in
        // layoutFile (View > Language), else the OS language.
        std::string language;
        // Called once per frame after the simulation tick (the tool updates its agent team here).
        std::function<void()> onFrame;
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
    // The Scene view's 2D mode (XY grid, planar gizmo), as its "2D" toggle.
    void SetSceneView2D(bool on);
    // Opens a project script in the Scripts panel (code editor).
    void OpenScript(const std::string& path);
    // Problems the Scripts panel shows for an open script: "error 12: ..." / "warning 3: ...".
    std::vector<std::string> ScriptProblems(const std::string& path) const;
    // Keyboard focus for the Game view (keys and mouse go to the game while playing).
    void FocusGameView(bool focus);
    bool GameViewFocused() const;
    // Select preview player/view and release held input on the previous peer.
    bool SetGamePeer(size_t peer);
    // Configure the next Play action (1..8 total players, bounded by project maxPlayers).
    bool SetNetworkPlayers(int players);
    // Bring Network diagnostics to the front for automated screenshots.
    void FocusNetworkPanel();
    // Agent team (shown when the tool registered the team.* commands). Bring the Team panel forward.
    void FocusTeamPanel();
    // Open the agent profile dialog for an agent id, or for a new agent (empty id).
    void EditAgent(const std::string& id);
    // Choose a backend in the open profile dialog as its combo does; false for a CLI that is
    // not installed (it cannot be chosen).
    bool SetAgentBackend(const std::string& backend);
    // Ask to remove an agent: the confirmation prompt opens.
    void RemoveAgent(const std::string& id);
    // Bring the Team Chat panel forward and give its input the keyboard.
    void FocusTeamChat();
    // What is typed in the chat input (tests check mention completion).
    std::string TeamChatInput() const;
    // The Stop button of the first running agent's line in the chat, in window pixels: x, y,
    // width, height (zero size when no turn runs).
    std::array<float, 4> TeamChatStopRect() const;
    // The Team Chat window in window pixels (a file dropped inside it is attached, not imported).
    std::array<float, 4> TeamChatRect() const;
    // Files waiting to be sent with the next chat message.
    std::vector<std::string> TeamChatAttachments() const;
    // Open the picture viewer on a project-relative image; which one it shows (empty = closed).
    void ViewChatImage(const std::string& path);
    std::string ViewedChatImage() const;
    // Bring undo/redo command history forward for inspection/screenshots.
    void FocusHistoryPanel();
    // Open a source prefab through the same save/discard workflow as the menu.
    void EditPrefab(const std::string& path);
    void ClosePrefab();
    // Launch a native chooser (unavailable headless platforms keep path pickers).
    enum class FilePurpose { OpenScene, SaveScene, ImportAsset };
    void BrowseFile(FilePurpose purpose);
    // Tile painting: the Scene view brush on/off and its tile character (Tiles
    // panel); turning it on switches to the 2D view framed on the selection.
    void SetTileBrush(bool paint, char brush);
    // Scene view image in window pixels: x, y, width, height.
    std::array<float, 4> SceneViewRect() const;
    // Game view image in window pixels: x, y, width, height (UI is edited there while stopped).
    std::array<float, 4> GameViewRect() const;
    // Last error or notice shown to the user (toast), for tests.
    std::string LastNotice() const;

    struct Impl;  // engine/editor/EditorInternal.h

private:
    std::unique_ptr<Impl> impl_;
};

// A built-in agent profile picture (PNG bytes embedded from engine/editor/avatars/); false when
// there is no picture of that name.
bool EditorAvatarPreset(const std::string& name, const unsigned char** data, size_t* size);

// Runs the editor in `window` until the user closes it. The caller keeps
// serving the HTTP/MCP API; posted jobs run inside this loop.
int RunNativeEditor(Engine& engine, Window& window, const NativeEditor::Options& options);

}  // namespace oe
