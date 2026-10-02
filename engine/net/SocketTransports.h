#pragma once
#include <array>
#include <deque>
#include <map>

#include "net/Transport.h"
#include "platform/Network.h"

namespace oe {

// Bounded incremental uint32-little-endian length framing, shared by all native TCP peers.
class TcpFrameReader {
public:
    // Handles split headers/bodies and coalesced frames; sticky failure on malformed/over-rate input.
    bool Feed(const uint8_t* bytes, size_t size, std::vector<std::vector<uint8_t>>& frames);
    bool Ok() const { return ok_; }
    size_t BufferedBytes() const { return body_.size(); }
    // True for a partial prefix or body; used to diagnose EOF in the middle of a frame.
    bool Incomplete() const { return prefixBytes_ != 0; }
private:
    std::array<uint8_t, 4> prefix_{};
    size_t prefixBytes_ = 0, bodyBytes_ = 0;
    std::vector<uint8_t> body_;
    bool ok_ = true;
};

// UDP never creates peer state from traffic; only registered exact source addresses are delivered.
class UdpTransport final : public ITransport {
public:
    static constexpr size_t kMaxDatagramBytes = 1200;
    static constexpr size_t kMaxPeers = 64;
    // Bind once; defaults to loopback/ephemeral port. A non-loopback address is explicit opt-in.
    bool Bind(const NetAddress& address, std::string* error);
    // Both id and address must be unique; incoming packets never create peers.
    bool AddPeer(PeerId peer, const NetAddress& address);
    bool RemovePeer(PeerId peer);
    // Bound endpoint, or port zero before binding.
    NetAddress LocalAddress() const;
    bool Send(PeerId peer, const uint8_t* bytes, size_t size) override;
    bool Poll(uint64_t frame, std::vector<TransportEvent>& events) override;
    uint64_t DroppedPackets() const { return dropped_; }
    const std::string& LastError() const { return error_; }
private:
    std::unique_ptr<NetSocket> socket_;
    std::map<PeerId, NetAddress> peers_;
    uint64_t frame_ = 0, dropped_ = 0;
    bool polled_ = false;
    std::string error_;
};

// Length-prefixed reliable byte streams; TCP_NODELAY is enforced by the platform socket factory.
// Accepted peers receive monotonically assigned ids and Connected/Disconnected events.
class TcpTransport final : public ITransport {
public:
    static constexpr size_t kMaxPeers = 64;
    static constexpr size_t kMaxQueuedFrames = 256;
    static constexpr size_t kMaxQueuedBytes = 4 * 1024 * 1024;
    // One non-blocking listener; defaults to loopback and an ephemeral port.
    bool Listen(const NetAddress& address, std::string* error);
    // Connect is asynchronous; Send can queue while connecting. Timeout: 300 explicit frames.
    bool Connect(PeerId peer, const NetAddress& address, std::string* error);
    // Immediate close discards queued bytes and reports a disconnect at the next distinct Poll.
    bool Disconnect(PeerId peer);
    // Listener endpoint, or port zero when this transport only connects as a client.
    NetAddress LocalAddress() const;
    // Accepted source endpoint for session IP validation; unknown peers have port zero.
    NetAddress PeerAddress(PeerId peer) const;
    bool Send(PeerId peer, const uint8_t* bytes, size_t size) override;
    bool Poll(uint64_t frame, std::vector<TransportEvent>& events) override;
    // Reserved transmit-buffer bytes, including prefixes and partially sent frames.
    size_t QueuedBytes(PeerId peer) const;
    const std::string& LastError() const { return error_; }
private:
    struct Peer {
        std::unique_ptr<NetSocket> socket;
        NetAddress address;
        TcpFrameReader reader;
        std::deque<std::vector<uint8_t>> outgoing;
        size_t queuedBytes = 0, offset = 0;
        uint64_t connectFrame = 0;
        bool notified = false;
    };
    std::unique_ptr<NetSocket> listener_;
    std::map<PeerId, Peer> peers_;
    std::vector<TransportEvent> notices_;
    PeerId nextPeer_ = 1;
    uint64_t frame_ = 0;
    bool polled_ = false;
    std::string error_;
};

// Browser binary WebSocket messages already preserve datagram boundaries; no TCP prefix is needed.
class WebSocketTransport final : public ITransport {
public:
    // Asynchronous browser connection; native platforms return false with an explanatory error.
    bool Connect(PeerId peer, const std::string& url, std::string* error);
    // Removes browser handlers, drops queued data and reports a disconnect at the next distinct Poll.
    bool Disconnect(PeerId peer);
    bool Send(PeerId peer, const uint8_t* bytes, size_t size) override;
    bool Poll(uint64_t frame, std::vector<TransportEvent>& events) override;
private:
    struct Peer {
        std::unique_ptr<PlatformWebSocket> socket;
        std::deque<std::vector<uint8_t>> outgoing;
        size_t queuedBytes = 0;
        uint64_t connectFrame = 0;
        bool notified = false;
    };
    std::map<PeerId, Peer> peers_;
    std::vector<TransportEvent> notices_;
    uint64_t frame_ = 0;
    bool polled_ = false;
};

}  // namespace oe
