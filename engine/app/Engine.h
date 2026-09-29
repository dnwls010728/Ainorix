#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "api/Commands.h"
#include "core/Json.h"
#include "render/Renderer.h"
#include "scene/Scene.h"
#include "scene/Systems.h"

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
    // Loads another scene at the end of the current frame, keeping the Lua
    // state and game data. sim.stop still restores the edit-time scene.
    void RequestSceneChange(const std::string& path) { pendingScene_ = path; }
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
    std::future<std::vector<uint8_t>> PostJob(std::function<std::vector<uint8_t>()> job);
    void RunPostedJobs();

private:
    void SimulateFrame();
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
    struct TimedLine {
        DebugLine line;
        double expires;  // sim time; < 0 = never
    };
    std::vector<TimedLine> debugLines_;
    std::string pendingScene_;
    std::string runtimeScene_;
    double simTime_ = 0.0;
    double accumulator_ = 0.0;

    std::vector<Json> undo_;
    std::vector<Json> redo_;
    std::string lastMergeKey_;  // component.set {merge} of the newest undo step

    std::mutex jobsMutex_;
    std::deque<std::function<void()>> jobs_;
};

}  // namespace oe
