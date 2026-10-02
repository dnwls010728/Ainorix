#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "api/Commands.h"
#include "app/SaveStore.h"
#include "audio/AudioSystem.h"
#include "core/Json.h"
#include "net/Session.h"
#include "net/sync/Authority.h"
#include "render/Renderer.h"
#include "scene/Scene.h"
#include "scene/Systems.h"
#include "script/ScriptHost.h"
#include "physics/PhysicsWorld.h"

namespace oe {

class ScriptHost;
class PhysicsWorld;
class AudioSystem;
class AssetManager;
class GpuDevice;
class GpuRenderer;
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
    // Instantiates a prefab file into the current scene. Throws ApiError.
    EntityId InstantiatePrefabFile(const std::string& path, EntityId parent);
    const std::string& ProjectName() const { return projectName_; }
    const std::string& ScenePath() const { return scenePath_; }
    bool Dirty() const { return dirty_; }
    void MarkDirty() { dirty_ = true; }

    // ----- Simulation ------------------------------------------------------
    Scene& GetScene() { return scene_; }
    InputState& Input() { return input_; }
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
    bool Playing() const { return playing_; }
    bool InPlaySession() const { return playSnapshot_ != nullptr; }
    uint64_t Frame() const { return frame_; }
    // Increments on every scene change (edits, undo, loads, simulation steps).
    // Tools poll it cheaply to know when to refresh.
    uint64_t Revision() const { return revision_; }
    void Touch() { ++revision_; }
    double SimTime() const { return simTime_; }

    // ----- Game runtime (valid during a play session) -------------------------
    // Data that survives scene changes (score, lives...). Reset when the
    // session ends. Scripts use game.get/game.set; tools read game.state.
    Json& GameData() { return gameData_; }
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

    // ----- Scripting -------------------------------------------------------
    ScriptHost& Scripts() { return *scripts_; }
    // Shared command/Lua session surface; absent/none projects allocate no session.
    Json NetworkCall(const std::string& command, const Json& args);
    // Preview peer index 0 is this engine; 1..7 are explicitly created local peers.
    Engine* LocalPeer(size_t index) { return index == 0 ? this : index <= localPeers_.size() ? localPeers_[index - 1].get() : nullptr; }
    // Cheap opt-in check: editor UI skips network queries in single-player projects.
    bool NetworkEnabled() const { return networkConfig_.mode != "none"; }
    Session* Network() const { return network_.get(); }
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
    GpuRenderer* Gpu() { return gpu_.get(); }
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

    // ----- API ---------------------------------------------------------------
    CommandRegistry& Commands() { return commands_; }
    // Runs a command and returns {"ok":true,"result":...} or
    // {"ok":false,"error":{"code","message","hint"}}. Main thread only.
    Json Call(const std::string& name, const Json& args);

    // ----- Threading ---------------------------------------------------------
    // Queues work for the main thread (used by the HTTP server thread).
    std::future<Json> PostCall(const std::string& name, const Json& args);
    // Called on the main thread after each PostCall command (HTTP, MCP): the
    // native editor uses it to show what an agent changed. Empty = none.
    using CallObserver = std::function<void(const std::string& name, const Json& args, const Json& result)>;
    void SetRemoteCallObserver(CallObserver observer) { remoteObserver_ = std::move(observer); }
    std::future<std::vector<uint8_t>> PostJob(std::function<std::vector<uint8_t>()> job);
    void RunPostedJobs();

private:
    void SimulateFrame();
    void ResetLocalPeers();
    void AdvanceLocalPeers();
    void AutoStartNetwork();
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
    std::vector<PhysicsEvent> StepAuthorityPhysics(float dt);
    Json ReplicatedWorld(uint32_t player);
    void ValidateReplicatedWorld(const Json& world) const;
    void ApplyReplicatedWorld(const Json& world, bool owned, bool remotes);
    void InterpolateAuthority();
    void ResetAuthority();
    struct UIEvent {
        EntityId id;
        const char* method;
        bool hasValue;
        float value;
    };
    // Pointer hover/press/click and slider drags for this frame's input.
    std::vector<UIEvent> UpdateUI();
    // UIButton.key: buttons held by the mouse or any finger hold their key down.
    std::set<EntityId> HeldInputButtons(const InputState& input, bool includeClick);
    InputState SampleNetworkInput();
    void UpdateButtonKeys();
    void BeginSessionIfNeeded();
    void ResetRuntime();
    void ApplySceneChange();
    void SetSceneLocation(const std::string& path);
    void ResetHistory();

    Scene scene_;
    InputState input_;
    std::unique_ptr<AssetManager> assets_;
    std::unique_ptr<IRenderer> renderer_;
    std::unique_ptr<GpuDevice> gpuDevice_;  // declared before gpu_: destroyed after it
    std::unique_ptr<GpuRenderer> gpu_;
    CommandRegistry commands_;
    std::unique_ptr<ScriptHost> scripts_;
    std::unique_ptr<PhysicsWorld> physics_;
    std::unique_ptr<AudioSystem> audio_;
    SessionConfig networkConfig_;
    std::unique_ptr<Session> network_;
    uint64_t networkFrame_ = 0;
    std::vector<std::unique_ptr<Engine>> localPeers_;
    std::string previewTransport_;
    bool dedicated_ = false, autoStart_ = false;
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
    uint64_t authorityInput_ = 0, authorityRemoteFrame_ = 0, authorityCorrections_ = 0;
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
    uint64_t frame_ = 0;
    uint64_t revision_ = 1;
    Json gameData_ = Json::MakeObject();
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
    std::set<EntityId> heldButtons_;    // UIButtons with a key, held this frame
    std::set<std::string> heldKeys_;    // keys those buttons hold down
    double simTime_ = 0.0;
    double accumulator_ = 0.0;

    std::vector<Json> undo_;
    std::vector<Json> redo_;
    std::string lastMergeKey_;  // component.set {merge} of the newest undo step

    CallObserver remoteObserver_;
    std::mutex jobsMutex_;
    std::deque<std::function<void()>> jobs_;
};

}  // namespace oe
