#include "net/WebSocketServer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include "core/Image.h"

namespace oe {
namespace {
uint32_t Rotate(uint32_t n, unsigned bits) { return (n << bits) | (n >> (32 - bits)); }
// SHA-1 is mandated by RFC 6455 section 4.2.2, used only for the HTTP upgrade token.
std::string Accept(const std::string& key) {
    std::string input = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::vector<uint8_t> data(input.begin(), input.end()); uint64_t bits = data.size() * 8;
    data.push_back(0x80); while (data.size() % 64 != 56) data.push_back(0);
    for (int i = 7; i >= 0; --i) data.push_back(static_cast<uint8_t>(bits >> (i * 8)));
    uint32_t h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    for (size_t offset = 0; offset < data.size(); offset += 64) {
        uint32_t w[80]{};
        for (size_t i = 0; i < 16; ++i) for (size_t b = 0; b < 4; ++b) w[i] = (w[i] << 8) | data[offset + i * 4 + b];
        for (size_t i = 16; i < 80; ++i) w[i] = Rotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (size_t i = 0; i < 80; ++i) {
            uint32_t f = i < 20 ? ((b & c) | (~b & d)) : i < 40 ? (b ^ c ^ d) : i < 60 ? ((b & c) | (b & d) | (c & d)) : (b ^ c ^ d);
            uint32_t k = i < 20 ? 0x5a827999 : i < 40 ? 0x6ed9eba1 : i < 60 ? 0x8f1bbcdc : 0xca62c1d6;
            uint32_t t = Rotate(a, 5) + f + e + k + w[i]; e = d; d = c; c = Rotate(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    std::array<uint8_t, 20> digest{};
    for (size_t i = 0; i < 5; ++i) for (size_t b = 0; b < 4; ++b) digest[i * 4 + b] = static_cast<uint8_t>(h[i] >> ((3 - b) * 8));
    return Base64Encode(digest.data(), digest.size());
}
std::string Lower(std::string text) { for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return text; }
std::string Trim(std::string text) {
    size_t first = text.find_first_not_of(" \t"), last = text.find_last_not_of(" \t");
    return first == std::string::npos ? "" : text.substr(first, last - first + 1);
}
bool Token(const std::string& text, const std::string& token) {
    size_t start = 0;
    while (start < text.size()) { size_t end = text.find(',', start); if (Trim(text.substr(start, end - start)) == token) return true;
        if (end == std::string::npos) break; start = end + 1; }
    return false;
}
bool Utf8(const std::vector<uint8_t>& bytes, size_t start) {
    for (size_t i = start; i < bytes.size();) {
        uint8_t c = bytes[i++]; if (c < 128) continue;
        unsigned n = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
        if (!n || i + n > bytes.size()) return false;
        uint8_t first = bytes[i];
        if ((c == 0xe0 && first < 0xa0) || (c == 0xed && first >= 0xa0) ||
            (c == 0xf0 && first < 0x90) || (c == 0xf4 && first >= 0x90)) return false;
        for (unsigned j = 0; j < n; ++j) if ((bytes[i++] & 0xc0) != 0x80) return false;
    }
    return true;
}
bool Upgrade(const std::string& text, std::string& response) {
    size_t end = text.find("\r\n"); std::string request = text.substr(0, end);
    if (request.compare(0, 5, "GET /") != 0 || request.size() < 14 || request.substr(request.size() - 9) != " HTTP/1.1") return false;
    std::map<std::string, std::string> headers; size_t start = end + 2;
    while ((end = text.find("\r\n", start)) != std::string::npos && end != start) {
        std::string line = text.substr(start, end - start); size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0) return false;
        std::string name = Lower(line.substr(0, colon));
        if (name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string::npos || headers.count(name)) return false;
        headers[name] = Trim(line.substr(colon + 1)); start = end + 2;
    }
    std::string key = headers["sec-websocket-key"];
    if (headers["host"].empty() || Lower(headers["upgrade"]) != "websocket" || !Token(Lower(headers["connection"]), "upgrade") ||
        headers["sec-websocket-version"] != "13" || key.size() != 24 || key.substr(22) != "==" ||
        key.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/") < 22 ||
        std::string("AQgw").find(key[21]) == std::string::npos || headers.count("transfer-encoding") ||
        (headers.count("content-length") && headers["content-length"] != "0")) return false;
    response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + Accept(key) + "\r\n\r\n";
    return true;
}
}
std::string WebSocketServerReader::TakeUpgrade() { std::string result; result.swap(upgrade_); return result; }
std::vector<uint8_t> WebSocketServerReader::Encode(uint8_t opcode, const uint8_t* bytes, size_t size) {
    if (size > ITransport::kMaxMessageBytes || (!bytes && size)) return {};
    std::vector<uint8_t> out; out.push_back(static_cast<uint8_t>(0x80 | opcode));
    if (size < 126) out.push_back(static_cast<uint8_t>(size));
    else if (size <= 65535) { out.push_back(126); out.push_back(static_cast<uint8_t>(size >> 8)); out.push_back(static_cast<uint8_t>(size)); }
    else { out.push_back(127); for (int i = 7; i >= 0; --i) out.push_back(static_cast<uint8_t>(static_cast<uint64_t>(size) >> (i * 8))); }
    if (size) out.insert(out.end(), bytes, bytes + size); return out;
}
bool WebSocketServerReader::Feed(const uint8_t* bytes, size_t size, std::vector<Frame>& frames) {
    auto fail = [&] { ok_ = false; buffer_.clear(); fragments_.clear(); return false; };
    if (!ok_ || (!bytes && size) || size > 16384 || buffer_.size() + size > ITransport::kMaxMessageBytes + 16398) return fail();
    if (closed_) return true;
    if (size) buffer_.insert(buffer_.end(), bytes, bytes + size);
    if (!ready_) {
        std::string text(buffer_.begin(), buffer_.end()); size_t end = text.find("\r\n\r\n");
        if (end == std::string::npos) return buffer_.size() <= 8192 ? true : fail();
        if (end + 4 > 8192 || !Upgrade(text.substr(0, end + 4), upgrade_)) return fail();
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<ptrdiff_t>(end + 4)); ready_ = true;
    }
    size_t consumed = 0, count = 0;
    while (buffer_.size() - consumed >= 2) {
        const uint8_t* p = buffer_.data() + consumed; size_t available = buffer_.size() - consumed;
        bool fin = (p[0] & 0x80) != 0; uint8_t opcode = p[0] & 15; uint64_t length = p[1] & 127; size_t header = 2;
        if ((p[0] & 0x70) || !(p[1] & 0x80) || (opcode != 0 && opcode != 2 && opcode != 8 && opcode != 9 && opcode != 10)) return fail();
        if (length == 126) { if (available < 4) break; length = (static_cast<uint64_t>(p[2]) << 8) | p[3]; header = 4; if (length < 126) return fail(); }
        else if (length == 127) { if (available < 10) break; length = 0; for (size_t i = 2; i < 10; ++i) length = (length << 8) | p[i]; header = 10; if (length <= 65535) return fail(); }
        if (length > ITransport::kMaxMessageBytes || (opcode >= 8 && (!fin || length > 125 || (opcode == 8 && length == 1)))) return fail();
        if (available < header + 4 + length) break;
        if (++count > 128) return fail();
        std::vector<uint8_t> payload(static_cast<size_t>(length));
        for (size_t i = 0; i < payload.size(); ++i) payload[i] = p[header + 4 + i] ^ p[header + i % 4];
        consumed += header + 4 + payload.size();
        if (opcode == 8 && payload.size() >= 2) {
            unsigned code = (static_cast<unsigned>(payload[0]) << 8) | payload[1];
            if (!((code >= 1000 && code <= 1014 && code != 1004 && code != 1005 && code != 1006) || (code >= 3000 && code <= 4999)) ||
                !Utf8(payload, 2)) return fail();
        }
        if (opcode >= 8) { frames.push_back({opcode, std::move(payload)}); if (opcode == 8) { closed_ = true; break; } continue; }
        if ((opcode == 0) != fragmented_ || payload.size() > ITransport::kMaxMessageBytes - fragments_.size()) return fail();
        if (opcode == 2 && fin) frames.push_back({2, std::move(payload)});
        else { fragments_.insert(fragments_.end(), payload.begin(), payload.end()); fragmented_ = !fin;
            if (fin) { frames.push_back({2, std::move(fragments_)}); fragments_.clear(); } }
    }
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<ptrdiff_t>(consumed));
    if (closed_) { buffer_.clear(); fragments_.clear(); } return true;
}
bool WebSocketServerTransport::Listen(const NetAddress& address, std::string* error) {
    if (listener_ || !peers_.empty()) { if (error) *error = "WebSocket listener is already active"; return false; }
    auto socket = CreateNetSocket(SocketKind::Tcp, error);
    if (!socket || !socket->Bind(address, error) || !socket->Listen(error)) return false;
    listener_ = std::move(socket); return true;
}
NetAddress WebSocketServerTransport::LocalAddress() const { return listener_ ? listener_->LocalAddress() : NetAddress{}; }
bool WebSocketServerTransport::Disconnect(PeerId peer) {
    if (!peers_.erase(peer)) return false;
    if (notices_.size() < 64) notices_.push_back({peer, {}, TransportEvent::Type::Disconnected, {}}); return true;
}
bool WebSocketServerTransport::Queue(Peer& peer, std::vector<uint8_t> bytes) {
    if (bytes.empty() || peer.outgoing.size() >= 256 || bytes.size() > 4 * 1024 * 1024 - peer.queued) return false;
    peer.queued += bytes.size(); peer.outgoing.push_back(std::move(bytes)); return true;
}
bool WebSocketServerTransport::Send(PeerId id, const uint8_t* bytes, size_t size) {
    auto it = peers_.find(id);
    return it != peers_.end() && !it->second.closing && it->second.reader.Ready() && Queue(it->second, WebSocketServerReader::Encode(2, bytes, size));
}
bool WebSocketServerTransport::Poll(uint64_t frame, std::vector<TransportEvent>& events) {
    if (polled_ && frame < frame_) return false; if (polled_ && frame == frame_) return true;
    polled_ = true; frame_ = frame;
    for (auto& event : notices_) events.push_back(std::move(event)); notices_.clear();
    if (listener_) for (size_t i = 0; i < 8; ++i) {
        NetAddress from; std::string error; auto socket = listener_->Accept(from, &error);
        if (!socket) { if (!error.empty()) return false; break; }
        if (peers_.size() >= 64 || !nextPeer_) continue;
        Peer peer; peer.socket = std::move(socket); peer.accepted = frame; peers_.emplace(nextPeer_++, std::move(peer));
    }
    for (auto it = peers_.begin(); it != peers_.end();) {
        Peer& peer = it->second; bool closed = peer.socket->State() == SocketState::Closed;
        if (!peer.reader.Ready() && frame - peer.accepted >= 300) closed = true;
        size_t received = 0;
        for (size_t io = 0; io < 16 && !closed && !peer.closing; ++io) {
            std::array<uint8_t, 16384> bytes{}; size_t size = 0;
            SocketIo result = peer.socket->Receive(bytes.data(), bytes.size(), size);
            if (result == SocketIo::WouldBlock) break;
            std::vector<WebSocketServerReader::Frame> frames;
            if (result != SocketIo::Progress || !peer.reader.Feed(bytes.data(), size, frames) || (received += frames.size()) > 128) { closed = true; break; }
            std::string upgrade = peer.reader.TakeUpgrade();
            if (!upgrade.empty() && !Queue(peer, std::vector<uint8_t>(upgrade.begin(), upgrade.end()))) { closed = true; break; }
            if (peer.reader.Ready() && !peer.notified) { peer.notified = true; events.push_back({it->first, {}, TransportEvent::Type::Connected, {}}); }
            for (auto& message : frames) {
                if (message.opcode == 2) events.push_back({it->first, std::move(message.bytes)});
                else if (message.opcode == 9 || message.opcode == 8) {
                    if (!Queue(peer, WebSocketServerReader::Encode(message.opcode == 9 ? 10 : 8, message.bytes.data(), message.bytes.size()))) closed = true;
                    if (message.opcode == 8) peer.closing = true;
                }
            }
        }
        for (size_t io = 0; io < 16 && !closed && !peer.outgoing.empty(); ++io) {
            const auto& bytes = peer.outgoing.front(); size_t sent = 0;
            SocketIo result = peer.socket->Send(bytes.data() + peer.offset, bytes.size() - peer.offset, sent);
            if (result == SocketIo::WouldBlock) break;
            if (result != SocketIo::Progress) { closed = true; break; }
            peer.offset += sent;
            if (peer.offset == bytes.size()) { peer.queued -= bytes.size(); peer.offset = 0; peer.outgoing.pop_front(); }
        }
        if (peer.closing && peer.outgoing.empty()) closed = true;
        if (closed) { events.push_back({it->first, {}, TransportEvent::Type::Disconnected, "WebSocket closed or invalid frame/upgrade"}); it = peers_.erase(it); }
        else ++it;
    }
    return true;
}
}  // namespace oe
