#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>
#include <set>
#include <string>
#include <vector>

#include "api/Commands.h"
#include "app/SaveStore.h"
#include "app/AnimationPreview.h"
#include "audio/AudioSystem.h"
#include "core/Json.h"
#include "net/Link.h"
#include "net/Session.h"
#include "net/sync/Authority.h"
#include "render/Renderer.h"
#include "scene/Scene.h"
#include "scene/Systems.h"
#include "script/ScriptHost.h"
#include "physics/PhysicsWorld.h"

namespace oe {

class ScriptHost;
struct UIRect;
class PhysicsWorld;
class Navigation;
class AudioSystem;
class AssetManager;
class GpuDevice;
class GpuRenderer;
struct Material;
struct Mesh;
struct Texture;
class Window;

// The engine instance: one scene, a fixed-step simulation, a renderer, an
// undo history and the command API. All state changes from tools go through
// Engine::Call so every front-end (CLI, editor, MCP, scripts) behaves the same.
class Engine {
public:
    static constexpr double kFixedDt = 1.0 / 60.0;

    Engine();
    ~Engine();

    // ----- Project / files -------------------------------------------------
    // Accepts a project directory, a project.json, or a *.scene.json file.
    bool Open(const std::string& path, std::string* error);
    bool LoadScene(const std::string& path, std::string* error);
    bool SaveScene(const std::string& path, std::string* error);
    void NewScene(const std::string& name);
    // Resolves a user supplied path against the project directory and refuses
    // paths that escape it (the API may be driven by untrusted callers).
    std::string ResolvePath(const std::string& path) const;
    const std::string& ProjectDir() const { return projectDir_; }
    // Reads and parses a project JSON file (prefab, scene). Throws ApiError.
    Json ReadProjectJson(const std::string& path) const;
    // Isolated VFX authoring document: commands share history, validation and atomic saves with the editor.
    Json VfxDocumentCall(const std::string& operation, const Json& args);
    // Active draft for a resolved asset path, or nullptr when that asset is not open.
    const Json* VfxDocument(const std::string& path) const;
    // Isolated animation preview commands, available with or without the editor.
    Json AnimationPreviewCall(const std::string& operation, const Json& args);
    // Stopped preview copy when active; otherwise the authored/live scene. Never used for simulation/saving.
    Scene& AnimationRenderScene();
    // Instantiates a prefab file into the current scene. Throws ApiError.
    EntityId InstantiatePrefabFile(const std::string& path, EntityId parent);
    const std::string& ProjectName() const { return projectName_; }
    const std::string& ScenePath() const { return scenePath_; }
    bool Dirty() const { return dirty_; }
    void MarkDirty() { dirty_ = true; }

