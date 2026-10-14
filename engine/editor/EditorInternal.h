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
#include "render/UI.h"

namespace oe {

struct Texture;
struct Sprite;

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
    EntityId entity = kNullEntity;         // scene objects (ChatMentions::Entities)
    bool enabled = true;                   // an offline agent is listed but cannot be chosen
};

// What `@` completes to in a chat input: team agents (team chat, team bubbles) or the scene's
// objects (direct conversations with one CLI, which has no teammates to mention).
enum class ChatMentions { Agents, Entities };

// The completion list of one chat input: the word it was built for, the chosen row and whether
// to scroll it into view.
struct ChatCompleteState {
    std::string key;
    int index = 0;
    bool scroll = false;
    bool listHovered = false;  // the pointer is on the list: it stays while the input loses focus to a click
    bool caretToEnd = false;   // after a completion: caret to the end, nothing selected
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
    bool windowPlacementRestored = false;
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
    bool requestConsoleFocus = false;
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
    bool showAnimation = false, focusAnimation = false, animationEditorFocused = false;
    double animationAccumulator = 0;
    int animationEvent = -1, animationDragIndex = -1;
    Json animationDragEvents;
    std::string animationEventName;
    std::map<std::string, std::string> animationNames;
    std::map<std::string, std::array<float, 4>> animationRects;
    bool animationFrames = false;
    bool showAnimationController = false, focusAnimationController = false, animationControllerEditorFocused = false;
    bool animationControllerClosePrompt = false;
    Json animationControllerDocument, animationControllerSaved, animationControllerLive;
    std::vector<Json> animationControllerHistory;
    size_t animationControllerHistoryIndex = 0;
    std::string animationControllerMerge, animationControllerProject, animationControllerLink;
    ImVec2 animationControllerNewAt{40,280};
    Json AnimationControllerCall(const std::string& op, const Json& args);
    bool animationControllerFit = true;
    std::string animationControllerPathInput = "animations/new.controller.json";
    std::string animationController, animationControllerState, animationControllerParameterName = "grounded", animationControllerParameterType = "boolean";
    int animationControllerTransition = -1;
    ImVec2 animationControllerPan{20, 20};
    float animationControllerZoom = 1;
    void AnimationControllerPanel();
    int animationFramePage = 0, animationSequenceFrame = 0;
    std::string animationNewClip = "new_clip";
    void AnimationSpriteFrames(const Json& state);
    void AnimationFrameStrip(const Json& state, ImVec2 origin, ImVec2 size);
    void AnimationPanel();
    void AnimationTick(double dt);
    void AnimationRect(const std::string& name);
    void AnimationWriteEvents(const Json& state, const Json& events, const std::string& merge = {});
    // Property tracks (EditorAnimationTracks.cpp): one lane per keyframed field under the ruler. `x0` is the
    // screen x of time 0, `scale` pixels per second, `gutter` the width of the lane label column.
    int animationTrack = -1, animationKey = -1;  // selected track and key
    bool animationKeyDragging = false;
    Json animationKeyDragTracks;                 // the tracks as they were when the key drag began
    std::string animationPropertyFilter;
    void AnimationTracks(const Json& state, float x0, float scale, float gutter);
    void AnimationAddPropertyPopup(const Json& state, const Json& tracks);
    void AnimationKeyEditor(const Json& state, Json tracks);
    bool AnimationWriteTracks(const Json& state, const Json& tracks, const std::string& merge = {});
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
    std::string assetKindFilter;  // empty = all asset kinds
    bool assetGrid = true;
    std::string selectedAsset;
    std::vector<ScriptTab> scripts;
    std::string activeScriptPath;  // the tab the Scripts panel showed last frame (editor.state)
    std::vector<std::string> closingScripts;  // Paths survive tab moves while the unsaved-changes prompt is open.
    int focusScript = -1;
    bool scriptEditorFocused = false;  // keys belong to the code editor (no scene shortcuts)
    double lastScriptErrorsPoll = -1;
    Json scriptErrors = Json::MakeArray();
    void CheckScript(ScriptTab& tab);
    void UpdateScriptMarkers(ScriptTab& tab);
    // One graph workspace; nodes use document indices until topological export.
    bool showShaders = false, focusShaders = false, shaderEditorFocused = false, shaderFit = false;
    Json shaderDocument, shaderSaved, shaderMaterial = Json::MakeObject(), shaderMaterialSaved;
    std::string shaderPath, shaderSavePath, shaderMaterialPath, shaderExistingMaterial, shaderError;
    std::string shaderUniformName = "parameter";
    std::vector<ImVec2> shaderPositions;
    ImVec2 shaderPan{30, 30}, shaderOutputPos{260, 30};
    float shaderZoom = 1.0f;
    int shaderSelected = -1, shaderLink = -1;
    std::map<std::string, std::array<float, 4>> shaderRects;  // automation: control and node bounds by name
    std::string shaderPaletteFilter;
    ImVec2 shaderPalettePos{0, 0};   // canvas position of the node the palette adds
    bool shaderPaletteOpen = false, shaderPaletteAtMouse = false, shaderPressedEmpty = false;
    bool shaderFitReadable = false;  // with shaderFit: not below a readable zoom (opening, arranging)
    void ShaderRect(const std::string& name);
    void ShaderPalette();
    std::vector<Json> shaderHistory;
    size_t shaderHistoryIndex = 0;
    bool shaderPendingOpen = false, shaderOpenPrompt = false, shaderPendingQuit = false;
    std::string shaderPendingPath;
    std::shared_ptr<const Texture> shaderPreview;
    std::string shaderPreviewKey;
    void OpenShader(const std::string& path, bool discard = false);
    void ShadersPanel();
    void ShaderCanvas();
    void ShaderProperties();
    void ShaderRecord();
    void ShaderArrange();
    bool SaveShader();
    bool SaveShaderMaterial();
    // Effects document is owned by Engine commands, so remote agents share its history.
    bool effectsGraph = false, effectsGraphFit = false, effectsLayoutDirty = false;
    std::string effectsLayoutPath, effectsGraphSelected, effectsGraphLink, effectsGraphHint, effectsGraphErrorNode;
    std::string effectsGraphField = "rate";
    std::map<std::string, ImVec2> effectsGraphPositions;
    std::map<std::string, int> effectsGraphModules;
    std::map<std::string, int> effectsGraphFlags;  // "<node>|ctx:<n>" collapsed, "<node>|all:<module>" every field shown
    std::vector<std::string> effectsGraphMaterials, effectsGraphAnchors;
    ImVec2 effectsGraphPan{24, 24};
    float effectsGraphZoom = 0.8f;
    void EffectsGraph();
    bool effectsValues = false, effectsValueFit = false;
    Json effectsValueGraph;
    Json effectsValueParamTypes;
    std::string effectsValueKey, effectsValueSource, effectsValueError;
    int effectsValueSelected = -1, effectsValueLink = -1;
    ImVec2 effectsValuePan{24,24};
    float effectsValueZoom = 0.8f;
    void EffectsValues();
    // Parts of the value nodes shared with the structure graph, which draws them beside the emitter they feed.
    bool effectsValueInGraph = false;
    std::string EffectsValueSync();
    void EffectsValuePalette(const std::string& prefix, ImVec2 spawn);
    void EffectsValueCanvas(class EditorNodeCanvas& canvas, float zoom, const std::string& prefix, ImVec2 origin, const ImVec2* sink);
    void EffectsValueProperties(const std::string& prefix);
    bool EffectsValueApply(const Json& candidate, const std::string& merge = {});
    void EffectsBlockFields();
    // The value widget of one field of a block (index among emitters then trails), one line high, applied through
    // vfx.set. Curves and expressions are a button that shows them in the inspector.
    void EffectsFieldValue(int block, const Json& field, const std::string& rect);
    void EffectsParameterValue(const std::string& name, float fieldWidth = 0);
    void EffectsGraphLayout(bool save);
    bool showEffects = false, focusEffects = false, effectsEditorFocused = false;
    Json effectsState, effectsDocument, effectsSchema, effectsPresets;
    std::string effectsPath, effectsNewPath = "effects/new.vfx.json", effectsPreset = "fire", effectsError;
    int effectsBlock = 0, effectsDragSerial = 0;
    int effectsModule = 0;
    std::map<std::string, int> effectsCurveKeys;
    std::map<std::string,std::pair<std::string,std::string>> effectsExpressionText; // canonical/pending source per channel
    float effectsCurveRange = 1;
    bool effectsPrompt = false, effectsPendingNew = false, effectsPendingClose = false;
    std::string effectsPendingPath;
    PendingAction effectsPendingAction;
    std::unique_ptr<Scene> effectsScene;
    std::shared_ptr<const Texture> effectsImage;
    EntityId effectsRoot = 0;
    int effectsFrame = 0, effectsCycleFrames = 180;
    bool effectsPlaying = true, effectsLoop = true, effectsMotion = false;
    double effectsAccumulator = 0, effectsLastTime = 0;
    float effectsZoom = 6;
    Vec3 effectsTarget{0, 1, 0};
    std::map<std::string, std::array<float, 4>> effectsRects;  // automation: actual interactive widget bounds
    std::map<std::string, std::array<float, 4>> inspectorRects;  // automation: "Type.field" value widget bounds
    void OpenEffect(const std::string& path, bool create = false);
    void EffectsPanel();
    void EffectsModals();
    void EffectsInspector();
    void EffectsPreview(bool compact = false);
    void ResetEffectsPreview();
    bool EffectsSave();
    void EffectsReplace(const Json& document, const std::string& merge = {});
    std::string EffectsMerge(const std::string& control);
    void EffectsRect(const std::string& name);
    int effectsBackdrop = 1;     // preview background: 0 dark, 1 mid, 2 light (dark smoke needs a light one)
    bool effectsGrid = true;     // ground grid in the preview
    std::string addComponentFilter;
    std::map<std::string, int> dragSerial;  // inspector drag -> undo merge group
    float capturedWheel = 0;                // wheel notches turned while the game held the mouse
    int gizmoSerial = 0;

