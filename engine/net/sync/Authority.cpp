#include "net/sync/Authority.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include "net/Bytes.h"

namespace oe {
namespace {
std::atomic<uint64_t> created{0};
constexpr uint8_t BeginMessage = 32, ReadyMessage = 33, GoMessage = 34, InputMessage = 35,
                  SnapshotMessage = 36, AckMessage = 37, StopMessage = 38;
bool Unsigned(const Json& j, uint64_t limit, uint64_t& value) {
    double n = j.asNumber(-1); if (!j.isNumber() || !std::isfinite(n) || n < 0 || n > static_cast<double>(limit) || std::floor(n) != n) return false;
    value = static_cast<uint64_t>(n); return true;
}
bool SafeJson(const std::string& text) {
    int depth = 0; bool quote = false, escape = false;
    for (char c : text) {
        if (quote) { if (escape) escape = false; else if (c == '\\') escape = true; else if (c == '"') quote = false; }
        else if (c == '"') quote = true;
        else if (c == '[' || c == '{') { if (++depth > 16) return false; }
        else if (c == ']' || c == '}') { if (--depth < 0) return false; }
    }
    return depth == 0 && !quote;
}
Json Encode(const FrameInput& input) {
    Json j = Json::MakeObject(); j["down"] = std::to_string(input.down); j["pulse"] = std::to_string(input.pulse);
    j["axes"] = Json::MakeArray(); for (int16_t axis : input.axes) j["axes"].push(static_cast<int>(axis)); return j;
}
bool Bits(const Json& value, uint64_t& out) {
    std::string text = value.asString(); if (text.empty() || text.size() > 20) return false;
    uint64_t n = 0; for (char c : text) { if (c < '0' || c > '9' || n > (UINT64_MAX - static_cast<unsigned>(c - '0')) / 10) return false; n = n * 10 + static_cast<unsigned>(c - '0'); }
    out = n; return true;
}
bool Decode(const Json& j, FrameInput& input) {
    if (!Bits(j["down"], input.down) || !Bits(j["pulse"], input.pulse) || !j["axes"].isArray() || j["axes"].size() != 6) return false;
    for (size_t i = 0; i < 6; ++i) {
        double n = j["axes"][i].asNumber(-40000); if (!std::isfinite(n) || n < -32767 || n > 32767 || std::floor(n) != n) return false;
        input.axes[i] = static_cast<int16_t>(n);
    }
    return true;
}
Json Delta(const Json& base, const Json& world) {
    Json delta = Json::MakeObject(); delta["set"] = Json::MakeObject(); delta["remove"] = Json::MakeArray();
    for (const auto& entity : world.members()) {
        Json patch = Json::MakeObject();
        if (!base.has(entity.first)) patch = entity.second;
        else for (const auto& field : entity.second.members()) {
            bool always = std::any_of(entity.second["always"].items().begin(), entity.second["always"].items().end(), [&](const Json& name) { return name.asString() == field.first; });
            if (always || base[entity.first][field.first] != field.second) patch[field.first] = field.second;
        }
        // Field removals (including ownerOnly changes) replace the complete entity.
        bool removed = false; for (const auto& field : base[entity.first].members()) removed = removed || !entity.second.has(field.first);
        if (removed) patch = entity.second;
        if (patch.size()) { patch["replace"] = removed || !base.has(entity.first); delta["set"][entity.first] = patch; }
    }
    for (const auto& entity : base.members()) if (!world.has(entity.first)) delta["remove"].push(entity.first);
    return delta;
}
}
Authority::Authority(const SessionConfig& config, bool host, uint32_t local, uint64_t blueprint, Send send)
    : config_(config), host_(host), local_(local), blueprint_(blueprint), send_(std::move(send)) { ++created; }
uint64_t Authority::InstancesCreated() { return created; }
bool Authority::Valid(const FrameInput& input) const {
    uint64_t mask = config_.sync.keys.size() == 64 ? UINT64_MAX : (uint64_t{1} << config_.sync.keys.size()) - 1;
    if ((input.down | input.pulse) & ~mask) return false;
    for (size_t i = 0; i < 6; ++i) if ((!config_.sync.axes[i] && input.axes[i]) || input.axes[i] == -32768 || (i >= 4 && input.axes[i] < 0)) return false;
    return true;
}
bool Authority::SendMessage(uint32_t player, uint8_t kind, const Json& body) {
    std::string text = body.dump(); if (text.size() > 56000) { Stop("authoritative payload exceeds 56000 bytes"); return false; }
    ByteWriter writer(56032); writer.WriteU8(kind); writer.WriteU64(epoch_);
    writer.WriteBlob(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    if (!writer.Ok() || !send_(player, writer.Data())) { state_ = "stopped"; error_ = "authoritative send queue is full"; return false; }
    return true;
}
void Authority::Broadcast(uint8_t kind, const Json& body) { for (uint32_t player : roster_) if (player != local_) SendMessage(player, kind, body); }
void Authority::Begin() {
    Json body = Json::MakeObject(); body["blueprint"] = std::to_string(blueprint_); body["schema"] = config_.sync.ToJson();
    body["players"] = Json::MakeArray(); for (uint32_t player : roster_) body["players"].push(player);
    body["snapshotRate"] = config_.snapshotRate; body["predictionFrames"] = config_.predictionFrames; Broadcast(BeginMessage, body);
}
bool Authority::Start(const std::vector<uint32_t>& roster, std::string* error) {
    if (!host_ || Active() || roster.empty() || roster.size() > config_.maxPlayers || roster.front() != 1 || !std::is_sorted(roster.begin(), roster.end()) || std::adjacent_find(roster.begin(), roster.end()) != roster.end()) {
        if (error) *error = "invalid authoritative start roster/state"; return false;
    }
    roster_ = roster; epoch_++; state_ = "preparing"; preparing_ = tick_; ready_ = {local_}; Begin();
    if (roster_.size() == 1) { state_ = "running"; start_ = true; } return true;
}
void Authority::Receive(uint32_t sender, const std::vector<uint8_t>& bytes) {
    auto reject = [&] { ++rejected_; };
    if (bytes.size() > 56032) { reject(); return; }
    ByteReader reader(bytes.data(), bytes.size()); uint8_t kind; uint64_t epoch; std::vector<uint8_t> blob;
    if (!reader.ReadU8(kind) || !reader.ReadU64(epoch) || !reader.ReadBlob(blob, 56000) || reader.Remaining()) { reject(); return; }
    std::string text(blob.begin(), blob.end()); if (!SafeJson(text)) { reject(); return; }
    Json body; try { body = Json::parse(text); } catch (...) { reject(); return; }
    if (!body.isObject()) { reject(); return; }
    if (kind == BeginMessage && !host_ && sender == 1 && state_ == "idle" && epoch) {
        if (body["blueprint"].asString() != std::to_string(blueprint_) || body["schema"] != config_.sync.ToJson() ||
            body["snapshotRate"].asNumber() != config_.snapshotRate || body["predictionFrames"].asNumber() != config_.predictionFrames) { Stop("authoritative blueprint/schema mismatch"); return; }
        std::vector<uint32_t> roster; if (!body["players"].isArray() || body["players"].size() > config_.maxPlayers) { reject(); return; }
        for (const auto& item : body["players"].items()) { uint64_t id; if (!Unsigned(item, UINT32_MAX - 2, id) || !id) { reject(); return; } roster.push_back(static_cast<uint32_t>(id)); }
        if (roster.empty() || roster.front() != 1 || !std::is_sorted(roster.begin(), roster.end()) || std::adjacent_find(roster.begin(), roster.end()) != roster.end() || std::find(roster.begin(), roster.end(), local_) == roster.end()) { reject(); return; }
        roster_ = roster; epoch_ = epoch; state_ = "preparing"; preparing_ = tick_; SendMessage(1, ReadyMessage, Json::MakeObject()); return;
    }
    if (epoch != epoch_ || !Active() || std::find(roster_.begin(), roster_.end(), sender) == roster_.end()) { reject(); return; }
    if (kind == StopMessage && (host_ || sender == 1)) { Stop(body["reason"].asString("peer stopped")); return; }
    if (kind == ReadyMessage && host_ && state_ == "preparing") {
        ready_.insert(sender); if (ready_.size() == roster_.size()) { state_ = "running"; start_ = true; Broadcast(GoMessage, Json::MakeObject()); } return;
    }
    if (kind == GoMessage && !host_ && sender == 1 && state_ == "preparing") { state_ = "running"; start_ = true; return; }
    if (!Running()) { reject(); return; }
    if (kind == InputMessage && host_ && sender != 1) {
        uint64_t frame; FrameInput input;
        if (!Unsigned(body["frame"], 9007199254740991ULL, frame) || !frame || frame <= processed_[sender] ||
            frame > processed_[sender] + config_.predictionFrames || !Decode(body["input"], input) || !Valid(input) ||
            body.size() != 2 || inputs_[sender].count(frame)) { reject(); return; }
        inputs_[sender][frame] = input; return;
    }
    if (kind == AckMessage && host_) {
        uint64_t sequence; if (!Unsigned(body["sequence"], sequence_, sequence)) { reject(); return; }
        auto& peer = peers_[sender]; if (!peer.worlds.count(sequence) || sequence <= peer.ack) { reject(); return; }
        peer.ack = sequence; return;
    }
    if (kind == SnapshotMessage && !host_ && sender == 1) {
        uint64_t sequence, base, frame, ack;
        if (!Unsigned(body["sequence"], 9007199254740991ULL, sequence) || sequence <= sequence_ ||
            !Unsigned(body["base"], sequence, base) || (base && !received_.count(base)) ||
            !Unsigned(body["frame"], 9007199254740991ULL, frame) ||
            !Unsigned(body["input"], nextInput_ - 1, ack) || ack < acknowledged_ ||
            !body["delta"]["set"].isObject() || !body["delta"]["remove"].isArray() ||
            body["delta"]["set"].size() > 1024 || body["delta"]["remove"].size() > 1024 || updates_.size() >= 16) { reject(); return; }
        Json world = base ? received_.at(base) : Json::MakeObject();
        for (const auto& key : body["delta"]["remove"].items()) { if (!key.isString()) { reject(); return; } world.erase(key.asString()); }
        for (const auto& entity : body["delta"]["set"].members()) {
            if (!entity.second.isObject() || entity.second.size() > 80) { reject(); return; }
            Json patch = entity.second; bool replace = patch["replace"].asBool(); patch.erase("replace");
            if (replace) world[entity.first] = patch;
            else for (const auto& field : patch.members()) world[entity.first][field.first] = field.second;
        }
        if (world.size() > 1024 || world.dump().size() > 56000) { reject(); return; }
        sequence_ = sequence; acknowledged_ = ack; received_[sequence] = world;
        while (received_.size() > 16) received_.erase(received_.begin());
        pendingInputs_.erase(pendingInputs_.begin(), pendingInputs_.upper_bound(ack));
        updates_.push_back({sequence, frame, ack, std::move(world)});
        Json reply = Json::MakeObject(); reply["sequence"] = sequence; SendMessage(1, AckMessage, reply); return;
    }
    reject();
}
void Authority::Tick(uint64_t tick) { tick_ = tick; if (state_ == "preparing" && tick - preparing_ >= config_.sync.waitFrames) Stop("authoritative ready barrier timed out"); }
bool Authority::TakeStart() { bool start = start_; start_ = false; return start; }
uint64_t Authority::Submit(const FrameInput& input) {
    if (host_ || !Running() || !CanPredict() || !Valid(input)) return 0;
    uint64_t frame = nextInput_++; pendingInputs_[frame] = input;
    Json body = Json::MakeObject(); body["frame"] = frame; body["input"] = Encode(input);
    if (!SendMessage(1, InputMessage, body)) return 0; return frame;
}
FrameInputs Authority::Consume(const FrameInput& local) {
    held_[local_] = local;
    for (uint32_t player : roster_) {
        if (player == local_) continue;
        auto& queue = inputs_[player]; auto it = queue.find(processed_[player] + 1);
        held_[player].pulse = 0;
        if (it != queue.end()) { held_[player] = it->second; processed_[player] = it->first; queue.erase(it); }
    }
    return held_;
}
void Authority::Snapshot(uint64_t frame, const std::map<uint32_t, Json>& worlds) {
    if (!host_ || !Running()) return; ++sequence_;
    for (const auto& world : worlds) {
        auto& peer = peers_[world.first]; uint64_t base = peer.worlds.count(peer.ack) ? peer.ack : 0;
        Json body = Json::MakeObject(); body["sequence"] = sequence_; body["base"] = base;
        body["frame"] = frame; body["input"] = processed_[world.first];
        body["delta"] = Delta(base ? peer.worlds.at(base) : Json::MakeObject(), world.second);
        if (SendMessage(world.first, SnapshotMessage, body)) peer.worlds[sequence_] = world.second;
        while (peer.worlds.size() > 16) peer.worlds.erase(peer.worlds.begin());
    }
}
std::vector<Authority::Update> Authority::DrainUpdates() { auto updates = std::move(updates_); updates_.clear(); return updates; }
void Authority::Stop(const std::string& reason) {
    bool broadcast = host_ && Active(); state_ = "stopped"; error_ = reason;
    if (broadcast || (!host_ && epoch_)) { Json body = Json::MakeObject(); body["reason"] = reason.substr(0, 256); if (host_) Broadcast(StopMessage, body); else SendMessage(1, StopMessage, body); }
}
Json Authority::State() const {
    Json j = Json::MakeObject(); j["state"] = state_; j["error"] = error_; j["rejected"] = rejected_;
    j["snapshot"] = sequence_; j["acknowledgedInput"] = acknowledged_; j["pendingInputs"] = static_cast<uint64_t>(pendingInputs_.size());
    j["snapshotRate"] = config_.snapshotRate; j["interpolationFrames"] = config_.interpolationFrames;
    return j;
}
}  // namespace oe
