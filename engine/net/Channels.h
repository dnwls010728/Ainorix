#pragma once
#include <array>
#include <deque>
#include <map>
#include <set>

#include "net/Packet.h"
#include "net/Transport.h"

namespace oe {

// Deterministic timing policy, measured in explicit simulation frames at the caller's tick rate.
struct ChannelConfig {
    uint32_t retryFrames = 12;       // minimum retransmit interval; RTT adapts it upwards
    uint32_t assemblyFrames = 120;   // incomplete messages expire; whole-message retries recover them
    uint32_t timeoutFrames = 600;    // reliable queue age fails the peer, including transport backpressure
};

// Per-peer diagnostics. Loss counts data attempts whose ACK deadline/window expired,
// not application messages. RTT/jitter are EWMA frame values, not wall-clock milliseconds.
struct ChannelStats {
    uint64_t packetsSent = 0, packetsReceived = 0;
    uint64_t bytesSent = 0, bytesReceived = 0;
    uint64_t ackedPackets = 0, lostPackets = 0, retransmittedPackets = 0;
    uint64_t invalidPackets = 0, droppedPackets = 0, duplicatePackets = 0;
    uint64_t messagesSent = 0, messagesReceived = 0;
    double rttFrames = 0, jitterFrames = 0;
    // Estimate in integer permille; zero before any data attempt has been resolved.
    uint32_t LossPermille() const;
};

// Delivered message or terminal reliable-send timeout. Session code decides leave/kick policy.
struct ChannelEvent {
    enum class Type { Message, TimedOut, Disconnected };
    Type type = Type::Message;
    PeerId peer = 0;
    NetChannel channel = NetChannel::ReliableOrdered;
    uint64_t sequence = 0;
    std::vector<uint8_t> bytes;
    std::string error;  // transport disconnect reason; timeout policy remains session-owned
};

// Explicitly constructed protocol endpoint; owns no sockets, threads or Engine state.
// The transport must outlive it and be polled exclusively through this endpoint.
class ChannelEndpoint {
public:
    explicit ChannelEndpoint(ITransport& transport, const ChannelConfig& config = {});
    ChannelEndpoint(const ChannelEndpoint&) = delete;
    ChannelEndpoint& operator=(const ChannelEndpoint&) = delete;
    static constexpr size_t kMaxPeers = 64;
    static constexpr size_t kMaxQueuedMessages = 64;
    static constexpr size_t kMaxBufferedBytes = 4 * 1024 * 1024;  // each of send/receive per peer
    static constexpr size_t kMaxAssemblies = 64;                // includes ordered completed messages
    static constexpr uint64_t kReliableWindow = 32;
    static constexpr size_t kPacketsPerFrame = 16;
    static constexpr size_t kIncomingPerFrame = 128;
    // Only explicitly registered peers can allocate protocol state (handshake is M4).
    bool AddPeer(PeerId peer);
    bool RemovePeer(PeerId peer);
    // Atomically queues one bounded message; false on invalid channel/peer, timeout or full queue.
    // Queue age starts at the last polled frame (initially 0); poll the current frame before sending.
    bool Send(PeerId peer, NetChannel channel, const uint8_t* bytes, size_t size);
    // Drains incoming packets, expires assemblies, emits ACKs/retries with a fixed send budget.
    // Appends events; same-frame calls are no-ops, backwards frames return false.
    bool Poll(uint64_t frame, std::vector<ChannelEvent>& events);
    // Diagnostics remain valid until RemovePeer; unknown peers return null/zero.
    const ChannelStats* Stats(PeerId peer) const;
    size_t PendingMessages(PeerId peer) const;
    // Combined send/receive reserved payload bytes, excluding headers and caller-owned events.
    size_t BufferedBytes(PeerId peer) const;
    // Malformed, unregistered-peer or rate-limited packets discarded by the endpoint.
    uint64_t RejectedPackets() const { return rejected_; }
    // Diagnostics used by the inactive engine pin (no initialization or per-frame work).
    static uint64_t InstancesCreated();
    static uint64_t PollCalls();
private:
    struct Outgoing {
        NetChannel channel;
        uint64_t sequence;
        std::vector<uint8_t> bytes;
        uint16_t nextFragment = 0;
        uint32_t passes = 0;
        uint64_t queuedFrame = 0, lastFrame = 0;
    };
    struct Assembly {
        uint32_t totalBytes;
        uint16_t fragments;
        uint64_t mask = 0, lastFrame = 0;
        bool complete = false;
        std::vector<uint8_t> bytes;
    };
    using MessageKey = std::pair<NetChannel, uint64_t>;
    struct Peer {
        ChannelStats stats;
        std::array<uint64_t, 4> nextMessage{{1, 1, 1, 1}};
        std::array<uint64_t, 4> receiveFloor{{1, 1, 1, 1}};
        std::array<AckWindow, 4> receivedMessages;
        AckWindow receivedPackets;
        uint64_t nextPacket = 1;
        uint64_t rttFixed = 0, jitterFixed = 0;  // Q8 frames; retry scheduling uses integer arithmetic
        std::deque<Outgoing> outgoing;
        std::map<MessageKey, Assembly> assemblies;
        std::map<uint64_t, uint64_t> sentFrames;
        std::set<MessageKey> messageAcks;
        size_t sendBytes = 0, receiveBytes = 0, incoming = 0;
        bool ackDirty = false, failed = false;
    };
    uint64_t RetryFrames(const Peer& peer) const;
    void ApplyAcks(Peer& peer, const NetPacket& packet);
    bool Receive(PeerId id, Peer& peer, const NetPacket& packet, std::vector<ChannelEvent>& events);
    bool Transmit(PeerId id, Peer& peer, NetPacket& packet, bool retransmit);
    void Flush(PeerId id, Peer& peer);
    void Expire(PeerId id, Peer& peer, std::vector<ChannelEvent>& events);
    ITransport& transport_;
    ChannelConfig config_;
    std::map<PeerId, Peer> peers_;
    uint64_t frame_ = 0, rejected_ = 0;
    bool polled_ = false;
};

}  // namespace oe