    // ----- Scene view
    EditorCamera cam;
    bool autoFramePending = true;  // frame the whole scene the first time it is shown, unless a saved camera loads
    bool showGrid = true, showColliders = false, showIcons = true;
    bool showNavMesh = false;  // the navigation mesh over the Scene view
    bool showEditorLight = false;  // editor fill light so a script-lit (night) scene is visible while editing
    bool showSceneUI = true;  // the Scene view draws the game's UI over the world and edits it there (UIEdit)
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
    Json teamUsage;                 // team.usage for today in UTC
    double teamUsagePoll = -100;
    Json teamState;                 // team.state, read every frame the panel shows
    Json teamBackends = Json::MakeArray();  // team.backends {async}
    Json teamPresets = Json::MakeArray();   // team.presets
    double teamListRevision = -1;   // team.state revision `team` was read for
    double teamBackendsPoll = -100;
    std::string selectedAgent;
    struct AgentForm {              // the profile dialog's working copy; nothing is stored before Save
        bool isNew = true, installed = false, customModel = false, showPathInput = false, askFull = false;
        std::string id, name, description, instructions, backend, model, access = "edit";
        double maxBudgetUsd = 0;
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
    // ----- Ask bubbles (EditorAsk.cpp): right-click an object in the Scene view and ask the team,
    // or the agent directly, from a bubble that stays next to the object. The agent is the one CLI
    // of Settings > Agents (team.settings askBackend, askModel, askInstructions): nothing is chosen
    // where the question is asked.
    struct AskBubble {
        int serial = 0;              // ImGui id
        EntityId entity = kNullEntity;  // the object asked about; for a spot, the surface it is on (or none)
        bool spot = false;           // about a place in the scene (point) rather than the object
        bool ui = false;             // the spot is on the game's UI: point.x, point.y are canvas reference pixels
        Vec3 point, normal;          // the spot: world position and surface normal
        std::string backend;         // agent CLI id; empty = the team
        std::string thread;          // team.ask conversation (agent bubbles)
        std::string input;
        Json state;                  // team.thread (agent bubbles)
        std::vector<uint64_t> sent;  // team chat messages this bubble sent (team bubbles)
        double polled = -100;
        ChatCompleteState complete;  // @ / # completion of its input
        bool focusInput = true;
        size_t shown = 0;            // messages drawn last frame: more scrolls to the newest
        ImVec2 size{0, 0};           // last frame's size, to keep it inside the view
        ImVec2 pos{0, 0};            // last frame's top-left corner (files dropped on it are attached)
        std::vector<std::string> attachments;  // files that go with the next message
    };
    std::vector<AskBubble> askBubbles;
    int askSerial = 0;
    EntityId askMenuEntity = kNullEntity;  // the object the Scene context menu is for (none: empty space)
    Vec3 askMenuPoint, askMenuNormal{0, 1, 0};  // the spot it was opened on (surface, or the ground plane)
    bool askMenuUI = false;      // it was opened on a UI element drawn over the Scene view: the spot is in canvas reference pixels
    bool sceneUIShown = false;   // this frame the Scene view draws the UI (bubbles of UI elements stand at their rectangles)
    bool sceneRightPressed = false;        // right button went down on the view (a click opens the menu)
    std::map<std::string, std::array<float, 4>> askRects;  // automation: "<serial>:input", "<serial>:send"
    void OpenAskBubble(EntityId entity, const std::string& backend);
    void OpenAskSpot(const Vec3& point, const Vec3& normal, EntityId surface, const std::string& backend, bool ui = false);
    std::string SpotLabel(const AskBubble& bubble) const;  // "(3.2, 0, -1.5) on Ground"
    void SceneAskMenu();
    // The CLI that answers direct conversations: the one set in Settings, else the first installed
    // one that can run turns; empty while none is known.
    std::string AskAgent() const;
    void AskBubbles(ImVec2 viewPos, ImVec2 viewSize);
    bool AskBubbleBody(AskBubble& bubble);  // true: closed
    void AskSend(AskBubble& bubble);
    std::string AskContext(const AskBubble& bubble) const;
    // A direct conversation's messages and running line (bubbles and the Agent Chat panel); a
    // change in what is drawn scrolls to the newest. `details`: turn tools/time/cost under replies.
    void DrawDirectConversation(const Json& state, const std::string& target, size_t& shown, bool details);
    // A message's attachments: pictures as thumbnails (click: the viewer), other files as buttons.
    void DrawAttachments(const Json& attachments, float thumbnailEm);
    // Files waiting to go with the next message, as chips (click removes one).
    void AttachmentChips(std::vector<std::string>& files);
    // The native file chooser; the chosen file is added to `files` (at most 8).
    void ChooseAttachment(std::vector<std::string>& files);

