#pragma once
// Shared state of the native editor (engine/editor/*.cpp). Not a public header.
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#define IMGUI_DEFINE_MATH_OPERATORS  // ImVec2 + ImVec2 (before the first imgui.h)
#include "imgui.h"
#include "imgui_stdlib.h"
#include "sokol_gfx.h"

#include "core/Json.h"
#include "editor/Editor.h"
#include "editor/EditorMath.h"

namespace oe {

struct EntityRow {
    EntityId id = kNullEntity;
    std::string name;
    EntityId parent = kNullEntity;
    std::vector<std::string> components;
    bool Has(const char* type) const {
        for (const std::string& c : components) {
            if (c == type) return true;
        }
        return false;
    }
};

struct LogLine {
    uint64_t seq = 0;
    std::string level;
    std::string category;
    std::string message;
};

struct Toast {
    std::string text;
    bool error = false;
    double until = 0;
};

struct ScriptTab {
    std::string path;
    std::string text;
    std::string saved;  // text on disk, to show the modified marker
    bool open = true;
};

enum class GizmoOp { None, Translate, Rotate, Scale };

// What to do after the "save changes?" prompt.
struct PendingAction {
    enum class Kind { None, LoadScene, NewScene, Quit } kind = Kind::None;
    std::string path;
};

// Drag and drop payload types.
constexpr const char* kAssetPayload = "OE_ASSET";    // project-relative path (char data)
constexpr const char* kEntityPayload = "OE_ENTITY";  // EntityId

struct NativeEditor::Impl {
    Impl(Engine& e, Window* w, NativeEditor::Options o) : engine(e), window(w), options(std::move(o)) {}

    Engine& engine;
    Window* window;
    NativeEditor::Options options;
    bool ready = false;
    double time = 0;  // editor clock (seconds since start)
    int width = 1280, height = 720;
    float dpiScale = 1.0f;
    float uiScale = 1.0f;          // user preference (View > Interface size)
    float appliedScale = 0.0f;     // dpiScale * uiScale the style was built for
    bool quit = false;
    InputState windowInput;
    std::string lastNotice;
    ImFont* monoFont = nullptr;  // code editor / console
    bool forceLanguage = false;  // Options::language wins over the saved choice

    // Offscreen target for DrawToImage.
    sg_image offImage{};
    sg_view offAtt{};
    int offW = 0, offH = 0;

    // ----- Data from the command API (refreshed when the revision changes)
    uint64_t seenRevision = 0;
    double lastRefresh = -1;
    std::vector<EntityRow> rows;
    std::map<EntityId, size_t> rowIndex;
    std::string sceneName, scenePath;
    Json sim;  // sim.state
    std::vector<EntityId> selection;  // first = primary (inspector, gizmo)
    Json selected;                    // entity.get of the primary
    EntityId selectedId = kNullEntity;
    uint64_t selectedRevision = 0;
    Json componentTypes;              // component.types
    std::map<std::string, const Json*> typeByName;
    std::vector<std::string> commandNames;
    Json assets = Json::MakeArray();  // asset.list
    double assetsTime = -100;
    std::vector<std::string> sceneFiles;

    // ----- Log / console
    std::deque<LogLine> log;
    uint64_t logSeq = 0;
    double lastLogPoll = -1;
    bool logShow[4] = {true, true, true, true};  // debug, info, warn, error
    std::string logFilter;
    bool logAutoScroll = true;
    std::string consoleInput;
    std::vector<std::string> consoleHistory;
    int historyPos = -1;
    int warnCount = 0, errorCount = 0;

    // ----- Panels
    bool showHierarchy = true, showInspector = true, showScene = true, showGame = true;
    bool showConsole = true, showAssets = true, showScripts = true, showMetrics = false;
    bool layoutBuilt = false;
    bool resetLayout = false;
    int focusSceneTab = 0;  // frames until the Scene tab is brought to front
    std::string hierarchyFilter;
    EntityId renaming = kNullEntity;
    std::string renameBuffer;
    EntityId anchorRow = kNullEntity;  // shift-click range start
    EntityId scrollToRow = kNullEntity;
    std::string assetFilter;
    std::string selectedAsset;
    std::vector<ScriptTab> scripts;
    int focusScript = -1;
    std::string addComponentFilter;
    std::map<std::string, int> dragSerial;  // inspector drag -> undo merge group
    int gizmoSerial = 0;

