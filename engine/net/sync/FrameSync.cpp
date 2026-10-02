#include "net/sync/FrameSync.h"

#include <algorithm>
#include <atomic>
#include <cmath>

#include "net/Bytes.h"

namespace oe {
namespace {
std::atomic<uint64_t> g_instances{0};
constexpr const char* kAxes[] = {"LeftX", "LeftY", "RightX", "RightY", "LT", "RT"};
enum Kind : uint8_t { BeginMessage = 1, ReadyMessage, GoMessage, InputMessage, MergedMessage, HashMessage, StopMessage, ReportMessage };
ByteWriter Message(uint8_t kind, uint64_t epoch) { ByteWriter w(60000); w.WriteU8(kind); w.WriteU64(epoch); return w; }
bool ReadInput(ByteReader& r, FrameInput& input) {
    if (!r.ReadU64(input.down) || !r.ReadU64(input.pulse)) return false;
    for (auto& axis : input.axes) { uint16_t value = 0; if (!r.ReadU16(value) || value == 65535) return false; axis = static_cast<int16_t>(static_cast<int>(value) - 32767); }
    return true;
}
void WriteInput(ByteWriter& w, const FrameInput& input) {
    w.WriteU64(input.down); w.WriteU64(input.pulse);
    for (int16_t axis : input.axes) w.WriteU16(static_cast<uint16_t>(static_cast<int>(axis) + 32767));
}
bool Integer(const Json& j, const char* key, uint32_t fallback, uint32_t lo, uint32_t hi, uint32_t& value) {
    double n = j.has(key) ? j[key].asNumber(-1) : fallback;
    if (!std::isfinite(n) || n < lo || n > hi || std::floor(n) != n) return false;
    value = static_cast<uint32_t>(n); return true;
}
}
bool SyncConfig::Parse(const Json& json, SyncConfig& out, std::string* error) {
    SyncConfig config; config.rollback = json["mode"].asString() == "rollback";
    auto fail = [&](const char* text) { if (error) *error = text; return false; };
    if (!Integer(json, "inputDelay", 2, 0, 8, config.delay) || !Integer(json, "rollbackFrames", 8, 1, 8, config.rollbackFrames) ||
        !Integer(json, "hashInterval", 60, 1, 600, config.hashInterval) || !Integer(json, "waitFrames", 300, 30, 600, config.waitFrames))
        return fail("inputDelay must be 0..8, rollbackFrames 1..8, hashInterval 1..600 and waitFrames 30..600");
    std::string policy = json["dropPolicy"].asString("kick");
    if (json.has("dropPolicy") && (!json["dropPolicy"].isString() || (policy != "kick" && policy != "empty")))
        return fail("dropPolicy must be kick (stop the match) or empty (substitute a missing input)");
    config.emptyOnTimeout = policy == "empty";
    for (const char* key : {"actions", "axes"}) if (json.has(key) && !json[key].isArray()) return fail("actions/axes must be arrays of key/axis names");
    if (json["actions"].size() > 64 || json["axes"].size() > 6) return fail("at most 64 actions and six axes are allowed");
    for (const Json& key : json["actions"].items()) {
        std::string name = key.asString();
        if (!key.isString() || name.empty() || name.size() > 32 ||
            !std::all_of(name.begin(), name.end(), [](unsigned char c) { return c >= 32 && c < 127; }) ||
            std::find(config.keys.begin(), config.keys.end(), name) != config.keys.end()) return fail("actions need unique printable ASCII key names (1..32 bytes)");
        config.keys.push_back(name);
    }
    for (const Json& axis : json["axes"].items()) {
        auto found = std::find_if(std::begin(kAxes), std::end(kAxes), [&](const char* name) { return axis.isString() && axis.asString() == name; });
        if (found == std::end(kAxes) || config.axes[static_cast<size_t>(found - std::begin(kAxes))]) return fail("axes must be unique standard gamepad axis names");
        config.axes[static_cast<size_t>(found - std::begin(kAxes))] = true;
    }
    out = std::move(config); return true;
}
Json SyncConfig::ToJson() const {
    Json j = Json::MakeObject(); j["actions"] = Json::MakeArray(); j["axes"] = Json::MakeArray();
    for (const auto& key : keys) j["actions"].push(key);
    for (size_t i = 0; i < axes.size(); ++i) if (axes[i]) j["axes"].push(kAxes[i]);
    j["inputDelay"] = delay; j["rollbackFrames"] = rollbackFrames; j["rollback"] = rollback;
    j["hashInterval"] = hashInterval; j["waitFrames"] = waitFrames; j["empty"] = emptyOnTimeout; return j;
}
FrameSync::FrameSync(SyncConfig config, bool host, uint32_t local, uint64_t blueprint, Send send)
    : config_(std::move(config)), host_(host), local_(local), blueprint_(blueprint), send_(std::move(send)) { ++g_instances; }
uint64_t FrameSync::InstancesCreated() { return g_instances.load(); }
bool FrameSync::Valid(const FrameInput& input) const {
    uint64_t mask = config_.keys.size() == 64 ? UINT64_MAX : (uint64_t{1} << config_.keys.size()) - 1;
    if ((input.down | input.pulse) & ~mask) return false;
    for (size_t i = 0; i < input.axes.size(); ++i) if ((!config_.axes[i] && input.axes[i] != 0) || input.axes[i] == INT16_MIN || (i >= 4 && input.axes[i] < 0)) return false;
    return true;
}
bool FrameSync::Queue(uint32_t player, std::vector<uint8_t> bytes) {
    if (outgoing_.size() >= 256 || queuedBytes_ + bytes.size() > 4 * 1024 * 1024) { outgoing_.clear(); queuedBytes_ = 0; state_ = "stopped"; error_ = "sync send queue overflow"; return false; }
    queuedBytes_ += bytes.size(); outgoing_.emplace_back(player, std::move(bytes)); return true;
}
void FrameSync::Broadcast(const std::vector<uint8_t>& bytes) { for (uint32_t player : players_) if (player != local_) Queue(player, bytes); }
void FrameSync::Begin(const std::vector<uint32_t>& players) {
    dropped_.clear(); players_ = players; ready_.clear(); ready_.insert(local_);
    submitted_.clear(); merged_.clear(); used_.clear(); localInputs_.clear(); hashes_.clear(); outgoing_.clear(); queuedBytes_ = 0;
    gameFrame_ = confirmed_ = 0; rollback_.reset(); report_ = Json::MakeObject(); error_.clear();
    state_ = "preparing"; started_ = waitStarted_ = tick_;
    for (uint64_t frame = 0; frame < config_.delay; ++frame) for (uint32_t player : players_) submitted_[frame][player] = {};
}
bool FrameSync::Start(const std::vector<uint32_t>& players, std::string* error) {
    if (!host_ || Active() || players.empty() || players.size() > 64 || players.front() != 1 || local_ != 1 ||
        !std::is_sorted(players.begin(), players.end()) || std::adjacent_find(players.begin(), players.end()) != players.end() || epoch_ == UINT64_MAX) {
        if (error) *error = "only an idle host can start with a unique sorted ready roster"; return false;
    }
    ++epoch_; Begin(players);
    auto w = Message(BeginMessage, epoch_); w.WriteU64(blueprint_);
    std::string schema = config_.ToJson().dump(); w.WriteBlob(reinterpret_cast<const uint8_t*>(schema.data()), schema.size());
    w.WriteU8(static_cast<uint8_t>(players.size())); for (uint32_t player : players) w.WriteU32(player);
    Broadcast(w.Data()); return true;
}
void FrameSync::Receive(uint32_t sender, const std::vector<uint8_t>& bytes) {
    ByteReader r(bytes.data(), bytes.size()); uint8_t kind = 0; uint64_t epoch = 0;
    if (bytes.size() > 60000 || !r.ReadU8(kind) || !r.ReadU64(epoch) || !epoch || kind < 1 || kind > ReportMessage) { ++rejected_; return; }
    if (kind == BeginMessage && !host_ && sender == 1 && epoch > epoch_) {
        uint64_t blueprint = 0; uint8_t count = 0; std::vector<uint8_t> schema;
        if (!r.ReadU64(blueprint) || !r.ReadBlob(schema, 4096) || !r.ReadU8(count) || count == 0 || count > 64) { ++rejected_; return; }
        std::vector<uint32_t> players; for (unsigned i = 0; i < count; ++i) { uint32_t player = 0; if (!r.ReadU32(player) || !player || player >= 0xfffffffe) { ++rejected_; return; } players.push_back(player); }
        if (r.Remaining() || players.front() != 1 || !std::is_sorted(players.begin(), players.end()) ||
            std::adjacent_find(players.begin(), players.end()) != players.end() || std::find(players.begin(), players.end(), local_) == players.end()) { ++rejected_; return; }
        epoch_ = epoch; Begin(players);
        if (blueprint != blueprint_ || std::string(schema.begin(), schema.end()) != config_.ToJson().dump()) { Stop("match scene/resources/input schema mismatch"); return; }
        Queue(1, Message(ReadyMessage, epoch_).Data()); return;
    }
    if (epoch != epoch_ || std::find(players_.begin(), players_.end(), sender) == players_.end()) { ++rejected_; return; }
    if (kind == StopMessage) {
        std::vector<uint8_t> reason; if (!r.ReadBlob(reason, 256) || r.Remaining()) { ++rejected_; return; }
        if (host_ || sender == 1) Stop(std::string(reason.begin(), reason.end())); return;
    }
    if (kind == ReadyMessage && host_ && state_ == "preparing" && !r.Remaining()) { ready_.insert(sender); return; }
    if (kind == GoMessage && !host_ && sender == 1 && state_ == "preparing" && !r.Remaining()) { state_ = "running"; startPending_ = true; return; }
    uint64_t frame = 0;
    if (!r.ReadU64(frame)) { ++rejected_; return; }
    if (kind == ReportMessage && !host_ && sender == 1) {
        uint32_t player = 0; uint64_t hostHash = 0, peerHash = 0; std::vector<uint8_t> hostScene, peerScene;
        if (!r.ReadU32(player) || !r.ReadU64(hostHash) || !r.ReadU64(peerHash) || !r.ReadBlob(hostScene, 24000) ||
            !r.ReadBlob(peerScene, 24000) || r.Remaining()) { ++rejected_; return; }
        report_["frame"] = frame; report_["player"] = player;
        report_["hostHash"] = std::to_string(hostHash); report_["peerHash"] = std::to_string(peerHash);
        report_["hostScene"] = std::string(hostScene.begin(), hostScene.end());
        report_["peerScene"] = std::string(peerScene.begin(), peerScene.end());
        report_["scenesOmitted"] = hostScene.empty() || peerScene.empty();
        state_ = "desync"; error_ = "desync at frame " + std::to_string(frame); return;
    }
    if (kind == InputMessage && host_ && Running() && frame >= confirmed_ && frame <= gameFrame_ + kFutureFrames) {
        FrameInput input;
        if (!ReadInput(r, input) || r.Remaining() || !Valid(input) || submitted_[frame].count(sender)) { ++rejected_; return; }
        submitted_[frame][sender] = input; return;
    }
    if (kind == MergedMessage && !host_ && sender == 1 && (Running() || state_ == "preparing") &&
        frame + kFutureFrames >= gameFrame_ && frame <= gameFrame_ + kFutureFrames) {
        uint8_t count = 0; FrameInputs inputs;
        if (!r.ReadU8(count) || count != players_.size()) { ++rejected_; return; }
        for (uint32_t player : players_) {
            uint32_t id = 0; FrameInput input;
            if (!r.ReadU32(id) || id != player || !ReadInput(r, input) || !Valid(input)) { ++rejected_; return; }
            inputs[id] = input;
        }
        if (r.Remaining() || merged_.count(frame)) { ++rejected_; return; }
        auto local = localInputs_.find(frame);
        if (local != localInputs_.end() && local->second != inputs.at(local_) && !(config_.emptyOnTimeout && inputs.at(local_) == FrameInput{})) { Stop("host changed local input"); return; }
        Merged(frame, std::move(inputs)); return;
    }
    if (kind == HashMessage && host_ && Running() && frame % config_.hashInterval == 0 && frame <= gameFrame_ + kFutureFrames &&
        frame + 16 * config_.hashInterval >= gameFrame_) {
        uint64_t hash = 0; std::vector<uint8_t> scene; if (!r.ReadU64(hash) || !r.ReadBlob(scene, 24000) || r.Remaining()) { ++rejected_; return; }
        hashes_[frame].values.emplace(sender, hash); hashes_[frame].scenes.emplace(sender, std::string(scene.begin(), scene.end())); CheckHash(frame); return;
    }
    ++rejected_;
}
void FrameSync::Merged(uint64_t frame, FrameInputs inputs) {
    auto used = used_.find(frame);
    if (used != used_.end() && used->second != inputs) rollback_ = rollback_ ? std::min(*rollback_, frame) : frame;
    merged_[frame] = std::move(inputs);
    while (merged_.count(confirmed_)) ++confirmed_;
}
void FrameSync::Commit() {
    while (host_ && Running()) {
        auto it = submitted_.find(confirmed_);
        if (it == submitted_.end() || it->second.size() != players_.size()) break;
        auto w = Message(MergedMessage, epoch_); w.WriteU64(confirmed_); w.WriteU8(static_cast<uint8_t>(players_.size()));
        for (const auto& input : it->second) { w.WriteU32(input.first); WriteInput(w, input.second); }
        Broadcast(w.Data());
        uint64_t frame = confirmed_; FrameInputs inputs = it->second; submitted_.erase(it); Merged(frame, std::move(inputs));
        waitStarted_ = tick_;
    }
}
void FrameSync::Tick(uint64_t tick) {
    tick_ = tick;
    if (state_ == "preparing" && tick_ - started_ >= config_.waitFrames) Stop("match start barrier timed out");
    if (host_ && state_ == "preparing" && ready_.size() == players_.size()) {
        Broadcast(Message(GoMessage, epoch_).Data()); state_ = "running"; startPending_ = true; waitStarted_ = tick_;
    }
    Commit();
    if (host_ && Running() && tick_ - waitStarted_ >= config_.waitFrames) {
        if (config_.emptyOnTimeout) { for (uint32_t player : players_) submitted_[confirmed_].emplace(player, FrameInput{}); Commit(); }
        else {
            for (uint32_t player : players_) if (player != local_ && !submitted_[confirmed_].count(player)) dropped_.push_back(player);
            Stop("missing input timeout; stalled players kicked and match stopped");
        }
    }
    size_t sent = 0;
    while (!outgoing_.empty() && sent++ < 64) {
        auto& message = outgoing_.front(); if (!send_(message.first, message.second)) break;
        queuedBytes_ -= message.second.size(); outgoing_.pop_front();
    }
    uint64_t floor = gameFrame_ > kFutureFrames ? gameFrame_ - kFutureFrames : 0;
    for (auto* map : {&submitted_, &merged_, &used_}) while (!map->empty() && map->begin()->first < floor) map->erase(map->begin());
    while (!localInputs_.empty() && localInputs_.begin()->first < floor) localInputs_.erase(localInputs_.begin());
    while (hashes_.size() > 16) hashes_.erase(hashes_.begin());
}
bool FrameSync::TakeStart() { bool value = startPending_; startPending_ = false; return value; }
bool FrameSync::NeedsInput() const { return Running() && (!config_.rollback || gameFrame_ + config_.delay < kHistoryFrames) && !localInputs_.count(gameFrame_ + config_.delay); }
bool FrameSync::Submit(const FrameInput& input) {
    if (!NeedsInput() || !Valid(input)) return false;
    uint64_t frame = gameFrame_ + config_.delay; localInputs_[frame] = input;
    if (host_) submitted_[frame][local_] = input;
    else { auto w = Message(InputMessage, epoch_); w.WriteU64(frame); WriteInput(w, input); Queue(1, w.Data()); }
    return true;
}
FrameInputs FrameSync::ReplayInputs(uint64_t frame, const FrameInputs& previous, const FrameInputs& old) const {
    auto actual = merged_.find(frame); if (actual != merged_.end()) return actual->second;
    FrameInputs prediction = previous.empty() ? old : previous;
    for (uint32_t player : players_) { prediction[player].pulse = 0; }
    auto known = submitted_.find(frame); if (known != submitted_.end()) for (const auto& input : known->second) prediction[input.first] = input.second;
    auto local = localInputs_.find(frame); if (local != localInputs_.end()) prediction[local_] = local->second;
    return prediction;
}
std::optional<FrameInputs> FrameSync::Next() {
    if (!Running()) return std::nullopt;
    if (config_.rollback && gameFrame_ >= kHistoryFrames) { Stop("match reached the 12000-frame reference history limit"); return std::nullopt; }
    auto actual = merged_.find(gameFrame_); if (actual != merged_.end()) return actual->second;
    if (!config_.rollback || gameFrame_ >= confirmed_ + config_.rollbackFrames) return std::nullopt;
    FrameInputs previous; auto it = used_.find(gameFrame_ == 0 ? 0 : gameFrame_ - 1); if (it != used_.end()) previous = it->second;
    return ReplayInputs(gameFrame_, previous, FrameInputs{});
}
void FrameSync::Applied(const FrameInputs& inputs) { used_[gameFrame_] = inputs; ++gameFrame_; }
std::optional<uint64_t> FrameSync::TakeRollback() { auto value = rollback_; rollback_.reset(); if (value) ++rollbacks_; return value; }
void FrameSync::Replayed(uint64_t frame, const FrameInputs& inputs) { if (frame + kFutureFrames >= gameFrame_) used_[frame] = inputs; }
void FrameSync::Hash(uint64_t frame, uint64_t hash, const std::string& scene) {
    if (!Running() || frame > confirmed_ || frame % config_.hashInterval) return;
    if (host_) { auto& hashes = hashes_[frame]; hashes.values[local_] = hash; hashes.scene = scene.size() <= 24000 ? scene : ""; CheckHash(frame); }
    else { auto w = Message(HashMessage, epoch_); w.WriteU64(frame); w.WriteU64(hash); w.WriteBlob(reinterpret_cast<const uint8_t*>(scene.data()), scene.size() <= 24000 ? scene.size() : 0); Queue(1, w.Data()); }
}
void FrameSync::CheckHash(uint64_t frame) {
    auto it = hashes_.find(frame); if (it == hashes_.end() || !it->second.values.count(1)) return;
    uint64_t reference = it->second.values.at(1);
    for (const auto& value : it->second.values) if (value.second != reference) {
        report_["frame"] = frame; report_["player"] = value.first;
        report_["hostHash"] = std::to_string(reference); report_["peerHash"] = std::to_string(value.second);
        report_["hostScene"] = it->second.scene;
        report_["peerScene"] = it->second.scenes[value.first];
        report_["scenesOmitted"] = it->second.scene.empty() || it->second.scenes[value.first].empty();
        auto w = Message(ReportMessage, epoch_); w.WriteU64(frame); w.WriteU32(value.first); w.WriteU64(reference); w.WriteU64(value.second);
        const auto& hostScene = it->second.scene; const auto& peerScene = it->second.scenes[value.first];
        w.WriteBlob(reinterpret_cast<const uint8_t*>(hostScene.data()), hostScene.size());
        w.WriteBlob(reinterpret_cast<const uint8_t*>(peerScene.data()), peerScene.size()); Broadcast(w.Data());
        Stop("desync at frame " + std::to_string(frame)); state_ = "desync"; break;
    }
}
void FrameSync::Stop(const std::string& reason) {
    if (state_ == "stopped" || state_ == "desync") return;
    state_ = "stopped"; error_ = reason;
    auto w = Message(StopMessage, epoch_ ? epoch_ : 1); w.WriteBlob(reinterpret_cast<const uint8_t*>(reason.data()), std::min<size_t>(reason.size(), 256));
    if (host_) Broadcast(w.Data()); else if (local_ != 1) Queue(1, w.Data());
}
Json FrameSync::State() const {
    Json j = Json::MakeObject(); j["state"] = state_; j["frame"] = gameFrame_; j["confirmed"] = confirmed_;
    j["waiting"] = Running() && (!config_.rollback ? gameFrame_ >= confirmed_ : gameFrame_ >= confirmed_ + config_.rollbackFrames);
    j["predicted"] = gameFrame_ > confirmed_ ? gameFrame_ - confirmed_ : 0; j["rollbacks"] = rollbacks_;
    j["rejected"] = rejected_; j["error"] = error_; j["epoch"] = epoch_; j["backend"] = config_.rollback ? "referenceReplay" : "lockstep"; return j;
}
}  // namespace oe