    // ----- Simulation ------------------------------------------------------
    Scene& GetScene() { return scene_; }
    InputState& Input() { return input_; }
    // The size of the view the game is drawn into (input.viewSize, camera.worldToScreen,
    // ui.screenToCanvas), kept across the network input stream. The editor's Game view sets it
    // every frame; windows set it through the platform input.
    void SetViewSize(int width, int height) {
        input_.viewWidth = deviceInput_.viewWidth = width;
        input_.viewHeight = deviceInput_.viewHeight = height;
    }
    // Lua input.setLook: stores the local look direction on the device input (see InputState::lookX).
    void SetLook(float x, float y);
    void Play();
    void Pause();
    void Stop();  // restores the scene captured when play started
    void Step(int frames);
    // Native full-state snapshots preserve Lua allocations and physics solver state.
    // Explicit recording avoids any journal work in ordinary single-player games.
    struct SimulationState;
    void RecordState();
    std::shared_ptr<const SimulationState> SaveState() const;
    void LoadState(const SimulationState& state);
    Json SnapshotCall(const std::string& operation, const Json& args);
    const InputState& PlayerInput(uint32_t player) const;
    // Advances the simulation by real elapsed time while playing (fixed steps).
    void Tick(double realDt);
    // Render interpolation (display only, never part of the simulation): Tick runs fixed 1/60 s
    // steps while windows draw at the monitor's rate, so without it moving things and the camera
    // judder (a frame shows 0 or 2 steps). Between Begin and End every Transform that existed one
    // step ago is put between its previous and current value by the time left in the accumulator;
    // End restores the simulated values. Inactive after Step (tools, tests, screenshots) and in
    // edit mode, so hashes and deterministic frames are unchanged. Moves longer than 3 m snap.
    void BeginInterpolatedRender();
    void EndInterpolatedRender();
    // Network watchdog for games in a window: when the main loop has not advanced the network
    // session for 0.1 s (a GPU or driver hitch, a long OS call outside the engine), a background
    // thread advances only the session's I/O at 60 Hz (heartbeats, acknowledgements, retransmits,
    // buffered messages) until the loop is back, so peers do not take the stall for a lost
    // connection. It never touches the scene or the simulation, and only runs while no engine
    // entry point (Tick, Step, Call) is active. Off by default: tools and tests step exactly.
    void EnableNetworkWatchdog(bool on);
    bool Playing() const { return playing_; }
    // The packaged player runs this engine: game data came from `oe package` (scripts may be bytecode).
    void SetPackaged(bool on) { packaged_ = on; }
    bool Packaged() const { return packaged_; }
    std::string ScriptReloadBlockReason() const;
    bool InPlaySession() const { return playSnapshot_ != nullptr; }
    uint64_t Frame() const { return frame_; }
    // Increments on every scene change (edits, undo, loads, simulation steps).
    // Tools poll it cheaply to know when to refresh.
    uint64_t Revision() const { return revision_; }
    void Touch();
    double SimTime() const { return simTime_; }
    // Calendar date "YYYY-MM-DD" in local time for game features outside the simulation (daily rewards).
    // Not part of the deterministic state: tests and tools fix it with SetDateOverride (time.date {set}).
    std::string Today() const;
    void SetDateOverride(const std::string& date) { dateOverride_ = date; }
    const std::string& DateOverride() const { return dateOverride_; }

    // ----- Host application (the web page, a store SDK layer) -----------------
    // Lets a game ask the app that embeds the player for things the engine does not do itself
    // (ads, purchases, analytics): host.call(name, data) goes out, answers come back as messages
    // the game reads with host.poll(). Like the mouse it is device state, outside the
    // deterministic simulation; nothing is sent while a network match replays frames.
    struct HostMessage {
        std::string name;
        Json data;
    };
    struct HostCallRecord {
        std::string name;
        Json data;
        uint64_t frame = 0;
        bool handled = false;
    };
    // The player installs the platform's handler (PlatformHostCall); it returns false when the
    // host does not know `name`, so games fall back to their own behaviour.
    void SetHostHandler(std::function<bool(const std::string& name, const std::string& json)> handler) { hostHandler_ = std::move(handler); }
    // Tools pretend a host handles these names ("*" = every name) to test the hosted path headless.
    void SetHostAccept(std::set<std::string> names) { hostAccept_ = std::move(names); }
    const std::set<std::string>& HostAccept() const { return hostAccept_; }
    bool HostCall(const std::string& name, const Json& data);
    // Queues a message from the host; the oldest are dropped beyond kMaxHostMessages.
    void PushHostMessage(const std::string& name, const Json& data);
    std::vector<HostMessage> TakeHostMessages();
    size_t PendingHostMessages() const { return hostInbox_.size(); }
    const std::vector<HostCallRecord>& HostCalls() const { return hostCalls_; }
    // ISO 639-1 language of the user ("en" when unknown); the player sets it from the platform.
    const std::string& UserLanguage() const { return userLanguage_; }
    void SetUserLanguage(const std::string& language) { userLanguage_ = language.empty() ? "en" : language; }
    static constexpr size_t kMaxHostMessages = 256;
    static constexpr size_t kMaxHostCalls = 64;

    // ----- Links (Lua link.*, commands link.*) ---------------------------------
    // Outbound framed TCP to services that are not engine sessions (net/Link.h). Device state like
    // the host bridge: pumped once per frame, closed whenever the play session's Lua state is reset
    // (sim.stop, a match start). Lockstep/rollback matches and predicting clients have none.
    LinkSet& Links() { return links_; }
    // Throws ApiError when links are unavailable in the current match or the endpoint is invalid.
    uint32_t OpenLink(const std::string& address, uint32_t port);

    // ----- Launch parameters (--set name=value, Lua game.arg) ------------------
    // Set by whoever starts the process (which channel a server is, where its services are). They
    // are not game data: they survive sim.stop, scene changes and match starts.
    bool SetLaunchArg(const std::string& name, const std::string& value, std::string* error);
    const std::map<std::string, std::string>& LaunchArgs() const { return launchArgs_; }
    static constexpr size_t kMaxLaunchArgs = 64;