    // ----- Agent Chat panel (EditorAsk.cpp): talk to one installed agent CLI, model chosen here,
    // outside the team (team.ask conversations "chat-<cli>[-n]"; object bubbles' ones are listed too).
    bool showAgentChat = true, requestAgentChatFocus = false, agentChatFocusInput = false;
    std::string agentChatBackend, agentChatThread, agentChatInput;
    Json agentChatState;                          // team.thread of agentChatThread
    Json agentChatThreads = Json::MakeArray();    // team.threads
    double agentChatPolled = -100, agentChatThreadsPolled = -100;
    size_t agentChatShown = 0;
    std::array<float, 4> agentChatInputRect{0, 0, 0, 0};
    ChatCompleteState agentChatComplete;
    std::vector<std::string> agentChatAttachments;  // files that go with the next message
    std::array<float, 4> agentChatRect{0, 0, 0, 0};  // the panel's window: a file dropped inside it is attached
    void AgentChatPanel();
    void AgentChatSelect(const std::string& backend, const std::string& thread);
    void AgentChatSend();

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
    ChatCompleteState chatComplete;
    std::vector<ChatCompletion> Completions(char marker, const std::string& prefix, ChatMentions mentions) const;
    ImU32 ChatTokenColor(const std::string& word, const std::vector<std::string>* files, size_t& length,
                         ChatMentions mentions = ChatMentions::Agents) const;
    // A chat input (Team Chat, Agent Chat, Scene bubbles): multi-line, Enter sends (returns true),
    // Shift+Enter or Ctrl+Enter adds a line; a typed @word or #word completes from a list floating
    // above it (Up / Down choose, Tab or a click takes, files are previewed beside it) and the
    // references in the text are tinted.
    // `bounds` / `active`: the field's screen rectangle and whether it is being typed in.
    bool ChatInput(const char* id, std::string& text, ImVec2 size, ChatCompleteState& complete, ChatMentions mentions, bool& focus,
                   std::array<float, 4>* bounds = nullptr, bool* active = nullptr);
    // A message's text as Markdown (headings, lists, quotes, tables, rules, fenced code; inline bold,
    // italic, code, strike and links), wrapped to the panel; `tint` colors @mentions and `files`.
    void DrawChatMarkdown(const std::string& text, const std::vector<std::string>& files, bool tint, ImU32 color);
    // One line of inline Markdown laid out from `origin` within `width`; returns its height.
    float DrawChatInline(const std::string& line, ImVec2 origin, float width, float size, ImU32 color, bool bold, const std::vector<std::string>* files);
    void DrawChatCode(const std::string& code, const std::string& language, float width);
    void OpenChatLink(const std::string& target);  // http(s) in the browser, project files in their editor
    void PollChat();
    void SendChat();
    void RouteDroppedFiles();
    bool DrawImageFile(const std::string& absolutePath, ImVec2 box, float zoom);
    void ImportChatImage(const std::string& path);
    void ImageViewer();
    void TeamChatPanel();

