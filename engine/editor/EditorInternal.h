#pragma once
// Shared state of the native editor (engine/editor/*.cpp). Not a public header.
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#define IMGUI_DEFINE_MATH_OPERATORS  // ImVec2 + ImVec2 (before the first imgui.h)
#include "imgui.h"
#include "imgui_stdlib.h"
#include "TextEditor.h"
#include "sokol_gfx.h"

#include "core/Json.h"
#include "editor/Editor.h"
#include "editor/EditorMath.h"

namespace oe {

struct Texture;

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
    ImU32 color = 0;  // border color; 0 = by kind (a team agent's edit uses the agent's color)
};

struct ScriptDiagnostic {
    int line = 0;          // 1-based
    bool error = false;    // error (does not compile / failed at run time) or warning
    bool runtime = false;  // reported by the running game (script.errors)
    std::string message;
};

struct ScriptTab {
    std::string path;
    std::unique_ptr<TextEditor> editor;  // syntax highlighting, line numbers, find/replace, markers
    std::string saved;                   // text on disk
    bool open = true;
    bool modified = false;
    size_t undoIndex = 0;                // editor undo index the checks ran for
    double editedAt = -1;                // pending script.check after typing stops
    std::vector<ScriptDiagnostic> diagnostics;  // script.check
    std::vector<ScriptDiagnostic> runtime;      // script.errors for this file
    std::string runtimeKey;
    bool markersDirty = true;
};

// One entry of the chat's completion list: what a typed `@` or `#` word becomes.
struct ChatCompletion {
    std::string insert;                    // replaces the word: "@id " or "path "
    std::string label, detail;             // shown in the list
    std::string avatar, id;                // agents: picture key and id
    std::string file;                      // files: project-relative path
    bool enabled = true;                   // an offline agent is listed but cannot be chosen
};

enum class GizmoOp { None, Translate, Rotate, Scale };

// What to do after the "save changes?" prompt.
struct PendingAction {
    enum class Kind { None, LoadScene, NewScene, Quit, EditPrefab, ClosePrefab } kind = Kind::None;
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
    bool showNetwork = true, requestNetworkFocus = false;
    bool showHistory = true, requestHistoryFocus = false;
    bool showThumbnails = true;
    struct Thumbnail {
        std::shared_ptr<const Texture> image;
        double generated = -100, lastUsed = 0;
        int64_t modified = 0;
        std::string error;
    };
    std::map<std::string, Thumbnail> thumbnails;
    int previewBudget = 1;
    std::shared_ptr<const Texture> AssetThumbnail(const std::string& path);
    int networkPlayers = 1, networkLatency = 0, gamePeer = 0;
    void NetworkPanel();
    Engine& GameEngine();
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
    bool scriptEditorFocused = false;  // keys belong to the code editor (no scene shortcuts)
    double lastScriptErrorsPoll = -1;
    Json scriptErrors = Json::MakeArray();
    void CheckScript(ScriptTab& tab);
    void UpdateScriptMarkers(ScriptTab& tab);
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

    // ----- Agent team (EditorTeam.cpp): Team panel and the agent profile dialog
    bool teamAvailable = false;     // the hosting tool registered the team.* commands
    bool showTeam = true, requestTeamFocus = false;
    Json team;                      // team.list
    Json teamState;                 // team.state, read every frame the panel shows
    Json teamBackends = Json::MakeArray();  // team.backends {async}
    Json teamPresets = Json::MakeArray();   // team.presets
    double teamListRevision = -1;   // team.state revision `team` was read for
    double teamBackendsPoll = -100;
    std::string selectedAgent;
    struct AgentForm {              // the profile dialog's working copy; nothing is stored before Save
        bool isNew = true, installed = false, customModel = false, showPathInput = false, askFull = false;
        std::string id, name, description, instructions, backend, model, access = "edit";
        std::string avatar, avatarPath, originalAvatar;  // "preset:<name>" or "file" (+ its project-relative path)
        std::string avatarSource;   // picture chosen for upload, applied by Save
        std::string pathInput, error;
    } agentForm;
    bool openAgentDialog = false, openRemoveAgent = false;
    std::string removeAgentId;
    struct AvatarImage {
        std::shared_ptr<const Texture> image;
        int64_t modified = 0;
        double lastUsed = 0;
    };
    std::map<std::string, AvatarImage> avatarTextures;  // "preset:<name>" / "file:<path>", at most 64
    std::shared_ptr<const Texture> AvatarTexture(const std::string& key);
    void DrawAvatar(ImDrawList* dl, ImVec2 pos, float size, const std::string& key, const std::string& id, const std::string& name);
    const Json* TeamBackend(const std::string& id) const;
    void RefreshTeam();
    void EditAgent(const std::string& id);  // empty = a new agent
    bool SetAgentFormBackend(const std::string& id);
    bool SaveAgentForm();
    void TeamPanel();
    void TeamModals();
    // Team Chat (EditorTeamChat.cpp)
    bool showTeamChat = true, requestTeamChatFocus = false;
    struct ChatEntry {
        Json message;                    // one team.messages entry
        std::vector<std::string> links;  // project files named in the text (open on click)
        std::vector<std::string> files;  // every existing project file the text names (tinted)
        bool rich = false;               // the text has references to tint (mentions, files)
        bool open = false;               // turn steps expanded
    };
    std::vector<ChatEntry> chat;
    bool chatLoaded = false, chatVisible = false, chatScrollToBottom = false, chatJustSent = false, chatFocusInput = false;
    double chatLast = 0, chatRevision = -1;  // newest message id read; team.state revision it was read for
    int chatUnread = 0;                      // arrived while the tab was hidden
    std::string chatInput, chatAutoMention;  // chatAutoMention: the "@id " a roster click put there (replaced by the next click)
    std::array<float, 4> chatStopRect{0, 0, 0, 0};  // first live line's Stop button (tests click it)
    std::vector<std::string> chatAttachments;       // files that go with the next message (dropped on the panel)
    std::array<float, 4> chatRect{0, 0, 0, 0};      // the panel's window: a file dropped inside it is attached
    std::vector<ImVec2> droppedAt;                  // where each entry of droppedFiles landed
    std::string chatViewerPath;                     // project-relative picture shown in the viewer; empty = closed
    bool chatViewerActual = false;                  // 100 % instead of fit
    std::vector<std::pair<std::string, bool>> MentionCandidates(const std::string& prefix) const;
    std::vector<std::string> FileCandidates(const std::string& prefix) const;
    int chatCompleteIndex = 0;            // chosen row of the completion list (Up / Down, the pointer)
    std::string chatCompleteKey;          // the word the list was built for; another word starts at the top
    bool chatCompleteScroll = false;      // bring the chosen row into view
    std::vector<ChatCompletion> Completions(char marker, const std::string& prefix) const;
    ImU32 ChatTokenColor(const std::string& word, const std::vector<std::string>* files, size_t& length) const;
    void DrawChatText(const std::string& text, const std::vector<std::string>& files);
    void PollChat();
    void SendChat();
    void RouteDroppedFiles();
    bool DrawImageFile(const std::string& absolutePath, ImVec2 box, float zoom);
    void ImportChatImage(const std::string& path);
    void ImageViewer();
    void TeamChatPanel();

