#pragma once
#include <memory>
#include <set>

#include "net/Transport.h"

namespace oe {

// All delays are simulation frames; probabilities are integer parts per thousand.
struct LoopbackConfig {
    uint32_t seed = 1;
    uint32_t latencyFrames = 0;
    uint32_t jitterFrames = 0;  // uniform +/- jitter, clamped at zero
    uint32_t lossPermille = 0;
    uint32_t duplicatePermille = 0;
    uint32_t reorderFrames = 0;  // additional uniform delay, permitting overtaking
};

// Shared in-memory wire. Endpoint destruction removes all traffic to/from that id.
// Only explicit construction allocates networking state; no global network instance.
class LoopbackNetwork {
public:
    // Invalid local configuration throws std::invalid_argument before creating peers.
    explicit LoopbackNetwork(const LoopbackConfig& config = {});
    static constexpr size_t kMaxPeers = 64;
    static constexpr size_t kMaxQueuedMessages = 1024;
    static constexpr size_t kMaxQueuedBytes = 4 * 1024 * 1024;
    size_t PendingMessages() const { return pending_.size(); }
    size_t PendingBytes() const { return pendingBytes_; }
    uint64_t DroppedMessages() const { return dropped_; }
    // Monotonic diagnostic counters used to pin inactive engine initialization/ticks.
    static uint64_t InstancesCreated();
    static uint64_t PollCalls();
private:
    friend class LoopbackTransport;
    struct Pending {
        PeerId from;
        PeerId to;
        uint64_t due;
        uint64_t order;
        std::vector<uint8_t> bytes;
    };
    uint32_t Random();
    uint64_t Delay();
    void RemovePeer(PeerId peer);
    LoopbackConfig config_;
    uint32_t random_;
    std::set<PeerId> peers_;
    std::vector<Pending> pending_;
    size_t pendingBytes_ = 0;
    uint64_t nextOrder_ = 0;
    uint64_t dropped_ = 0;
};

// Endpoints share a wire and use unique, nonzero ids. Poll before Send each tick;
// Send uses the sender's last polled frame (initially 0). Fixed call order is required
// for deterministic fault injection. Receiver Poll order does not change queued deadlines.
class LoopbackTransport final : public ITransport {
public:
    // Null wire, duplicate/zero id, or too many peers throws std::invalid_argument.
    LoopbackTransport(std::shared_ptr<LoopbackNetwork> network, PeerId localPeer);
    ~LoopbackTransport() override;
    LoopbackTransport(const LoopbackTransport&) = delete;
    LoopbackTransport& operator=(const LoopbackTransport&) = delete;
    bool Send(PeerId peer, const uint8_t* data, size_t size) override;
    bool Poll(uint64_t frame, std::vector<TransportEvent>& events) override;
private:
    std::shared_ptr<LoopbackNetwork> network_;
    PeerId localPeer_;
    uint64_t frame_ = 0;
};

}  // namespace oe
