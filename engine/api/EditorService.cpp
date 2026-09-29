#include "api/EditorService.h"

#include <chrono>
#include <cstdlib>
#include <cstring>

#include "app/Engine.h"
#include "core/FileSystem.h"
#include "core/Log.h"
#include "physics/PhysicsWorld.h"
#include "platform/Platform.h"
#include "render/GpuRenderer.h"

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

// One editor viewport frame: the game camera, or the free editor camera with grid.
struct FrameRequest {
    int width = 640, height = 360;
    bool game = false;
    Vec3 eye{6, 5, 8};
    Vec3 target{0, 0, 0};
    float fov = 60.0f;
    bool grid = false;
    bool colliders = false;
    EntityId selected = kNullEntity;
};

// Main thread only.
Image RenderFrame(Engine& engine, const FrameRequest& fr) {
    int w = fr.width < 16 ? 16 : (fr.width > 4096 ? 4096 : fr.width);
    int h = fr.height < 16 ? 16 : (fr.height > 4096 ? 4096 : fr.height);
    RenderTarget rt;
    rt.Resize(w, h);
    float aspect = static_cast<float>(w) / static_cast<float>(h);
    RenderView view;
    MakeSceneView(engine.GetScene(), aspect, view);
    if (!fr.game) {
        Color clear = view.clearColor;
        float fov = fr.fov > 1.0f && fr.fov < 179.0f ? fr.fov : 60.0f;
        view = MakeLookAtView(fr.eye, fr.target, fov, aspect);
        view.clearColor = clear;
        view.drawGrid = fr.grid;
    }
    view.highlight = fr.selected;
    if (fr.colliders) AppendColliderLines(engine.GetScene(), view.lines);
    engine.AppendDebugLines(view.lines);
    engine.DisplayRenderer().Render(engine.GetScene(), view, rt);
    return rt.ToImage();
}

Vec3 JsonVec(const Json& j, Vec3 def) {
    if (!j.isArray() || j.size() < 3) return def;
    return Vec3(j[0].asFloat(def.x), j[1].asFloat(def.y), j[2].asFloat(def.z));
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
        FrameRequest fr;
        fr.width = std::atoi(req.Query("w", "640").c_str());
        fr.height = std::atoi(req.Query("h", "360").c_str());
        fr.game = req.Query("game") == "1";
        fr.eye = ParseVec(req.Query("eye"), fr.eye);
        fr.target = ParseVec(req.Query("target"), fr.target);
        fr.fov = static_cast<float>(std::atof(req.Query("fov", "60").c_str()));
        fr.grid = req.Query("grid") == "1";
        fr.colliders = req.Query("colliders") == "1";
        fr.selected = static_cast<EntityId>(std::strtoul(req.Query("sel", "0").c_str(), nullptr, 10));
        auto future = engine.PostJob([&engine, fr] { return EncodePng(RenderFrame(engine, fr)); });
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

bool AcceptEditorStream(const HttpRequest& req, int* status) {
    *status = 403;
    if (req.path != "/api/stream") {
        *status = 404;
        return false;
    }
    if (!HostAllowed(req)) return false;
    // Browsers send Origin on WebSocket upgrades and do not apply CORS to
    // them, so without this check any web page could watch the viewport.
    std::string origin = req.Header("origin");
    if (!origin.empty()) {
        std::string host = origin.substr(origin.find("://") == std::string::npos ? 0 : origin.find("://") + 3);
        std::string name = host.substr(0, host.rfind(':'));
        if (name != "127.0.0.1" && name != "localhost" && name != "[::1]") return false;
    }
    return true;
}

void RunEditorStream(Engine& engine, const HttpRequest&, WebSocket& ws) {
    // Tell the editor which renderer it is looking at (it picks the frame size from that).
    auto info = engine.PostJob([&engine] {
        std::string name = engine.DisplayRenderer().Name();
        return std::vector<uint8_t>(name.begin(), name.end());
    });
    if (!WaitFor(info)) return;
    std::vector<uint8_t> nameBytes = info.get();
    std::string name(nameBytes.begin(), nameBytes.end());
    Json h = Json::MakeObject();
    h["type"] = "hello";
    h["renderer"] = name;
    h["gpu"] = name.rfind("gpu", 0) == 0;
    if (!ws.SendText(h.dump())) return;

    std::string message;
    while (ws.Read(message)) {
        std::string err;
        Json j = Json::parse(message, &err);
        if (!err.empty() || !j.isObject()) continue;
        FrameRequest fr;
        fr.width = j["w"].asInt(640);
        fr.height = j["h"].asInt(360);
        fr.game = j["game"].asBool(false);
        fr.eye = JsonVec(j["eye"], fr.eye);
        fr.target = JsonVec(j["target"], fr.target);
        fr.fov = j["fov"].asFloat(60.0f);
        fr.grid = j["grid"].asBool(false);
        fr.colliders = j["colliders"].asBool(false);
        fr.selected = static_cast<EntityId>(j["sel"].asNumber(0));
        int quality = j["quality"].asInt(85);
        // Render + read back on the main thread, encode here (off the main thread).
        std::shared_ptr<Image> image = std::make_shared<Image>();
        auto job = engine.PostJob([&engine, fr, image] {
            *image = RenderFrame(engine, fr);
            return std::vector<uint8_t>();
        });
        if (!WaitFor(job)) break;
        std::vector<uint8_t> jpeg = EncodeJpeg(*image, quality);
        if (!ws.SendBinary(jpeg.data(), jpeg.size())) break;
    }
}

bool StartEditorServer(HttpServer& server, Engine& engine, int port, std::string* error) {
    std::string editorDir = FindEditorDir();
    server.OnWebSocket(AcceptEditorStream, [&engine](const HttpRequest& r, WebSocket& ws) { RunEditorStream(engine, r, ws); });
    return server.Start(port, [&engine, editorDir](const HttpRequest& r) { return HandleEditorRequest(engine, editorDir, r); }, error);
}

}  // namespace oe