    // ----- Scene view
    EditorCamera cam;
    bool showGrid = true, showColliders = false, showIcons = true;
    GizmoOp gizmoOp = GizmoOp::Translate;
    bool gizmoLocal = false;
    bool snap = false;
    float snapMove = 0.5f, snapAngle = 15.0f, snapScale = 0.1f;
    float flySpeed = 6.0f;
    bool gizmoWasUsing = false;
    bool gizmoUsing = false;         // dragged or hovered this frame (clicks go to the gizmo)
    EntityId markerHover = kNullEntity;
    bool scenePressPending = false;  // left press in the view, waiting for the release
    EntityId scenePressMarker = kNullEntity;
    bool sceneHovered = false, sceneFocused = false;
    bool sceneLooking = false;  // right mouse fly mode
    bool sceneButtonActive = false;  // a mouse button went down on the view and is held
    Mat4 sceneViewMat, sceneProjMat;
    ImVec2 sceneImagePos{0, 0}, sceneImageSize{0, 0};

    // ----- Game view
    int gameAspect = 0;  // 0 free, 1 16:9, 2 16:10, 3 4:3
    bool gameFocused = false;
    bool gameHovered = false;
    ImVec2 gameImagePos{0, 0}, gameImageSize{0, 0};
    std::set<std::string> gameKeysDown;
    bool gameMouseDown[2] = {false, false};
    float lastGameMouse[2] = {-1, -1};
    bool gameWantsLock = false;
    bool requestGameFocus = false;

    // ----- Modals / notices
    PendingAction pending;
    bool openSavePrompt = false;
    bool openSaveAs = false;
    std::string saveAsPath;
    bool openPrefabPrompt = false;
    std::string prefabPath;
    std::vector<Toast> toasts;
    std::vector<std::string> droppedFiles;
    std::map<EntityId, double> remoteEdits;  // entity -> editor time of the last API (agent) edit
    void OnRemoteCall(const std::string& name, const Json& args, const Json& result);
    double fpsTimer = 0;
    int fpsFrames = 0;
    int fps = 0;

    // ----- Command helpers (Editor.cpp)
    // Runs a command; on failure shows the error (and hint) as a toast.
    Json Call(const std::string& name, const Json& args, bool quiet = false);
    bool Ok(const Json& r) const { return r["ok"].asBool(); }
    void Notify(const std::string& text, bool error = false);
    const EntityRow* Row(EntityId id) const;
    std::string UniqueName(const std::string& base) const;
    EntityId Primary() const { return selection.empty() ? kNullEntity : selection.front(); }
    bool IsSelected(EntityId id) const;
    void SelectOnly(EntityId id);
    void ToggleSelect(EntityId id);
    void Refresh(bool force);
    void RefreshAssets(bool force);
    void PollLog();
    bool Playing() const;
    bool InPlaySession() const;
    bool Dirty() const;

    // Actions shared by menus, toolbar and shortcuts.
    void Save();
    void TogglePlay();
    void Undo();
    void Redo();
    void DeleteSelection();
    void DuplicateSelection();
    void FrameSelection();
    void CreatePreset(const char* key);
    void RequestAction(PendingAction action);  // asks to save first when dirty
    void RunAction(const PendingAction& action);
    void OpenScript(const std::string& path);
    void ImportDroppedFiles();
    void RunConsoleCommand(const std::string& line);

    // UI (Editor.cpp)
    void ApplyStyle();
    void ApplyEvents(const std::vector<WindowEvent>& events);
    void Shortcuts();
    void MainMenu();
    void Toolbar(float barHeight);
    void StatusBar(float barHeight);
    void DockLayout(ImGuiID dockspace);
    void Modals();
    void Toasts();
    void UpdateCursor();

    // Panels (EditorPanels.cpp)
    void HierarchyPanel();
    void HierarchyNode(const EntityRow& row, const std::map<EntityId, std::vector<EntityId>>& children, const std::set<EntityId>& visible);
    void EntityContextMenu(const EntityRow* row);
    void InspectorPanel();
    bool FieldEditor(EntityId id, const std::string& type, const std::string& field, const Json& schema, const Json& value);
    void AddComponentPopup(EntityId id);
    void AssetsPanel();
    void ConsolePanel();
    void ScriptsPanel();
    bool AssetPicker(const char* popupId, const std::string& kind, const std::string& current, std::string& out);
    void CreateMenuItems();

    // Views (EditorViewports.cpp)
    void ScenePanel();
    void GamePanel();
    void SceneCameraInput(ImGuiIO& io);
    void SceneOverlays(ImDrawList* dl);
    void SceneGizmo();
    void ScenePick(float x, float y, bool additive);
    void SceneDrop(const std::string& path, float x, float y);
    void GameInput();
    void ReleaseGameInput();
    ImTextureID ViewTexture(sg_view view) const;
    bool FlipViews() const;
};

// Small helpers shared by the editor files.
Json Vec3Json(const Vec3& v);
Vec3 JsonVec3(const Json& j, Vec3 def = Vec3(0, 0, 0));
Json ObjectOf(std::initializer_list<std::pair<const char*, Json>> members);
void HelpTooltip(const std::string& text);
// Kind of asset a string field holds ("model", "texture", ...), "" for plain text.
std::string AssetKindForField(const std::string& type, const std::string& field);

}  // namespace oe
