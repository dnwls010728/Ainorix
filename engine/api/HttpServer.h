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

#include "core/Json.h"
#include "platform/Network.h"

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

// Minimal HTTP/1.1 server on a background thread, built on the platform sockets
// (platform/Network.h). Binds to 127.0.0.1 only; requests are handled one at a time and a
// silent client is dropped after an idle timeout. Port 0 picks a free port (see Port()).
class HttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    ~HttpServer();
    bool Start(int port, Handler handler, std::string* error);
    void Stop();
    int Port() const { return port_; }

private:
    void Loop();
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::unique_ptr<NetSocket> listener_;
    int port_ = 0;
    Handler handler_;
};

// Blocking HTTP POST to 127.0.0.1 (used by `oe mcp --connect`).
bool HttpPostLocal(int port, const std::string& path, const std::string& body, std::string& response, std::string* error);

std::string UrlDecode(const std::string& s);

}  // namespace oe
