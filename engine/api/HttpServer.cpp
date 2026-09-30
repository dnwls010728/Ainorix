#include "api/HttpServer.h"

#include <cctype>
#include <cstring>


#include "core/Log.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
#define OE_INVALID_SOCKET INVALID_SOCKET
#define OE_CLOSE_SOCKET closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
#define OE_INVALID_SOCKET (-1)
#define OE_CLOSE_SOCKET ::close
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

// ----- HttpServer -------------------------------------------------------------------

HttpServer::~HttpServer() { Stop(); }

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
