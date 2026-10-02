#include "net/SocketTransports.h"

#include <algorithm>
#include <utility>

#include "net/Bytes.h"

namespace oe {
namespace {
constexpr size_t kMaxIoCalls = 16;
constexpr size_t kMaxReceiveFrames = 128;
bool Fail(const std::string& message, std::string* error) { if (error) *error = message; return false; }
}

bool TcpFrameReader::Feed(const uint8_t* bytes, size_t size, std::vector<std::vector<uint8_t>>& frames) {
    if (!ok_ || (!bytes && size != 0)) { ok_ = false; return false; }
    size_t emitted = 0;
    while (size != 0) {
        if (prefixBytes_ < 4) {
            const size_t count = std::min(size, 4 - prefixBytes_);
            std::copy(bytes, bytes + count, prefix_.begin() + static_cast<ptrdiff_t>(prefixBytes_));
            prefixBytes_ += count;
            bytes += count;
            size -= count;
            if (prefixBytes_ < 4) continue;
            ByteReader reader(prefix_.data(), prefix_.size());
            uint32_t length = 0;
            if (!reader.ReadU32(length) || length > ITransport::kMaxMessageBytes) { ok_ = false; return false; }
            body_.resize(length);
        }
        const size_t count = std::min(size, body_.size() - bodyBytes_);
        if (count != 0) std::copy(bytes, bytes + count, body_.begin() + static_cast<ptrdiff_t>(bodyBytes_));
        bytes += count;
        size -= count;
        bodyBytes_ += count;
        if (bodyBytes_ == body_.size()) {
            if (++emitted > kMaxReceiveFrames) { ok_ = false; return false; }
            frames.push_back(std::move(body_));
            body_.clear();
            prefixBytes_ = bodyBytes_ = 0;
        }
    }
    return true;
}

bool UdpTransport::Bind(const NetAddress& address, std::string* error) {
    if (socket_) return Fail("UDP transport is already bound", error);
    if (!ValidNetAddress(address, true)) return Fail("Use a numeric IPv4 bind address; port 0 chooses an ephemeral port", error);
    auto socket = CreateNetSocket(SocketKind::Udp, error);
    if (!socket || !socket->Bind(address, error)) return false;
    socket_ = std::move(socket);
    return true;
}
bool UdpTransport::AddPeer(PeerId peer, const NetAddress& address) {
    if (!peer || !ValidNetAddress(address) || peers_.size() >= kMaxPeers || peers_.count(peer)) return false;
    for (const auto& entry : peers_) if (entry.second == address) return false;
    peers_.emplace(peer, address);
    return true;
}
bool UdpTransport::RemovePeer(PeerId peer) { return peers_.erase(peer) != 0; }
NetAddress UdpTransport::LocalAddress() const { return socket_ ? socket_->LocalAddress() : NetAddress{}; }
bool UdpTransport::Send(PeerId peer, const uint8_t* bytes, size_t size) {
    auto it = peers_.find(peer);
    if (!socket_ || it == peers_.end() || size > kMaxDatagramBytes || (!bytes && size)) return false;
    SocketIo result = socket_->SendTo(it->second, bytes, size);
    if (result == SocketIo::Error) error_ = socket_->LastError();
    return result == SocketIo::Progress;
}
bool UdpTransport::Poll(uint64_t frame, std::vector<TransportEvent>& events) {
    if (!socket_ || (polled_ && frame < frame_)) return false;
    if (polled_ && frame == frame_) return true;
    frame_ = frame;
    polled_ = true;
    std::array<uint8_t, kMaxDatagramBytes> bytes{};
    std::map<PeerId, size_t> counts;
    for (size_t i = 0; i < 256; ++i) {
        NetAddress from;
        size_t size = 0;
        SocketIo result = socket_->ReceiveFrom(from, bytes.data(), bytes.size(), size);
        if (result == SocketIo::WouldBlock) break;
        if (result == SocketIo::TooLarge) { ++dropped_; continue; }
        if (result != SocketIo::Progress) { error_ = socket_->LastError(); return false; }
        auto peer = std::find_if(peers_.begin(), peers_.end(), [&](const auto& entry) { return entry.second == from; });
        if (peer == peers_.end() || ++counts[peer->first] > kMaxReceiveFrames) { ++dropped_; continue; }
        events.push_back({peer->first, std::vector<uint8_t>(bytes.begin(), bytes.begin() + static_cast<ptrdiff_t>(size))});
        events.back().datagram = true;
    }
    return true;
}

bool TcpTransport::Listen(const NetAddress& address, std::string* error) {
    if (listener_) return Fail("TCP transport is already listening", error);
    if (!ValidNetAddress(address, true)) return Fail("Use a numeric IPv4 bind address; port 0 chooses an ephemeral port", error);
    auto socket = CreateNetSocket(SocketKind::Tcp, error);
    if (!socket || !socket->Bind(address, error) || !socket->Listen(error)) return false;
    listener_ = std::move(socket);
    return true;
}
bool TcpTransport::Connect(PeerId peer, const NetAddress& address, std::string* error) {
    if (!peer || peers_.count(peer) || peers_.size() >= kMaxPeers || !ValidNetAddress(address))
        return Fail("TCP connect requires a unique peer id and numeric IPv4 endpoint", error);
    auto socket = CreateNetSocket(SocketKind::Tcp, error);
    if (!socket || !socket->Connect(address, error)) return false;
    Peer state;
    state.socket = std::move(socket);
    state.address = address;
    state.connectFrame = frame_;
    peers_.emplace(peer, std::move(state));
    return true;
}
bool TcpTransport::Disconnect(PeerId peer) {
    if (!peers_.erase(peer)) return false;
    if (notices_.size() < kMaxPeers) notices_.push_back({peer, {}, TransportEvent::Type::Disconnected, {}});
    return true;
}
NetAddress TcpTransport::LocalAddress() const { return listener_ ? listener_->LocalAddress() : NetAddress{}; }
NetAddress TcpTransport::PeerAddress(PeerId peer) const {
    auto it = peers_.find(peer);
    return it == peers_.end() ? NetAddress{} : it->second.address;
}
size_t TcpTransport::QueuedBytes(PeerId peer) const {
    auto it = peers_.find(peer);
    return it == peers_.end() ? 0 : it->second.queuedBytes;
}
bool TcpTransport::Send(PeerId peer, const uint8_t* bytes, size_t size) {
    auto it = peers_.find(peer);
    if (it == peers_.end() || size > kMaxMessageBytes || (!bytes && size)) return false;
    auto& state = it->second;
    if (state.outgoing.size() >= kMaxQueuedFrames || size + 4 > kMaxQueuedBytes - state.queuedBytes) return false;
    ByteWriter writer(size + 4);
    if (!writer.WriteBlob(bytes, size)) return false;
    state.outgoing.push_back(writer.Data());
    state.queuedBytes += size + 4;
    return true;
}
bool TcpTransport::Poll(uint64_t frame, std::vector<TransportEvent>& events) {
    if (polled_ && frame < frame_) return false;
    if (polled_ && frame == frame_) return true;
    frame_ = frame;
    polled_ = true;
    for (auto& notice : notices_) events.push_back(std::move(notice));
    notices_.clear();
    if (listener_) {
        for (size_t i = 0; i < 8; ++i) {
            NetAddress from;
            std::string error;
            auto socket = listener_->Accept(from, &error);
            if (!socket) { if (!error.empty()) { error_ = error; return false; } break; }
            if (peers_.size() >= kMaxPeers) continue;
            while (nextPeer_ && peers_.count(nextPeer_)) ++nextPeer_;
            if (!nextPeer_) continue;
            Peer state;
            state.socket = std::move(socket);
            state.address = from;
            state.connectFrame = frame;
            peers_.emplace(nextPeer_++, std::move(state));
        }
    }
    for (auto it = peers_.begin(); it != peers_.end();) {
        Peer& peer = it->second;
        std::string reason;
        SocketState state = peer.socket->State();
        bool closed = state == SocketState::Closed;
        if (closed) reason = peer.socket->LastError();
        if (state == SocketState::Connecting && frame - peer.connectFrame >= 300) {
            closed = true;
            reason = "TCP connect timed out after 300 frames";
        }
        if (state == SocketState::Open && !peer.notified) {
            peer.notified = true;
            events.push_back({it->first, {}, TransportEvent::Type::Connected, {}});
        }
        if (state == SocketState::Open) {
            size_t receivedFrames = 0;
            for (size_t i = 0; i < kMaxIoCalls && !closed; ++i) {
                std::array<uint8_t, 16384> bytes{};
                size_t size = 0;
                SocketIo result = peer.socket->Receive(bytes.data(), bytes.size(), size);
                if (result == SocketIo::WouldBlock) break;
                if (result != SocketIo::Progress) {
                    closed = true;
                    reason = result == SocketIo::Closed && peer.reader.Incomplete() ?
                             "TCP stream closed during a frame" : peer.socket->LastError();
                    break;
                }
                std::vector<std::vector<uint8_t>> frames;
                if (!peer.reader.Feed(bytes.data(), size, frames) || receivedFrames + frames.size() > kMaxReceiveFrames) {
                    closed = true;
                    reason = "TCP frame length or receive rate exceeded its limit";
                    break;
                }
                receivedFrames += frames.size();
                for (auto& message : frames) events.push_back({it->first, std::move(message)});
            }
            for (size_t i = 0; i < kMaxIoCalls && !closed && !peer.outgoing.empty(); ++i) {
                const auto& bytes = peer.outgoing.front();
                size_t sent = 0;
                SocketIo result = peer.socket->Send(bytes.data() + peer.offset, bytes.size() - peer.offset, sent);
                if (result == SocketIo::WouldBlock) break;
                if (result != SocketIo::Progress) { closed = true; reason = peer.socket->LastError(); break; }
                peer.offset += sent;
                if (peer.offset == bytes.size()) {
                    peer.queuedBytes -= bytes.size();
                    peer.offset = 0;
                    peer.outgoing.pop_front();
                }
            }
        }
        if (closed) {
            events.push_back({it->first, {}, TransportEvent::Type::Disconnected, std::move(reason)});
            it = peers_.erase(it);
        } else ++it;
    }
    return true;
}

bool WebSocketTransport::Connect(PeerId peer, const std::string& url, std::string* error) {
    if (!peer || peers_.count(peer) || peers_.size() >= TcpTransport::kMaxPeers)
        return Fail("WebSocket connect requires a unique nonzero peer id", error);
    auto socket = CreatePlatformWebSocket(url, error);
    if (!socket) return false;
    Peer state;
    state.socket = std::move(socket);
    state.connectFrame = frame_;
    peers_.emplace(peer, std::move(state));
    return true;
}
bool WebSocketTransport::Disconnect(PeerId peer) {
    if (!peers_.erase(peer)) return false;
    if (notices_.size() < TcpTransport::kMaxPeers) notices_.push_back({peer, {}, TransportEvent::Type::Disconnected, {}});
    return true;
}
bool WebSocketTransport::Send(PeerId peer, const uint8_t* bytes, size_t size) {
    auto it = peers_.find(peer);
    if (it == peers_.end() || size > kMaxMessageBytes || (!bytes && size)) return false;
    Peer& state = it->second;
    if (state.outgoing.size() >= TcpTransport::kMaxQueuedFrames || size > TcpTransport::kMaxQueuedBytes - state.queuedBytes)
        return false;
    state.outgoing.emplace_back();
    if (size) state.outgoing.back().assign(bytes, bytes + size);
    state.queuedBytes += size;
    return true;
}
bool WebSocketTransport::Poll(uint64_t frame, std::vector<TransportEvent>& events) {
    if (polled_ && frame < frame_) return false;
    if (polled_ && frame == frame_) return true;
    frame_ = frame;
    polled_ = true;
    for (auto& notice : notices_) events.push_back(std::move(notice));
    notices_.clear();
    for (auto it = peers_.begin(); it != peers_.end();) {
        Peer& peer = it->second;
        SocketState state = peer.socket->State();
        bool closed = state == SocketState::Closed;
        std::string reason = peer.socket->LastError();
        if (state == SocketState::Connecting && frame - peer.connectFrame >= 300) {
            closed = true;
            reason = "WebSocket connect timed out after 300 frames";
        }
        if (state == SocketState::Open && !peer.notified) {
            peer.notified = true;
            events.push_back({it->first, {}, TransportEvent::Type::Connected, {}});
        }
        std::vector<std::vector<uint8_t>> messages;
        peer.socket->Receive(messages);
        for (auto& message : messages) events.push_back({it->first, std::move(message)});
        for (size_t i = 0; i < kMaxIoCalls && !closed && state == SocketState::Open && !peer.outgoing.empty(); ++i) {
            const auto& bytes = peer.outgoing.front();
            SocketIo result = peer.socket->Send(bytes.data(), bytes.size());
            if (result == SocketIo::WouldBlock) break;
            if (result != SocketIo::Progress) { closed = true; reason = peer.socket->LastError(); break; }
            peer.queuedBytes -= bytes.size();
            peer.outgoing.pop_front();
        }
        if (closed) {
            events.push_back({it->first, {}, TransportEvent::Type::Disconnected, std::move(reason)});
            it = peers_.erase(it);
        } else ++it;
    }
    return true;
}

}  // namespace oe