    // ----- Game runtime (valid during a play session) -------------------------
    // Data that survives scene changes (score, lives...). Reset when the
    // session ends. Scripts use game.get/game.set; tools read game.state.
    Json& GameData() { return gameData_; }
    // Gameplay pause (menus, level-up dialogs): scripts keep running with dt = 0 and UI stays clickable,
    // while physics, built-in systems, timers and particles stand still. Cleared on scene changes and sim.stop.
    bool GamePaused() const { return gamePaused_; }
    // Button or slider with the keyboard/gamepad focus (UICanvas.navigation), kNullEntity = none. A focus on
    // something that cannot be reached (hidden, disabled, under a modal panel) is dropped on the next frame.
    EntityId UIFocus() const { return uiFocus_; }
    void SetUIFocus(EntityId id) { uiFocus_ = id; }
    // A UIInput has the focus: the platform should offer text entry (Window::SetTextInput; the soft keyboard on phones).
    bool TextInputActive() const;
    // Clipboard of UIInput copy/cut/paste. Tools and tests keep a private one; a game window uses the system's.
    void UseSystemClipboard(bool on) { systemClipboard_ = on; }
    std::string ClipboardText() const;
    void SetClipboardText(const std::string& text);
    void SetGamePaused(bool paused) { gamePaused_ = paused; }
    // game.quit: the game asks its window to close. Game runtimes (oe run, the packaged player) end their
    // loop; tools and the editor only report it (game.state). Cleared when the play session stops.
    void RequestQuit() { quitRequested_ = true; }
    bool QuitRequested() const { return quitRequested_; }
    // Save slots survive sim.stop and scene transitions; memory-only by default.
    SaveStore& Saves() { return saves_; }
    // Loads another scene at the end of the current frame, keeping the Lua
    // state and game data. sim.stop still restores the edit-time scene.
    void RequestSceneChange(const std::string& path) {
        if (authority_ && authority_->Active()) throw ApiError("match_scene", "authoritative scene transitions require restarting the match", "Leave/stop, load the same scene on each peer, then start a new ready barrier.");
        pendingScene_ = path;
    }
    const std::string& RuntimeScene() const { return runtimeScene_; }

    // ----- Debug drawing -----------------------------------------------------
    // Lines drawn in every view until `seconds` of simulated time pass
    // (0 = until the next simulated frame; negative = until cleared).
    void AddDebugLine(const Vec3& a, const Vec3& b, const Color& color, float seconds);
    void ClearDebugLines() { debugLines_.clear(); }
    void AppendDebugLines(std::vector<DebugLine>& out) const;
    size_t DebugLineCount() const { return debugLines_.size(); }

    // ----- Navigation --------------------------------------------------------
    // The scene's navigation mesh (engine/nav), baked from its static geometry when first asked for.
    // While the game is stopped it follows edits; during play it stays as baked until `rebake`
    // (nav.bake), since scripts move things every frame. Not baked: see NavInfo()["error"].
    Navigation& Nav(bool rebake = false);
    // The last bake: {baked, colliders, meshes, triangles, warnings, error?} + the mesh's numbers.
    const Json& NavInfo() const { return navInfo_; }

    // ----- Scripting -------------------------------------------------------
    ScriptHost& Scripts() { return *scripts_; }
    // Shared command/Lua session surface; absent/none projects allocate no session.
    Json NetworkCall(const std::string& command, const Json& args);
    // Preview peer index 0 is this engine; 1..7 are explicitly created local peers.
    Engine* LocalPeer(size_t index) { return index == 0 ? this : index <= localPeers_.size() ? localPeers_[index - 1].get() : nullptr; }
    // Cheap opt-in check: editor UI skips network queries in single-player projects.
    bool NetworkEnabled() const { return networkConfig_.mode != "none"; }
    Session* Network() const { return network_.get(); }
    std::shared_ptr<Session> SharedNetwork() const { return network_; }
    // Dedicated server, one world of several: this idle engine (its scene loaded) serves world `world`
    // of the session another engine hosts (net.serve there, then Session::SetWorlds). The engines must be
    // stepped in world order, world 0 first, each tick.
    void ServeWorld(std::shared_ptr<Session> session, uint32_t world);
    bool PredictEntity(EntityId id) const;
    PhysicsWorld& Physics() { return *physics_; }
    AudioSystem& Audio() { return *audio_; }
    AssetManager& Assets() { return *assets_; }
    // Sends mixed audio to the speakers (interactive modes only).
    void EnableAudioOutput();

