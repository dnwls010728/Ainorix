#include "app/Engine.h"

#include <algorithm>
#include <cctype>

#include "core/FileSystem.h"
#include "core/Log.h"
#include "scene/Prefab.h"
#include "assets/Assets.h"
#include "audio/AudioSystem.h"
#include "physics/PhysicsWorld.h"
#include "platform/Platform.h"
#include "render/GpuDevice.h"
#include "render/GpuRenderer.h"
#include "render/UI.h"
#include "script/ScriptHost.h"

namespace oe {

namespace {
constexpr size_t kMaxUndo = 200;

bool EndsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

Engine::Engine() : assets_(std::make_unique<AssetManager>(*this)), renderer_(std::make_unique<SoftwareRenderer>(assets_.get())) {
    scripts_ = std::make_unique<ScriptHost>(*this);
    physics_ = std::make_unique<PhysicsWorld>();
    audio_ = std::make_unique<AudioSystem>(*this);
    RegisterBuiltinCommands(commands_);
    projectDir_ = AbsolutePath(".");
}

Engine::~Engine() = default;

bool Engine::Open(const std::string& rawPath, std::string* error) {
    std::string path = AbsolutePath(rawPath);
    std::string projectFile;
    if (IsDirectory(path)) {
        projectFile = JoinPath(path, "project.json");
    } else if (EndsWith(path, "project.json")) {
        projectFile = path;
    }

    if (!projectFile.empty()) {
        std::string text;
        if (!ReadTextFile(projectFile, text)) {
            if (error) *error = "no project.json found at " + projectFile + " (create one with `oe new <dir>`)";
            return false;
        }
        std::string parseError;
        Json project = Json::parse(text, &parseError);
        if (!parseError.empty()) {
            if (error) *error = projectFile + ": " + parseError;
            return false;
        }
        projectDir_ = ParentPath(projectFile);
        projectName_ = project["name"].asString("Untitled");
        std::string start = project["startScene"].asString("");
        if (start.empty()) {
            NewScene("Main");
            return true;
        }
        return LoadScene(JoinPath(projectDir_, start), error);
    }

    // A bare scene file: use the nearest enclosing project if there is one.
    std::string dir = ParentPath(path);
    projectDir_ = dir;
    for (std::string d = dir; !d.empty(); d = ParentPath(d)) {
        if (FileExists(JoinPath(d, "project.json"))) {
            projectDir_ = d;
            std::string text;
            ReadTextFile(JoinPath(d, "project.json"), text);
            projectName_ = Json::parse(text)["name"].asString("Untitled");
            break;
        }
        if (ParentPath(d) == d) break;
    }
    return LoadScene(path, error);
}

bool Engine::LoadScene(const std::string& path, std::string* error) {
    std::string text;
    if (!ReadTextFile(path, text)) {
        if (error) *error = "cannot read scene file " + path;
        return false;
    }
    std::string parseError;
    Json json = Json::parse(text, &parseError);
    if (!parseError.empty()) {
        if (error) *error = path + ": " + parseError;
        return false;
    }
    if (!scene_.FromJson(json, error)) {
        if (error) *error = path + ": " + *error;
        return false;
    }
    playing_ = false;
    playSnapshot_.reset();
    ResetRuntime();
    frame_ = 0;
    simTime_ = 0;
    SetSceneLocation(path);
    ResetHistory();
    dirty_ = false;
    Touch();
    OE_LOG_INFO("scene", "loaded %s (%zu entities)", path.c_str(), scene_.Entities().size());
    return true;
}

bool Engine::SaveScene(const std::string& path, std::string* error) {
    const Json& json = playSnapshot_ ? *playSnapshot_ : scene_.ToJson();
    if (!WriteTextFile(path, json.dump(2) + "\n")) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    SetSceneLocation(path);
    dirty_ = false;
    OE_LOG_INFO("scene", "saved %s", path.c_str());
    return true;
}

void Engine::NewScene(const std::string& name) {
    scene_.Clear();
    scene_.name = name;
    playing_ = false;
    playSnapshot_.reset();
    ResetRuntime();
    frame_ = 0;
    simTime_ = 0;
    scenePath_.clear();
    dirty_ = true;
    Touch();
}

void Engine::SetSceneLocation(const std::string& path) { scenePath_ = AbsolutePath(path); }

std::string Engine::ResolvePath(const std::string& path) const {
    if (path.empty()) throw ApiError("invalid_path", "path is empty");
    std::string full = AbsolutePath(path.size() > 1 && (path[1] == ':' || path[0] == '/') ? path : JoinPath(projectDir_, path));
    std::string root = Lower(AbsolutePath(projectDir_));
    std::string lower = Lower(full);
    if (lower.compare(0, root.size(), root) != 0 || (lower.size() > root.size() && lower[root.size()] != '/' && root.back() != '/')) {
        throw ApiError("path_outside_project", "path '" + path + "' resolves outside the project directory " + projectDir_,
                       "Use a path relative to the project directory, e.g. \"scenes/level1.scene.json\".");
    }
    return full;
}

Json Engine::ReadProjectJson(const std::string& path) const {
    std::string full = ResolvePath(path);
    std::string text;
    if (!ReadTextFile(full, text)) throw ApiError("not_found", "cannot read '" + path + "'", "Paths are relative to the project directory.");
    std::string err;
    Json json = Json::parse(text, &err);
    if (!err.empty()) throw ApiError("invalid_json", path + ": " + err);
    return json;
}

EntityId Engine::InstantiatePrefabFile(const std::string& path, EntityId parent) {
    Json prefab = ReadProjectJson(path);
    std::string err;
    EntityId root = InstantiatePrefab(scene_, prefab, path, parent, &err);
    if (root == kNullEntity) throw ApiError("invalid_prefab", err);
    return root;
}

// ----- Simulation ----------------------------------------------------------

void Engine::BeginSessionIfNeeded() {
    if (playSnapshot_) return;
    playSnapshot_ = std::make_unique<Json>(scene_.ToJson());
    frame_ = 0;
    simTime_ = 0;
    ResetRuntime();  // fresh Lua state and physics world for every session
    gameData_ = Json::MakeObject();
    runtimeScene_ = scenePath_.empty() ? "" : RelativePath(scenePath_, projectDir_);
    pendingScene_.clear();
}

void Engine::EnableAudioOutput() {
    if (audio_->HasDevice()) return;
    std::unique_ptr<AudioDevice> device = CreateAudioDevice(kAudioSampleRate);
    if (device) {
        OE_LOG_INFO("audio", "output: %s", device->Name());
        audio_->AttachDevice(std::move(device));
    } else {
        OE_LOG_WARN("audio", "no audio output device; sounds are mixed but not played");
    }
}

void Engine::ResetRuntime() {
    debugLines_.clear();
    scripts_->Reset();
    physics_->Reset();
    audio_->Reset();
    pendingScene_.clear();
}

void Engine::ApplySceneChange() {
    std::string path = pendingScene_;
    pendingScene_.clear();
    std::string err;
    try {
        Json json = ReadProjectJson(path);
        if (!scene_.FromJson(json, &err)) throw ApiError("invalid_scene", path + ": " + err);
    } catch (const ApiError& e) {
        scripts_->RecordError("", kNullEntity, std::string("game.loadScene failed: ") + e.what());
        return;
    }
    physics_->Reset();
    scripts_->ResetInstances();
    audio_->OnSceneChanged();
    runtimeScene_ = path;
    Touch();
    OE_LOG_INFO("game", "scene changed to %s at frame %llu", path.c_str(), static_cast<unsigned long long>(frame_));
}

void Engine::Play() {
    BeginSessionIfNeeded();
    playing_ = true;
    accumulator_ = 0;
    OE_LOG_INFO("sim", "play");
}

void Engine::Pause() {
    playing_ = false;
    OE_LOG_INFO("sim", "pause at frame %llu", static_cast<unsigned long long>(frame_));
}

void Engine::Stop() {
    playing_ = false;
    if (playSnapshot_) {
        std::string err;
        scene_.FromJson(*playSnapshot_, &err);
        playSnapshot_.reset();
    }
    ResetRuntime();
    input_.down.clear();
    input_.pressedThisFrame.clear();
    gameData_ = Json::MakeObject();
    frame_ = 0;
    simTime_ = 0;
    Touch();
    OE_LOG_INFO("sim", "stop (scene restored)");
}

void Engine::Step(int frames) {
    BeginSessionIfNeeded();
    scripts_->PollHotReload();  // pick up script / asset files edited since the last step
    assets_->PollChanges();
    for (int i = 0; i < frames; ++i) SimulateFrame();
    if (frames > 0) Touch();
}

void Engine::Tick(double realDt) {
    if (!playing_) return;
    hotReloadTimer_ += realDt;
    if (hotReloadTimer_ >= 0.5) {
        hotReloadTimer_ = 0.0;
        scripts_->PollHotReload();
        assets_->PollChanges();
    }
    accumulator_ += std::min(realDt, 0.25);
    int steps = 0;
    while (accumulator_ >= kFixedDt && steps < 8) {
        SimulateFrame();
        accumulator_ -= kFixedDt;
        ++steps;
    }
    if (steps > 0) Touch();
}

void Engine::AddDebugLine(const Vec3& a, const Vec3& b, const Color& color, float seconds) {
    if (debugLines_.size() >= 20000) return;
    debugLines_.push_back({{a, b, color}, seconds < 0 ? -1.0 : simTime_ + static_cast<double>(seconds)});
}

void Engine::AppendDebugLines(std::vector<DebugLine>& out) const {
    for (const TimedLine& t : debugLines_) out.push_back(t.line);
}

void Engine::SimulateFrame() {
    // Expire debug lines whose time is up (0-second lines live for one frame).
    debugLines_.erase(std::remove_if(debugLines_.begin(), debugLines_.end(), [&](const TimedLine& t) { return t.expires >= 0 && t.expires <= simTime_; }),
                      debugLines_.end());
    // Scripts run first so they see this frame's edge-triggered input.
    const float dt = static_cast<float>(kFixedDt);
    scripts_->Update(dt);
    EntityId clicked = kNullEntity;
    if (input_.pressedThisFrame.count("MouseLeft")) {
        clicked = HitTestButton(scene_, input_.mouseX * static_cast<float>(input_.viewWidth), input_.mouseY * static_cast<float>(input_.viewHeight),
                                input_.viewWidth, input_.viewHeight);
    }
    UpdateSystems(scene_, input_, dt);
    std::vector<PhysicsEvent> events = physics_->Step(scene_, dt);
    UpdateLateSystems(scene_, dt);
    scripts_->DispatchPhysicsEvents(events);
    if (clicked != kNullEntity && scene_.Exists(clicked)) {
        OE_LOG_INFO("ui", "button '%s' clicked", scene_.Record(clicked)->name.c_str());
        scripts_->Notify(clicked, "onClick");
    }
    if (!pendingScene_.empty()) ApplySceneChange();
    audio_->Update(scene_);
    audio_->Render();
    ++frame_;
    simTime_ += kFixedDt;
}

RenderStats Engine::RenderGameView(RenderTarget& target) {
    RenderView view;
    MakeSceneView(scene_, static_cast<float>(target.width) / static_cast<float>(target.height), view);
    AppendDebugLines(view.lines);
    return renderer_->Render(scene_, view, target);
}

bool Engine::EnableGpu(Window* window, std::string* error) {
    if (gpu_) return true;
    gpuDevice_ = CreateGpuDevice(window, error);
    if (!gpuDevice_) return false;
    gpu_ = std::make_unique<GpuRenderer>(*gpuDevice_, assets_.get());
    OE_LOG_INFO("render", "GPU renderer ready: %s", gpu_->Name());
    return true;
}

IRenderer& Engine::DisplayRenderer() {
    if (gpu_) return *gpu_;
    return *renderer_;
}

bool Engine::PresentGameView(float renderScale) {
    if (!gpu_) return false;
    int w = 0, h = 0;
    gpu_->WindowSize(&w, &h);
    if (w <= 0 || h <= 0) return false;
    RenderView view;
    MakeSceneView(scene_, static_cast<float>(w) / static_cast<float>(h), view);
    AppendDebugLines(view.lines);
    return gpu_->RenderToWindow(scene_, view, renderScale);
}

// ----- History -------------------------------------------------------------

void Engine::ResetHistory() {
    undo_.clear();
    redo_.clear();
    lastMergeKey_.clear();
}

bool Engine::Undo() {
    if (undo_.empty() || playSnapshot_) return false;
    lastMergeKey_.clear();
    redo_.push_back(scene_.ToJson());
    std::string err;
    scene_.FromJson(undo_.back(), &err);
    undo_.pop_back();
    dirty_ = true;
    Touch();
    return true;
}

bool Engine::Redo() {
    if (redo_.empty() || playSnapshot_) return false;
    lastMergeKey_.clear();
    undo_.push_back(scene_.ToJson());
    std::string err;
    scene_.FromJson(redo_.back(), &err);
    redo_.pop_back();
    dirty_ = true;
    Touch();
    return true;
}

// ----- API -----------------------------------------------------------------

Json Engine::Call(const std::string& name, const Json& rawArgs) {
    Json response = Json::MakeObject();
    const Command* cmd = commands_.Find(name);
    try {
        if (!cmd) {
            // Suggest commands sharing the same prefix to help callers recover.
            std::string prefix = name.substr(0, name.find('.'));
            std::string similar;
            for (const Command& c : commands_.All()) {
                if (c.name.compare(0, prefix.size(), prefix) == 0) similar += (similar.empty() ? "" : ", ") + c.name;
            }
            throw ApiError("unknown_command", "unknown command '" + name + "'",
                           similar.empty() ? "Call api.list to see all commands." : "Did you mean one of: " + similar + "?");
        }
        Json args = rawArgs.isNull() ? Json::MakeObject() : rawArgs;
        ValidateArgs(*cmd, args);
        const bool record = cmd->mutates && !playSnapshot_;
        Json before = record ? scene_.ToJson() : Json();
        Json result = cmd->run(*this, args);
        if (record) {
            // Consecutive calls with the same non-empty merge key (e.g. an
            // editor drag) collapse into one undo step.
            const Json* merge = args.find("merge");
            std::string mergeKey = merge && merge->isString() ? merge->asString() : std::string();
            if (mergeKey.empty() || mergeKey != lastMergeKey_ || undo_.empty()) {
                undo_.push_back(std::move(before));
                if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
            }
            lastMergeKey_ = mergeKey;
            redo_.clear();
            dirty_ = true;
        }
        if (cmd->mutates) Touch();
        response["ok"] = true;
        response["result"] = std::move(result);
    } catch (const ApiError& e) {
        response["ok"] = false;
        Json err = Json::MakeObject();
        err["code"] = e.code;
        err["message"] = e.what();
        if (!e.hint.empty()) err["hint"] = e.hint;
        response["error"] = err;
        OE_LOG_WARN("api", "%s failed: %s", name.c_str(), e.what());
    } catch (const std::exception& e) {
        response["ok"] = false;
        Json err = Json::MakeObject();
        err["code"] = "internal_error";
        err["message"] = e.what();
        response["error"] = err;
        OE_LOG_ERROR("api", "%s threw: %s", name.c_str(), e.what());
    }
    return response;
}

std::future<Json> Engine::PostCall(const std::string& name, const Json& args) {
    auto promise = std::make_shared<std::promise<Json>>();
    std::future<Json> future = promise->get_future();
    std::lock_guard<std::mutex> lock(jobsMutex_);
    jobs_.push_back([this, name, args, promise] { promise->set_value(Call(name, args)); });
    return future;
}

std::future<std::vector<uint8_t>> Engine::PostJob(std::function<std::vector<uint8_t>()> job) {
    auto promise = std::make_shared<std::promise<std::vector<uint8_t>>>();
    auto future = promise->get_future();
    std::lock_guard<std::mutex> lock(jobsMutex_);
    jobs_.push_back([job = std::move(job), promise] {
        try {
            promise->set_value(job());
        } catch (...) {
            promise->set_value({});
        }
    });
    return future;
}

void Engine::RunPostedJobs() {
    std::deque<std::function<void()>> pending;
    {
        std::lock_guard<std::mutex> lock(jobsMutex_);
        pending.swap(jobs_);
    }
    for (auto& job : pending) job();
}

}  // namespace oe
