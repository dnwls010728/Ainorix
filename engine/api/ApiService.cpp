#include "api/ApiService.h"

#include <chrono>

#include "app/Engine.h"

namespace oe {

namespace {

HttpResponse JsonResponse(const Json& j, int status = 200) {
    HttpResponse r;
    r.status = status;
    r.body = j.dump();
    return r;
}

HttpResponse Error(int status, const char* code, const std::string& message) {
    Json j = Json::MakeObject();
    j["ok"] = false;
    j["error"]["code"] = code;
    j["error"]["message"] = message;
    return JsonResponse(j, status);
}

// Blocks DNS-rebinding style access: only local host names are accepted.
bool HostAllowed(const HttpRequest& req) {
    std::string host = req.Header("host");
    std::string name = host.substr(0, host.rfind(':'));
    return name == "127.0.0.1" || name == "localhost" || name == "[::1]";
}

template <class T>
bool WaitFor(std::future<T>& f) {
    return f.wait_for(std::chrono::seconds(30)) == std::future_status::ready;
}

}  // namespace

HttpResponse HandleApiRequest(Engine& engine, const HttpRequest& req) {
    if (!HostAllowed(req)) return Error(403, "forbidden_host", "only localhost requests are accepted");

    if (req.path == "/api/call") {
        if (req.method != "POST") return Error(405, "method_not_allowed", "use POST");
        // Requiring JSON forces a CORS preflight for cross-origin pages, which we never grant.
        if (req.Header("content-type").find("application/json") == std::string::npos) {
            return Error(400, "content_type", "Content-Type must be application/json");
        }
        std::string parseError;
        Json body = Json::parse(req.body, &parseError);
        if (!parseError.empty()) return Error(400, "invalid_json", parseError);
        std::string command = body["command"].asString("");
        if (command.empty()) return Error(400, "missing_command", "body must be {\"command\": \"name\", \"args\": {...}}");
        auto future = engine.PostCall(command, body["args"]);
        if (!WaitFor(future)) return Error(500, "timeout", "engine did not respond");
        return JsonResponse(future.get());
    }

    if (req.path == "/api/commands" && req.method == "GET") {
        auto future = engine.PostCall("api.list", Json());
        if (!WaitFor(future)) return Error(500, "timeout", "engine did not respond");
        return JsonResponse(future.get());
    }

    return Error(404, "not_found", "unknown route " + req.method + " " + req.path + " (use POST /api/call)");
}

bool StartApiServer(HttpServer& server, Engine& engine, int port, std::string* error) {
    return server.Start(port, [&engine](const HttpRequest& r) { return HandleApiRequest(engine, r); }, error);
}

}  // namespace oe
