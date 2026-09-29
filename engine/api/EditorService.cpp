#include "api/EditorService.h"

#include <chrono>
#include <cstdlib>
#include <cstring>

#include "app/Engine.h"
#include "core/FileSystem.h"
#include "core/Log.h"
#include "platform/Platform.h"

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

bool SafeFileName(const std::string& name) {
    if (name.empty() || name[0] == '.') return false;
    for (char c : name) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

const char* MimeFor(const std::string& name) {
    auto ends = [&](const char* ext) {
        size_t n = std::strlen(ext);
        return name.size() >= n && name.compare(name.size() - n, n, ext) == 0;
    };
    if (ends(".html")) return "text/html; charset=utf-8";
    if (ends(".js")) return "text/javascript; charset=utf-8";
    if (ends(".css")) return "text/css; charset=utf-8";
    if (ends(".svg")) return "image/svg+xml";
    if (ends(".png")) return "image/png";
    if (ends(".json")) return "application/json";
    return "application/octet-stream";
}

Vec3 ParseVec(const std::string& s, Vec3 def) {
    if (s.empty()) return def;
    float v[3] = {def.x, def.y, def.z};
    const char* p = s.c_str();
    for (int i = 0; i < 3 && *p; ++i) {
        char* end = nullptr;
        v[i] = std::strtof(p, &end);
        if (end == p) break;
        p = end;
        if (*p == ',') ++p;
    }
    return Vec3(v[0], v[1], v[2]);
}

template <class T>
bool WaitFor(std::future<T>& f) {
    return f.wait_for(std::chrono::seconds(30)) == std::future_status::ready;
}

}  // namespace

std::string FindEditorDir() {
    std::string candidates[] = {JoinPath(ExecutableDirectory(), "editor"), JoinPath(OE_SOURCE_DIR, "editor")};
    for (const std::string& c : candidates) {
        if (FileExists(JoinPath(c, "index.html"))) return c;
    }
    return candidates[1];
}

HttpResponse HandleEditorRequest(Engine& engine, const std::string& editorDir, const HttpRequest& req) {
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

    if (req.path == "/api/frame.png" && req.method == "GET") {
        int w = std::atoi(req.Query("w", "640").c_str());
        int h = std::atoi(req.Query("h", "360").c_str());
        w = w < 16 ? 16 : (w > 2048 ? 2048 : w);
        h = h < 16 ? 16 : (h > 2048 ? 2048 : h);
        bool game = req.Query("game") == "1";
        Vec3 eye = ParseVec(req.Query("eye"), Vec3(6, 5, 8));
        Vec3 target = ParseVec(req.Query("target"), Vec3(0, 0, 0));
        float fov = static_cast<float>(std::atof(req.Query("fov", "60").c_str()));
        bool grid = req.Query("grid") == "1";
        EntityId sel = static_cast<EntityId>(std::strtoul(req.Query("sel", "0").c_str(), nullptr, 10));
        auto future = engine.PostJob([&engine, w, h, game, eye, target, fov, grid, sel] {
            RenderTarget rt;
            rt.Resize(w, h);
            float aspect = static_cast<float>(w) / static_cast<float>(h);
            RenderView view;
            MakeSceneView(engine.GetScene(), aspect, view);
            if (!game) {
                Color clear = view.clearColor;
                view = MakeLookAtView(eye, target, fov, aspect);
                view.clearColor = clear;
                view.drawGrid = grid;
            }
            view.highlight = sel;
            engine.Renderer().Render(engine.GetScene(), view, rt);
            return EncodePng(rt.ToImage());
        });
        if (!WaitFor(future)) return Error(500, "timeout", "engine did not respond");
        std::vector<uint8_t> png = future.get();
        HttpResponse r;
        r.contentType = "image/png";
        r.body.assign(png.begin(), png.end());
        return r;
    }

    if (req.method == "GET") {
        std::string name = req.path == "/" ? "index.html" : req.path.substr(1);
        if (!SafeFileName(name)) return Error(404, "not_found", "no such file");
        std::string text;
        if (!ReadTextFile(JoinPath(editorDir, name), text)) return Error(404, "not_found", "no such file: " + name);
        HttpResponse r;
        r.contentType = MimeFor(name);
        r.body = std::move(text);
        return r;
    }

    return Error(404, "not_found", "unknown route " + req.method + " " + req.path);
}

}  // namespace oe
