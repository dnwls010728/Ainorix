#include "net/FallbackTransport.h"

#include <stdexcept>
#include <utility>

#include "net/Bytes.h"

namespace oe {
namespace {
constexpr uint32_t kProbeMagic = 0x5055454f;  // "OEUP", separate from the packet codec's "OENP"
std::vector<uint8_t> Probe(uint8_t kind, uint64_t challenge) {
    ByteWriter writer(14);
    writer.WriteU32(kProbeMagic);
    writer.WriteU8(1);
    writer.WriteU8(kind);
    writer.WriteU64(challenge);
    return writer.Data();
}
bool ReadProbe(const std::vector<uint8_t>& bytes, uint8_t& kind, uint64_t& challenge) {
    if (bytes.size() != 14) return false;
    ByteReader reader(bytes.data(), bytes.size());
    uint32_t magic = 0;
    uint8_t version = 0;
    return reader.ReadU32(magic) && magic == kProbeMagic && reader.ReadU8(version) && version == 1 &&
           reader.ReadU8(kind) && (kind == 1 || kind == 2) && reader.ReadU64(challenge) && challenge != 0;
}
}
FallbackTransport::FallbackTransport(ITransport& udp, ITransport& tcp, uint32_t probeFrames, bool forwardControl)
    : udp_(udp), tcp_(tcp), probeFrames_(probeFrames), forwardControl_(forwardControl) {
    if (probeFrames == 0 || probeFrames > 3600) throw std::invalid_argument("invalid UDP probe timeout");
}
bool FallbackTransport::AddPeer(PeerId id, uint64_t challengeSeed) {
    if (!id || !challengeSeed || challengeSeed == UINT64_MAX || peers_.size() >= 64 || peers_.count(id)) return false;
    Peer peer;
    peer.challenge = challengeSeed;
    peer.probeStart = frame_;
    peers_.emplace(id, peer);
    return true;
}
bool FallbackTransport::RemovePeer(PeerId peer) { return peers_.erase(peer) != 0; }
FallbackTransport::Route FallbackTransport::Selected(PeerId peer) const {
    auto it = peers_.find(peer);
    return it == peers_.end() ? Route::Tcp : it->second.route;
}
bool FallbackTransport::Send(PeerId peer, const uint8_t* bytes, size_t size) {
    auto it = peers_.find(peer);
    if (it == peers_.end()) return false;
    return it->second.route == Route::Udp ? udp_.Send(peer, bytes, size) : tcp_.Send(peer, bytes, size);
}
bool FallbackTransport::Poll(uint64_t frame, std::vector<TransportEvent>& events) {
    if (polled_ && frame < frame_) return false;
    if (polled_ && frame == frame_) return true;
    std::vector<TransportEvent> udpEvents, tcpEvents;
    // A failed UDP socket must not prevent the control/fallback stream from advancing.
    const bool udpOk = udp_.Poll(frame, udpEvents);
    if (!tcp_.Poll(frame, tcpEvents)) return false;
    frame_ = frame;
    polled_ = true;
    for (auto& event : tcpEvents) {
        if (!peers_.count(event.peer) && !forwardControl_) continue;
        if (event.type == TransportEvent::Type::Disconnected) peers_.erase(event.peer);
        events.push_back(std::move(event));
    }
    std::map<PeerId, size_t> responses;
    for (auto& event : udpEvents) {
        auto it = peers_.find(event.peer);
        if (it == peers_.end()) continue;
        Peer& peer = it->second;
        uint8_t kind = 0;
        uint64_t challenge = 0;
        if (event.type == TransportEvent::Type::Data && ReadProbe(event.bytes, kind, challenge)) {
            if (kind == 1 && ++responses[event.peer] <= 1) {
                auto reply = Probe(2, challenge);
                udp_.Send(event.peer, reply.data(), reply.size());
            } else if (kind == 2 && peer.probing && challenge == peer.challenge && peer.route != Route::Tcp) {
                peer.route = Route::Udp;
                peer.probing = false;
                peer.lastConfirmed = frame;
            }
        } else if (event.type == TransportEvent::Type::Data && peer.route == Route::Udp) {
            event.datagram = true;
            events.push_back(std::move(event));
        }
    }
    for (auto& entry : peers_) {
        Peer& peer = entry.second;
        if (!udpOk) { peer.route = Route::Tcp; peer.probing = false; }
        if (peer.route == Route::Tcp) continue;
        if (!peer.probing && frame - peer.lastConfirmed >= probeFrames_) {
            if (++peer.challenge == UINT64_MAX) { peer.route = Route::Tcp; continue; }
            peer.probing = true;
            peer.probeStart = frame;
            peer.sent = false;
        }
        if (!peer.probing) continue;
        if (frame - peer.probeStart >= probeFrames_) { peer.route = Route::Tcp; peer.probing = false; continue; }
        if (!peer.sent || frame - peer.lastSend >= 6) {
            auto probe = Probe(1, peer.challenge);
            udp_.Send(entry.first, probe.data(), probe.size());
            peer.lastSend = frame;
            peer.sent = true;
        }
    }
    return true;
}
}  // namespace oe
