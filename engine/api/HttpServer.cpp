#include "api/HttpServer.h"

#include <cctype>
#include <cstring>
#include <thread>

#include "core/Log.h"
#include "platform/Platform.h"

namespace oe {

namespace {

// The platform sockets are non-blocking; this server runs on its own thread and waits by
// sleeping briefly between polls, so no OS socket API is needed outside engine/platform.
constexpr double kPollSeconds = 0.001;
constexpr double kIdleSeconds = 15.0;     // a silent or stalled peer cannot hold the server
constexpr double kConnectSeconds = 5.0;
// OS sleeps can be as coarse as ~15 ms. While an exchange is in progress (and shortly after
// one) the thread yields instead, so a request does not pay several sleep quanta.
constexpr double kHotSeconds = 0.02;

void Wait(double since) {
    if (PlatformTimeSeconds() - since < kHotSeconds) std::this_thread::yield();
    else PlatformSleep(kPollSeconds);
}

const char* StatusText(int status) {
    switch (status) {
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        default: return "Status";
    }
}

// Sends everything, waiting while the peer's window is full. False on error or idle timeout.
bool SendAll(NetSocket& s, const char* data, size_t size) {
    double idle = PlatformTimeSeconds();
    while (size > 0) {
        size_t sent = 0;
        SocketIo result = s.Send(reinterpret_cast<const uint8_t*>(data), size > (1u << 20) ? (1u << 20) : size, sent);
        if (result == SocketIo::WouldBlock) {
            if (PlatformTimeSeconds() - idle > kIdleSeconds) return false;
            Wait(idle);
            continue;
        }
        if (result != SocketIo::Progress) return false;
        data += sent;
        size -= sent;
        idle = PlatformTimeSeconds();
    }
    return true;
}

// Appends available bytes. Returns 1 on data, 0 when the peer closed, -1 on error or idle timeout
// (idleSeconds <= 0 waits without a limit, for long-running commands on the client side).
int ReceiveSome(NetSocket& s, std::string& data, double idleSeconds, const std::atomic<bool>* running = nullptr) {
    uint8_t buf[8192];
    const double start = PlatformTimeSeconds();
    for (;;) {
        size_t n = 0;
        SocketIo result = s.Receive(buf, sizeof(buf), n);
        if (result == SocketIo::Progress) { data.append(reinterpret_cast<const char*>(buf), n); return 1; }
        if (result == SocketIo::Closed) return 0;
        if (result != SocketIo::WouldBlock) return -1;
        if ((running && !*running) || (idleSeconds > 0 && PlatformTimeSeconds() - start > idleSeconds)) return -1;
        Wait(start);
    }
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Reads one request. Returns false on malformed input or disconnect.
bool ReadRequest(NetSocket& s, HttpRequest& req, int& errorStatus, const std::atomic<bool>& running) {
    std::string data;
    size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        if (ReceiveSome(s, data, kIdleSeconds, &running) <= 0) return false;
        headerEnd = data.find("\r\n\r\n");
        if (data.size() > 64 * 1024 && headerEnd == std::string::npos) {
            errorStatus = 413;
            return false;
        }
    }
    std::string head = data.substr(0, headerEnd);
    size_t lineEnd = head.find("\r\n");
    std::string requestLine = head.substr(0, lineEnd);
    size_t sp1 = requestLine.find(' ');
    size_t sp2 = requestLine.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        errorStatus = 400;
        return false;
    }
    req.method = requestLine.substr(0, sp1);
    std::string target = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);
    size_t q = target.find('?');
    req.path = UrlDecode(target.substr(0, q));
    req.query = q == std::string::npos ? "" : target.substr(q + 1);

    size_t pos = lineEnd == std::string::npos ? head.size() : lineEnd + 2;
    while (pos < head.size()) {
        size_t end = head.find("\r\n", pos);
        if (end == std::string::npos) end = head.size();
        std::string line = head.substr(pos, end - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string value = line.substr(colon + 1);
            size_t start = value.find_first_not_of(' ');
            req.headers[Lower(line.substr(0, colon))] = start == std::string::npos ? "" : value.substr(start);
        }
        pos = end + 2;
    }

    size_t contentLength = static_cast<size_t>(std::strtoull(req.Header("content-length").c_str(), nullptr, 10));
    if (contentLength > 32 * 1024 * 1024) {
        errorStatus = 413;
        return false;
    }
    req.body = data.substr(headerEnd + 4);
    while (req.body.size() < contentLength) {
        if (ReceiveSome(s, req.body, kIdleSeconds, &running) <= 0) return false;
    }
    req.body.resize(contentLength);
    return true;
}

