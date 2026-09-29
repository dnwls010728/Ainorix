#include "api/HttpServer.h"

#include <cctype>
#include <cstring>

#include "core/Image.h"

#include "core/Log.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
#define OE_INVALID_SOCKET INVALID_SOCKET
#define OE_CLOSE_SOCKET closesocket
#define OE_SHUTDOWN_BOTH SD_BOTH
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
#define OE_INVALID_SOCKET (-1)
#define OE_CLOSE_SOCKET ::close
#define OE_SHUTDOWN_BOTH SHUT_RDWR
#endif

namespace oe {

namespace {

bool EnsureSockets() {
#ifdef _WIN32
    static bool ok = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ok;
#else
    return true;
#endif
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

bool SendAll(SocketHandle s, const char* data, size_t size) {
    while (size > 0) {
        int n = send(s, data, static_cast<int>(size > 1 << 20 ? 1 << 20 : size), 0);
        if (n <= 0) return false;
        data += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// SHA-1 (FIPS 180-1), only used for the WebSocket handshake.
std::string Sha1(const std::string& input) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    std::string msg = input;
    uint64_t bits = static_cast<uint64_t>(input.size()) * 8;
    msg += static_cast<char>(0x80);
    while (msg.size() % 64 != 56) msg += '\0';
    for (int i = 7; i >= 0; --i) msg += static_cast<char>((bits >> (i * 8)) & 0xFF);
    auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const unsigned char* p = reinterpret_cast<const unsigned char*>(msg.data()) + chunk + static_cast<size_t>(i) * 4;
            w[i] = (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3];
        }
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999u;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1u;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDCu;
            else f = b ^ c ^ d, k = 0xCA62C1D6u;
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    std::string out;
    for (uint32_t v : h) {
        for (int i = 3; i >= 0; --i) out += static_cast<char>((v >> (i * 8)) & 0xFF);
    }
    return out;
}

bool RecvAll(SocketHandle s, void* data, size_t size) {
    char* p = static_cast<char*>(data);
    while (size > 0) {
        int n = recv(s, p, static_cast<int>(size > 1 << 20 ? 1 << 20 : size), 0);
        if (n <= 0) return false;
        p += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

// Reads one request. Returns false on malformed input or disconnect.
bool ReadRequest(SocketHandle s, HttpRequest& req, int& errorStatus) {
    std::string data;
    char buf[8192];
    size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        data.append(buf, static_cast<size_t>(n));
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
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        req.body.append(buf, static_cast<size_t>(n));
    }
    req.body.resize(contentLength);
    return true;
}

void WriteResponse(SocketHandle s, const HttpResponse& res) {
    std::string head = "HTTP/1.1 " + std::to_string(res.status) + " " + StatusText(res.status) + "\r\n";
    head += "Content-Type: " + res.contentType + "\r\n";
    head += "Content-Length: " + std::to_string(res.body.size()) + "\r\n";
    head += "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
    SendAll(s, head.data(), head.size());
    SendAll(s, res.body.data(), res.body.size());
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

// ----- WebSocket ------------------------------------------------------------------

bool WebSocket::SendFrame(int opcode, const void* data, size_t size) {
    std::lock_guard<std::mutex> lock(sendMutex_);
    unsigned char head[10];
    size_t n = 0;
    head[n++] = static_cast<unsigned char>(0x80 | opcode);  // FIN + opcode; server frames are not masked
    if (size < 126) {
        head[n++] = static_cast<unsigned char>(size);
    } else if (size <= 0xFFFF) {
        head[n++] = 126;
        head[n++] = static_cast<unsigned char>(size >> 8);
        head[n++] = static_cast<unsigned char>(size);
    } else {
        head[n++] = 127;
        for (int i = 7; i >= 0; --i) head[n++] = static_cast<unsigned char>(static_cast<uint64_t>(size) >> (i * 8));
    }
    SocketHandle s = static_cast<SocketHandle>(socket_);
    return SendAll(s, reinterpret_cast<const char*>(head), n) && (size == 0 || SendAll(s, static_cast<const char*>(data), size));
}

bool WebSocket::SendText(const std::string& text) { return SendFrame(0x1, text.data(), text.size()); }
bool WebSocket::SendBinary(const void* data, size_t size) { return SendFrame(0x2, data, size); }

bool WebSocket::Read(std::string& message, bool* binary) {
    SocketHandle s = static_cast<SocketHandle>(socket_);
    message.clear();
    int messageOpcode = -1;
    for (;;) {
        unsigned char head[2];
        if (!RecvAll(s, head, 2)) return false;
        bool fin = (head[0] & 0x80) != 0;
        int opcode = head[0] & 0x0F;
        bool masked = (head[1] & 0x80) != 0;
        uint64_t len = head[1] & 0x7F;
        if (len == 126) {
            unsigned char ext[2];
            if (!RecvAll(s, ext, 2)) return false;
            len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
        } else if (len == 127) {
            unsigned char ext[8];
            if (!RecvAll(s, ext, 8)) return false;
            len = 0;
            for (unsigned char b : ext) len = (len << 8) | b;
        }
        if (!masked || len > 4 * 1024 * 1024 || message.size() + len > 4 * 1024 * 1024) return false;  // clients must mask; keep messages small
        unsigned char mask[4];
        if (!RecvAll(s, mask, 4)) return false;
        std::string payload(static_cast<size_t>(len), '\0');
        if (len > 0 && !RecvAll(s, &payload[0], static_cast<size_t>(len))) return false;
        for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
        if (opcode == 0x8) {  // close: echo it and stop
            SendFrame(0x8, payload.data(), std::min<size_t>(payload.size(), 2));
            return false;
        }
        if (opcode == 0x9) {  // ping
            SendFrame(0xA, payload.data(), payload.size());
            continue;
        }
        if (opcode == 0xA) continue;  // pong
        if (opcode != 0x0) messageOpcode = opcode;
        message += payload;
        if (fin && messageOpcode >= 0) {
            if (binary) *binary = messageOpcode == 0x2;
            return true;
        }
    }
}

// ----- HttpServer -------------------------------------------------------------------

HttpServer::~HttpServer() { Stop(); }

void HttpServer::OnWebSocket(WebSocketAccept accept, WebSocketHandler handler) {
    wsAccept_ = std::move(accept);
    wsHandler_ = std::move(handler);
}

void HttpServer::StartSession(intptr_t client, const HttpRequest& request) {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    // Reap finished sessions (editor reloads open new streams).
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if ((*it)->done) {
            (*it)->thread.join();
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
    auto session = std::make_unique<Session>();
    Session* sp = session.get();
    sp->socket = client;
    sp->thread = std::thread([this, sp, request] {
        WebSocket ws(sp->socket);
        try {
            wsHandler_(request, ws);
        } catch (const std::exception& e) {
            OE_LOG_WARN("http", "websocket session ended: %s", e.what());
        }
        std::lock_guard<std::mutex> guard(sessionsMutex_);
        OE_CLOSE_SOCKET(static_cast<SocketHandle>(sp->socket));
        sp->socket = -1;
        sp->done = true;
    });
    sessions_.push_back(std::move(session));
}

bool HttpServer::Start(int port, Handler handler, std::string* error) {
    if (!EnsureSockets()) {
        if (error) *error = "socket library initialization failed";
        return false;
    }
    SocketHandle s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == OE_INVALID_SOCKET) {
        if (error) *error = "socket() failed";
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // local only
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s, 16) != 0) {
        OE_CLOSE_SOCKET(s);
        if (error) *error = "cannot listen on 127.0.0.1:" + std::to_string(port) + " (port in use?)";
        return false;
    }
    socket_ = static_cast<intptr_t>(s);
    port_ = port;
    handler_ = std::move(handler);
    running_ = true;
    thread_ = std::thread([this] { Loop(); });
    OE_LOG_INFO("http", "listening on http://127.0.0.1:%d", port);
    return true;
}

void HttpServer::Stop() {
    if (!running_.exchange(false)) return;
    OE_CLOSE_SOCKET(static_cast<SocketHandle>(socket_));
    if (thread_.joinable()) thread_.join();
    std::vector<std::unique_ptr<Session>> sessions;
    {
        // Unblock the sessions' recv() calls, then wait for them outside the lock.
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        for (auto& session : sessions_) {
            if (session->socket != -1) shutdown(static_cast<SocketHandle>(session->socket), OE_SHUTDOWN_BOTH);
        }
        sessions.swap(sessions_);
    }
    for (auto& session : sessions) session->thread.join();
}

void HttpServer::Loop() {
    SocketHandle listenSocket = static_cast<SocketHandle>(socket_);
    while (running_) {
        SocketHandle client = accept(listenSocket, nullptr, nullptr);
        if (client == OE_INVALID_SOCKET) {
            if (!running_) break;
            continue;
        }
        HttpRequest req;
        int errorStatus = 400;
        HttpResponse res;
        bool readOk = ReadRequest(client, req, errorStatus);
        if (readOk && wsHandler_ && Lower(req.Header("upgrade")) == "websocket") {
            int status = 403;
            std::string key = req.Header("sec-websocket-key");
            if (!key.empty() && wsAccept_(req, &status)) {
                std::string accept = Base64Encode(reinterpret_cast<const uint8_t*>(Sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").data()), 20);
                std::string head = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n";
                if (SendAll(client, head.data(), head.size())) {
                    StartSession(static_cast<intptr_t>(client), req);
                    continue;  // the session owns the socket now
                }
                OE_CLOSE_SOCKET(client);
                continue;
            }
            res.status = key.empty() ? 400 : status;
            res.body = "{\"ok\":false,\"error\":{\"code\":\"websocket_refused\",\"message\":\"WebSocket upgrade refused\"}}";
            WriteResponse(client, res);
            OE_CLOSE_SOCKET(client);
            continue;
        }
        if (readOk) {
            try {
                res = handler_(req);
            } catch (const std::exception& e) {
                res.status = 500;
                res.body = std::string("{\"ok\":false,\"error\":{\"code\":\"internal_error\",\"message\":\"") + e.what() + "\"}}";
            }
        } else {
            res.status = errorStatus;
            res.body = "{\"ok\":false,\"error\":{\"code\":\"bad_request\",\"message\":\"malformed HTTP request\"}}";
        }
        WriteResponse(client, res);
        OE_CLOSE_SOCKET(client);
    }
}

bool HttpPostLocal(int port, const std::string& path, const std::string& body, std::string& response, std::string* error) {
    if (!EnsureSockets()) {
        if (error) *error = "socket library initialization failed";
        return false;
    }
    SocketHandle s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == OE_INVALID_SOCKET) {
        if (error) *error = "socket() failed";
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        OE_CLOSE_SOCKET(s);
        if (error) *error = "cannot connect to 127.0.0.1:" + std::to_string(port) + " (is `oe editor` running?)";
        return false;
    }
    std::string req = "POST " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                      "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    SendAll(s, req.data(), req.size());
    std::string data;
    char buf[8192];
    int n;
    while ((n = recv(s, buf, sizeof(buf), 0)) > 0) data.append(buf, static_cast<size_t>(n));
    OE_CLOSE_SOCKET(s);
    size_t split = data.find("\r\n\r\n");
    if (split == std::string::npos) {
        if (error) *error = "malformed HTTP response";
        return false;
    }
    response = data.substr(split + 4);
    return true;
}

}  // namespace oe
