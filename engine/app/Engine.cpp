#include "app/Engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "core/FileSystem.h"
#include "core/Log.h"
#include "scene/Prefab.h"
#include "assets/Assets.h"
#include "audio/AudioSystem.h"
#include "scene/Vfx.h"
#include "physics/PhysicsWorld.h"
#include "platform/Platform.h"
#include "render/GpuDevice.h"
#include "render/GpuRenderer.h"
#include "render/UI.h"
#include "scene/Components.h"
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
    physics_->SetTilesets(assets_->Tilesets());
    audio_ = std::make_unique<AudioSystem>(*this);
    RegisterBuiltinCommands(commands_);
    projectDir_ = AbsolutePath(".");
}

Engine::~Engine() { EnableNetworkWatchdog(false); }

namespace {
int64_t SteadyMicroseconds() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

void Engine::EnableNetworkWatchdog(bool on) {
#ifndef __EMSCRIPTEN__
    if (on == watchdog_.joinable()) return;
    if (!on) {
        watchdogStop_ = true;
        watchdog_.join();
        return;
    }
    watchdogStop_ = false;
    lastNetworkTick_ = SteadyMicroseconds();
    watchdog_ = std::thread([this] {
        int64_t next = 0;  // when the next I/O tick is due: 60 Hz paced by the clock, not by sleep granularity
        while (!watchdogStop_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            const int64_t now = SteadyMicroseconds();
            if (now - lastNetworkTick_ < 100000) { next = 0; continue; }
            if (next == 0) next = now;
            if (now < next) continue;
            std::unique_lock<std::recursive_mutex> lock(engineMutex_, std::try_to_lock);
            if (!lock.owns_lock() || !network_ || networkShared_) continue;
            for (int i = 0; i < 4 && now >= next; ++i, next += 16667) {
                network_->Advance(networkFrame_++);  // I/O only: drained and simulated when the loop is back
                ++watchdogTicks_;
            }
            if (now >= next) next = now + 16667;  // far behind: no burst
        }
    });
#else
    (void)on;  // the browser drives the loop; no threads in this build
#endif
}

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
        SessionConfig config;
        if (!SessionConfig::Parse(project, config, error)) return false;
        ResetAuthority(); sync_.reset(); checkpoints_.clear(); journal_.reset(); snapshots_.clear(); playerInputs_.clear(); frameInputs_.clear(); deviceInput_ = InputState{}; syncHashes_.clear();
        ResetLocalPeers(); network_.reset(); networkWorld_ = 0; networkShared_ = false; networkFrame_ = 0; networkConfig_ = config;
        projectDir_ = ParentPath(projectFile);
        projectName_ = project["name"].asString("Untitled");
        // Headless sessions have no window: the game view starts at the project's window size,
        // so scripts that adapt to the screen (input.viewSize) see the shape the game ships with.
        const int viewW = project["window"]["width"].asInt(0), viewH = project["window"]["height"].asInt(0);
        if (viewW > 0 && viewH > 0) {
            input_.viewWidth = viewW;
            input_.viewHeight = viewH;
        }
        std::string start = project["startScene"].asString("");
        if (start.empty()) {
            NewScene("Main");
            return true;
        }
        return LoadScene(JoinPath(projectDir_, start), error);
    }

    // A bare scene file: use the nearest enclosing project if there is one.
    ResetAuthority(); sync_.reset(); checkpoints_.clear(); journal_.reset(); snapshots_.clear(); playerInputs_.clear(); frameInputs_.clear(); deviceInput_ = InputState{}; syncHashes_.clear();
    ResetLocalPeers(); network_.reset(); networkWorld_ = 0; networkShared_ = false; networkFrame_ = 0; networkConfig_ = SessionConfig{};
    std::string dir = ParentPath(path);
    projectDir_ = dir;
    for (std::string d = dir; !d.empty(); d = ParentPath(d)) {
        if (FileExists(JoinPath(d, "project.json"))) {
            projectDir_ = d;
            std::string text;
            ReadTextFile(JoinPath(d, "project.json"), text);
            Json project = Json::parse(text);
            if (!SessionConfig::Parse(project, networkConfig_, error)) return false;
            projectName_ = project["name"].asString("Untitled");
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
    ResetAuthority(); sync_.reset(); checkpoints_.clear(); journal_.reset(); snapshots_.clear(); playerInputs_.clear(); frameInputs_.clear(); deviceInput_ = InputState{}; syncHashes_.clear();
    ResetLocalPeers(); network_.reset(); networkWorld_ = 0; networkShared_ = false; networkFrame_ = 0;
    playSnapshot_.reset();
    loadedScene_.reset();
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
    Touch();  // Remote saves must refresh editor path and dirty-state caches.
    OE_LOG_INFO("scene", "saved %s", path.c_str());
    return true;
}

void Engine::NewScene(const std::string& name) {
    ResetAuthority(); sync_.reset(); checkpoints_.clear(); journal_.reset(); snapshots_.clear(); playerInputs_.clear(); frameInputs_.clear(); deviceInput_ = InputState{}; syncHashes_.clear();
    ResetLocalPeers(); network_.reset(); networkWorld_ = 0; networkShared_ = false; networkFrame_ = 0;
    scene_.Clear();
    scene_.name = name;
    playing_ = false;
    playSnapshot_.reset();
    loadedScene_.reset();
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
    gamePaused_ = false;
    runtimeScene_ = scenePath_.empty() ? "" : RelativePath(scenePath_, projectDir_);
    pendingScene_.clear();
    loadedScene_.reset();
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
    networkButtonKeys_.clear();
    debugLines_.clear();
    links_.Clear();  // their ids lived in the Lua state that goes away
    scripts_->Reset();
    physics_->Reset();
    audio_->Reset();
    pendingScene_.clear();
    audio_->SetOutputMode(false, false); saves_.DeferFlush(false); saves_.FreezeReads(false);
}

void Engine::ApplySceneChange() {
    gamePaused_ = false;
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
    loadedScene_ = std::make_unique<Json>(scene_.ToJson());
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
    ResetAuthority(); sync_.reset(); checkpoints_.clear(); journal_.reset(); snapshots_.clear(); playerInputs_.clear(); frameInputs_.clear(); deviceInput_ = InputState{}; syncHashes_.clear();
    ResetLocalPeers(); network_.reset(); networkWorld_ = 0; networkShared_ = false; networkFrame_ = 0;
    playing_ = false;
    loadedScene_.reset();
    if (playSnapshot_) {
        std::string err;
        scene_.FromJson(*playSnapshot_, &err);
        playSnapshot_.reset();
    }
    ResetRuntime();
    uiHovered_ = uiPressed_ = uiFocus_ = kNullEntity;
    heldButtons_.clear();
    heldKeys_.clear();
    input_.touches.clear();
    input_.down.clear();
    input_.axes.clear();
    input_.pressedThisFrame.clear();
    input_.text.clear();
    uiRepeat_ = 0;
    uiRepeatKey_.clear();
    uiListPress_ = kNullEntity;
    input_.mouseDX = input_.mouseDY = 0.0f;
    input_.mouseLocked = false;
    input_.lookX = input_.lookY = 0.0f;
    gameData_ = Json::MakeObject();
    gamePaused_ = false;
    quitRequested_ = false;
    frame_ = 0;
    simTime_ = 0;
    Touch();
    OE_LOG_INFO("sim", "stop (scene restored)");
}

std::string Engine::ScriptReloadBlockReason() const {
    if (journal_) return "recording";
    if ((sync_ && sync_->Active()) || (authority_ && authority_->Active())) return "match_active";
    return {};
}

void Engine::Step(int frames) {
    std::lock_guard<std::recursive_mutex> lock(engineMutex_);
    BeginSessionIfNeeded();
    if (!journal_ && (!sync_ || !sync_->Active()) && (!authority_ || !authority_->Active())) { scripts_->PollHotReload(); assets_->PollChanges(); }
    renderInterpolate_ = false;  // tools step exactly: draw the simulated state
    for (int i = 0; i < frames; ++i) SimulateFrame();
    if (frames > 0) Touch();
}

void Engine::Tick(double realDt) {
    std::lock_guard<std::recursive_mutex> lock(engineMutex_);
    if (!playing_) return;
    hotReloadTimer_ += realDt;
    if (hotReloadTimer_ >= 0.5 && !journal_ && (!sync_ || !sync_->Active()) && (!authority_ || !authority_->Active())) {
        hotReloadTimer_ = 0.0;
        scripts_->PollHotReload();
        assets_->PollChanges();
    }
    accumulator_ += std::min(realDt, 0.25);
    int steps = 0;
    while (accumulator_ >= kFixedDt && steps < 8) {
        // The state before the step, for render interpolation (only the last step's is kept).
        renderPrevious_.clear();
        for (const auto& entry : scene_.Pool<Transform>()) renderPrevious_.emplace_back(entry.first, entry.second);
        SimulateFrame();
        accumulator_ -= kFixedDt;
        ++steps;
    }
    if (steps > 0) { Touch(); renderInterpolate_ = true; }
    renderAlpha_ = std::clamp(accumulator_ / kFixedDt, 0.0, 1.0);
}

void Engine::BeginInterpolatedRender() {
    renderSaved_.clear();
    if (!renderInterpolate_ || !playing_ || renderAlpha_ >= 1.0) return;
    const float t = static_cast<float>(renderAlpha_);
    auto turn = [](float from, float to) { float d = std::fmod(to - from + 540.0f, 360.0f) - 180.0f; return d; };
    for (const auto& previous : renderPrevious_) {
        Transform* now = scene_.Get<Transform>(previous.first);
        if (!now) continue;
        const Transform& was = previous.second;
        const Vec3 move = now->position - was.position;
        if (move.x * move.x + move.y * move.y + move.z * move.z > 9.0f) continue;  // a teleport: no streak
        renderSaved_.emplace_back(previous.first, *now);
        now->position = was.position + move * t;
        now->rotation = {was.rotation.x + turn(was.rotation.x, now->rotation.x) * t,
                         was.rotation.y + turn(was.rotation.y, now->rotation.y) * t,
                         was.rotation.z + turn(was.rotation.z, now->rotation.z) * t};
        now->scale = was.scale + (now->scale - was.scale) * t;
    }
}

void Engine::EndInterpolatedRender() {
    for (const auto& saved : renderSaved_)
        if (Transform* now = scene_.Get<Transform>(saved.first)) *now = saved.second;
    renderSaved_.clear();
}

void Engine::AddDebugLine(const Vec3& a, const Vec3& b, const Color& color, float seconds) {
    if (debugLines_.size() >= 20000) return;
    debugLines_.push_back({{a, b, color}, seconds < 0 ? -1.0 : simTime_ + static_cast<double>(seconds)});
}

void Engine::AppendDebugLines(std::vector<DebugLine>& out) const {
    for (const TimedLine& t : debugLines_) out.push_back(t.line);
}

void Engine::SimulateFrame() {
    if (!localPeers_.empty()) AdvanceLocalPeers();
    if (network_) {
        uint32_t seed = network_->Seed();
        if (networkShared_) ++networkFrame_;  // world 0's engine advanced the session this tick
        else network_->Advance(networkFrame_++);
        lastNetworkTick_ = SteadyMicroseconds();  // the watchdog stays out while the loop advances the session
        if (seed != network_->Seed()) scripts_->SetNetworkSeed(network_->Seed());
        std::string scene;
        if (network_->TakeWorldChange(scene)) EnterWorld(scene);
    }
    if (dedicated_) input_ = deviceInput_ = InputState{};
    if (links_.Count()) links_.Pump();
    AutoStartNetwork();
    if (!PollAuthority() || !PollSync()) return;
    if (autoStart_ && (!authority_ || !authority_->Running()) && (!sync_ || !sync_->Running())) return;
    CaptureCheckpoint();
    if (journal_ && sync_ && sync_->Running()) journal_->frames.push_back({input_, frameInputs_});
    SimulateWorld();
    AuthorityApplied();
    if (authority_ && authority_->Running()) {
        // The frame consumed the relative mouse motion and may have changed the lock (input.lockMouse).
        bool locked = input_.mouseLocked;
        input_ = deviceInput_; input_.mouseLocked = locked; input_.mouseDX = input_.mouseDY = 0.0f;
    } else if (sync_ && sync_->Running()) input_ = deviceInput_;
    // A lockstep/rollback client that fell behind consumes already confirmed frames faster. Game
    // frames stay identical on every peer; only the number simulated per I/O tick differs.
    for (int extra = 0; extra < FrameSync::kCatchUpFrames && sync_ && sync_->Lagging(); ++extra) {
        if (!PollSync()) break;
        CaptureCheckpoint();
        if (journal_) journal_->frames.push_back({input_, frameInputs_});
        SimulateWorld();
        input_ = deviceInput_;
    }
}

void Engine::SimulateWorld() {
    inWorld_ = true;
    // Expire debug lines whose time is up (0-second lines live for one frame).
    debugLines_.erase(std::remove_if(debugLines_.begin(), debugLines_.end(), [&](const TimedLine& t) { return t.expires >= 0 && t.expires <= simTime_; }),
                      debugLines_.end());
    // Scripts run first so they see this frame's edge-triggered input.
    // A paused game still runs scripts and UI, with a zero step, so dialogs work while the world waits.
    const bool paused = gamePaused_;
    const float dt = paused ? 0.0f : static_cast<float>(kFixedDt);
    if ((!sync_ || !sync_->Active()) && (!authority_ || !authority_->Active())) UpdateButtonKeys();
    {
        // While a text field has the focus the keyboard types: scripts see only the mouse and the gamepad.
        const bool typing = TextInputActive();
        std::set<std::string> down, pressed;
        if (typing) {
            down = input_.down;
            pressed = input_.pressedThisFrame;
            auto keyboard = [](const std::string& key) { return key.compare(0, 5, "Mouse") != 0 && key.compare(0, 7, "Gamepad") != 0; };
            for (auto* keys : {&input_.down, &input_.pressedThisFrame}) {
                for (auto it = keys->begin(); it != keys->end();) it = keyboard(*it) ? keys->erase(it) : std::next(it);
            }
        }
        scripts_->Update(dt);
        if (typing) {
            input_.down = std::move(down);
            input_.pressedThisFrame = std::move(pressed);
        }
    }
    if (network_ && authority_ && authority_->Running() && !network_->IsHost()) {
        if (replaying_) {
            auto it = authorityEvents_.find(frame_); if (it != authorityEvents_.end()) scripts_->DispatchNetwork(it->second);
        } else {
            auto events = network_->DrainEvents(networkWorld_);
            if (!events.empty()) {
                size_t count = events.size(); for (const auto& batch : authorityEvents_) count += batch.second.size();
                if (count > 256) throw ApiError("network_limit", "prediction RPC event window is full");
                authorityEvents_[frame_] = events; scripts_->DispatchNetwork(events);
            }
        }
    } else if (network_ && !replaying_ && (!sync_ || !sync_->Active())) scripts_->DispatchNetwork(network_->DrainEvents(networkWorld_));
    std::vector<UIEvent> uiEvents = UpdateUI();
    UpdateSystems(scene_, input_, dt, assets_.get());
    std::vector<PhysicsEvent> events;
    if (!paused) events = authority_ && authority_->Running() && !network_->IsHost() ? StepAuthorityPhysics(dt) : physics_->Step(scene_, dt);
    if (!paused) UpdateLateSystems(scene_, dt);
    scripts_->DispatchPhysicsEvents(events);
    for (const UIEvent& ev : uiEvents) {
        if (!scene_.Exists(ev.id)) continue;
        if (std::strcmp(ev.method, "onClick") == 0) OE_LOG_INFO("ui", "button '%s' clicked", scene_.Record(ev.id)->name.c_str());
        if (ev.hasText) scripts_->NotifyText(ev.id, ev.method, ev.text);
        else if (ev.isFlag) scripts_->NotifyFlag(ev.id, ev.method, ev.value != 0.0f);
        else if (ev.hasValue) scripts_->Notify(ev.id, ev.method, ev.value);
        else scripts_->Notify(ev.id, ev.method);
    }
    if (!pendingScene_.empty()) ApplySceneChange();
    // UI animation state last: an element a script or a click showed this frame starts its entrance
    // before the frame is drawn (never one frame at its final look). Real frames: also while paused.
    UpdateUIMotion(scene_, static_cast<float>(kFixedDt));
    audio_->Update(scene_);
    audio_->Render();
    ++frame_;
    if (!paused) simTime_ += kFixedDt;  // timers and shader time wait with the world
    inWorld_ = false;
    if (sync_ && sync_->Running() && !replaying_) {
        sync_->Applied(frameInputs_);
        if (frame_ % sync_->Config().hashInterval == 0) syncHashes_[frame_] = SyncWorld();
        audio_->Confirm(std::min(frame_, sync_->Confirmed()));
        outputConfirmed_ = std::min(frame_, sync_->Confirmed());
    }
}

std::set<EntityId> Engine::HeldInputButtons(const InputState& input, bool includeClick) {
    std::set<EntityId> held;
    bool any = false;
    for (auto& kv : scene_.Pool<UIButton>()) any = any || !kv.second.key.empty();
    const int w = input.viewWidth, h = input.viewHeight;
    if (any && w > 0 && h > 0) {
        std::vector<std::pair<float, float>> pointers;
        if (!input.mouseLocked && (input.IsDown("MouseLeft") || (includeClick && input.pressedThisFrame.count("MouseLeft")))) pointers.push_back({input.mouseX, input.mouseY});
        for (const InputState::Touch& t : input.touches) pointers.push_back({t.x, t.y});
        std::vector<UIRect> rects;
        if (!pointers.empty()) rects = LayoutUI(scene_, w, h, assets_.get());
        for (const auto& p : pointers) {
            const UIRect* hit = HitTestUI(rects, p.first * static_cast<float>(w), p.second * static_cast<float>(h));
            const UIButton* b = hit && hit->kind == UIRect::Kind::Button ? scene_.Get<UIButton>(hit->entity) : nullptr;
            if (b && b->interactable && !b->key.empty()) held.insert(hit->entity);
        }
    }
    return held;
}

InputState Engine::SampleNetworkInput() {
    // Only declared actions enter the wire; local pointer coordinates never enter the world.
    InputState sample = input_;
    if (TextInputActive()) {
        // The keyboard types into the focused UIInput: none of it is a game action (chat while standing still).
        auto keyboard = [](const std::string& key) { return key.compare(0, 5, "Mouse") != 0 && key.compare(0, 7, "Gamepad") != 0; };
        for (auto* held : {&sample.down, &sample.pressedThisFrame}) {
            for (auto it = held->begin(); it != held->end();) it = keyboard(*it) ? held->erase(it) : std::next(it);
        }
    }
    std::set<std::string> keys;
    for (EntityId id : HeldInputButtons(input_, true)) keys.insert(scene_.Get<UIButton>(id)->key);
    for (const auto& key : keys) {
        if (!networkButtonKeys_.count(key) && !sample.IsDown(key)) sample.pressedThisFrame.insert(key);
        sample.down.insert(key);
    }
    networkButtonKeys_ = std::move(keys);
    UpdateJoysticks(sample);
    if (networkConfig_.sync.look[0]) sample.axes["RightX"] = sample.lookX;
    if (networkConfig_.sync.look[1]) sample.axes["RightY"] = sample.lookY;
    return sample;
}

void Engine::MergeLocalInput(const InputState& device) {
    const auto& declared = networkConfig_.sync.keys;
    auto local = [&](const std::string& key) { return std::find(declared.begin(), declared.end(), key) == declared.end(); };
    for (const std::string& key : device.down) if (local(key)) input_.down.insert(key);
    for (const std::string& key : device.pressedThisFrame) if (local(key)) input_.pressedThisFrame.insert(key);
    input_.mouseX = device.mouseX; input_.mouseY = device.mouseY;
    input_.viewWidth = device.viewWidth; input_.viewHeight = device.viewHeight;
    input_.wheel = device.wheel; input_.touches = device.touches; input_.text = device.text; input_.composing = device.composing;
}

void Engine::SetLook(float x, float y) {
    // Replayed frames must not move the device: they only re-run what the stream already holds.
    if (replaying_) return;
    auto grid = [](float v) { return std::round(Clamp(std::isfinite(v) ? v : 0.0f, -1.0f, 1.0f) * 32767.0f) / 32767.0f; };
    // In a match input_ is the decoded sample; the device copy is what the next frame samples.
    InputState& device = (sync_ && sync_->Running()) || (authority_ && authority_->Running()) ? deviceInput_ : input_;
    device.lookX = grid(x); device.lookY = grid(y);
}

void Engine::UpdateButtonKeys() {
    std::set<EntityId> held = HeldInputButtons(input_, false);
    std::set<std::string> keys;
    for (EntityId id : held) keys.insert(scene_.Get<UIButton>(id)->key);
    for (const std::string& k : keys) {
        if (heldKeys_.count(k)) continue;
        if (!input_.IsDown(k)) input_.pressedThisFrame.insert(k);
        input_.down.insert(k);
    }
    for (const std::string& k : heldKeys_) {
        if (!keys.count(k)) input_.down.erase(k);
    }
    heldKeys_ = std::move(keys);
    heldButtons_ = std::move(held);
    UpdateJoysticks(input_);
}

void Engine::UpdateJoysticks(InputState& input) {
    if (scene_.Pool<UIJoystick>().empty()) return;
    const int w = input.viewWidth, h = input.viewHeight;
    if (w <= 0 || h <= 0) return;
    // Fingers; the mouse only when there are none (platforms report the first finger as the mouse too).
    struct Pointer {
        int id;
        float x, y;
        bool began;
    };
    std::vector<Pointer> pointers;
    for (const InputState::Touch& t : input.touches) pointers.push_back({t.id, t.x * static_cast<float>(w), t.y * static_cast<float>(h), t.began});
    const bool click = input.pressedThisFrame.count("MouseLeft") > 0;
    if (pointers.empty() && !input.mouseLocked && (input.IsDown("MouseLeft") || click)) {
        pointers.push_back({UIJoystick::kMousePointer, input.mouseX * static_cast<float>(w), input.mouseY * static_cast<float>(h), click});
    }
    const std::vector<UIRect> rects = LayoutUI(scene_, w, h, assets_.get());
    for (auto& kv : scene_.Pool<UIJoystick>()) {
        UIJoystick& stick = kv.second;
        const UIRect* rect = nullptr;
        for (const UIRect& r : rects) {
            if (r.entity == kv.first && r.kind == UIRect::Kind::Joystick) rect = &r;
        }
        const Pointer* holder = nullptr;
        for (const Pointer& p : pointers) {
            if (rect && stick.held == p.id) holder = &p;
        }
        for (const Pointer& p : pointers) {  // a pointer that just went down on the stick takes it
            if (holder || !rect || !p.began) continue;
            const UIRect* hit = HitTestUI(rects, p.x, p.y);
            if (hit && hit->entity == kv.first && hit->kind == UIRect::Kind::Joystick) holder = &p;
        }
        const bool was = stick.held != UIJoystick::kNoPointer;
        if (holder) {
            const float radius = std::max(1.0f, std::min(rect->w, rect->h) * 0.5f);
            float vx = (holder->x - (rect->x + rect->w * 0.5f)) / radius, vy = -(holder->y - (rect->y + rect->h * 0.5f)) / radius;
            const float length = std::sqrt(vx * vx + vy * vy);
            if (length > 1.0f) vx /= length, vy /= length;
            stick.held = holder->id;
            stick.valueX = vx;
            stick.valueY = vy;
        } else {
            stick.held = UIJoystick::kNoPointer;
            stick.valueX = stick.valueY = 0.0f;
        }
        if (holder || was) {
            input.SetAxis(stick.axisX, stick.valueX);
            input.SetAxis(stick.axisY, stick.valueY);
        }
    }
}

std::vector<Engine::UIEvent> Engine::UpdateUI() {
    std::vector<UIEvent> events;
    const int w = input_.viewWidth, h = input_.viewHeight;
    const float px = input_.mouseX * static_cast<float>(w), py = input_.mouseY * static_cast<float>(h);
    std::vector<UIRect> rects;
    const UIRect* hit = nullptr;
    if (w > 0 && h > 0 && !input_.mouseLocked) {
        rects = LayoutUI(scene_, w, h, assets_.get());
        hit = HitTestUI(rects, px, py);
    }
    EntityId hover = hit ? hit->entity : kNullEntity;
    if (hover != uiHovered_) {
        if (uiHovered_ != kNullEntity) events.push_back({uiHovered_, "onPointerExit", false, 0.0f});
        if (hover != kNullEntity) events.push_back({hover, "onPointerEnter", false, 0.0f});
        uiHovered_ = hover;
    }
    // Scroll views: the wheel scrolls the topmost one under the pointer, a press on its empty area drags
    // the content, and its bar can be grabbed (or its track clicked) to move straight to a position.
    bool barPress = false;
    {
        const float scale = std::max(1e-3f, UIScale(scene_, w, h));
        const UIRect* over = nullptr;
        for (const UIRect& r : rects) {
            if (!r.scroll || px < r.x || px >= r.x + r.w || py < r.y || py >= r.y + r.h) continue;
            if (r.clipped && !(px >= r.clip[0] && px < r.clip[2] && py >= r.clip[1] && py < r.clip[3])) continue;
            over = &r;
        }
        auto thumbOf = [&](const UIRect& r, const UIScroll& sc) {
            const float view = r.scrollHorizontal ? r.w : r.h;
            return ScrollThumb(view, view + r.scrollMax * scale, sc.scroll * scale, scale);
        };
        const bool onList = hit && hit->kind == UIRect::Kind::Dropdown;  // an open drop-down list takes the wheel
        if (over && input_.wheel != 0.0f && !onList) {
            if (UIScroll* sc = scene_.Get<UIScroll>(over->entity)) sc->scroll = Clamp(sc->scroll - input_.wheel * sc->wheelStep, 0.0f, over->scrollMax);
        }
        if (input_.pressedThisFrame.count("MouseLeft")) {
            uiScrollDrag_ = uiScrollBar_ = kNullEntity;
            const UIScroll* sc = over ? scene_.Get<UIScroll>(over->entity) : nullptr;
            if (sc && sc->bar && over->scrollMax > 0.0f) {
                // the strip along the far edge belongs to the bar, wider than the thumb so it is easy to hit
                const float across = over->scrollHorizontal ? over->y + over->h - py : over->x + over->w - px;
                if (across >= 0.0f && across <= 18.0f * scale) {
                    const UIThumb t = thumbOf(*over, *sc);
                    const float along = over->scrollHorizontal ? px - over->x : py - over->y;
                    uiScrollBar_ = over->entity;
                    uiScrollGrab_ = along >= t.at && along <= t.at + t.length ? along - t.at : t.length * 0.5f;
                    barPress = true;
                }
            }
            if (!barPress) {
                uiScrollDrag_ = over && !hit ? over->entity : kNullEntity;
                if (over) uiScrollLast_ = over->scrollHorizontal ? px : py;
            }
        } else if (!input_.IsDown("MouseLeft")) {
            uiScrollDrag_ = uiScrollBar_ = kNullEntity;
        }
        for (const UIRect& r : rects) {
            if (!r.scroll) continue;
            if (r.entity == uiScrollBar_) {
                UIScroll* sc = scene_.Get<UIScroll>(r.entity);
                const UIThumb t = thumbOf(r, *sc);
                const float along = r.scrollHorizontal ? px - r.x : py - r.y;
                if (t.travel > 0.0f) sc->scroll = Clamp((along - uiScrollGrab_ - t.margin) / t.travel, 0.0f, 1.0f) * r.scrollMax;
            } else if (r.entity == uiScrollDrag_) {
                UIScroll* sc = scene_.Get<UIScroll>(r.entity);
                const float now = r.scrollHorizontal ? px : py;
                sc->scroll = Clamp(sc->scroll - (now - uiScrollLast_) / scale, 0.0f, r.scrollMax);
                uiScrollLast_ = now;
            }
        }
    }
    // Drop-downs: a press on one opens or closes its list; a press anywhere else closes an open list and does
    // nothing more. In the list a press that is released on the same row selects it, a drag scrolls a long list
    // (as does the wheel).
    bool closedList = false;
    const bool pressNow = input_.pressedThisFrame.count("MouseLeft") > 0 && !barPress, heldNow = input_.IsDown("MouseLeft");
    for (auto& kv : scene_.Pool<UIDropdown>()) {
        UIDropdown& dd = kv.second;
        const bool mine = hit && hit->kind == UIRect::Kind::Dropdown && hit->entity == kv.first;
        const bool row = mine && hit->item >= 0;
        dd.hoverItem = row && dd.open ? hit->item : -1;
        auto choose = [&](int item) {
            if (dd.selected != item) {
                dd.selected = item;
                events.push_back({kv.first, "onValueChanged", true, static_cast<float>(item)});
            }
            dd.open = false;
        };
        if (dd.open) {
            int visible = 0;
            for (const UIRect& r : rects) visible += r.entity == kv.first && r.item >= 0 ? 1 : 0;
            if (mine && input_.wheel != 0.0f) dd.first -= input_.wheel > 0.0f ? 1 : -1;
            if (uiListPress_ == kv.first && heldNow && !pressNow) {
                const float rowH = std::max(1.0f, hit && row ? hit->h : 48.0f);
                const int steps = static_cast<int>((uiListY_ - py) / rowH);
                if (steps != 0) dd.first += steps, uiListY_ -= static_cast<float>(steps) * rowH, uiListDragged_ = true;
            }
            dd.first = std::max(0, std::min(dd.first, static_cast<int>(dd.options.size()) - std::max(1, visible)));
        }
        if (uiListPress_ == kv.first && !heldNow && !pressNow) {  // released
            if (row && dd.open && !uiListDragged_) choose(hit->item);
            uiListPress_ = kNullEntity;
        }
        if (!pressNow) continue;
        if (row && !heldNow) {  // a tap within one frame
            choose(hit->item);
        } else if (row) {
            uiListPress_ = kv.first;
            uiListY_ = py;
            uiListDragged_ = false;
        } else if (mine) {
            dd.open = !dd.open;
            dd.first = std::max(0, dd.selected - 2);
        } else if (dd.open) {
            dd.open = false;
            closedList = true;
        }
    }
    if (input_.pressedThisFrame.count("MouseLeft")) {
        // a press on a scroll bar is not a click on the row under it
        uiPressed_ = barPress || closedList ? kNullEntity : hover;
        if (!barPress && !closedList && hit && hit->kind == UIRect::Kind::Button) {
            // clickOn release waits for the pointer to come up; a tap that went down and up within this frame clicks now.
            const bool tap = !input_.IsDown("MouseLeft");
            if (scene_.Get<UIButton>(hover)->clickOn != "release") ClickButton(hover, events);
            else if (tap) ClickButton(hover, events), uiPressed_ = kNullEntity;
        }
    } else if (!input_.IsDown("MouseLeft")) {
        if (uiPressed_ != kNullEntity && uiPressed_ == hover && hit->kind == UIRect::Kind::Button &&
            scene_.Get<UIButton>(hover)->clickOn == "release") ClickButton(hover, events);
        uiPressed_ = kNullEntity;
    }
    // A pressed slider follows the pointer (also outside its rectangle) until release.
    if (UISlider* s = uiPressed_ != kNullEntity ? scene_.Get<UISlider>(uiPressed_) : nullptr) {
        for (const UIRect& r : rects) {
            if (r.entity != uiPressed_ || r.kind != UIRect::Kind::Slider || !s->interactable) continue;
            float v = SliderValueAt(*s, r, px, py);
            if (v != s->value) {
                s->value = v;
                events.push_back({uiPressed_, "onValueChanged", true, v});
            }
            break;
        }
    }
    UpdateUIFocus(rects, events);
    for (auto& kv : scene_.Pool<UIButton>()) {
        kv.second.hovered = kv.first == uiHovered_;
        kv.second.pressed = kv.first == uiPressed_ || heldButtons_.count(kv.first) > 0;
        kv.second.focused = kv.first == uiFocus_;
    }
    for (auto& kv : scene_.Pool<UISlider>()) {
        kv.second.hovered = kv.first == uiHovered_;
        kv.second.pressed = kv.first == uiPressed_;
        kv.second.focused = kv.first == uiFocus_;
    }
    for (auto& kv : scene_.Pool<UIInput>()) {
        kv.second.hovered = kv.first == uiHovered_;
        kv.second.focused = kv.first == uiFocus_;
        if (kv.second.focused) kv.second.composing = input_.composing;
        else kv.second.composing.clear();
    }
    for (auto& kv : scene_.Pool<UIDropdown>()) {
        kv.second.hovered = kv.first == uiHovered_;
        kv.second.focused = kv.first == uiFocus_;
    }
    return events;
}

bool Engine::TextInputActive() const { return uiFocus_ != kNullEntity && scene_.Get<UIInput>(uiFocus_) != nullptr; }

std::string Engine::ClipboardText() const { return systemClipboard_ ? PlatformClipboardText() : clipboard_; }

void Engine::SetClipboardText(const std::string& text) {
    if (systemClipboard_) PlatformSetClipboardText(text);
    else clipboard_ = text;
}

void Engine::ClickButton(EntityId id, std::vector<UIEvent>& events) {
    events.push_back({id, "onClick", false, 0.0f});
    UIButton* button = scene_.Get<UIButton>(id);
    if (!button->toggle || (button->on && !button->group.empty())) return;  // the chosen one of a radio group stays on
    button->on = !button->on;
    events.push_back({id, "onToggle", true, button->on ? 1.0f : 0.0f, true});
    if (!button->on || button->group.empty()) return;
    for (auto& kv : scene_.Pool<UIButton>()) {
        UIButton& other = kv.second;
        if (kv.first == id || !other.toggle || !other.on || other.group != button->group) continue;
        other.on = false;
        events.push_back({kv.first, "onToggle", true, 0.0f, true});
    }
}

void Engine::UpdateUIFocus(const std::vector<UIRect>& rects, std::vector<UIEvent>& events) {
    const auto& canvases = scene_.Pool<UICanvas>();
    const bool navigation = !canvases.empty() && canvases.begin()->second.navigation;
    // A press with the pointer takes the focus; a press anywhere else takes it away from a text field.
    const bool press = input_.pressedThisFrame.count("MouseLeft") > 0;
    EntityId wanted = uiFocus_;
    if (press && uiPressed_ != kNullEntity) wanted = uiPressed_;
    else if (press && scene_.Get<UIInput>(uiFocus_)) wanted = kNullEntity;
    // What the pointer could reach at its centre: not hidden, clipped away or under a modal panel.
    std::vector<const UIRect*> reach;
    const UIRect* current = nullptr;
    for (const UIRect& r : rects) {
        if (!r.interactable || r.item >= 0 || r.kind == UIRect::Kind::Joystick) continue;
        const UIRect* top = HitTestUI(rects, r.x + r.w * 0.5f, r.y + r.h * 0.5f);
        if (!top || top->entity != r.entity) continue;
        reach.push_back(&r);
        if (r.entity == wanted) current = &r;
    }
    // Without navigation only a text field holds the focus (to be typed into).
    if (current && !navigation && current->kind != UIRect::Kind::Input) current = nullptr;
    uiFocus_ = current ? current->entity : kNullEntity;
    auto pressed = [&](const char* key, const char* pad) { return input_.pressedThisFrame.count(key) || input_.pressedThisFrame.count(pad) ? 1 : 0; };

    UIInput* field = current && current->kind == UIRect::Kind::Input ? scene_.Get<UIInput>(uiFocus_) : nullptr;
    // Editing keys repeat while held.
    std::string heldKey;
    for (const char* key : {"Backspace", "Delete", "Left", "Right", "Up", "Down"}) {
        if (field && heldKey.empty() && input_.IsDown(key)) heldKey = key;
    }
    uiRepeat_ = !heldKey.empty() && heldKey == uiRepeatKey_ ? uiRepeat_ + 1 : 0;
    uiRepeatKey_ = heldKey;
    auto fire = [&](const char* key) { return input_.pressedThisFrame.count(key) > 0 || (uiRepeatKey_ == key && uiRepeat_ > 24 && uiRepeat_ % 3 == 0); };
    if (field) {
        auto lead = [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; };  // first byte of a UTF-8 character
        std::string text = field->text;
        auto size = [&] { return static_cast<int>(text.size()); };
        auto snap = [&](int at) {  // inside the text, on a character boundary
            at = std::max(0, std::min(at, size()));
            while (at > 0 && at < size() && !lead(text[static_cast<size_t>(at)])) --at;
            return at;
        };
        auto prev = [&](int at) {
            if (at > 0) --at;
            while (at > 0 && !lead(text[static_cast<size_t>(at)])) --at;
            return at;
        };
        auto next = [&](int at) {
            if (at < size()) ++at;
            while (at < size() && !lead(text[static_cast<size_t>(at)])) ++at;
            return at;
        };
        int caret = snap(field->caretAt), anchor = snap(field->selectAt);
        if (!field->focused && !press) caret = anchor = size();  // focused by a key or a script: continue at the end
        const bool ctrl = input_.IsDown("Control"), shift = input_.IsDown("Shift");
        auto move = [&](int to) {
            caret = to;
            if (!shift) anchor = caret;
        };
        auto eraseSelection = [&] {
            if (caret == anchor) return false;
            const int lo = std::min(caret, anchor), hi = std::max(caret, anchor);
            text.erase(static_cast<size_t>(lo), static_cast<size_t>(hi - lo));
            caret = anchor = lo;
            return true;
        };
        auto insert = [&](const std::string& piece) {  // replaces the selection, a character at a time up to maxLength
            eraseSelection();
            int count = static_cast<int>(std::count_if(text.begin(), text.end(), lead));
            for (size_t at = 0; at < piece.size();) {
                size_t end = at + 1;
                while (end < piece.size() && !lead(piece[end])) ++end;
                std::string ch = piece.substr(at, end - at);
                at = end;
                if (ch == "\n" && !field->multiline) ch = " ";
                if (static_cast<unsigned char>(ch[0]) < 32 && ch != "\n") continue;
                if (field->maxLength > 0 && count >= field->maxLength) break;
                text.insert(static_cast<size_t>(caret), ch);
                caret += static_cast<int>(ch.size());
                ++count;
            }
            anchor = caret;
        };
        // The pointer places the caret; dragging extends the selection.
        const int w = input_.viewWidth, h = input_.viewHeight;
        const float px = input_.mouseX * static_cast<float>(w), py = input_.mouseY * static_cast<float>(h);
        if (uiPressed_ == uiFocus_ && (press || input_.IsDown("MouseLeft"))) {
            caret = snap(UIInputOffsetAt(scene_, *current, px, py, w, h, assets_.get()));
            if (press && !shift) anchor = caret;
        }
        if (fire("Left")) move(caret != anchor && !shift ? std::min(caret, anchor) : prev(caret));
        if (fire("Right")) move(caret != anchor && !shift ? std::max(caret, anchor) : next(caret));
        if (input_.pressedThisFrame.count("Home")) move(caret > 0 ? static_cast<int>(text.rfind('\n', static_cast<size_t>(caret - 1)) + 1) : 0);
        if (input_.pressedThisFrame.count("End")) {
            const size_t end = text.find('\n', static_cast<size_t>(caret));
            move(end == std::string::npos ? size() : static_cast<int>(end));
        }
        if (field->multiline && fire("Up")) move(snap(UIInputOffsetVertical(scene_, *current, caret, -1, w, h, assets_.get())));
        if (field->multiline && fire("Down")) move(snap(UIInputOffsetVertical(scene_, *current, caret, 1, w, h, assets_.get())));
        if (ctrl && input_.pressedThisFrame.count("A")) anchor = 0, caret = size();
        if (ctrl && (input_.pressedThisFrame.count("C") || input_.pressedThisFrame.count("X")) && caret != anchor && !field->password) {
            SetClipboardText(text.substr(static_cast<size_t>(std::min(caret, anchor)), static_cast<size_t>(std::abs(caret - anchor))));
            if (input_.pressedThisFrame.count("X")) eraseSelection();
        }
        if (ctrl && input_.pressedThisFrame.count("V")) insert(ClipboardText());
        if (fire("Backspace") && !eraseSelection() && caret > 0) {
            const int from = prev(caret);
            text.erase(static_cast<size_t>(from), static_cast<size_t>(caret - from));
            caret = anchor = from;
        }
        if (fire("Delete") && !eraseSelection() && caret < size()) text.erase(static_cast<size_t>(caret), static_cast<size_t>(next(caret) - caret));
        if (!ctrl && !input_.text.empty()) insert(input_.text);
        bool submit = false;
        if (input_.pressedThisFrame.count("Enter")) {
            if (field->multiline && !ctrl) insert("\n");
            else submit = true;
        }
        if (caret != field->caretAt || anchor != field->selectAt || text != field->text) field->caret = 0.0f;  // shown at once after it moves
        field->caretAt = caret;
        field->selectAt = anchor;
        auto notify = [&](const char* method) {
            UIEvent ev{uiFocus_, method, false, 0.0f};
            ev.hasText = true;
            ev.text = text;
            events.push_back(ev);
        };
        if (text != field->text) {
            field->text = text;
            notify("onTextChanged");
        }
        if (submit) {
            notify("onSubmit");
            if (!navigation) uiFocus_ = kNullEntity;
            return;
        }
        if (input_.pressedThisFrame.count("Escape")) {
            uiFocus_ = kNullEntity;
            return;
        }
    }
    if (!navigation) return;

    const int dx = field ? 0 : pressed("Right", "GamepadDPadRight") - pressed("Left", "GamepadDPadLeft");
    const int dy = dx != 0 || (field && field->multiline) ? 0 : pressed("Down", "GamepadDPadDown") - pressed("Up", "GamepadDPadUp");
    const bool accept = pressed("Enter", "GamepadA") != 0;
    if (current && current->kind == UIRect::Kind::Button && accept) ClickButton(uiFocus_, events);
    // A focused drop-down: Enter opens and closes its list, up/down pick the option while it is open.
    if (UIDropdown* dd = current && current->kind == UIRect::Kind::Dropdown ? scene_.Get<UIDropdown>(uiFocus_) : nullptr) {
        if (accept) dd->open = !dd->open, dd->first = std::max(0, dd->selected - 2);
        if (dd->open && dy != 0) {
            const int item = std::max(0, std::min(dd->selected + dy, static_cast<int>(dd->options.size()) - 1));
            if (item != dd->selected) {
                dd->selected = item;
                events.push_back({uiFocus_, "onValueChanged", true, static_cast<float>(item)});
            }
            // keep the chosen row in the window of a long list
            int visible = 0;
            for (const UIRect& r : rects) visible += r.entity == uiFocus_ && r.item >= 0 ? 1 : 0;
            dd->first = std::max(std::min(dd->first, item), item - std::max(1, visible) + 1);
            return;
        }
    }
    if (dx == 0 && dy == 0) return;
    if (!current) {  // nothing focused yet: start at the top-left element
        for (const UIRect* r : reach) {
            if (!current || r->y < current->y || (r->y == current->y && r->x < current->x)) current = r;
        }
        uiFocus_ = current ? current->entity : kNullEntity;
        return;
    }
    // A focused slider is changed along its own axis; across it the focus moves on.
    if (UISlider* s = current->kind == UIRect::Kind::Slider ? scene_.Get<UISlider>(uiFocus_) : nullptr) {
        const bool horizontal = s->direction == "left-to-right" || s->direction == "right-to-left";
        if (horizontal ? dx != 0 : dy != 0) {
            const float sign = s->direction == "left-to-right" ? static_cast<float>(dx) : s->direction == "right-to-left" ? static_cast<float>(-dx)
                             : s->direction == "top-to-bottom" ? static_cast<float>(dy) : static_cast<float>(-dy);
            const float amount = s->step > 0.0f ? s->step : std::fabs(s->max - s->min) / 20.0f;
            const float v = Clamp(s->value + sign * amount, std::min(s->min, s->max), std::max(s->min, s->max));
            if (v != s->value) {
                s->value = v;
                events.push_back({uiFocus_, "onValueChanged", true, v});
            }
            return;
        }
    }
    // The nearest element in that direction; being off to the side counts double. Draw order breaks ties.
    const float fx = current->x + current->w * 0.5f, fy = current->y + current->h * 0.5f;
    const UIRect* best = nullptr;
    float bestScore = 0.0f;
    for (const UIRect* r : reach) {
        const float vx = r->x + r->w * 0.5f - fx, vy = r->y + r->h * 0.5f - fy;
        const float along = vx * static_cast<float>(dx) + vy * static_cast<float>(dy);
        if (r == current || along < 1.0f) continue;
        const float score = along + 2.0f * (dx != 0 ? std::fabs(vy) : std::fabs(vx));
        if (!best || score < bestScore) best = r, bestScore = score;
    }
    if (best) uiFocus_ = best->entity;
}

RenderStats Engine::RenderGameView(RenderTarget& target) {
    RenderView view;
    MakeSceneView(scene_, static_cast<float>(target.width) / static_cast<float>(target.height), view);
    view.shaderTime = static_cast<float>(SimTime());
    view.playing = InPlaySession();
    AppendDebugLines(view.lines);
    return renderer_->Render(scene_, view, target);
}

bool Engine::EnableGpu(Window* window, std::string* error) {
    if (gpu_) return true;
    gpuDevice_ = CreateGpuDevice(window, error);
    if (!gpuDevice_) return false;
    GpuRenderer::Settings settings;
    settings.shaderCacheDir = gpuShaderCache_;
    settings.asyncShaders = gpuAsyncShaders_;
    gpu_ = std::make_unique<GpuRenderer>(*gpuDevice_, assets_.get(), settings);
    OE_LOG_INFO("render", "GPU renderer ready: %s", gpu_->Name());
    gpu_->Preload(preloadMaterials_, preloadTextures_, preloadMeshes_);
    preloadMaterials_.clear();
    preloadTextures_.clear();
    preloadMeshes_.clear();
    return true;
}

Json Engine::Preload(const std::vector<std::string>& paths) {
    std::vector<std::shared_ptr<const Material>> materials;
    std::vector<std::shared_ptr<const Texture>> textures;
    std::vector<std::shared_ptr<const Mesh>> meshes;
    Json loaded = Json::MakeArray(), errors = Json::MakeArray();
    // A file inside another (an effect's texture) is reported under the path that was asked for.
    std::string asked;
    bool failed = false;
    auto fail = [&](const std::string& message) {
        Json j = Json::MakeObject();
        j["path"] = asked;
        j["message"] = message;
        errors.push(j);
        failed = true;
    };
    auto texture = [&](const std::string& path) {
        if (path.empty()) return;
        std::string error;
        if (auto t = assets_->GetTexture(path, &error)) textures.push_back(t);
        else fail(error.empty() ? "cannot load texture '" + path + "'" : error);
    };
    auto material = [&](const std::string& path) {
        if (path.empty()) return;
        std::string error;
        if (auto m = assets_->GetMaterial(path, &error)) materials.push_back(m);
        else fail(error.empty() ? "cannot load material '" + path + "'" : error);
    };
    auto mesh = [&](const std::string& name) {
        if (name.empty()) return;
        std::string error;
        if (auto m = assets_->GetMesh(name, &error)) meshes.push_back(m);
        else fail(error.empty() ? "cannot load mesh '" + name + "'" : error);
    };
    for (const std::string& path : paths) {
        asked = path;
        failed = false;
        const std::string kind = AssetManager::KindOf(path);
        std::string error;
        if (kind == "vfx") {
            if (auto effect = assets_->GetVfx(path, &error)) {
                for (const VfxEmitterDefinition& e : effect->emitters) {
                    texture(e.emitter.texture);
                    material(e.emitter.material);
                    mesh(e.emitter.mesh);
                }
                for (const VfxTrailDefinition& t : effect->trails) {
                    texture(t.trail.texture);
                    material(t.trail.material);
                }
            } else {
                fail(error.empty() ? "cannot load effect '" + path + "'" : error);
            }
        } else if (kind == "material") {
            material(path);
        } else if (kind == "texture") {
            texture(path);
        } else if (kind == "model") {
            mesh(path);
        } else if (kind == "shader") {
            if (auto graph = assets_->GetShader(path, &error)) {
                auto m = std::make_shared<Material>();
                m->shader = graph;
                materials.push_back(m);
            } else {
                fail(error.empty() ? "cannot load shader '" + path + "'" : error);
            }
        } else if (kind == "font") {
            if (!assets_->GetFont(path, &error)) fail(error.empty() ? "cannot load font '" + path + "'" : error);
        } else if (kind == "audio") {
            if (!audio_->Preload(path, &error)) fail(error);
        } else {
            fail("nothing to preload for this kind of file (effects, materials, textures, models, shaders, fonts and sounds are)");
        }
        if (!failed) {
            Json j = Json::MakeObject();
            j["path"] = path;
            j["kind"] = kind;
            loaded.push(j);
        }
    }
    if (gpu_) {
        gpu_->Preload(materials, textures, meshes);
    } else {  // a GPU renderer enabled later (a tool's first GPU screenshot) gets them then
        preloadMaterials_.insert(preloadMaterials_.end(), materials.begin(), materials.end());
        preloadTextures_.insert(preloadTextures_.end(), textures.begin(), textures.end());
        preloadMeshes_.insert(preloadMeshes_.end(), meshes.begin(), meshes.end());
    }
    Json out = Json::MakeObject();
    out["loaded"] = loaded;
    out["errors"] = errors;
    out["gpu"] = gpu_ != nullptr;
    return out;
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
    BeginInterpolatedRender();
    RenderView view;
    MakeSceneView(scene_, static_cast<float>(w) / static_cast<float>(h), view);
    view.shaderTime = static_cast<float>(SimTime());
    view.playing = InPlaySession();
    AppendDebugLines(view.lines);
    const bool presented = gpu_->RenderToWindow(scene_, view, renderScale);
    EndInterpolatedRender();
    return presented;
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
    redo_.push_back({scene_.ToJson(), undo_.back().command, undo_.back().args});
    std::string err;
    scene_.FromJson(undo_.back().scene, &err);
    undo_.pop_back();
    dirty_ = true;
    Touch();
    return true;
}

bool Engine::Redo() {
    if (redo_.empty() || playSnapshot_) return false;
    lastMergeKey_.clear();
    undo_.push_back({scene_.ToJson(), redo_.back().command, redo_.back().args});
    std::string err;
    scene_.FromJson(redo_.back().scene, &err);
    redo_.pop_back();
    dirty_ = true;
    Touch();
    return true;
}

Json Engine::HistoryState() const {
    Json result = Json::MakeObject(), entries = Json::MakeArray();
    auto append = [&](const HistoryEntry& entry, bool applied) {
        Json row = Json::MakeObject();
        row["command"] = entry.command;
        row["args"] = entry.args;
        row["applied"] = applied;
        entries.push(row);
    };
    for (const auto& entry : undo_) append(entry, true);
    for (auto it = redo_.rbegin(); it != redo_.rend(); ++it) append(*it, false);
    result["entries"] = entries;
    result["cursor"] = static_cast<uint64_t>(undo_.size());
    return result;
}

bool Engine::BeginPrefabEdit(const std::string& path, std::string* error) {
    if (InPlaySession() || prefabBackup_) {
        if (error) *error = "stop play and close the current prefab before editing another";
        return false;
    }
    Json prefab = ReadProjectJson(path);
    if (!EndsWith(path, ".prefab.json") || prefab["format"].asString() != kPrefabFormat) {
        if (error) *error = "expected a .prefab.json file";
        return false;
    }
    // Validate in a temporary scene before changing the current edit context.
    Scene isolated;
    Json document = prefab;
    document["format"] = "ownengine.scene";
    if (!isolated.FromJson(document, error)) return false;
    if (isolated.Entities().empty()) {
        if (error) *error = "a prefab must contain one root";
        return false;
    }
    size_t roots = 0;
    for (const auto& entry : isolated.Entities()) roots += entry.second.parent == kNullEntity;
    if (roots != 1) {
        if (error) *error = "a prefab must contain exactly one root";
        return false;
    }
    prefabBackup_ = std::make_unique<PrefabEditBackup>();
    prefabBackup_->scene = scene_.ToJson();
    prefabBackup_->scenePath = scenePath_;
    prefabBackup_->dirty = dirty_;
    prefabBackup_->undo = std::move(undo_);
    prefabBackup_->redo = std::move(redo_);
    prefabBackup_->mergeKey = lastMergeKey_;
    scene_.ReplaceWith(isolated);
    prefabPath_ = path;
    scenePath_.clear();
    ResetHistory();
    ResetRuntime();
    dirty_ = false;
    Touch();
    return true;
}

bool Engine::SavePrefabEdit(std::string* error) {
    if (!prefabBackup_) {
        if (error) *error = "no prefab is being edited";
        return false;
    }
    EntityId root = kNullEntity;
    for (const auto& entry : scene_.Entities()) if (entry.second.parent == kNullEntity) {
        if (root != kNullEntity) {
            if (error) *error = "parent all entities under a single prefab root before saving";
            return false;
        }
        root = entry.first;
    }
    if (root == kNullEntity) {
        if (error) *error = "the prefab root was deleted; undo before saving";
        return false;
    }
    if (!WriteTextFileAtomic(ResolvePath(prefabPath_), MakePrefab(scene_, root).dump(2) + "\n", error)) return false;
    dirty_ = false;
    Touch();
    return true;
}

bool Engine::EndPrefabEdit(bool discard, std::string* error) {
    if (!prefabBackup_) {
        if (error) *error = "no prefab is being edited";
        return false;
    }
    if (dirty_ && !discard) {
        if (error) *error = "prefab has unsaved changes; save it or close with discard:true";
        return false;
    }
    scene_.FromJson(prefabBackup_->scene, error);
    scenePath_ = prefabBackup_->scenePath;
    dirty_ = prefabBackup_->dirty;
    undo_ = std::move(prefabBackup_->undo);
    redo_ = std::move(prefabBackup_->redo);
    lastMergeKey_ = prefabBackup_->mergeKey;
    prefabBackup_.reset();
    prefabPath_.clear();
    ResetRuntime();
    Touch();
    return true;
}

// ----- API -----------------------------------------------------------------

Json Engine::Call(const std::string& name, const Json& rawArgs) {
    std::lock_guard<std::recursive_mutex> lock(engineMutex_);
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
        if (prefabBackup_ && (name == "scene.load" || name == "scene.new" || name == "scene.save" ||
                             (name.compare(0, 4, "sim.") == 0 && name != "sim.state") || name == "game.load_scene" ||
                             name.compare(0, 4, "net.") == 0))
            throw ApiError("prefab_editing", "this command is unavailable while editing a prefab",
                           "Call prefab.save, then prefab.close, or prefab.close {discard:true}.");
        if (((sync_ && sync_->Active()) || (authority_ && authority_->Active())) && !inWorld_ && !replaying_ && (name == "script.eval" || (cmd->mutates && name.compare(0, 6, "input.") != 0 && name.compare(0, 4, "sim.") != 0)))
            throw ApiError("match_active", "external gameplay edits are disabled during a match", "Put deterministic gameplay logic in scripts before net.start.");
        if (journal_ && !inWorld_ && !replaying_ && cmd->mutates && name != "script.eval" &&
            name.compare(0, 6, "input.") != 0 && name.compare(0, 4, "sim.") != 0)
            throw ApiError("state_recording", "external scene edits are disabled while recording", "Use scripted deterministic edits, or sim.stop before editing.");
        if (journal_ && !inWorld_ && !replaying_ && name == "script.eval") {
            if ((sync_ && sync_->Active()) || (authority_ && authority_->Active())) throw ApiError("match_active", "external Lua evaluation can desynchronize a match", "Put gameplay logic in scripts before net.start.");
            size_t total = 0; for (const auto& entries : journal_->commands) total += entries.second.size();
            if (total >= 512 || args.dump().size() > 65536) throw ApiError("state_limit", "replay supports 512 external evaluations of at most 64 KiB");
            journal_->commands[frame_].emplace_back(name, args);
        }
        if ((journal_ || (sync_ && sync_->Active()) || (authority_ && authority_->Active())) && !inWorld_ && !replaying_ &&
            (name == "audio.play" || name == "audio.stop" || name == "game.load_scene" || name == "save.set" || name == "save.clear" || name == "save.flush" || name == "script.write" || name == "script.reload" ||
             name == "vfx.create" || name == "vfx.set" || name == "vfx.from_entity" || name == "vfx.play" || name == "vfx.stop" ||
             name == "vfx.burst" || name == "vfx.clear" || name == "vfx.set_param" || name == "vfx.edit" || name == "vfx.undo" ||
             name == "vfx.redo" || name == "vfx.save" || name == "vfx.close" || name == "vfx.edit_replace"))
            throw ApiError("state_recording", "external side effects are disabled while recording", "Use deterministic Lua gameplay or stop recording before edits.");
        const bool record = cmd->mutates && !playSnapshot_;
        Json before = record ? scene_.ToJson() : Json();
        Json result = cmd->run(*this, args);
        if (record) {
            // Consecutive calls with the same non-empty merge key (e.g. an
            // editor drag) collapse into one undo step.
            const Json* merge = args.find("merge");
            std::string mergeKey = merge && merge->isString() ? merge->asString() : std::string();
            if (mergeKey.empty() || mergeKey != lastMergeKey_ || undo_.empty()) {
                undo_.push_back({std::move(before), name, args});
                if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
            } else {
                undo_.back().args = args;
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

std::future<Json> Engine::PostCall(const std::string& name, const Json& args, const std::string& agent) {
    auto promise = std::make_shared<std::promise<Json>>();
    std::future<Json> future = promise->get_future();
    std::lock_guard<std::mutex> lock(jobsMutex_);
    jobs_.push_back([this, name, args, agent, promise] {
        remoteAgent_ = agent;  // known to the command itself (team.send counts an agent's hops) and to the observer
        Json result = Call(name, args);
        if (remoteObserver_) remoteObserver_(name, args, result);
        remoteAgent_.clear();
        promise->set_value(std::move(result));
    });
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

bool Engine::HostCall(const std::string& name, const Json& data) {
    if (replaying_) return false;  // a replayed frame already made this call
    bool handled = hostAccept_.count("*") > 0 || hostAccept_.count(name) > 0;
    if (!handled && hostHandler_) handled = hostHandler_(name, data.dump());
    if (hostCalls_.size() >= kMaxHostCalls) hostCalls_.erase(hostCalls_.begin());
    hostCalls_.push_back({name, data, frame_, handled});
    return handled;
}

uint32_t Engine::OpenLink(const std::string& address, uint32_t port) {
    if (sync_ && sync_->Active())
        throw ApiError("link_unavailable", "links are not available in a lockstep or rollback match", "Every peer must compute the same frames; use links before the match or in an authoritative game.");
    if (authority_ && authority_->Running() && network_ && !network_->IsHost())
        throw ApiError("link_unavailable", "a client cannot open a link while it predicts a match", "Open links before joining, or on the server and pass results by RPC.");
    NetAddress target;
    target.host = address;
    target.port = static_cast<uint16_t>(port);
    std::string error;
    uint32_t id = port >= 1 && port <= 65535 ? links_.Open(target, &error) : 0;
    if (!id) throw ApiError("link_open", error.empty() ? "link port must be 1..65535" : error, "Pass a numeric IPv4 address and a port; at most 8 links, close the ones you no longer need. Browsers have no TCP.");
    return id;
}

bool Engine::SetLaunchArg(const std::string& name, const std::string& value, std::string* error) {
    auto fail = [&](const char* message) { if (error) *error = message; return false; };
    if (name.empty() || name.size() > 64) return fail("a launch parameter name has 1..64 characters");
    for (char c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '.' && c != '-') return fail("a launch parameter name uses letters, digits, '_', '.' and '-'");
    if (value.size() > 4096) return fail("a launch parameter value has at most 4096 bytes");
    if (!launchArgs_.count(name) && launchArgs_.size() >= kMaxLaunchArgs) return fail("at most 64 launch parameters");
    launchArgs_[name] = value;
    return true;
}

void Engine::PushHostMessage(const std::string& name, const Json& data) {
    if (hostInbox_.size() >= kMaxHostMessages) {
        OE_LOG_WARN("host", "host message queue full, dropping the oldest message (%s)", hostInbox_.front().name.c_str());
        hostInbox_.erase(hostInbox_.begin());
    }
    hostInbox_.push_back({name, data});
}

std::vector<Engine::HostMessage> Engine::TakeHostMessages() {
    std::vector<HostMessage> out;
    out.swap(hostInbox_);
    return out;
}

std::string Engine::Today() const {
    if (!dateOverride_.empty()) return dateOverride_;
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _MSC_VER
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char text[16];
    std::snprintf(text, sizeof(text), "%04d-%02d-%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    return text;
}

}  // namespace oe