    // ----- Rendering -------------------------------------------------------
    // The deterministic software renderer: frame hashes, tests, picking, `oe render`.
    IRenderer& Renderer() { return *renderer_; }
    RenderStats RenderGameView(RenderTarget& target);

    // Optional GPU renderer (render/GpuRenderer.h) for what people look at.
    // Bound to `window` it can present there; headless it renders offscreen.
    // Returns false (with `error`) when no GPU backend is available. Only one
    // GPU renderer can exist; a second call just reports whether it is on.
    bool EnableGpu(Window* window, std::string* error);
    // Before EnableGpu: a disk cache for compiled shaders and background compiles of shader graphs
    // (GpuRenderer::Settings). The editor turns both on; games and tests compile on first use.
    void SetGpuShaderOptions(const std::string& cacheDir, bool async) {
        gpuShaderCache_ = cacheDir;
        gpuAsyncShaders_ = async;
    }
    GpuRenderer* Gpu() { return gpu_.get(); }
    // Loads project files before they are needed, so that their first use does not stall a frame
    // (asset.preload, Lua game.preload): effects with everything their emitters draw with, materials,
    // textures, models, shader graphs, fonts and sounds. With a GPU renderer the textures and meshes
    // are uploaded and graph programs compiled as well (at once, or when the GPU renderer is enabled). Returns {loaded: [{path, kind}], errors:
    // [{path, message}], gpu}; a file that cannot be loaded is reported, the others are still loaded.
    Json Preload(const std::vector<std::string>& paths);
    // Reflection probes: {probes: [{id, name, position, min, max, resolution, captured, width, height, levels}], max}.
    // `bake` takes every probe's picture again now (probe.bake); otherwise pictures are taken when a
    // frame first needs them and `captured` tells which exist.
    Json ProbeState(bool bake);
    // GPU when enabled, otherwise the software renderer (editor viewport, game window).
    IRenderer& DisplayRenderer();
    // Draws the game view into the GPU window (EnableGpu with a window) and
    // presents it. `renderScale` < 1 renders the 3D image smaller and upscales it.
    bool PresentGameView(float renderScale = 1.0f);

    // ----- History ---------------------------------------------------------
    bool Undo();
    bool Redo();
    size_t UndoDepth() const { return undo_.size(); }
    size_t RedoDepth() const { return redo_.size(); }
    // Command metadata in chronological order; cursor is the applied entry count.
    Json HistoryState() const;
    // Prefab editing isolates a source file, preserving the scene and its history.
    bool BeginPrefabEdit(const std::string& path, std::string* error);
    bool SavePrefabEdit(std::string* error);
    bool EndPrefabEdit(bool discard, std::string* error);
    const std::string& EditingPrefab() const { return prefabPath_; }

    // ----- API ---------------------------------------------------------------
    CommandRegistry& Commands() { return commands_; }
    // Runs a command and returns {"ok":true,"result":...} or
    // {"ok":false,"error":{"code","message","hint"}}. Main thread only.
    Json Call(const std::string& name, const Json& args);

