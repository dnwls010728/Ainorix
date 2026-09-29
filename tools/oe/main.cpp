// `oe` - OwnEngine command line front-end.
//
// Every sub-command prints machine-readable JSON on stdout (logs go to
// stderr) and returns a non-zero exit code on failure, so AI agents and
// scripts can drive the engine without a UI.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "api/EditorService.h"
#include "api/HttpServer.h"
#include "api/McpServer.h"
#include "app/Engine.h"
#include "app/Project.h"
#include "assets/Assets.h"
#include "core/FileSystem.h"
#include "core/Log.h"
#include "physics/PhysicsWorld.h"
#include "platform/Platform.h"

using namespace oe;

namespace {

struct Args {
    std::vector<std::string> positional;
    std::map<std::string, std::string> flags;
    bool Has(const std::string& f) const { return flags.count(f) > 0; }
    std::string Get(const std::string& f, const std::string& def = "") const {
        auto it = flags.find(f);
        return it == flags.end() ? def : it->second;
    }
    int GetInt(const std::string& f, int def) const { return Has(f) ? std::atoi(Get(f).c_str()) : def; }
};

// Flags that take a value; everything else starting with -- is a boolean switch.
const char* kValueFlags[] = {"--out", "--width", "--height", "--frames", "--port", "--name", "--eye", "--target", "--fov", "--connect", "--size", "--to"};

Args ParseArgs(int argc, char** argv, int start) {
    Args a;
    for (int i = start; i < argc; ++i) {
        std::string s = argv[i];
        if (s.rfind("--", 0) == 0) {
            bool takesValue = false;
            for (const char* f : kValueFlags) takesValue = takesValue || s == f;
            a.flags[s] = takesValue && i + 1 < argc ? argv[++i] : "1";
        } else {
            a.positional.push_back(s);
        }
    }
    return a;
}

void PrintJson(const Json& j) {
    std::string s = j.dump(2);
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

int Fail(const std::string& code, const std::string& message, const std::string& hint = "") {
    Json err = Json::MakeObject();
    err["ok"] = false;
    err["error"]["code"] = code;
    err["error"]["message"] = message;
    if (!hint.empty()) err["error"]["hint"] = hint;
    PrintJson(err);
    return 1;
}

bool OpenOrFail(Engine& engine, const Args& a, int& exitCode) {
    std::string path = a.positional.empty() ? "." : a.positional[0];
    std::string err;
    if (!engine.Open(path, &err)) {
        exitCode = Fail("open_failed", err, "Pass a project directory, project.json or *.scene.json. Create a project with `oe new <dir>`.");
        return false;
    }
    return true;
}

Vec3 ParseVec(const std::string& s, Vec3 def) {
    if (s.empty()) return def;
    float v[3] = {def.x, def.y, def.z};
    std::sscanf(s.c_str(), "%f,%f,%f", &v[0], &v[1], &v[2]);
    return Vec3(v[0], v[1], v[2]);
}

// Runs the engine main loop: posted API jobs, real-time simulation and the
// optional native window. Returns when the window closes or `quit` is set.
void MainLoop(Engine& engine, Window* window, const std::atomic<bool>& quit) {
    RenderTarget frame;
    double last = PlatformTimeSeconds();
    double fpsTimer = last;
    int frames = 0;
    while (!quit) {
        engine.RunPostedJobs();
        if (window && !window->PumpEvents(engine.Input())) break;
        double now = PlatformTimeSeconds();
        engine.Tick(now - last);
        last = now;
        if (window) {
            int w = window->Width(), h = window->Height();
            if (w > 0 && h > 0) {
                if (frame.width != w || frame.height != h) frame.Resize(w, h);
                engine.RenderGameView(frame);
                window->Present(frame);
            }
            ++frames;
            if (now - fpsTimer >= 1.0) {
                window->SetTitle(Format("OwnEngine - %s - %d fps - frame %llu", engine.GetScene().name.c_str(), frames,
                                        static_cast<unsigned long long>(engine.Frame())));
                frames = 0;
                fpsTimer = now;
            }
            PlatformSleep(0.001);
        } else {
            PlatformSleep(0.002);
        }
    }
}

bool StartServer(HttpServer& server, Engine& engine, int port) {
    std::string editorDir = FindEditorDir();
    std::string err;
    if (!server.Start(port, [&engine, editorDir](const HttpRequest& r) { return HandleEditorRequest(engine, editorDir, r); }, &err)) {
        OE_LOG_ERROR("http", "%s", err.c_str());
        return false;
    }
    return true;
}

// ----- sub-commands ---------------------------------------------------------

int CmdHelp() {
    std::fprintf(stdout,
                 "OwnEngine %s (%s) - AI-friendly game engine\n\n"
                 "Usage: oe <command> [args]\n\n"
                 "  new <dir> [--name N]              Create a project with a sample scene\n"
                 "  run [path] [--port P]             Play the game in a native window (optionally serve the API)\n"
                 "  editor [path] [--port 7777] [--no-browser] [--window]\n"
                 "                                    Start the web editor (http://127.0.0.1:7777)\n"
                 "  render [path] --out f.png [--width W --height H --frames N --eye x,y,z --target x,y,z --grid --colliders]\n"
                 "                                    Headless render to PNG (after simulating N frames)\n"
                 "  exec [path] <command> [json] [--save]\n"
                 "                                    Run one API command, print the JSON result\n"
                 "  script [path] [--save]            Run newline-delimited {\"command\",\"args\"} from stdin\n"
                 "  mcp [path] [--port P]             MCP server on stdio (optionally also serve the editor)\n"
                 "  mcp --connect <port>              MCP server that drives a running editor\n"
                 "  import <path> <file> [--to rel]   Copy a model/texture/sound into the project (prints asset.info)\n"
                 "  package [path] [--out dist/Name] [--name N]\n"
                 "                                    Build a standalone game folder (Name.exe + game data)\n"
                 "  api [--markdown]                  Print the command reference\n"
                 "  version                           Print version info as JSON\n\n"
                 "[path] = project directory (default .), project.json or *.scene.json\n",
                 OE_VERSION, PlatformName());
    return 0;
}

int CmdNew(const Args& a) {
    if (a.positional.empty()) return Fail("missing_argument", "usage: oe new <dir> [--name N]");
    std::string dir = a.positional[0];
    std::string name = a.Get("--name", dir.substr(dir.find_last_of("/\\") + 1));
    std::string err;
    if (!CreateProject(dir, name, &err)) return Fail("create_failed", err);
    Json out = Json::MakeObject();
    out["ok"] = true;
    out["project"] = AbsolutePath(dir);
    out["next"] = "oe editor " + dir;
    PrintJson(out);
    return 0;
}

int CmdRender(const Args& a) {
    Engine engine;
    int code = 0;
    if (!OpenOrFail(engine, a, code)) return code;
    int frames = a.GetInt("--frames", 0);
    if (frames > 0) engine.Step(frames);

    RenderTarget rt;
    rt.Resize(a.GetInt("--width", 640), a.GetInt("--height", 360));
    float aspect = static_cast<float>(rt.width) / static_cast<float>(rt.height);
    RenderView view;
    bool hasCamera = MakeSceneView(engine.GetScene(), aspect, view);
    bool custom = a.Has("--eye") || a.Has("--target");
    if (custom) {
        Color clear = view.clearColor;
        view = MakeLookAtView(ParseVec(a.Get("--eye"), Vec3(6, 5, 8)), ParseVec(a.Get("--target"), Vec3(0, 0, 0)),
                              static_cast<float>(std::atof(a.Get("--fov", "60").c_str())), aspect);
        view.clearColor = clear;
    }
    view.drawGrid = a.Has("--grid");
    if (a.Has("--colliders")) AppendColliderLines(engine.GetScene(), view.lines);
    RenderStats stats = engine.Renderer().Render(engine.GetScene(), view, rt);

    std::string out = AbsolutePath(a.Get("--out", "frame.png"));
    if (!WritePng(out, rt.ToImage())) return Fail("write_failed", "cannot write " + out);
    Json res = Json::MakeObject();
    res["ok"] = true;
    res["result"]["path"] = out;
    res["result"]["width"] = rt.width;
    res["result"]["height"] = rt.height;
    res["result"]["frame"] = static_cast<uint64_t>(engine.Frame());
    res["result"]["camera"] = custom ? "custom" : (hasCamera ? "scene" : "default (scene has no active camera)");
    res["result"]["hash"] = Format("%016llx", static_cast<unsigned long long>(rt.Hash()));
    res["result"]["stats"]["entities"] = stats.drawnEntities;
    res["result"]["stats"]["triangles"] = stats.triangles;
    res["result"]["stats"]["ms"] = stats.milliseconds;
    PrintJson(res);
    return 0;
}

int CmdExec(const Args& a) {
    // oe exec [path] <command> [json]
    Args b = a;
    std::string path = ".";
    std::vector<std::string>& p = b.positional;
    if (p.empty()) return Fail("missing_argument", "usage: oe exec [path] <command> [json-args] [--save]");
    // First positional is a path if it exists on disk or looks like one.
    if (p.size() >= 2 && (FileExists(p[0]) || p[0] == ".")) {
        path = p[0];
        p.erase(p.begin());
    }
    Engine engine;
    Args openArgs;
    openArgs.positional.push_back(path);
    int code = 0;
    if (!OpenOrFail(engine, openArgs, code)) return code;
    std::string command = p[0];
    Json args = Json::MakeObject();
    if (p.size() >= 2) {
        std::string err;
        args = Json::parse(p[1], &err);
        if (!err.empty()) return Fail("invalid_json", err, "Arguments must be one JSON object, e.g. '{\"id\": 3}'. Quote it for your shell.");
    }
    Json res = engine.Call(command, args);
    if (res["ok"].asBool() && a.Has("--save")) {
        Json saved = engine.Call("scene.save", Json());
        if (!saved["ok"].asBool()) res = saved;
    }
    PrintJson(res);
    return res["ok"].asBool() ? 0 : 1;
}

int CmdScript(const Args& a) {
    Engine engine;
    int code = 0;
    if (!OpenOrFail(engine, a, code)) return code;
    std::string line;
    bool allOk = true;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::string err;
        Json msg = Json::parse(line, &err);
        Json res = err.empty() ? engine.Call(msg["command"].asString(""), msg["args"])
                               : Json::parse("{\"ok\":false,\"error\":{\"code\":\"invalid_json\"}}");
        allOk = allOk && res["ok"].asBool();
        std::string s = res.dump();
        std::fwrite(s.data(), 1, s.size(), stdout);
        std::fputc('\n', stdout);
    }
    if (allOk && a.Has("--save")) {
        Json saved = engine.Call("scene.save", Json());
        allOk = saved["ok"].asBool();
    }
    std::fflush(stdout);
    return allOk ? 0 : 1;
}

int CmdRun(const Args& a) {
    Engine engine;
    int code = 0;
    if (!OpenOrFail(engine, a, code)) return code;
    std::unique_ptr<Window> window = CreatePlatformWindow("OwnEngine - " + engine.GetScene().name, a.GetInt("--width", 1280), a.GetInt("--height", 720));
    if (!window) return Fail("no_window", std::string("platform '") + PlatformName() + "' has no native window", "Use `oe render` or `oe editor` instead.");
    HttpServer server;
    if (a.Has("--port") && !StartServer(server, engine, a.GetInt("--port", 7777))) return Fail("server_failed", "cannot start API server");
    if (!a.Has("--mute")) engine.EnableAudioOutput();
    engine.Play();
    std::atomic<bool> quit{false};
    MainLoop(engine, window.get(), quit);
    return 0;
}

int CmdEditor(const Args& a) {
    Engine engine;
    int code = 0;
    if (!OpenOrFail(engine, a, code)) return code;
    int port = a.GetInt("--port", 7777);
    HttpServer server;
    if (!StartServer(server, engine, port)) return Fail("server_failed", "cannot listen on port " + std::to_string(port), "Pick another port with --port.");
    std::string url = "http://127.0.0.1:" + std::to_string(port) + "/";
    OE_LOG_INFO("editor", "editor ready at %s (Ctrl+C to quit)", url.c_str());
    if (!a.Has("--no-browser")) PlatformOpenUrl(url);
    if (!a.Has("--mute")) engine.EnableAudioOutput();
    std::unique_ptr<Window> window;
    if (a.Has("--window")) window = CreatePlatformWindow("OwnEngine Game View", 960, 540);
    std::atomic<bool> quit{false};
    MainLoop(engine, window.get(), quit);
    return 0;
}

int CmdMcp(const Args& a) {
    Log::SetMinEchoLevel(LogLevel::Warn);  // keep stderr quiet for MCP hosts
    PlatformSetBinaryStdio();
    if (a.Has("--connect")) {
        int port = a.GetInt("--connect", 7777);
        CommandCaller call = [port](const std::string& command, const Json& args) {
            Json body = Json::MakeObject();
            body["command"] = command;
            body["args"] = args;
            std::string response, err;
            if (!HttpPostLocal(port, "/api/call", body.dump(), response, &err)) {
                Json e = Json::MakeObject();
                e["ok"] = false;
                e["error"]["code"] = "editor_unreachable";
                e["error"]["message"] = err;
                return e;
            }
            return Json::parse(response);
        };
        return RunMcpServer(call, std::cin, std::cout);
    }

    Engine engine;
    int code = 0;
    if (!OpenOrFail(engine, a, code)) return code;
    HttpServer server;
    if (a.Has("--port")) StartServer(server, engine, a.GetInt("--port", 7777));
    std::atomic<bool> quit{false};
    // MCP I/O on a worker thread; engine work stays on the main thread.
    std::thread io([&] {
        RunMcpServer([&engine](const std::string& c, const Json& args) { return engine.PostCall(c, args).get(); }, std::cin, std::cout);
        quit = true;
    });
    MainLoop(engine, nullptr, quit);
    io.join();
    return 0;
}

int CmdApi(const Args& a) {
    Engine engine;
    Json list = engine.Call("api.list", Json())["result"];
    if (!a.Has("--markdown")) {
        PrintJson(list);
        return 0;
    }
    std::string md = "# OwnEngine API reference\n\n"
                     "Generated by `oe api --markdown`. The same commands are available through `oe exec`, "
                     "HTTP `POST /api/call` and MCP tools (dots become underscores).\n\n";
    std::string group;
    for (const Json& c : list.items()) {
        std::string name = c["name"].asString();
        std::string g = name.substr(0, name.find('.'));
        if (g != group) {
            md += "## " + g + "\n\n";
            group = g;
        }
        md += "### `" + name + "`" + (c["mutates"].asBool() ? " *(undoable edit)*" : "") + "\n\n" + c["summary"].asString() + "\n\n";
        const Json& props = c["params"]["properties"];
        if (props.size() > 0) {
            md += "| arg | type | required | description |\n|---|---|---|---|\n";
            for (const auto& kv : props.members()) {
                bool req = false;
                for (const Json& r : c["params"]["required"].items()) req = req || r.asString() == kv.first;
                std::string type = kv.second["type"].isArray() ? "integer \\| string" : kv.second["type"].asString("");
                md += "| `" + kv.first + "` | " + type + " | " + (req ? "yes" : "") + " | " + kv.second["description"].asString("") + " |\n";
            }
            md += "\n";
        }
    }
    md += "## Component types\n\n";
    for (const Json& t : engine.Call("component.types", Json())["result"].items()) {
        md += "### " + t["name"].asString() + "\n\n" + t["doc"].asString() + "\n\n| field | type | default | description |\n|---|---|---|---|\n";
        for (const auto& kv : t["schema"]["properties"].members()) {
            md += "| `" + kv.first + "` | " + kv.second["x-oe-type"].asString() + " | `" + t["defaults"][kv.first].dump() + "` | " +
                  kv.second["description"].asString() + " |\n";
        }
        md += "\n";
    }
    std::fwrite(md.data(), 1, md.size(), stdout);
    return 0;
}

// Copies an external file (model, texture, sound) into the project. This is a
// CLI-only command: the API stays sandboxed to the project directory.
int CmdImport(const Args& a) {
    if (a.positional.size() < 2) return Fail("missing_argument", "usage: oe import <project> <file> [--to assets/models/name.glb]");
    Engine engine;
    Args open;
    open.positional.push_back(a.positional[0]);
    int code = 0;
    if (!OpenOrFail(engine, open, code)) return code;
    std::string src = a.positional[1];
    std::vector<unsigned char> bytes;
    if (!ReadBinaryFile(src, bytes)) return Fail("not_found", "cannot read " + src);
    std::string name = src.substr(src.find_last_of("/\\") + 1);
    std::string kind = AssetManager::KindOf(name);
    std::string folder = kind == "model" ? "assets/models/" : kind == "texture" ? "assets/textures/" : kind == "audio" ? "sounds/" : "assets/";
    std::string rel = a.Get("--to", folder + name);
    std::string dest;
    try {
        dest = engine.ResolvePath(rel);
    } catch (const ApiError& e) {
        return Fail(e.code, e.what(), e.hint);
    }
    CreateDirectories(ParentPath(dest));
    FILE* f = std::fopen(dest.c_str(), "wb");
    if (!f || std::fwrite(bytes.data(), 1, bytes.size(), f) != bytes.size()) {
        if (f) std::fclose(f);
        return Fail("write_failed", "cannot write " + dest);
    }
    std::fclose(f);
    Json res = Json::MakeObject();
    res["ok"] = true;
    res["result"]["path"] = rel;
    res["result"]["kind"] = kind;
    if (kind == "model" || kind == "texture" || kind == "audio") {
        Json info = engine.Call("asset.info", Json(Json::Object{{"path", rel}}));
        if (info["ok"].asBool()) res["result"]["info"] = info["result"];
    }
    PrintJson(res);
    return 0;
}

// Builds a standalone game folder: <out>/<Name>.exe (the player runtime) plus
// <out>/game/ with the project's scenes, scripts and assets.
int CmdPackage(const Args& a) {
    Engine engine;
    int code = 0;
    if (!OpenOrFail(engine, a, code)) return code;
    const std::string projectDir = engine.ProjectDir();
    if (!FileExists(JoinPath(projectDir, "project.json"))) {
        return Fail("not_a_project", "no project.json in " + projectDir, "Package a project directory (created with `oe new`).");
    }
    std::string name = a.Get("--name", engine.ProjectName().empty() ? "Game" : engine.ProjectName());
    for (char& c : name) {
        if (std::string("<>:\"/\\|?*").find(c) != std::string::npos || static_cast<unsigned char>(c) < 32) c = '_';
    }
    std::string out = AbsolutePath(a.Get("--out", "dist/" + name));
    const std::string projectAbs = AbsolutePath(projectDir);
    if (out == projectAbs || out.rfind(projectAbs + "/", 0) == 0) {
        return Fail("invalid_output", "the output folder must be outside the project", "Use --out dist/" + name + ".");
    }

#ifdef _WIN32
    const std::string exeExt = ".exe";
#else
    const std::string exeExt = "";
#endif
    std::string player = JoinPath(ExecutableDirectory(), "oe_player" + exeExt);
    if (!FileExists(player)) return Fail("player_missing", "player runtime not found at " + player, "Build it with build.bat (target oe_player).");

    // Only reuse a folder that is empty or an earlier package (has game/project.json).
    std::string gameDir = JoinPath(out, "game");
    if (IsDirectory(out) && !ListFiles(out, "", false).empty() && !FileExists(JoinPath(gameDir, "project.json"))) {
        return Fail("output_not_empty", out + " exists and is not an OwnEngine package", "Pick another --out folder or empty it.");
    }
    if (!RemoveAll(gameDir)) return Fail("write_failed", "cannot clean " + gameDir, "Close the running game first.");

    // Game data: everything in the project except hidden files and agent notes.
    Json files = Json::MakeArray();
    double bytes = 0;
    for (const std::string& src : ListFiles(projectDir, "", true)) {
        std::string rel = RelativePath(src, projectDir);
        bool hidden = rel[0] == '.' || rel.find("/.") != std::string::npos;
        if (hidden || rel == "AGENTS.md" || rel == "CLAUDE.md") continue;
        if (!CopyFileTo(src, JoinPath(gameDir, rel))) return Fail("write_failed", "cannot copy " + rel);
        std::vector<unsigned char> data;
        if (ReadBinaryFile(src, data)) bytes += static_cast<double>(data.size());
        files.push(rel);
    }
    std::string exe = JoinPath(out, name + exeExt);
    if (!CopyFileTo(player, exe)) return Fail("write_failed", "cannot write " + exe, "Close the running game first.");

    Json res = Json::MakeObject();
    res["ok"] = true;
    res["result"]["exe"] = exe;
    res["result"]["gameDir"] = gameDir;
    res["result"]["files"] = files;
    res["result"]["dataBytes"] = bytes;
    res["result"]["next"] = "Run " + exe + " or zip the folder " + out + " to share it.";
    PrintJson(res);
    return 0;
}

int CmdVersion() {
    Json v = Json::MakeObject();
    v["engine"] = "OwnEngine";
    v["version"] = OE_VERSION;
    v["platform"] = PlatformName();
    v["renderer"] = "software";
    PrintJson(v);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return CmdHelp();
    std::string cmd = argv[1];
    Args a = ParseArgs(argc, argv, 2);
    if (cmd == "help" || cmd == "--help" || cmd == "-h") return CmdHelp();
    if (cmd == "version" || cmd == "--version") return CmdVersion();
    if (cmd == "new") return CmdNew(a);
    if (cmd == "run") return CmdRun(a);
    if (cmd == "editor") return CmdEditor(a);
    if (cmd == "render") return CmdRender(a);
    if (cmd == "exec") return CmdExec(a);
    if (cmd == "script") return CmdScript(a);
    if (cmd == "mcp") return CmdMcp(a);
    if (cmd == "api") return CmdApi(a);
    if (cmd == "import") return CmdImport(a);
    if (cmd == "package") return CmdPackage(a);
    return Fail("unknown_command", "unknown command '" + cmd + "'", "Run `oe help`.");
}
