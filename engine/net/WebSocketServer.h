#pragma once
#include <deque>
#include <map>
#include <memory>
#include "net/Transport.h"
#include "platform/Network.h"

namespace oe {
// RFC 6455 server handshake and masked binary/continuation/control frame decoder.
// No extensions/text messages; 8 KiB headers, 64 KiB assembled messages, sticky failure.
class WebSocketServerReader {
public:
    struct Frame { uint8_t opcode; std::vector<uint8_t> bytes; };
    bool Feed(const uint8_t* bytes, size_t size, std::vector<Frame>& frames);
    bool Ready() const { return ready_; }
    std::string TakeUpgrade();
    // Unmasked final server frame, bounded to one transport message.
    static std::vector<uint8_t> Encode(uint8_t opcode, const uint8_t* bytes, size_t size);
private:
    std::vector<uint8_t> buffer_, fragments_;
    std::string upgrade_;
    bool ready_ = false, ok_ = true, fragmented_ = false, closed_ = false;
};
// Nonblocking native WebSocket listener, sharing Session's binary datagram protocol.
// TLS belongs to a reverse proxy; the engine binds numeric IPv4, loopback by default.
class WebSocketServerTransport final : public ITransport {
public:
    bool Listen(const NetAddress& address, std::string* error);
    NetAddress LocalAddress() const;
    bool Disconnect(PeerId peer);
    bool Send(PeerId peer, const uint8_t* bytes, size_t size) override;
    bool Poll(uint64_t frame, std::vector<TransportEvent>& events) override;
private:
    struct Peer {
        std::unique_ptr<NetSocket> socket;
        WebSocketServerReader reader;
        std::deque<std::vector<uint8_t>> outgoing;
        size_t queued = 0, offset = 0;
        uint64_t accepted = 0;
        bool notified = false, closing = false;
    };
    bool Queue(Peer& peer, std::vector<uint8_t> bytes);
    std::unique_ptr<NetSocket> listener_;
    std::map<PeerId, Peer> peers_;
    std::vector<TransportEvent> notices_;
    PeerId nextPeer_ = 1;
    uint64_t frame_ = 0;
    bool polled_ = false;
};
}  // namespace oe