    // ----- Threading ---------------------------------------------------------
    // Queues work for the main thread (used by the HTTP server thread).
    // `agent`: who asked, when the caller says so (a team agent's id from `oe mcp --connect
    // --agent`); empty for an anonymous API call.
    std::future<Json> PostCall(const std::string& name, const Json& args, const std::string& agent = std::string());
    // While a posted call runs and inside the remote-call observer: the `agent` that made it, else empty.
    const std::string& RemoteCallAgent() const { return remoteAgent_; }
    // Called on the main thread after each PostCall command (HTTP, MCP): the
    // native editor uses it to show what an agent changed. Empty = none.
    using CallObserver = std::function<void(const std::string& name, const Json& args, const Json& result)>;
    void SetRemoteCallObserver(CallObserver observer) { remoteObserver_ = std::move(observer); }
    // The native editor showing this engine (oe editor) answers editor.state and editor.select
    // through this (op "state" / "select"); empty when no editor is open.
    using EditorHandler = std::function<Json(const std::string& op, const Json& args)>;
    void SetEditorHandler(EditorHandler handler) { editorHandler_ = std::move(handler); }
    const EditorHandler& GetEditorHandler() const { return editorHandler_; }
    std::future<std::vector<uint8_t>> PostJob(std::function<std::vector<uint8_t>()> job);
    void RunPostedJobs();

private:
    friend struct OfficialAgreementTestAccess;
    // Private test seam for controlled mixed-build barriers; never exposed through commands/Lua.
    std::function<std::string()> officialFingerprintRecordForTests_;
    void SimulateFrame();
    void ResetLocalPeers();
    void AdvanceLocalPeers();
    void AutoStartNetwork();
    void EnterWorld(const std::string& scene);  // client: the host moved us to the world of that scene
    Json LocalPeersState();
    void SimulateWorld();
    void EnsureSync(bool refresh = false);
    bool PollSync();
    void ApplyInputs(const FrameInputs& inputs);
    uint64_t ContentHash() const;
    std::string SyncWorld() const;
    struct NativeState;
    std::shared_ptr<const NativeState> CaptureNative(bool output) const;
    void RestoreNative(const NativeState& state);
    void CaptureCheckpoint();
    void EnsureAuthority(bool refresh = false);
    bool PollAuthority();
    void AuthorityApplied();
    // Host: removes departed participants from a running match (avatars destroyed, other owned
    // entities become server-owned); stops only when no remote participant remains.
    void DropAuthorityPlayers();
    std::vector<PhysicsEvent> StepAuthorityPhysics(float dt);
    Json ReplicatedWorld(uint32_t player);
    Json PredictedOwnedFields();
    void CorrectOwnedPrediction(const Authority::Update& update);
    bool PredictionAgrees(const Authority::Update& update) const;
    void ValidateReplicatedWorld(const Json& world) const;
    void ApplyReplicatedWorld(const Json& world, bool owned, bool remotes);
    // Poses remote entities from buffered snapshots; advance moves the playback frame (once per tick).
    void InterpolateAuthority(bool advance = true);
    void ResetAuthority();
    struct UIEvent {
        EntityId id;
        const char* method;
        bool hasValue;         // the script gets `value`
        float value;
        bool isFlag;           // ... as a boolean (`value != 0`)
        bool hasText = false;  // the script gets `text`
        std::string text;
        UIEvent(EntityId entity, const char* name, bool withValue = false, float number = 0.0f, bool flag = false)
            : id(entity), method(name), hasValue(withValue), value(number), isFlag(flag) {}
    };
    // Pointer hover/press/click and slider drags for this frame's input.
    std::vector<UIEvent> UpdateUI();
    // A click on a button: onClick, and for a toggle the new `on` (radio groups turn the others off) with onToggle.
    void ClickButton(EntityId id, std::vector<UIEvent>& events);
    // Typing into the focused UIInput; UICanvas.navigation: arrows / D-pad move the focus or change a focused
    // slider or drop-down, Enter / GamepadA clicks.
    void UpdateUIFocus(const std::vector<UIRect>& rects, std::vector<UIEvent>& events);
    // UIButton.key: buttons held by the mouse or any finger hold their key down.
    std::set<EntityId> HeldInputButtons(const InputState& input, bool includeClick);
    // Host: creates (or adopts a preplaced) NetPlayer entity for a participant from network.playerPrefab.
    void SpawnAuthorityPlayer(uint32_t player);
    // Late-join host: invites ready lobby players into the running match and spawns those that arrived.
    void AdmitAuthorityPlayers();
    InputState SampleNetworkInput();
    // Authoritative match: adds what only this device knows (pointer, typed text, touches, keys the project did
    // not declare as network actions) to the decoded local input, so local UI and scripts keep working. The
    // server never sees it; declared actions stay exactly what the input stream carries.
    void MergeLocalInput(const InputState& device);
    void UpdateButtonKeys();
    // UIJoystick: sticks held by the mouse or a finger drive their axes in `input`.
    void UpdateJoysticks(InputState& input);
    void BeginSessionIfNeeded();
    void ResetRuntime();
    void ApplySceneChange();
    void SetSceneLocation(const std::string& path);
    void ResetHistory();

