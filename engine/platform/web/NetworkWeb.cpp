#include "platform/Network.h"

#include <deque>
#include <utility>

#include <emscripten.h>

namespace oe {
namespace {
// Check JS binary lengths before copying to the Wasm heap. Browser callbacks never touch Engine state.
EM_JS(int, OpenWebSocket, (const char* url, void* owner), {
    if (typeof WebSocket === 'undefined') return 0;
    try {
        var registry = Module.oeNetSockets || (Module.oeNetSockets = { next: 1, sockets: new Map() });
        if (registry.next > 2147483647) return 0;
        var ws = new WebSocket(UTF8ToString(url));
        ws.binaryType = 'arraybuffer';
        var id = registry.next++;
        registry.sockets.set(id, ws);
        ws.onopen = function() { Module['_oe_NetworkWebState'](owner, 1); };
        ws.onerror = function() { Module['_oe_NetworkWebState'](owner, 2); };
        ws.onclose = function() { Module['_oe_NetworkWebState'](owner, 0); };
        ws.onmessage = function(event) {
            if (!(event.data instanceof ArrayBuffer) || event.data.byteLength > 65536) {
                Module['_oe_NetworkWebState'](owner, 3);
                ws.close(1009, 'invalid binary message');
                return;
            }
            var size = event.data.byteLength;
            var buffer = _malloc(Math.max(size, 1));
            if (!buffer) { Module['_oe_NetworkWebState'](owner, 3); ws.close(1009); return; }
            HEAPU8.set(new Uint8Array(event.data), buffer);
            var accepted = Module['_oe_NetworkWebData'](owner, buffer, size);
            _free(buffer);
            if (!accepted) ws.close(1009, 'receive queue full');
        };
        return id;
    } catch (error) { return 0; }
});
EM_JS(int, SendWebSocket, (int id, const uint8_t* bytes, size_t size), {
    var registry = Module.oeNetSockets;
    var ws = registry && registry.sockets.get(id);
    if (!ws || ws.readyState > 1) return 2;
    if (ws.readyState !== 1 || ws.bufferedAmount + size > 4194304) return 1;
    try { ws.send(HEAPU8.slice(bytes, bytes + size)); return 0; } catch (error) { return 2; }
});
EM_JS(void, CloseWebSocket, (int id), {
    var registry = Module.oeNetSockets;
    var ws = registry && registry.sockets.get(id);
    if (!ws) return;
    ws.onopen = ws.onerror = ws.onclose = ws.onmessage = null;
    registry.sockets.delete(id);
    try { ws.close(1000); } catch (error) {}
});

class BrowserSocket final : public PlatformWebSocket {
public:
    ~BrowserSocket() override { if (id_) CloseWebSocket(id_); }
    bool Open(const std::string& url) { id_ = OpenWebSocket(url.c_str(), this); return id_ != 0; }
    SocketState State() const override { return state_; }
    const std::string& LastError() const override { return error_; }
    SocketIo Send(const uint8_t* bytes, size_t size) override {
        if (size > 65536 || (!bytes && size)) return SocketIo::Error;
        int result = SendWebSocket(id_, bytes, size);
        if (result == 0) return SocketIo::Progress;
        if (result == 1) return SocketIo::WouldBlock;
        SetState(2);
        return SocketIo::Error;
    }
    void Receive(std::vector<std::vector<uint8_t>>& messages) override {
        while (!incoming_.empty()) {
            messages.push_back(std::move(incoming_.front()));
            incoming_.pop_front();
        }
        buffered_ = 0;
    }
    void SetState(int state) {
        if (state == 1 && state_ != SocketState::Closed) state_ = SocketState::Open;
        else {
            state_ = SocketState::Closed;
            if (state == 2) error_ = "WebSocket connection failed";
            if (state == 3) error_ = "WebSocket binary size or receive queue limit exceeded";
        }
    }
    bool Message(const uint8_t* bytes, size_t size) {
        if (state_ == SocketState::Closed || size > 65536 || incoming_.size() >= 128 || size > 4 * 1024 * 1024 - buffered_) {
            SetState(3);
            return false;
        }
        incoming_.emplace_back();
        if (size) incoming_.back().assign(bytes, bytes + size);
        buffered_ += size;
        return true;
    }
private:
    int id_ = 0;
    SocketState state_ = SocketState::Connecting;
    std::string error_;
    std::deque<std::vector<uint8_t>> incoming_;
    size_t buffered_ = 0;
};
}

extern "C" {
EMSCRIPTEN_KEEPALIVE void oe_NetworkWebState(void* owner, int state) { static_cast<BrowserSocket*>(owner)->SetState(state); }
EMSCRIPTEN_KEEPALIVE int oe_NetworkWebData(void* owner, const uint8_t* bytes, size_t size) {
    return static_cast<BrowserSocket*>(owner)->Message(bytes, size) ? 1 : 0;
}
}

std::unique_ptr<PlatformWebSocket> CreatePlatformWebSocket(const std::string& url, std::string* error) {
    if (url.size() > 2048 || (url.rfind("ws://", 0) != 0 && url.rfind("wss://", 0) != 0)) {
        if (error) *error = "Use a ws:// or wss:// URL of at most 2048 bytes";
        return nullptr;
    }
    auto socket = std::make_unique<BrowserSocket>();
    if (!socket->Open(url)) { if (error) *error = "WebSocket unavailable or URL invalid"; return nullptr; }
    return socket;
}
std::unique_ptr<NetSocket> CreateNetSocket(SocketKind, std::string* error) {
    if (error) *error = "Browser builds have no native UDP/TCP sockets; use WebSocketTransport";
    return nullptr;
}
}  // namespace oe
