#pragma once
#include <array>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>

#include "core/Json.h"

namespace oe {
// Explicit action schema; inputs are bounded bitfields and signed normalized axes.
struct SyncConfig {
    std::vector<std::string> keys;
    std::array<bool, 6> axes{};
    // LookX/LookY travel in the RightX/RightY slots (same wire format) but carry InputState::lookX/lookY.
    std::array<bool, 2> look{};
    uint32_t delay = 2, rollbackFrames = 8, hashInterval = 60, waitFrames = 300;
    bool rollback = false, emptyOnTimeout = false;
    static bool Parse(const Json& network, SyncConfig& out, std::string* error);
    Json ToJson() const;
};
struct FrameInput {
    uint64_t down = 0, pulse = 0;
    std::array<int16_t, 6> axes{};
    bool operator==(const FrameInput& other) const { return down == other.down && pulse == other.pulse && axes == other.axes; }
    bool operator!=(const FrameInput& other) const { return !(*this == other); }
};
using FrameInputs = std::map<uint32_t, FrameInput>;

// Transport-independent bounded star coordinator. Tick is I/O time; Frame is game time.
// Session authenticates sender ids; no network data advances the engine here.
class FrameSync {
public:
    using Send = std::function<bool(uint32_t, const std::vector<uint8_t>&)>;
    FrameSync(SyncConfig config, bool host, uint32_t local, uint64_t blueprint, Send send);
    // Host starts with a frozen sorted roster. Begin/ready/go form a barrier before frame zero.
    bool Start(const std::vector<uint32_t>& players, std::string* error);
    void Receive(uint32_t sender, const std::vector<uint8_t>& bytes);
    void Tick(uint64_t tick);
    bool TakeStart();
    std::vector<uint32_t> TakeDropped() { auto dropped = std::move(dropped_); dropped_.clear(); return dropped; }
    // Capture exactly once for frame+delay, preserving input while a frame waits.
    bool NeedsInput() const;
    bool Submit(const FrameInput& input);
    std::optional<FrameInputs> Next();
    void Applied(const FrameInputs& inputs);
    // Late authoritative inputs mark the earliest differing predicted frame.
    std::optional<uint64_t> TakeRollback();
    FrameInputs ReplayInputs(uint64_t frame, const FrameInputs& previous, const FrameInputs& old) const;
    void Replayed(uint64_t frame, const FrameInputs& inputs);
    // Hash only confirmed worlds. Host relays a bounded first mismatch report.
    void Hash(uint64_t frame, uint64_t hash, const std::string& scene);
    Json Report() const { return report_; }
    Json State() const;
    void Stop(const std::string& reason);
    bool Active() const { return state_ == "running" || state_ == "preparing"; }
    bool Running() const { return state_ == "running"; }
    // A client whose confirmed inputs are well ahead of its simulation should step extra frames
    // (Engine does, up to kCatchUpFrames per tick) so a peer that fell behind rejoins the pace.
    bool Lagging() const { return !host_ && Running() && confirmed_ > gameFrame_ + config_.delay + 2; }
    uint64_t Frame() const { return gameFrame_; }
    uint64_t Confirmed() const { return confirmed_; }
    const std::vector<uint32_t>& Players() const { return players_; }
    const SyncConfig& Config() const { return config_; }
    static uint64_t InstancesCreated();
    static constexpr uint64_t kFutureFrames = 32;
    // Confirmed frames a late client may buffer ahead of its simulation (one minute).
    static constexpr uint64_t kBacklogFrames = 3600;
    static constexpr int kCatchUpFrames = 4;
private:
    bool Valid(const FrameInput& input) const;
    bool Queue(uint32_t player, std::vector<uint8_t> bytes);
    void Broadcast(const std::vector<uint8_t>& bytes);
    void Begin(const std::vector<uint32_t>& players);
    void Commit();
    void Merged(uint64_t frame, FrameInputs inputs);
    void CheckHash(uint64_t frame);
    SyncConfig config_;
    bool host_, startPending_ = false;
    uint32_t local_;
    uint64_t blueprint_, epoch_ = 0, tick_ = 0, started_ = 0, waitStarted_ = 0;
    uint64_t gameFrame_ = 0, confirmed_ = 0, rejected_ = 0, rollbacks_ = 0;
    std::optional<uint64_t> rollback_;
    Send send_;
    std::string state_ = "idle", error_;
    std::vector<uint32_t> players_;
    std::set<uint32_t> ready_;
    // dropPolicy "empty": players whose input timed out. Their missing frames are neutral without
    // another wait until one of their inputs arrives close to the confirmed frame again.
    std::set<uint32_t> stalled_;
    std::vector<uint32_t> dropped_;
    std::map<uint64_t, FrameInputs> submitted_, merged_, used_;
    std::map<uint64_t, FrameInput> localInputs_;
    struct Hashes { std::map<uint32_t, uint64_t> values; std::string scene; std::map<uint32_t, std::string> scenes; };
    std::map<uint64_t, Hashes> hashes_;
    std::deque<std::pair<uint32_t, std::vector<uint8_t>>> outgoing_;
    size_t queuedBytes_ = 0;
    Json report_ = Json::MakeObject();
};
}  // namespace oe
