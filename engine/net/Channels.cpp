#include "net/Channels.h"

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <utility>

namespace oe {
namespace {
std::atomic<uint64_t> g_channelInstances{0};
std::atomic<uint64_t> g_channelPolls{0};
constexpr size_t kMaxSentHistory = 256;

size_t ChannelIndex(NetChannel channel) { return static_cast<size_t>(channel); }
uint16_t FragmentCount(size_t bytes) {
    return static_cast<uint16_t>(std::max<size_t>(1, (bytes + kNetFragmentBytes - 1) / kNetFragmentBytes));
}
}

uint32_t ChannelStats::LossPermille() const {
    const double resolved = static_cast<double>(lostPackets) + static_cast<double>(ackedPackets);
    return resolved == 0 ? 0 : static_cast<uint32_t>(1000.0 * static_cast<double>(lostPackets) / resolved);
}

ChannelEndpoint::ChannelEndpoint(ITransport& transport, const ChannelConfig& config)
    : transport_(transport), config_(config) {
    if (config.retryFrames == 0 || config.retryFrames > 120 || config.assemblyFrames == 0 ||
        config.assemblyFrames > 3600 || config.timeoutFrames < config.retryFrames || config.timeoutFrames > 36000)
        throw std::invalid_argument("invalid channel timing configuration");
    ++g_channelInstances;
}
uint64_t ChannelEndpoint::InstancesCreated() { return g_channelInstances.load(); }
uint64_t ChannelEndpoint::PollCalls() { return g_channelPolls.load(); }

bool ChannelEndpoint::AddPeer(PeerId peer) {
    if (peer == 0 || peers_.size() >= kMaxPeers) return false;
    return peers_.emplace(peer, Peer{}).second;
}
bool ChannelEndpoint::RemovePeer(PeerId peer) { return peers_.erase(peer) != 0; }
const ChannelStats* ChannelEndpoint::Stats(PeerId peer) const {
    auto it = peers_.find(peer);
    return it == peers_.end() ? nullptr : &it->second.stats;
}
size_t ChannelEndpoint::PendingMessages(PeerId peer) const {
    auto it = peers_.find(peer);
    return it == peers_.end() ? 0 : it->second.outgoing.size();
}
size_t ChannelEndpoint::BufferedBytes(PeerId peer) const {
    auto it = peers_.find(peer);
    return it == peers_.end() ? 0 : it->second.sendBytes + it->second.receiveBytes;
}

bool ChannelEndpoint::Send(PeerId id, NetChannel channel, const uint8_t* bytes, size_t size) {
    auto it = peers_.find(id);
    if (it == peers_.end() || ChannelIndex(channel) >= 4 || size > kNetMaxMessageBytes || (!bytes && size != 0)) return false;
    Peer& peer = it->second;
    uint64_t& sequence = peer.nextMessage[ChannelIndex(channel)];
    if (peer.failed || sequence == UINT64_MAX || peer.outgoing.size() >= kMaxQueuedMessages ||
        size > kMaxBufferedBytes - peer.sendBytes) return false;
    Outgoing outgoing{channel, sequence, {}};
    outgoing.queuedFrame = frame_;
    if (size != 0) outgoing.bytes.assign(bytes, bytes + size);
    peer.outgoing.push_back(std::move(outgoing));
    peer.sendBytes += size;
    ++sequence;
    ++peer.stats.messagesSent;
    return true;
}

uint64_t ChannelEndpoint::RetryFrames(const Peer& peer) const {
    const uint64_t learned = (peer.rttFixed * 2 + peer.jitterFixed * 4 + 255) / 256;
    return std::clamp<uint64_t>(learned, config_.retryFrames, 120);
}

void ChannelEndpoint::ApplyAcks(Peer& peer, const NetPacket& packet) {
    for (auto it = peer.sentFrames.begin(); it != peer.sentFrames.end();) {
        const uint64_t sequence = it->first;
        if (sequence <= packet.ack && packet.ack - sequence < 64 &&
            (packet.ackMask & (UINT64_C(1) << (packet.ack - sequence))) != 0) {
            // Q8 EWMA keeps retry scheduling independent of floating-point contraction/rounding.
            const int64_t sample = static_cast<int64_t>(std::min<uint64_t>(frame_ - it->second, 36000) * 256);
            if (peer.stats.ackedPackets == 0) peer.rttFixed = static_cast<uint64_t>(sample);
            else {
                const int64_t delta = sample - static_cast<int64_t>(peer.rttFixed);
                const int64_t distance = delta < 0 ? -delta : delta;
                peer.jitterFixed = static_cast<uint64_t>(static_cast<int64_t>(peer.jitterFixed) +
                                                       (distance - static_cast<int64_t>(peer.jitterFixed)) / 4);
                peer.rttFixed = static_cast<uint64_t>(static_cast<int64_t>(peer.rttFixed) + delta / 8);
            }
            peer.stats.rttFrames = static_cast<double>(peer.rttFixed) / 256;
            peer.stats.jitterFrames = static_cast<double>(peer.jitterFixed) / 256;
            ++peer.stats.ackedPackets;
            it = peer.sentFrames.erase(it);
        } else ++it;
    }
    if (packet.kind != PacketKind::Ack || packet.message == 0) return;
    auto it = std::find_if(peer.outgoing.begin(), peer.outgoing.end(), [&](const Outgoing& outgoing) {
        return outgoing.channel == packet.channel && outgoing.sequence == packet.message && outgoing.passes != 0;
    });
    if (it != peer.outgoing.end()) {
        peer.sendBytes -= it->bytes.size();
        peer.outgoing.erase(it);
    }
}

bool ChannelEndpoint::Receive(PeerId id, Peer& peer, const NetPacket& packet, std::vector<ChannelEvent>& events) {
    // A peer cannot acknowledge a packet that has never been sent.
    if (packet.ack >= peer.nextPacket) return false;
    ApplyAcks(peer, packet);
    if (packet.kind == PacketKind::Ack) return true;
    const size_t channel = ChannelIndex(packet.channel);
    const bool reliable = IsReliable(packet.channel);
    const MessageKey key{packet.channel, packet.message};
    auto messageAck = [&]() {
        if (reliable && peer.messageAcks.size() < kMaxQueuedMessages) peer.messageAcks.insert(key);
    };
    auto& received = peer.receivedMessages[channel];
    const uint64_t floor = peer.receiveFloor[channel];
    const bool completed = reliable ? packet.message < floor || received.Contains(packet.message) :
                                     received.Contains(packet.message);
    if (peer.receivedPackets.Contains(packet.sequence)) {
        ++peer.stats.duplicatePackets;
        peer.ackDirty = true;
        if (completed) messageAck();
        return true;
    }
    if (packet.sequence <= peer.receivedPackets.Highest() && peer.receivedPackets.Highest() - packet.sequence >= 64) {
        ++peer.stats.droppedPackets;
        return true;
    }
    if (completed) {
        peer.receivedPackets.Accept(packet.sequence);
        peer.ackDirty = true;
        messageAck();
        ++peer.stats.duplicatePackets;
        return true;
    }
    if (reliable && packet.message - floor >= kReliableWindow) return false;
    if (!reliable && packet.message <= received.Highest() &&
        (packet.channel == NetChannel::Sequenced || received.Highest() - packet.message >= 64)) {
        peer.receivedPackets.Accept(packet.sequence);
        peer.ackDirty = true;
        ++peer.stats.droppedPackets;
        return true;
    }

    auto it = peer.assemblies.find(key);
    if (it == peer.assemblies.end()) {
        if (peer.assemblies.size() >= kMaxAssemblies || packet.totalBytes > kMaxBufferedBytes - peer.receiveBytes) {
            ++peer.stats.droppedPackets;
            return true;  // deliberately no packet/message ACK; sender must retry
        }
        Assembly assembly{packet.totalBytes, packet.fragments, 0, frame_, false, {}};
        assembly.bytes.resize(packet.totalBytes);
        it = peer.assemblies.emplace(key, std::move(assembly)).first;
        peer.receiveBytes += packet.totalBytes;
    }
    Assembly& assembly = it->second;
    if (assembly.totalBytes != packet.totalBytes || assembly.fragments != packet.fragments) return false;
    const uint64_t bit = UINT64_C(1) << packet.fragment;
    if ((assembly.mask & bit) == 0) {
        std::copy(packet.payload.begin(), packet.payload.end(),
                  assembly.bytes.begin() + static_cast<ptrdiff_t>(static_cast<size_t>(packet.fragment) * kNetFragmentBytes));
        assembly.mask |= bit;
    } else ++peer.stats.duplicatePackets;
    assembly.lastFrame = frame_;
    peer.receivedPackets.Accept(packet.sequence);
    peer.ackDirty = true;
    const uint64_t completeMask = assembly.fragments == 64 ? UINT64_MAX : (UINT64_C(1) << assembly.fragments) - 1;
    if (assembly.mask != completeMask) return true;
    assembly.complete = true;
    received.Accept(packet.message);
    messageAck();
    auto deliver = [&](decltype(it) ready) {
        peer.receiveBytes -= ready->second.bytes.size();
        events.push_back({ChannelEvent::Type::Message, id, ready->first.first, ready->first.second,
                          std::move(ready->second.bytes)});
        ++peer.stats.messagesReceived;
        peer.assemblies.erase(ready);
    };
    if (packet.channel == NetChannel::ReliableOrdered) {
        for (;;) {
            auto ready = peer.assemblies.find({packet.channel, peer.receiveFloor[channel]});
            if (ready == peer.assemblies.end() || !ready->second.complete) break;
            deliver(ready);
            ++peer.receiveFloor[channel];
        }
    } else {
        deliver(it);
        if (reliable) {
            while (received.Contains(peer.receiveFloor[channel])) ++peer.receiveFloor[channel];
        } else if (packet.channel == NetChannel::Sequenced) {
            for (auto stale = peer.assemblies.begin(); stale != peer.assemblies.end();) {
                if (stale->first.first == packet.channel && stale->first.second < packet.message) {
                    peer.receiveBytes -= stale->second.bytes.size();
                    stale = peer.assemblies.erase(stale);
                } else ++stale;
            }
        }
    }
    return true;
}

bool ChannelEndpoint::Transmit(PeerId id, Peer& peer, NetPacket& packet, bool retransmit) {
    if (packet.kind == PacketKind::Data && peer.nextPacket == UINT64_MAX) return false;
    packet.sequence = packet.kind == PacketKind::Data ? peer.nextPacket : 0;
    packet.ack = peer.receivedPackets.Highest();
    packet.ackMask = peer.receivedPackets.Mask();
    std::vector<uint8_t> bytes;
    if (!EncodePacket(packet, bytes) || !transport_.Send(id, bytes.data(), bytes.size())) return false;
    ++peer.stats.packetsSent;
    peer.stats.bytesSent += bytes.size();
    peer.ackDirty = false;
    if (packet.kind == PacketKind::Data) {
        if (peer.sentFrames.size() >= kMaxSentHistory) {
            peer.sentFrames.erase(peer.sentFrames.begin());
            ++peer.stats.lostPackets;
        }
        peer.sentFrames.emplace(peer.nextPacket++, frame_);
        if (retransmit) ++peer.stats.retransmittedPackets;
    }
    return true;
}

void ChannelEndpoint::Flush(PeerId id, Peer& peer) {
    size_t budget = kPacketsPerFrame;
    // Reserve most of the budget for data to avoid ACK storms starving queued sends.
    for (size_t i = 0; i < 4 && !peer.messageAcks.empty(); ++i) {
        NetPacket ack;
        ack.kind = PacketKind::Ack;
        ack.channel = peer.messageAcks.begin()->first;
        ack.message = peer.messageAcks.begin()->second;
        if (!Transmit(id, peer, ack, false)) return;
        peer.messageAcks.erase(peer.messageAcks.begin());
        --budget;
    }
    size_t skipped = 0;
    while (budget != 0 && !peer.outgoing.empty() && skipped < peer.outgoing.size()) {
        Outgoing& outgoing = peer.outgoing.front();
        bool eligible = true;
        if (IsReliable(outgoing.channel)) {
            uint64_t oldest = outgoing.sequence;
            for (const auto& candidate : peer.outgoing)
                if (candidate.channel == outgoing.channel) oldest = std::min(oldest, candidate.sequence);
            eligible = outgoing.sequence - oldest < kReliableWindow;
            if (outgoing.nextFragment == 0 && outgoing.passes != 0 && frame_ - outgoing.lastFrame < RetryFrames(peer))
                eligible = false;
        }
        if (!eligible) {
            std::rotate(peer.outgoing.begin(), peer.outgoing.begin() + 1, peer.outgoing.end());
            ++skipped;
            continue;
        }
        NetPacket packet;
        packet.channel = outgoing.channel;
        packet.message = outgoing.sequence;
        packet.fragment = outgoing.nextFragment;
        packet.fragments = FragmentCount(outgoing.bytes.size());
        packet.totalBytes = static_cast<uint32_t>(outgoing.bytes.size());
        const size_t offset = static_cast<size_t>(packet.fragment) * kNetFragmentBytes;
        const size_t count = std::min(kNetFragmentBytes, outgoing.bytes.size() - offset);
        if (count != 0) packet.payload.assign(outgoing.bytes.begin() + static_cast<ptrdiff_t>(offset),
                                             outgoing.bytes.begin() + static_cast<ptrdiff_t>(offset + count));
        if (!Transmit(id, peer, packet, outgoing.passes != 0)) return;
        ++outgoing.nextFragment;
        --budget;
        skipped = 0;
        if (outgoing.nextFragment == packet.fragments) {
            if (!IsReliable(outgoing.channel)) {
                peer.sendBytes -= outgoing.bytes.size();
                peer.outgoing.pop_front();
                continue;
            }
            outgoing.nextFragment = 0;
            ++outgoing.passes;
            outgoing.lastFrame = frame_;
        }
        std::rotate(peer.outgoing.begin(), peer.outgoing.begin() + 1, peer.outgoing.end());
    }
    if (budget != 0 && peer.ackDirty) {
        NetPacket ack;
        ack.kind = PacketKind::Ack;
        Transmit(id, peer, ack, false);
    }
}

void ChannelEndpoint::Expire(PeerId id, Peer& peer, std::vector<ChannelEvent>& events) {
    for (const auto& outgoing : peer.outgoing) {
        if (IsReliable(outgoing.channel) && frame_ - outgoing.queuedFrame >= config_.timeoutFrames) {
            peer.failed = true;
            events.push_back({ChannelEvent::Type::TimedOut, id, outgoing.channel, outgoing.sequence, {}});
            peer.stats.lostPackets += peer.sentFrames.size();
            peer.outgoing.clear();
            peer.assemblies.clear();
            peer.sentFrames.clear();
            peer.messageAcks.clear();
            peer.sendBytes = peer.receiveBytes = 0;
            return;
        }
    }
    const uint64_t deadline = RetryFrames(peer) * 4;
    for (auto it = peer.sentFrames.begin(); it != peer.sentFrames.end();) {
        if (frame_ - it->second >= deadline) {
            ++peer.stats.lostPackets;
            it = peer.sentFrames.erase(it);
        } else ++it;
    }
}

bool ChannelEndpoint::Poll(uint64_t frame, std::vector<ChannelEvent>& events) {
    if (polled_ && frame < frame_) return false;
    if (polled_ && frame == frame_) return true;
    std::vector<TransportEvent> packets;
    if (!transport_.Poll(frame, packets)) return false;
    ++g_channelPolls;
    frame_ = frame;
    polled_ = true;
    for (auto& entry : peers_) {
        Peer& peer = entry.second;
        peer.incoming = 0;
        for (auto it = peer.assemblies.begin(); it != peer.assemblies.end();) {
            // Expire before receiving so a late fragment cannot revive an old partial payload.
            // Completed ordered messages have been ACKed and must survive until the gap closes.
            if (!it->second.complete && frame_ - it->second.lastFrame >= config_.assemblyFrames) {
                peer.receiveBytes -= it->second.bytes.size();
                ++peer.stats.droppedPackets;
                it = peer.assemblies.erase(it);
            } else ++it;
        }
    }
    for (const auto& packet : packets) {
        auto it = peers_.find(packet.peer);
        if (it == peers_.end()) { ++rejected_; continue; }
        Peer& peer = it->second;
        ++peer.stats.packetsReceived;
        peer.stats.bytesReceived += packet.bytes.size();
        if (peer.failed || ++peer.incoming > kIncomingPerFrame) {
            ++peer.stats.droppedPackets;
            ++rejected_;
            continue;
        }
        NetPacket decoded;
        if (!DecodePacket(packet.bytes.data(), packet.bytes.size(), decoded) || !Receive(packet.peer, peer, decoded, events)) {
            ++peer.stats.invalidPackets;
            ++rejected_;
        }
    }
    for (auto& entry : peers_) {
        if (entry.second.failed) continue;
        Expire(entry.first, entry.second, events);
        if (!entry.second.failed) Flush(entry.first, entry.second);
    }
    return true;
}

}  // namespace oe
