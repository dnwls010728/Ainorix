#pragma once
#include <deque>
#include <map>
#include <set>
#include "net/Session.h"

namespace oe {
class Authority {
public:
    using Send = FrameSync::Send;
    Authority(const SessionConfig& config, bool host, uint32_t local, uint64_t blueprint, Send send);
    // Optional loss-tolerant path for snapshots and their ACKs (deltas are built against the last
    // acknowledged image, so a lost snapshot only delays the next one). Without it they use send.
    void SetUnreliableSend(Send send) { unreliable_ = std::move(send); }
    bool Start(const std::vector<uint32_t>& roster, std::string* error);
    void Receive(uint32_t sender, const std::vector<uint8_t>& bytes);
    bool TakeStart();
    void Tick(uint64_t tick);
    bool Running() const { return state_ == "running"; }
    bool Active() const { return Running() || state_ == "preparing"; }
    bool CanPredict() const { return nextInput_ - acknowledged_ <= config_.predictionFrames; }
    uint64_t Submit(const FrameInput& input);
    FrameInputs Consume(const FrameInput& local);
    void Snapshot(uint64_t frame, const std::map<uint32_t, Json>& worlds);
    // Host, running match: removes a departed participant so the others keep playing.
    void Drop(uint32_t player);
    // Participants whose reliable queue overflowed; the caller removes them from the session.
    std::vector<uint32_t> TakeDropped() { auto dropped = std::move(dropped_); dropped_.clear(); return dropped; }
    struct Update { uint64_t sequence = 0, frame = 0, acknowledged = 0; Json world; };
    std::vector<Update> DrainUpdates();
    const std::map<uint64_t, FrameInput>& PendingInputs() const { return pendingInputs_; }
    uint64_t Acknowledged() const { return acknowledged_; }
    const std::vector<uint32_t>& Players() const { return roster_; }
    const SessionConfig& Config() const { return config_; }
    Json State() const;
    void Stop(const std::string& reason);
    static uint64_t InstancesCreated();
private:
    bool Valid(const FrameInput& input) const;
    bool SendMessage(uint32_t player, uint8_t kind, const Json& body);
    void Broadcast(uint8_t kind, const Json& body);
    void Begin();
    SessionConfig config_; bool host_; uint32_t local_; uint64_t blueprint_, epoch_ = 0;
    Send send_, unreliable_; std::string state_ = "idle", error_;
    uint64_t tick_ = 0, preparing_ = 0;
    bool start_ = false; uint64_t rejected_ = 0, nextInput_ = 1, acknowledged_ = 0, sequence_ = 0;
    std::vector<uint32_t> roster_, dropped_; std::set<uint32_t> ready_;
    std::map<uint32_t, std::map<uint64_t, FrameInput>> inputs_;
    std::map<uint32_t, uint64_t> processed_;
    FrameInputs held_;
    std::map<uint64_t, FrameInput> pendingInputs_;
    struct Peer { uint64_t ack = 0; std::map<uint64_t, Json> worlds; };
    std::map<uint32_t, Peer> peers_;
    std::map<uint64_t, Json> received_;
    std::vector<Update> updates_;
};
}  // namespace oe
