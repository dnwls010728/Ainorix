#pragma once
#include <map>

#include "net/Transport.h"

namespace oe {

// Established TCP remains usable while registered UDP endpoints prove bidirectional reachability.
// Echo challenges are reachability probes, not authentication; M4 supplies session/cookie validation.
class FallbackTransport final : public ITransport {
public:
    enum class Route { Probing, Udp, Tcp };
    FallbackTransport(ITransport& udp, ITransport& tcp, uint32_t probeFrames = 30, bool forwardControl = false);
    // Nonzero challenge seed must differ across connection incarnations (from the session layer).
    // Probe age uses the last polled frame, initially zero; poll the current frame before registering.
    bool AddPeer(PeerId peer, uint64_t challengeSeed);
    bool RemovePeer(PeerId peer);
    // Unknown peers report Tcp but reject Send. The route only selects the send path: registered
    // peers are received on both TCP and UDP, so the two sides may select different routes.
    Route Selected(PeerId peer) const;
    bool Send(PeerId peer, const uint8_t* bytes, size_t size) override;
    bool Poll(uint64_t frame, std::vector<TransportEvent>& events) override;
private:
    struct Peer {
        Route route = Route::Probing;
        uint64_t challenge = 0, probeStart = 0, lastConfirmed = 0, lastSend = 0;
        bool probing = true, sent = false;
    };
    ITransport& udp_;
    ITransport& tcp_;
    uint32_t probeFrames_;
    uint64_t frame_ = 0;
    bool polled_ = false;
    bool forwardControl_ = false;  // sessions validate TCP handshakes before peer registration
    std::map<PeerId, Peer> peers_;
};

}  // namespace oe