    // ----- Settings window (EditorSettings.cpp): Agents tab (CLIs installed, login, how they
    // reach the editor, permissions of direct conversations).
    bool showSettings = false;
    std::string settingsTab;  // tab to select on the next frame ("agents"); empty = keep
    void SettingsWindow();
    void SettingsAgentsTab();
    double settingsPolled = -100;
    Json settingsConnection;  // team.connection
    std::string settingsInstructions;        // the agent's instructions as typed (saved when the field is left)
    bool settingsInstructionsLoaded = false;

    // ----- Tile painting (EditorTiles.cpp): Tiles panel + brush in the Scene view
    bool showTiles = true;
    bool tilePaint = false;        // Scene view clicks paint the selected Tilemap
    bool requestTilesFocus = false;
    int tileLayer = -1;
    int sliceWidth = 16, sliceHeight = 16;
    std::string slicePath = "sprites/sheet.atlas.json";
    int collisionPoint = -1;
    int collisionSerial = 0;
    std::string tileDataKey;
    std::map<std::string, std::array<float, 4>> twoDimensionalRects;
    void TwoDimensionalRect(const std::string& name);
    void TileRuleTools();
    void TileLayerTools();  // layer picker, add / rename / move / remove, visible, collision
    std::string tileLayerName;
    bool sliceTrim = false;
    float outlineCutoff = 0.5f;
    bool collisionSnap = true;
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

