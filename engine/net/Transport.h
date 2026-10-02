#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace oe {

// Transport addresses are local handles, not session player ids. Zero is invalid.
using PeerId = uint32_t;

// One datagram received from a peer. Upper layers validate framing and session ids.
struct TransportEvent {
    PeerId peer = 0;
    std::vector<uint8_t> bytes;
};

// Single-threaded, non-blocking byte transport, independent of the sync model.
// No sockets or workers are opened implicitly by the engine.
class ITransport {
public:
    virtual ~ITransport() = default;
    // Hard datagram limit shared by M1 transports; packet fragmentation is M2.
    static constexpr size_t kMaxMessageBytes = 64 * 1024;
    // True means accepted (simulated loss may still discard it). False means invalid/full.
    virtual bool Send(PeerId peer, const uint8_t* data, size_t size) = 0;
    // Explicit monotonic simulation frame, never wall time. Appends to events.
    // False rejects a backwards frame without changing state or output.
    virtual bool Poll(uint64_t frame, std::vector<TransportEvent>& events) = 0;
};

}  // namespace oe
