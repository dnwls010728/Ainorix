#pragma once
#include <map>
#include <string>
#include <vector>

#include "core/Json.h"
#include "net/SocketTransports.h"

namespace oe {

// Outbound framed TCP connections of a game to services that are not engine sessions (a login
// server, a lobby service, a database front end). The wire is the TcpTransport framing with a
// message type in front of the payload:
//
//   u32 length (of what follows, <= 65536)   u16 type   payload
//
// all little endian. Nothing blocks: Pump() moves bytes once per call (the engine calls it at the
// frame boundary), state and messages are read afterwards. Like the host bridge this is device
// state outside the deterministic simulation. No socket exists before the first Open.
class LinkSet {
public:
    static constexpr size_t kMaxLinks = 8;
    static constexpr size_t kMaxPayloadBytes = ITransport::kMaxMessageBytes - 2;
    static constexpr size_t kMaxQueuedBytes = 256 * 1024;  // unsent bytes per link; more closes the link
    static constexpr size_t kMaxInboxMessages = 512;       // unread messages per link; more closes the link
    struct Message {
        uint16_t type = 0;
        std::vector<uint8_t> payload;
    };
    // Starts connecting to a numeric IPv4 endpoint. Returns the link id (never reused), 0 on failure.
    // When all slots are used, the oldest closed link is forgotten to make room.
    uint32_t Open(const NetAddress& address, std::string* error);
    // Queues one message. False (with `error`) for an unknown or closed link or an oversized payload;
    // a full send queue closes the link.
    bool Send(uint32_t id, uint16_t type, const uint8_t* payload, size_t size, std::string* error);
    // Unread messages in arrival order; the link keeps none of them.
    std::vector<Message> Take(uint32_t id);
    // "connecting", "open", "closed"; empty for an unknown id.
    std::string State(uint32_t id) const;
    // Why a closed link closed (the other side, a connect failure, a limit).
    std::string Error(uint32_t id) const;
    // Closes and forgets the link; unsent bytes are dropped.
    bool Close(uint32_t id);
    void Clear();
    void Pump();
    size_t Count() const { return links_.size(); }
    // [{id, address, port, state, error, pending, queuedBytes}]
    Json Status() const;

private:
    struct Entry {
        NetAddress address;
        std::string state = "connecting", error;
        std::vector<Message> inbox;
    };
    void Fail(uint32_t id, Entry& entry, const std::string& reason);
    TcpTransport wire_;
    std::map<uint32_t, Entry> links_;
    uint32_t next_ = 1;
    uint64_t tick_ = 0;
};

}  // namespace oe