    // ----- Sprite Editor (EditorSprite.cpp): Slice Sheet and per-frame Collision of the selected Sprite
    bool showSpriteEditor = false, focusSpriteEditor = false;
    int spriteEditorTab = 0;                 // 0 = Slice Sheet, 1 = Collision
    EntityId spriteEditorEntity = kNullEntity;  // the sprite the view was fitted for
    bool spriteFit = true;                   // keep the picture fitted to the view (until the person zooms or pans)
    float spriteZoom = 1;                    // Slice: screen pixels per texel; Collision: per local unit
    ImVec2 spritePan{0, 0};                  // screen offset of the picture (Slice) or the entity origin (Collision)
    std::string spriteFrameKey;              // the frame the Collision tab edits (Collider2D.framePoints key)
    int sliceMode = 0;                       // 0 cell size, 1 cell count, 2 find sprites
    int sliceColumns = 4, sliceRows = 4, sliceGap = 2, slicePivot = 0;
    int sliceOffset[2] = {0, 0}, sliceSpacing[2] = {0, 0};
    bool sliceOverwrite = false;
    bool sliceEdited = false;                // settings changed since the last apply: the canvas shows the preview
    Json slicePreview;                       // atlas.create {dryRun} for the current settings
    std::string slicePreviewKey;
    bool collisionEditDefault = false;       // edit Collider2D.points instead of the frame's own outline
    bool collisionOnion = true;
    std::map<std::string, std::array<float, 4>> spriteRects;  // automation: control bounds by name
    struct SpriteSheetFrame {
        std::string key;  // framePoints key: atlas frame name or sheet index
        float x = 0, y = 0, w = 0, h = 0;  // pixels in the sheet
    };
    // The frames the sprite's atlas or grid cuts its sheet into; `sheet` gets the image path.
    std::vector<SpriteSheetFrame> SpriteSheetFrames(const Sprite& sprite, std::string* sheet);
    void SpriteRect(const std::string& name);
    void SpriteEditorPanel();
    void SpriteSliceTab(const Sprite& sprite);
    void SpriteCollisionTab(const Sprite& sprite);
    void SpriteCanvasInput(ImVec2 origin, bool hovered, bool active);
    void SpriteZoomBadge(ImVec2 origin, ImVec2 size, float scale);  // scale: zoom units -> screen pixels per image pixel