    Scene scene_;
    std::unique_ptr<AnimationPreview> animationPreview_;
    std::string vfxEditPath_, vfxEditProject_, vfxEditFile_;
    Json vfxEditDocument_, vfxEditSaved_;
    std::vector<Json> vfxEditHistory_;
    size_t vfxEditCursor_ = 0;
    std::string vfxEditMerge_;
    InputState input_;
    std::unique_ptr<AssetManager> assets_;
    std::unique_ptr<IRenderer> renderer_;
    std::unique_ptr<GpuDevice> gpuDevice_;  // declared before gpu_: destroyed after it
    std::unique_ptr<GpuRenderer> gpu_;
    // Preloaded before a GPU renderer existed: handed to it when it is enabled.
    std::vector<std::shared_ptr<const Material>> preloadMaterials_;
    std::vector<std::shared_ptr<const Texture>> preloadTextures_;
    std::vector<std::shared_ptr<const Mesh>> preloadMeshes_;
    std::string gpuShaderCache_;
    bool gpuAsyncShaders_ = false;
    CommandRegistry commands_;
    std::unique_ptr<ScriptHost> scripts_;
    std::unique_ptr<PhysicsWorld> physics_;
    std::unique_ptr<Navigation> nav_;
    bool packaged_ = false;
    bool navTried_ = false;       // a bake was attempted for the scene as it is
    uint64_t navRevision_ = 0;    // revision_ the geometry was last collected at
    uint64_t navHash_ = 0;        // NavGeometry::Hash of the baked input
    Json navInfo_ = Json::MakeObject();
    std::unique_ptr<AudioSystem> audio_;
    SessionConfig networkConfig_;
    // Shared when this engine runs one world of a session with several (ServeWorld): the engine of
    // world 0 made the session and advances it; the others only use their world's part of it.
    std::shared_ptr<Session> network_;
    uint32_t networkWorld_ = 0;
    bool networkShared_ = false;
    uint64_t networkFrame_ = 0;
    std::vector<std::unique_ptr<Engine>> localPeers_;
    std::string previewTransport_;
    bool dedicated_ = false, autoStart_ = false;
    bool quitRequested_ = false;
    uint32_t minimumPlayers_ = 1;
    std::unique_ptr<FrameSync> sync_;
    std::unique_ptr<Authority> authority_;
    std::map<EntityId, uint32_t> authorityIds_;
    std::map<uint32_t, EntityId> replicatedEntities_;
    std::map<EntityId, Json> initialAuthorityEntities_;
    uint32_t nextAuthorityId_ = 1;
    std::map<uint64_t, std::shared_ptr<const NativeState>> predictionStates_;
    std::deque<Authority::Update> authorityWorlds_;
    std::map<uint64_t, std::vector<SessionEvent>> authorityEvents_;
    std::map<uint64_t, InputState> authorityLocal_;  // device-only input per unacknowledged input frame, for replays
    std::set<uint32_t> admitted_;  // network.admission "script": lobby players the game let in (net.admit)
    uint64_t authorityInput_ = 0, authorityRemoteFrame_ = 0, authorityCorrections_ = 0, authoritySkipped_ = 0;
    uint64_t authorityOwnedCorrections_ = 0;  // corrections done the network.prediction "owned" way
    // Per predicted input frame: the replicated fields of this client's own predicted entities, as
    // the server would send them (quantized). A snapshot that agrees needs no restore and replay.
    std::map<uint64_t, Json> predictedOwned_;
    std::map<uint32_t, InputState> playerInputs_;
    FrameInputs frameInputs_;
    std::set<std::string> networkButtonKeys_;  // device button edges, outside replay checkpoints
    InputState deviceInput_;
    struct JournalFrame { InputState input; FrameInputs players; };
public:
    struct SimulationState {
        std::shared_ptr<const NativeState> native;
        uint64_t firstFrame = 0;
        Json baseline, gameData, scene;
        std::string runtimeScene;
        std::vector<JournalFrame> frames;
        std::map<uint64_t, std::vector<std::pair<std::string, Json>>> commands;
        std::shared_ptr<const AudioSystem::Snapshot> baselineAudio, audio;
        SaveStore baselineSaves, saves;
        InputState input;
        std::vector<ScriptError> errors;
        uint64_t content = 0; uint32_t seed = 0; EntityId allocationCursor = 1;
        bool playing = false; double accumulator = 0;
    };
private:
    std::map<uint64_t, std::shared_ptr<const NativeState>> checkpoints_;
    std::unique_ptr<SimulationState> journal_;
    std::map<std::string, std::shared_ptr<const SimulationState>> snapshots_;
    bool replaying_ = false, inWorld_ = false;
    uint64_t outputConfirmed_ = 0;
    bool desyncNotified_ = false;
    double replayMilliseconds_ = 0;
    std::map<uint64_t, std::string> syncHashes_;
    double hotReloadTimer_ = 0.0;

