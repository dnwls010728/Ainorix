#include "app/Engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "core/FileSystem.h"
#include "core/Image.h"
#include "script/ScriptHost.h"

namespace oe {
namespace {
uint64_t HashText(const std::string& text) { return Fnv1a64(reinterpret_cast<const uint8_t*>(text.data()), text.size()); }
InputState Decode(const FrameInput& value, const SyncConfig& config) {
    InputState input;
    for (size_t i = 0; i < config.keys.size(); ++i) {
        if (value.down & (uint64_t{1} << i)) input.down.insert(config.keys[i]);
        if (value.pulse & (uint64_t{1} << i)) input.pressedThisFrame.insert(config.keys[i]);
    }
    for (size_t i = 0; i < config.axes.size(); ++i) if (config.axes[i])
        input.axes[InputState::kAxisNames[i]] = static_cast<float>(value.axes[i]) / 32767.0f;
    return input;
}
}
struct Engine::NativeState {
    const Engine* owner = nullptr;
    Scene scene;
    std::shared_ptr<const ScriptHost::Snapshot> scripts;
    std::shared_ptr<const PhysicsWorld::Snapshot> physics;
    std::shared_ptr<const AudioSystem::Snapshot> audio;
    SaveStore saves;
    Json data;
    InputState input;
    FrameInputs frameInputs;
    std::map<uint32_t, InputState> playerInputs;
    std::map<uint32_t, EntityId> replicatedEntities;
    std::vector<TimedLine> lines;
    std::string runtimeScene, pendingScene;
    std::set<EntityId> buttons;
    std::set<std::string> keys;
    EntityId hover = 0, press = 0;
    uint64_t frame = 0;
    double time = 0;
};
std::shared_ptr<const Engine::NativeState> Engine::CaptureNative(bool output) const {
    auto s = std::make_shared<NativeState>(); s->owner = this;
    s->scene = scene_; s->scripts = scripts_->SaveState(); s->physics = physics_->SaveState();
    s->audio = audio_->SaveState(output); s->saves = saves_; s->data = gameData_;
    s->input = input_; s->frameInputs = frameInputs_; s->playerInputs = playerInputs_; s->replicatedEntities = replicatedEntities_;
    s->lines = debugLines_; s->runtimeScene = runtimeScene_; s->pendingScene = pendingScene_;
    s->buttons = heldButtons_; s->keys = heldKeys_; s->hover = uiHovered_; s->press = uiPressed_;
    s->frame = frame_; s->time = simTime_; return s;
}
void Engine::RestoreNative(const NativeState& s) {
    if (s.owner != this) throw ApiError("state_invalid", "snapshots belong to their originating engine");
    scene_ = s.scene; scripts_->LoadState(*s.scripts); physics_->LoadState(*s.physics);
    audio_->LoadState(*s.audio); saves_ = s.saves; gameData_ = s.data;
    input_ = s.input; frameInputs_ = s.frameInputs; playerInputs_ = s.playerInputs; replicatedEntities_ = s.replicatedEntities;
    debugLines_ = s.lines; runtimeScene_ = s.runtimeScene; pendingScene_ = s.pendingScene;
    heldButtons_ = s.buttons; heldKeys_ = s.keys; uiHovered_ = s.hover; uiPressed_ = s.press;
    frame_ = s.frame; simTime_ = s.time;
}
void Engine::CaptureCheckpoint() {
    if (!sync_ || !sync_->Running() || !sync_->Config().rollback) return;
    checkpoints_[frame_] = CaptureNative(false);
    while (checkpoints_.size() > sync_->Config().rollbackFrames + 1) checkpoints_.erase(checkpoints_.begin());
    // Only the window can be corrected. No history or PCM growth per checkpoint.
    while (!replaying_ && journal_->frames.size() > sync_->Config().rollbackFrames + 1) {
        journal_->frames.erase(journal_->frames.begin()); ++journal_->firstFrame;
    }
}
uint64_t Engine::ContentHash() const {
    std::string manifest;
    auto files = ListFiles(projectDir_, "", true); std::sort(files.begin(), files.end());
    size_t count = 0;
    for (const auto& path : files) {
        std::string relative = RelativePath(path, projectDir_);
        if (relative == "project.json" || relative == "AGENTS.md" || relative == "CLAUDE.md" || relative.compare(0, 6, "tools/") == 0 ||
            relative.front() == '.' || relative.find("/.") != std::string::npos) continue;
        if (++count > 4096) throw ApiError("state_resource", "reference synchronization supports at most 4096 project files");
        std::vector<uint8_t> bytes;
        if (!ReadBinaryFile(path, bytes)) throw ApiError("state_resource", "cannot read replay resource", "Keep project resources immutable during recording.");
        manifest += relative + ":" + std::to_string(bytes.size()) + ":" + std::to_string(Fnv1a64(bytes.data(), bytes.size())) + "\n";
    }
    return HashText(manifest);
}
std::string Engine::SyncWorld() const {
    Json world = scene_.ToJson(), audio = audio_->State();
    world["runtime"] = Json::MakeObject(); world["runtime"]["gameData"] = gameData_;
    world["runtime"]["voices"] = audio["voices"]; world["runtime"]["audioEvents"] = audio["played"];
    return world.dump();
}
void Engine::EnsureSync(bool refresh) {
    if (refresh && sync_ && !sync_->Active()) sync_.reset();
    if (sync_ || !network_ || !network_->Connected() || networkConfig_.mode == "authoritative") return;
    BeginSessionIfNeeded();
    uint64_t blueprint = HashText(playSnapshot_->dump() + ":" + std::to_string(ContentHash()) + ":" + std::to_string(network_->Seed()));
    sync_ = std::make_unique<FrameSync>(networkConfig_.sync, network_->IsHost(), network_->LocalPlayer(), blueprint,
        [this](uint32_t player, const std::vector<uint8_t>& bytes) { return network_ && network_->SendSync(player, bytes); });
    sync_->Tick(networkFrame_);
}
const InputState& Engine::PlayerInput(uint32_t player) const {
    auto it = playerInputs_.find(player);
    if (it != playerInputs_.end()) return it->second;
    if ((!sync_ || !sync_->Active()) && player == (network_ ? network_->LocalPlayer() : 1)) return input_;
    static const InputState empty;
    return empty;
}
void Engine::ApplyInputs(const FrameInputs& inputs) {
    frameInputs_ = inputs; playerInputs_.clear();
    for (const auto& player : inputs) playerInputs_[player.first] = Decode(player.second, (sync_ ? sync_->Config() : networkConfig_.sync));
    input_ = PlayerInput(network_->LocalPlayer());
}
bool Engine::PollSync() {
    if (!network_ || networkConfig_.mode == "authoritative") return true;
    EnsureSync();
    if (!sync_) return true;
    for (const auto& message : network_->DrainSync()) {
        if (!message.second.empty() && message.second.front() == 1 && !network_->IsHost() && !sync_->Active()) EnsureSync(true);
        sync_->Receive(message.first, message.second);
    }
    if (sync_->Active()) {
        network_->Seal(); network_->DrainEvents();
        auto players = network_->Players();
        if (!network_->Connected() || players.size() != sync_->Players().size()) sync_->Stop("match participant disconnected");
    }
    sync_->Tick(networkFrame_);
    for (uint32_t player : sync_->TakeDropped()) { std::string error; network_->Kick(player, "missing match input", &error); }
    if (sync_->TakeStart()) {
        std::string error; scene_.FromJson(*playSnapshot_, &error);
        ResetRuntime(); audio_->ResetTimeline(); scripts_->ClearErrors(); scripts_->SetNetworkSeed(network_->Seed());
        frame_ = 0; simTime_ = 0; gameData_ = Json::MakeObject();
        runtimeScene_ = scenePath_.empty() ? "" : RelativePath(scenePath_, projectDir_);
        uiHovered_ = uiPressed_ = kNullEntity; heldButtons_.clear(); heldKeys_.clear();
        checkpoints_.clear(); journal_.reset(); snapshots_.clear(); syncHashes_.clear(); outputConfirmed_ = 0; desyncNotified_ = false;
        if (sync_->Config().rollback) RecordState();
        saves_.FreezeReads(true); saves_.DeferFlush(true);
    }
    if (!sync_->Active()) {
        if (sync_->State()["state"].asString() == "desync" && !desyncNotified_) {
            desyncNotified_ = true;
            scripts_->DispatchNetwork({{SessionEvent::Type::State, 1, 0, "desync"}});
            Json args = Json::MakeArray(); args.push(sync_->Report()); scripts_->DispatchRpc(1, "net.desync", args);
        }
        return sync_->State()["state"].asString() == "idle";
    }
    if (!sync_->Running()) return false;
    if (auto rollback = sync_->TakeRollback()) {
        auto checkpoint = checkpoints_.find(*rollback);
        if (!journal_ || checkpoint == checkpoints_.end() || *rollback < journal_->firstFrame) {
            sync_->Stop("missing rollback checkpoint"); return false;
        }
        uint64_t end = frame_; InputState raw = input_; auto output = audio_->TakeOutput();
        auto started = std::chrono::steady_clock::now();
        RestoreNative(*checkpoint->second); audio_->DropPendingBefore(outputConfirmed_);
        checkpoints_.erase(checkpoints_.lower_bound(*rollback), checkpoints_.end());
        syncHashes_.erase(syncHashes_.upper_bound(*rollback), syncHashes_.end());
        FrameInputs previous = *rollback > journal_->firstFrame ? journal_->frames.at(static_cast<size_t>(*rollback - journal_->firstFrame - 1)).players : FrameInputs{}; replaying_ = true;
        for (uint64_t i = *rollback; i < end; ++i) {
            auto& frame = journal_->frames.at(static_cast<size_t>(i - journal_->firstFrame));
            frame.players = sync_->ReplayInputs(i, previous, frame.players);
            frame.input = Decode(frame.players.at(network_->LocalPlayer()), sync_->Config());
            previous = frame.players; sync_->Replayed(i, frame.players);
            CaptureCheckpoint(); ApplyInputs(frame.players);
            audio_->SetOutputMode(i < outputConfirmed_, true); SimulateWorld();
            if (frame_ > outputConfirmed_ && frame_ % sync_->Config().hashInterval == 0) syncHashes_[frame_] = SyncWorld();
        }
        replaying_ = false; input_ = raw; audio_->RestoreOutput(std::move(output));
        replayMilliseconds_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    }
    for (auto it = syncHashes_.begin(); it != syncHashes_.end() && it->first <= sync_->Confirmed();) {
        sync_->Hash(it->first, HashText(it->second), it->second); it = syncHashes_.erase(it);
        if (!sync_->Running()) return false;
    }
    audio_->Confirm(std::min(frame_, sync_->Confirmed())); outputConfirmed_ = std::min(frame_, sync_->Confirmed());
    if (sync_->NeedsInput()) {
        const InputState device = SampleNetworkInput();
        FrameInput sample; const auto& config = sync_->Config();
        for (size_t i = 0; i < config.keys.size(); ++i) {
            if (device.IsDown(config.keys[i])) sample.down |= uint64_t{1} << i;
            if (device.pressedThisFrame.count(config.keys[i])) sample.pulse |= uint64_t{1} << i;
        }
        for (size_t i = 0; i < config.axes.size(); ++i) if (config.axes[i]) {
            auto axis = device.axes.find(InputState::kAxisNames[i]); float value = axis == device.axes.end() ? 0 : axis->second;
            if (!std::isfinite(value)) value = 0;
            sample.axes[i] = static_cast<int16_t>(std::round(std::max(i >= 4 ? 0.0f : -1.0f, std::min(1.0f, value)) * 32767));
        }
        sync_->Submit(sample); input_.pressedThisFrame.clear();
    }
    sync_->Tick(networkFrame_);
    auto inputs = sync_->Next(); if (!inputs) return false;
    deviceInput_ = input_; ApplyInputs(*inputs);
    audio_->SetOutputMode(false, sync_->Config().rollback);
    return true;
}
void Engine::RecordState() {
    if (journal_) return;
    if (network_ && (!sync_ || !sync_->Running()) && (!authority_ || !authority_->Running())) throw ApiError("state_recording", "manual recording requires an offline simulation", "Leave/stop networking before sim.record_state.");
    BeginSessionIfNeeded();
    if (frame_ != 0) throw ApiError("state_recording", "recording must begin before the first simulation frame", "Use sim.stop, then sim.record_state before sim.step.");
    if (!physics_->EnableSnapshots()) throw ApiError("state_recording", "native recording must precede physics world creation", "Call sim.stop, then sim.record_state before physics queries.");
    scripts_->EnableSnapshots();
    saves_.FreezeReads(true); saves_.DeferFlush(true);
    journal_ = std::make_unique<SimulationState>();
    journal_->allocationCursor = scene_.AllocationCursor(); journal_->baseline = scene_.ToJson(); journal_->runtimeScene = runtimeScene_;
    journal_->content = ContentHash(); journal_->seed = network_ ? network_->Seed() : 0;
    journal_->baselineAudio = audio_->SaveState(); journal_->baselineSaves = saves_;
}
std::shared_ptr<const Engine::SimulationState> Engine::SaveState() const {
    if (!journal_ || inWorld_) throw ApiError("state_recording", "no completed recorded state", "Call sim.record_state before sim.step.");
    auto state = std::make_shared<SimulationState>(*journal_);
    state->native = CaptureNative(true);
    state->audio = audio_->SaveState(); state->saves = saves_; state->input = input_;
    state->scene = scene_.ToJson(); state->gameData = gameData_; state->errors = scripts_->Errors(); state->playing = playing_; state->accumulator = accumulator_; return state;
}
void Engine::LoadState(const SimulationState& state) {
    if ((sync_ && sync_->Active()) || (authority_ && authority_->Active())) throw ApiError("match_active", "manual state loading is disabled during a match", "Automatic rollback restores its own journal.");
    if (!state.audio || !state.baselineAudio) throw ApiError("state_invalid", "invalid snapshot");
    if (ContentHash() != state.content) throw ApiError("state_resource", "project resources changed since recording", "Restore the original resources before loading state.");
    if (!state.native || state.native->owner != this) throw ApiError("state_invalid", "snapshots belong to their originating engine");
    journal_ = std::make_unique<SimulationState>(state);
    auto started = std::chrono::steady_clock::now();
    if (!state.native) throw ApiError("state_invalid", "native snapshot is missing");
    RestoreNative(*state.native);
    replayMilliseconds_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    audio_->LoadState(*state.audio); saves_ = state.saves; input_ = state.input; scripts_->RestoreErrors(state.errors); playing_ = state.playing; accumulator_ = state.accumulator;
    if (scene_.ToJson() != state.scene || gameData_ != state.gameData)
        throw ApiError("state_desync", "replay did not reproduce saved state", "Avoid unrecorded external state changes while recording.");
    Touch();
}
Json Engine::SnapshotCall(const std::string& operation, const Json& args) {
    std::string slot = args["slot"].asString("default");
    if (slot.empty() || slot.size() > 32) throw ApiError("invalid_argument", "snapshot slot must be 1..32 bytes");
    if (operation == "record") RecordState();
    else if (operation == "save") {
        if (!snapshots_.count(slot) && snapshots_.size() >= 8) throw ApiError("state_limit", "at most eight snapshot slots are allowed");
        snapshots_[slot] = SaveState();
    } else if (operation == "load") {
        auto it = snapshots_.find(slot); if (it == snapshots_.end()) throw ApiError("state_missing", "snapshot slot does not exist");
        LoadState(*it->second);
    }
    Json out = Json::MakeObject(); out["recording"] = journal_ != nullptr; out["frame"] = frame_;
    out["slots"] = static_cast<uint64_t>(snapshots_.size()); out["replayMilliseconds"] = replayMilliseconds_; return out;
}
}  // namespace oe