    // ----- Game view
    int gameAspect = 0;  // 0 free, 1 16:9, 2 16:10, 3 4:3
    bool gameFocused = false;
    bool gameHovered = false;
    ImVec2 gameImagePos{0, 0}, gameImageSize{0, 0};
    std::set<std::string> gameKeysDown;
    std::string composing;       // what the input method is composing (WindowEvent Composition), drawn at the caret
    bool gameComposing = false;  // ... and the game's focused UIInput has it (input.text)
    std::set<std::string> gameAxesForwarded;  // only these axes are released when Game loses focus
    bool gameMouseDown[2] = {false, false};
    float lastGameMouse[2] = {-1, -1};
    // UI editing where it is drawn (the Scene view, the stopped Game view): select, drag to move, handles to resize.
    int uiDragHandle = -1;        // -1 none, 0 = move, 1..8 = handles clockwise from the top-left corner
    EntityId uiDragEntity = kNullEntity;
    std::string uiDragType;       // UI component being edited
    float uiDragStart[4] = {0, 0, 0, 0};  // x, y, width, height when the drag began (reference pixels)
    ImVec2 uiDragMouse{0, 0};
    int uiDragSerial = 0;
    // `scene`: in the Scene view, where the world is behind the UI: a click that hits no element is
    // left to the world (returns false), and panels that draw nothing do not take clicks.
    // Returns true while the pointer is busy with UI (an element was pressed or is being dragged).
    bool UIEdit(ImDrawList* dl, ImVec2 pos, int w, int h, bool hovered, bool scene);
    // The topmost element of a layout under a point of its view, or kNullEntity. `scene`: as above,
    // panels that draw nothing are passed through (unless selected).
    EntityId UIElementAt(const std::vector<UIRect>& rects, float x, float y, bool scene) const;
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
    void FrameScene();  // fit the whole scene in the Scene view
    bool DefaultViewShowsGeometry(float aspect);  // any drawable inside the current Scene-view frustum
    Json EditorState();                     // editor.state
    Json EditorSelect(const Json& args);    // editor.select {ids, frame}
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
    void UpdateTextInput();

    // Panels (EditorPanels.cpp)
    void HierarchyPanel();
    void HierarchyNode(const EntityRow& row, const std::map<EntityId, std::vector<EntityId>>& children, const std::set<EntityId>& visible);
    void EntityContextMenu(const EntityRow* row);
    void InspectorPanel();
    bool FieldEditor(EntityId id, const std::string& type, const std::string& field, const Json& schema, const Json& value);
    void AxisLockRow(EntityId id, const std::string& type, const std::string& group, const Json& properties, const Json& values);
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

    // Bones (EditorBones.cpp).
    bool showBones = true;                 // skeleton of the selected model over the Scene view
    std::string boneFilter, namePickFilter, shapeFilter;
    EntityId boneHoverModel = kNullEntity;  // bone under the mouse in the inspector, lit in the Scene view
    std::string boneHover;
    int boneHoverFrame = -1;
    // Model the selection's bones belong to: the entity's own, or its parent's for a BoneIK / BoneAttachment entity.
    std::shared_ptr<const Mesh> BoneModel(EntityId id, EntityId* model);
    bool NameInModel(EntityId id, bool bone, const std::string& name);
    // Popup listing the model's bones (bone = true) or clips; true when one was picked into `out`.
    bool NamePicker(const char* popupId, EntityId id, bool bone, const std::string& current, std::string& out);
    // Holds `existing` (or a new entity) on a bone of `model`; AddIkGoal creates a goal entity on the bone.
    void AttachToBone(EntityId model, const std::string& bone, EntityId existing);
    void AddIkGoal(EntityId model, const std::string& bone);
    // A child entity whose SpringBone swings `bone` and the bones below it.
    void AddSpringBone(EntityId model, const std::string& bone);
    void BonesSection(EntityId id);
    // Retarget Animation window (EditorRetarget.cpp).
    struct RetargetRow { std::string target, source; bool end = false; };
    struct RetargetClip { std::string source, name; float duration = 0; bool use = true; };
    bool showRetarget = false, focusRetarget = false, retargetError = false, retargetAttach = true;
    std::string retargetTarget, retargetSource, retargetOut, retargetHips, retargetStatus, retargetLoaded, retargetFilter;
    std::vector<RetargetRow> retargetRows;
    std::vector<RetargetClip> retargetClips;
    std::map<std::string, std::array<float, 4>> retargetRects;  // automation: control bounds by name
    void OpenRetarget(const std::string& target, const std::string& source);
    void RetargetLoad();
    void RetargetAutoMap();
    void RetargetRun();
    void RetargetWindow();
    void BlendShapesSection(EntityId id);
    void BonesOverlay(ImDrawList* dl);
    void SceneGizmo();
    void ScenePick(float x, float y, bool additive);
    EntityId ScenePickId(float x, float y, Vec3* point = nullptr, Vec3* normal = nullptr);  // render.pick at a Scene view pixel
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
// "startSize" -> "Start Size", "uv" -> "UV": schema names as labels where no translated label exists.
std::string HumanizeName(const std::string& name);
// Kind of asset a string field holds ("model", "texture", ...), "" for plain text.
std::string AssetKindForField(const std::string& type, const std::string& field);

}  // namespace oe
