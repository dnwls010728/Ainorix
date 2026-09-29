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

    // ----- Rendering -------------------------------------------------------
    IRenderer& Renderer() { return *renderer_; }
    RenderStats RenderGameView(RenderTarget& target);

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
    void SetSceneLocation(const std::string& path);
    void ResetHistory();

    Scene scene_;
    InputState input_;
    std::unique_ptr<IRenderer> renderer_;
    CommandRegistry commands_;

    std::string projectDir_;
    std::string projectName_ = "Untitled";
    std::string scenePath_;
    bool dirty_ = false;

    bool playing_ = false;
    std::unique_ptr<Json> playSnapshot_;
    uint64_t frame_ = 0;
    uint64_t revision_ = 1;
    double simTime_ = 0.0;
    double accumulator_ = 0.0;

    std::vector<Json> undo_;
    std::vector<Json> redo_;

    std::mutex jobsMutex_;
    std::deque<std::function<void()>> jobs_;
};

}  // namespace oe
