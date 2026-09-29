#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace oe {

struct HttpRequest {
    std::string method;
    std::string path;   // without query string
    std::string query;  // raw query string (after '?')
    std::map<std::string, std::string> headers;  // lower-case names
    std::string body;
    std::string Query(const std::string& key, const std::string& def = "") const;
    std::string Header(const std::string& lowerName) const;
};

struct HttpResponse {
    int status = 200;
    std::string contentType = "application/json";
    std::string body;
};

// One WebSocket connection (RFC 6455), served on its own thread with
// blocking I/O. Sending is thread safe.
class WebSocket {
public:
    explicit WebSocket(intptr_t socket) : socket_(socket) {}
    // Waits for the next text or binary message (answers pings, joins
    // fragments). Returns false once the peer closed or the connection broke.
    bool Read(std::string& message, bool* binary = nullptr);
    bool SendText(const std::string& text);
    bool SendBinary(const void* data, size_t size);

private:
    bool SendFrame(int opcode, const void* data, size_t size);
    intptr_t socket_;
    std::mutex sendMutex_;
};

// Minimal blocking HTTP/1.1 server on a background thread. Binds to
// 127.0.0.1 only; requests are handled one at a time. WebSocket upgrades
// (see OnWebSocket) get a thread each so a stream never blocks requests.
class HttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;
    // Decides whether an upgrade request may open a WebSocket; on refusal
    // returns false and sets the HTTP status to answer with.
    using WebSocketAccept = std::function<bool(const HttpRequest&, int* status)>;
    using WebSocketHandler = std::function<void(const HttpRequest&, WebSocket&)>;

    ~HttpServer();
    // Call before Start.
    void OnWebSocket(WebSocketAccept accept, WebSocketHandler handler);
    bool Start(int port, Handler handler, std::string* error);
    void Stop();
    int Port() const { return port_; }

private:
    struct Session {
        std::thread thread;
        intptr_t socket = -1;
        std::atomic<bool> done{false};
    };
    void Loop();
    void StartSession(intptr_t client, const HttpRequest& request);
    std::thread thread_;
    std::atomic<bool> running_{false};
    intptr_t socket_ = -1;
    int port_ = 0;
    Handler handler_;
    WebSocketAccept wsAccept_;
    WebSocketHandler wsHandler_;
    std::mutex sessionsMutex_;
    std::vector<std::unique_ptr<Session>> sessions_;
};

// Blocking HTTP POST to 127.0.0.1 (used by `oe mcp --connect`).
bool HttpPostLocal(int port, const std::string& path, const std::string& body, std::string& response, std::string* error);

std::string UrlDecode(const std::string& s);

}  // namespace oe