    // ----- Tile painting (EditorTiles.cpp): Tiles panel + brush in the Scene view
    bool showTiles = true;
    bool tilePaint = false;        // Scene view clicks paint the selected Tilemap
    char tileBrush = '#';
    Json tileInfo;                 // tilemap.info of the selected tilemap
    EntityId tileInfoId = kNullEntity;
    uint64_t tileInfoRevision = ~0ull;
    int tileSerial = 0;            // brush stroke -> undo merge group
    enum class TileStroke { None, Paint, Erase, Rect } tileStroke = TileStroke::None;
    int strokeCol = 0, strokeRow = 0;  // last painted cell / rectangle corner
    bool tileHover = false;
    int hoverCol = 0, hoverRow = 0;
    bool TilePainting() const;     // paint mode with a Tilemap selected
    const Json& TileInfo();
    bool TileCellAt(float x, float y, int& col, int& row) const;  // Scene view pixel -> cell
    void PaintCells(const std::vector<std::pair<int, int>>& cells, char c);
    void TileSceneInput(ImDrawList* dl, bool clickedLeft, bool clickedRight);
    void TilesPanel();

    // ----- Game view
    int gameAspect = 0;  // 0 free, 1 16:9, 2 16:10, 3 4:3
    bool gameFocused = false;
    bool gameHovered = false;
    ImVec2 gameImagePos{0, 0}, gameImageSize{0, 0};
    std::set<std::string> gameKeysDown;
    std::set<std::string> gameAxesForwarded;  // only these axes are released when Game loses focus
    bool gameMouseDown[2] = {false, false};
    float lastGameMouse[2] = {-1, -1};
    // UI editing in the stopped Game view: select, drag to move, handles to resize.
    int uiDragHandle = -1;        // -1 none, 0 = move, 1..8 = handles clockwise from the top-left corner
    EntityId uiDragEntity = kNullEntity;
    std::string uiDragType;       // UI component being edited
    float uiDragStart[4] = {0, 0, 0, 0};  // x, y, width, height when the drag began (reference pixels)
    ImVec2 uiDragMouse{0, 0};
    int uiDragSerial = 0;
    void GameUIEdit(ImDrawList* dl, ImVec2 pos, int w, int h);
    // Component that carries the entity's `visible` flag (UI, Sprite, MeshRenderer, Tilemap), or empty.
    std::string VisibilityComponent(EntityId id, bool* visible) const;
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
    std::map<EntityId, ImU32> remoteEditColors;  // ... and the color of the team agent that made it, when known
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
    void CopySelection();
    void PasteSelection();
    void BrowseFile(NativeEditor::FilePurpose purpose);
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
    // Script component params as typed rows (from script.params), inside the inspector table.
    void ScriptParamsRows(EntityId id, const Json& script);
    const Json& ScriptParamSchema(const std::string& path);
    std::map<std::string, std::pair<double, Json>> paramSchemas;  // script path -> (time fetched, script.params)
    std::set<EntityId> fieldParams;                              // entities showing params as typed fields (default: JSON text)
    std::string newParamName;
    int newParamType = 0;
    void AssetsPanel();
    void ConsolePanel();
    void HistoryPanel();
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
// A team agent's color (from its id): its name in the chat, its edits in the Hierarchy and notices.
ImU32 AgentColor(const std::string& id);
Json Vec3Json(const Vec3& v);
Vec3 JsonVec3(const Json& j, Vec3 def = Vec3(0, 0, 0));
Json ObjectOf(std::initializer_list<std::pair<const char*, Json>> members);
void HelpTooltip(const std::string& text);
// Kind of asset a string field holds ("model", "texture", ...), "" for plain text.
std::string AssetKindForField(const std::string& type, const std::string& field);

}  // namespace oe