    std::string projectDir_;
    std::string projectName_ = "Untitled";
    std::string scenePath_;
    bool dirty_ = false;

    bool playing_ = false;
    std::unique_ptr<Json> playSnapshot_;
    // The scene game.loadScene put in place during this play session, as it was loaded. An
    // authoritative match starts from it (else from playSnapshot_): a server started on another scene
    // file and a client that loaded that scene before joining begin from the same world.
    std::unique_ptr<Json> loadedScene_;
    uint64_t frame_ = 0;
    uint64_t revision_ = 1;
    Json gameData_ = Json::MakeObject();
    std::string dateOverride_;  // empty = the machine's clock
    std::function<bool(const std::string&, const std::string&)> hostHandler_;
    std::set<std::string> hostAccept_;
    std::vector<HostMessage> hostInbox_;
    std::vector<HostCallRecord> hostCalls_;  // most recent kMaxHostCalls, for host.state
    std::string userLanguage_ = "en";
    LinkSet links_;
    std::map<std::string, std::string> launchArgs_;
    bool gamePaused_ = false;
    SaveStore saves_;
    struct TimedLine {
        DebugLine line;
        double expires;  // sim time; < 0 = never
    };
    std::vector<TimedLine> debugLines_;
    std::string pendingScene_;
    std::string runtimeScene_;
    EntityId uiHovered_ = kNullEntity;
    EntityId uiPressed_ = kNullEntity;
    EntityId uiFocus_ = kNullEntity;       // keyboard/gamepad focus
    std::string uiRepeatKey_;              // editing key held in a UIInput and for how many frames (key repeat)
    int uiRepeat_ = 0;
    EntityId uiListPress_ = kNullEntity;   // UIDropdown whose open list is pressed: a release selects, a drag scrolls
    float uiListY_ = 0.0f;                 // pointer position the next scroll step is measured from (pixels)
    bool uiListDragged_ = false;
    bool systemClipboard_ = false;
    std::string clipboard_;
    EntityId uiScrollDrag_ = kNullEntity;  // UIScroll being dragged by the pointer
    float uiScrollLast_ = 0.0f;            // pointer position along its axis at the previous frame (pixels)
    EntityId uiScrollBar_ = kNullEntity;   // UIScroll whose bar thumb is held
    float uiScrollGrab_ = 0.0f;            // where on the thumb it was grabbed (pixels from its start)
    std::set<EntityId> heldButtons_;    // UIButtons with a key, held this frame
    std::set<std::string> heldKeys_;    // keys those buttons hold down
    double simTime_ = 0.0;
    double accumulator_ = 0.0;
    std::vector<std::pair<EntityId, Transform>> renderPrevious_, renderSaved_;  // see BeginInterpolatedRender
    double renderAlpha_ = 1.0;
    bool renderInterpolate_ = false;

    struct HistoryEntry {
        Json scene;
        std::string command;
        Json args;
    };
    std::vector<HistoryEntry> undo_;
    std::vector<HistoryEntry> redo_;
    std::string lastMergeKey_;  // component.set {merge} of the newest undo step
    struct PrefabEditBackup {
        Json scene;
        std::string scenePath;
        bool dirty = false;
        std::vector<HistoryEntry> undo, redo;
        std::string mergeKey;
    };
    std::unique_ptr<PrefabEditBackup> prefabBackup_;
    std::string prefabPath_;

    CallObserver remoteObserver_;
    EditorHandler editorHandler_;
    std::string remoteAgent_;
    std::mutex jobsMutex_;
    // Held by Tick, Step and Call (they may touch the network session); the watchdog advances the
    // session only while it is free.
    std::recursive_mutex engineMutex_;
    std::thread watchdog_;
    std::atomic<bool> watchdogStop_{false};
    std::atomic<int64_t> lastNetworkTick_{0};  // steady clock, microseconds
    std::atomic<uint64_t> watchdogTicks_{0};
    std::deque<std::function<void()>> jobs_;
};

}  // namespace oe
