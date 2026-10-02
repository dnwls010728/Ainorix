#include "net/LoopbackTransport.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <utility>

namespace oe {
namespace {
std::atomic<uint64_t> g_instancesCreated{0};
std::atomic<uint64_t> g_pollCalls{0};
constexpr uint32_t kMaxDelayFrames = 3600;
}

LoopbackNetwork::LoopbackNetwork(const LoopbackConfig& config) : config_(config), random_(config.seed ? config.seed : 1) {
    if (config.lossPermille > 1000 || config.duplicatePermille > 1000 ||
        config.latencyFrames > kMaxDelayFrames || config.jitterFrames > kMaxDelayFrames ||
        config.reorderFrames > kMaxDelayFrames) throw std::invalid_argument("invalid loopback fault configuration");
    ++g_instancesCreated;
}
uint64_t LoopbackNetwork::InstancesCreated() { return g_instancesCreated.load(); }
uint64_t LoopbackNetwork::PollCalls() { return g_pollCalls.load(); }

uint32_t LoopbackNetwork::Random() {
    // xorshift32: explicit unsigned arithmetic is identical on all targets.
    random_ ^= random_ << 13;
    random_ ^= random_ >> 17;
    random_ ^= random_ << 5;
    return random_;
}
uint64_t LoopbackNetwork::Delay() {
    int64_t delay = config_.latencyFrames;
    if (config_.jitterFrames) {
        delay += static_cast<int64_t>(Random() % (config_.jitterFrames * 2 + 1)) - config_.jitterFrames;
    }
    delay = std::max<int64_t>(delay, 0);
    if (config_.reorderFrames) delay += Random() % (config_.reorderFrames + 1);
    return static_cast<uint64_t>(delay);
}
void LoopbackNetwork::RemovePeer(PeerId peer) {
    peers_.erase(peer);
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const Pending& packet) {
        if (packet.from != peer && packet.to != peer) return false;
        pendingBytes_ -= packet.bytes.size();
        return true;
    }), pending_.end());
}

LoopbackTransport::LoopbackTransport(std::shared_ptr<LoopbackNetwork> network, PeerId localPeer)
    : network_(std::move(network)), localPeer_(localPeer) {
    if (!network_ || localPeer == 0 || network_->peers_.size() >= LoopbackNetwork::kMaxPeers ||
        !network_->peers_.insert(localPeer).second) throw std::invalid_argument("invalid or duplicate loopback peer");
}
LoopbackTransport::~LoopbackTransport() { network_->RemovePeer(localPeer_); }

bool LoopbackTransport::Send(PeerId peer, const uint8_t* data, size_t size) {
    auto& wire = *network_;
    auto reject = [&]() { ++wire.dropped_; return false; };
    if (size > kMaxMessageBytes || (!data && size != 0) || wire.peers_.count(peer) == 0) return reject();
    if (wire.pending_.size() >= LoopbackNetwork::kMaxQueuedMessages ||
        size > LoopbackNetwork::kMaxQueuedBytes - wire.pendingBytes_) return reject();
    if (wire.config_.lossPermille && wire.Random() % 1000 < wire.config_.lossPermille) {
        ++wire.dropped_;
        return true;
    }
    const bool duplicate = wire.config_.duplicatePermille && wire.Random() % 1000 < wire.config_.duplicatePermille;
    const size_t copies = duplicate ? 2 : 1;
    if (copies > LoopbackNetwork::kMaxQueuedMessages - wire.pending_.size() ||
        size > (LoopbackNetwork::kMaxQueuedBytes - wire.pendingBytes_) / copies) return reject();
    // Check both deadlines before enqueuing either copy (no partial acceptance).
    uint64_t delays[2] = {wire.Delay(), 0};
    if (duplicate) delays[1] = wire.Delay();
    for (size_t i = 0; i < copies; ++i) {
        if (delays[i] > std::numeric_limits<uint64_t>::max() - frame_) return reject();
    }
    if (wire.nextOrder_ > std::numeric_limits<uint64_t>::max() - copies) return reject();
    for (size_t i = 0; i < copies; ++i) {
        LoopbackNetwork::Pending packet{localPeer_, peer, frame_ + delays[i], wire.nextOrder_++, {}};
        if (size != 0) packet.bytes.assign(data, data + size);
        wire.pending_.push_back(std::move(packet));
        wire.pendingBytes_ += size;
    }
    return true;
}

bool LoopbackTransport::Poll(uint64_t frame, std::vector<TransportEvent>& events) {
    if (frame < frame_) return false;
    ++g_pollCalls;
    frame_ = frame;
    auto& wire = *network_;
    std::sort(wire.pending_.begin(), wire.pending_.end(), [](const LoopbackNetwork::Pending& a,
                                                          const LoopbackNetwork::Pending& b) {
        return a.due < b.due || (a.due == b.due && a.order < b.order);
    });
    wire.pending_.erase(std::remove_if(wire.pending_.begin(), wire.pending_.end(), [&](LoopbackNetwork::Pending& packet) {
        if (packet.to != localPeer_ || packet.due > frame) return false;
        wire.pendingBytes_ -= packet.bytes.size();
        events.push_back({packet.from, std::move(packet.bytes)});
        return true;
    }), wire.pending_.end());
    return true;
}

}  // namespace oe