void WriteResponse(NetSocket& s, const HttpResponse& res) {
    std::string head = "HTTP/1.1 " + std::to_string(res.status) + " " + StatusText(res.status) + "\r\n";
    head += "Content-Type: " + res.contentType + "\r\n";
    head += "Content-Length: " + std::to_string(res.body.size()) + "\r\n";
    head += "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
    if (SendAll(s, head.data(), head.size())) SendAll(s, res.body.data(), res.body.size());
}

}  // namespace

std::string UrlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out += static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string HttpRequest::Query(const std::string& key, const std::string& def) const {
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        std::string pair = query.substr(pos, amp - pos);
        size_t eq = pair.find('=');
        if (UrlDecode(pair.substr(0, eq)) == key) return eq == std::string::npos ? "" : UrlDecode(pair.substr(eq + 1));
        pos = amp + 1;
    }
    return def;
}

std::string HttpRequest::Header(const std::string& lowerName) const {
    auto it = headers.find(lowerName);
    return it == headers.end() ? "" : it->second;
}

// ----- HttpServer -------------------------------------------------------------------

HttpServer::~HttpServer() { Stop(); }

bool HttpServer::Start(int port, Handler handler, std::string* error) {
    if (port < 0 || port > 65535) {
        if (error) *error = "port must be 0..65535";
        return false;
    }
    std::string detail;
    auto listener = CreateNetSocket(SocketKind::Tcp, &detail);
    // Local only: the API never binds anything but the loopback address.
    if (!listener || !listener->Bind({"127.0.0.1", static_cast<uint16_t>(port)}, &detail) || !listener->Listen(&detail)) {
        if (error) *error = "cannot listen on 127.0.0.1:" + std::to_string(port) + " (port in use?): " + detail;
        return false;
    }
    listener_ = std::move(listener);
    port_ = listener_->LocalAddress().port;
    handler_ = std::move(handler);
    running_ = true;
    thread_ = std::thread([this] { Loop(); });
    OE_LOG_INFO("http", "listening on http://127.0.0.1:%d", port_);
    return true;
}

void HttpServer::Stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    listener_.reset();
}

void HttpServer::Loop() {
    double served = -1.0;  // time of the last exchange; agents usually send calls in bursts
    while (running_) {
        NetAddress from;
        std::string acceptError;
        std::unique_ptr<NetSocket> client = listener_->Accept(from, &acceptError);
        if (!client) {
            if (!acceptError.empty()) OE_LOG_WARN("http", "%s", acceptError.c_str());
            Wait(served);
            continue;
        }
        HttpRequest req;
        int errorStatus = 400;
        HttpResponse res;
        bool readOk = ReadRequest(*client, req, errorStatus, running_);
        if (readOk) {
            try {
                res = handler_(req);
            } catch (const std::exception& e) {
                res.status = 500;
                res.body = std::string("{\"ok\":false,\"error\":{\"code\":\"internal_error\",\"message\":\"") + JsonEscape(e.what()) + "\"}}";
            }
        } else {
            res.status = errorStatus;
            res.body = "{\"ok\":false,\"error\":{\"code\":\"bad_request\",\"message\":\"malformed HTTP request\"}}";
        }
        WriteResponse(*client, res);
        served = PlatformTimeSeconds();
    }
}

bool HttpPostLocal(int port, const std::string& path, const std::string& body, std::string& response, std::string* error) {
    auto fail = [&](const std::string& message) { if (error) *error = message; return false; };
    const std::string refused = "cannot connect to 127.0.0.1:" + std::to_string(port) + " (is `oe editor` running?)";
    if (port <= 0 || port > 65535) return fail(refused);
    std::string detail;
    auto s = CreateNetSocket(SocketKind::Tcp, &detail);
    if (!s) return fail(detail.empty() ? "socket creation failed" : detail);
    if (!s->Connect({"127.0.0.1", static_cast<uint16_t>(port)}, &detail)) return fail(refused);
    const double start = PlatformTimeSeconds();
    while (s->State() == SocketState::Connecting) {
        if (PlatformTimeSeconds() - start > kConnectSeconds) return fail(refused);
        Wait(start);
    }
    if (s->State() != SocketState::Open) return fail(refused);
    std::string req = "POST " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                      "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    if (!SendAll(*s, req.data(), req.size())) return fail("cannot send the HTTP request");
    std::string data;
    // Commands such as renders can take long: wait for the server to close without a limit.
    while (ReceiveSome(*s, data, 0) > 0) {}
    size_t split = data.find("\r\n\r\n");
    if (split == std::string::npos) return fail("malformed HTTP response");
    response = data.substr(split + 4);
    return true;
}

}  // namespace oe
