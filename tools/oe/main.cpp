// `oe` - OwnEngine command line front-end.
//
// Every sub-command prints machine-readable JSON on stdout (logs go to
// stderr) and returns a non-zero exit code on failure, so AI agents and
// scripts can drive the engine without a UI.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "api/ApiService.h"
#include "api/HttpServer.h"
#include "api/McpServer.h"
#include "app/AndroidPackage.h"
#include "app/Engine.h"
#include "app/DedicatedServer.h"
#include "app/Project.h"
#include "assets/Assets.h"
#include "core/FileSystem.h"
#include "core/Log.h"
#include "physics/PhysicsWorld.h"
#include "platform/Platform.h"
#include "render/GpuRenderer.h"
#include "team/TeamCommands.h"
#if OE_NATIVE_EDITOR
#include "editor/Editor.h"
#endif

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
const char* kValueFlags[] = {"--out", "--width", "--height", "--frames", "--port", "--name", "--eye", "--target", "--fov", "--connect", "--size", "--to", "--renderer", "--screenshot", "--select", "--lang", "--script",
                            "--package", "--sdk", "--keystore", "--ks-pass", "--key-alias", "--key-pass", "--abi", "--version-code", "--version-name", "--orientation", "--save-dir", "--min-players", "--seed", "--api-port", "--players", "--agent"};

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
    if (a.Has("--save-dir")) engine.Saves().Configure(a.Get("--save-dir"));
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

// Turns on the GPU renderer as asked by --renderer: auto (default: GPU when
// available, else software), gpu (required) or software. With a window the
// GPU device presents into it. Returns false (and sets exitCode) only when
// the GPU was required and is unavailable.
bool SetupRenderer(Engine& engine, const Args& a, Window* window, int& exitCode) {
    std::string mode = a.Get("--renderer", "auto");
    if (mode == "software") return true;
    if (mode != "auto" && mode != "gpu") {
        exitCode = Fail("invalid_argument", "--renderer must be auto, gpu or software");
        return false;
    }
    std::string err;
    if (engine.EnableGpu(window, &err)) return true;
    if (mode == "gpu") {
        exitCode = Fail("gpu_unavailable", err, "Use --renderer software.");
        return false;
    }
    OE_LOG_WARN("render", "GPU renderer unavailable, using the software renderer: %s", err.c_str());
    return true;
}

// Runs the engine main loop: posted API jobs, real-time simulation and the
// optional native window. Returns when the window closes or `quit` is set.
// The window shows the GPU renderer when it was enabled for that window.
// `team` (optional) is updated every iteration so agent turns advance without API calls.
void MainLoop(Engine& engine, Window* window, const std::atomic<bool>& quit, bool gpuWindow = false, const TeamHandle* team = nullptr) {
    RenderTarget frame;
    double last = PlatformTimeSeconds();
    double fpsTimer = last;
    int frames = 0;
    while (!quit) {
        engine.RunPostedJobs();
        if (team) team->update();
        if (window && !window->PumpEvents(engine.Input())) break;
        double now = PlatformTimeSeconds();
        engine.Tick(now - last);
        last = now;
        if (window) {
            int w = window->Width(), h = window->Height();
            if (engine.Gpu() && gpuWindow) {
                engine.PresentGameView();
            } else if (w > 0 && h > 0) {
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
    std::string err;
    if (!StartApiServer(server, engine, port, &err)) {
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
                 "  editor [path] [--port 7777] [--lang en|ko|ja]\n"
                 "                                    Editor window; agents attach to its API on the port\n"
                 "  editor [path] --screenshot f.png [--width W --height H --frames N --select Name --2d --play --game --players N --script scripts/x.lua --team --chat --agent new|id --lang ko]\n"
                 "                                    Headless: render the native editor UI to a PNG\n"
                 "  render [path] --out f.png [--width W --height H --frames N --eye x,y,z --target x,y,z --grid --colliders]\n"
                 "                                    Headless render to PNG (after simulating N frames)\n"
                 "  serve-game [path] [--port P --min-players N --seed S --frames N --api-port P]\n"
                 "                                    Dedicated 60 Hz game server, no window/audio (Ctrl+C stops)\n"
                 "  serve <dir> [--port 8080] [--no-browser]\n"
                 "                                    Serve a web package (oe package --web) on http://127.0.0.1\n"
                 "  exec [path] <command> [json] [--save]\n"
                 "                                    Run one API command, print the JSON result\n"
                 "  script [path] [--save]            Run newline-delimited {\"command\",\"args\"} from stdin\n"
                 "  mcp [path] [--port P]             MCP server on stdio (optionally also serve the HTTP API)\n"
                 "  mcp --connect <port>              MCP server that drives a running editor (or oe run/mcp --port)\n"
                 "  exec --connect <port> <command> [json] [--save]   one command to a running editor, without MCP\n"
                 "  script --connect <port> [--save]  many commands (JSON lines on stdin) to a running editor\n"
                 "  import <path> <file> [--to rel]   Copy a model/texture/sound into the project (prints asset.info)\n"
                 "  package [path] [--out dist/Name] [--name N] [--web]\n"
                 "                                    Build a standalone game: Name.exe + game data, or with --web\n"
                 "                                    an HTML5/WebGL2 folder (index.html + wasm) for any web host\n"
                 "  package [path] --android [--aab] [--package com.x.y] [--install] [--keystore f --ks-pass P --key-alias A]\n"
                 "                                    Build a signed Android APK, or with --aab an App Bundle for Google Play\n"
                 "                                    (debug key unless --keystore); see docs/ANDROID.md\n"
                 "  api [--markdown]                  Print the command reference\n"
                 "  version                           Print version info as JSON\n\n"
                 "[path] = project directory (default .), project.json or *.scene.json\n"
                 "--renderer auto|gpu|software (run, mcp --port, render): auto = GPU when available.\n"
                 "  render defaults to software (deterministic hash); the others to auto.\n"
                 "--save-dir <dir>: opt into persistent save slots; otherwise saves stay in memory.\n",
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
    view.shaderTime = static_cast<float>(engine.SimTime());
    if (a.Has("--colliders")) {
        TilesetLookup tilesets = engine.Assets().Tilesets();
        AppendColliderLines(engine.GetScene(), view.lines, &tilesets);
    }
    IRenderer* renderer = &engine.Renderer();
    if (a.Get("--renderer", "software") != "software") {
        if (!SetupRenderer(engine, a, nullptr, code)) return code;
        if (engine.Gpu()) renderer = engine.Gpu();
    }
    RenderStats stats = renderer->Render(engine.GetScene(), view, rt);

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
    res["result"]["renderer"] = renderer->Name();
    res["result"]["stats"]["entities"] = stats.drawnEntities;
    res["result"]["stats"]["triangles"] = stats.triangles;
    res["result"]["stats"]["ms"] = stats.milliseconds;
    PrintJson(res);
    return 0;
}

// One command sent to a running engine (`oe editor`, or `oe run` / `oe mcp` with --port) over
// its HTTP API: what `--connect <port>` does for exec, script and mcp. `agent` (optional) is a
// team agent's id, shown by the editor next to the edit.
Json RemoteCall(int port, const std::string& command, const Json& args, const std::string& agent) {
    Json body = Json::MakeObject();
    body["command"] = command;
    body["args"] = args.isNull() ? Json::MakeObject() : args;
    if (!agent.empty()) body["agent"] = agent;
    std::string response, err;
    if (!HttpPostLocal(port, "/api/call", body.dump(), response, &err)) {
        Json e = Json::MakeObject();
        e["ok"] = false;
        e["error"]["code"] = "editor_unreachable";
        e["error"]["message"] = err;
        e["error"]["hint"] = "Nothing answers on 127.0.0.1:" + std::to_string(port) + ". Start `oe editor <project>` (port 7777) or pass the port it reports.";
        return e;
    }
    return Json::parse(response);
}

int CmdExec(const Args& a) {
    // oe exec [path] <command> [json]
    Args b = a;
    std::string path = ".";
    std::vector<std::string>& p = b.positional;
    if (p.empty()) return Fail("missing_argument", "usage: oe exec [path] <command> [json-args] [--save] | oe exec --connect <port> <command> [json-args] [--save]");
    // First positional is a path if it exists on disk or looks like one.
    if (p.size() >= 2 && (FileExists(p[0]) || p[0] == ".")) {
        path = p[0];
        p.erase(p.begin());
    }
    if (a.Has("--connect")) {
        // The running editor's scene, live: no project is opened here (a path, if given, is ignored).
        const int port = a.GetInt("--connect", 7777);
        Json args = Json::MakeObject();
        if (p.size() >= 2) {
            std::string err;
            args = Json::parse(p[1], &err);
            if (!err.empty()) return Fail("invalid_json", err, "Arguments must be one JSON object, e.g. '{\"id\": 3}'. Quote it for your shell.");
        }
        Json res = RemoteCall(port, p[0], args, a.Get("--agent"));
        if (res["ok"].asBool() && a.Has("--save")) {
            Json saved = RemoteCall(port, "scene.save", Json(), a.Get("--agent"));
            if (!saved["ok"].asBool()) res = saved;
        }
        PrintJson(res);
        return res["ok"].asBool() ? 0 : 1;
    }
    Engine engine;
    RegisterTeamCommands(engine.Commands());
    Args openArgs;
    openArgs.flags = a.flags;
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
    const bool remote = a.Has("--connect");  // lines go to the running editor instead of a project opened here
    const int port = a.GetInt("--connect", 7777);
    Engine engine;
    RegisterTeamCommands(engine.Commands());
    int code = 0;
    if (!remote && !OpenOrFail(engine, a, code)) return code;
    auto call = [&](const std::string& command, const Json& args) { return remote ? RemoteCall(port, command, args, a.Get("--agent")) : engine.Call(command, args); };
    std::string line;
    bool allOk = true;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::string err;
        Json msg = Json::parse(line, &err);
        Json res = err.empty() ? call(msg["command"].asString(""), msg["args"])
                               : Json::parse("{\"ok\":false,\"error\":{\"code\":\"invalid_json\"}}");
        allOk = allOk && res["ok"].asBool();
        std::string s = res.dump();
        std::fwrite(s.data(), 1, s.size(), stdout);
        std::fputc('\n', stdout);
    }
    if (allOk && a.Has("--save")) {
        Json saved = call("scene.save", Json());
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
    if (!SetupRenderer(engine, a, window.get(), code)) return code;
    HttpServer server;
    if (a.Has("--port") && !StartServer(server, engine, a.GetInt("--port", 7777))) return Fail("server_failed", "cannot start API server");
    if (!a.Has("--mute")) engine.EnableAudioOutput();
    engine.Play();
    std::atomic<bool> quit{false};
    MainLoop(engine, window.get(), quit, true);
    return 0;
}

#if OE_NATIVE_EDITOR
std::string EditorLayoutFile(const Engine& engine) {
    return JoinPath(JoinPath(engine.ProjectDir().empty() ? std::string(".") : engine.ProjectDir(), ".oe"), "editor.ini");
}

// `oe editor --screenshot`: the native editor drawn offscreen (GPU required),
// so agents and tests can look at the editor itself.
int CmdEditorScreenshot(Engine& engine, const Args& a) {
    int code = 0;
    if (!SetupRenderer(engine, a, nullptr, code)) return code;
    if (!engine.Gpu()) return Fail("gpu_unavailable", "the native editor needs the GPU renderer", "Install a GPU backend (Linux: libegl-dev libgles-dev) or use `oe render`.");
    NativeEditor::Options options;  // no layout file: the default layout, reproducible
    options.language = a.Get("--lang", "en");
    NativeEditor editor(engine, nullptr, options);
    std::string err;
    if (!editor.Init(&err)) return Fail("editor_failed", err);
    if (a.Has("--select")) {
        Json r = engine.Call("entity.get", Json(Json::Object{{"id", Json(a.Get("--select"))}}));
        if (!r["ok"].asBool()) return Fail("not_found", "no entity " + a.Get("--select"));
        editor.Select(static_cast<EntityId>(r["result"]["id"].asNumber(0)));
    }
    if (a.Has("--2d")) editor.SetSceneView2D(true);
    if (a.Has("--players")) {
        int players = a.GetInt("--players", 1);
        editor.SetNetworkPlayers(players);
        if (players < 1 || players > 8) return Fail("invalid_argument", "--players requires 1..8 players");
        if (players > 1) {
            Json args = Json::MakeObject(); args["count"] = players - 1;
            Json result = engine.Call("net.spawn_local_peers", args);
            if (!result["ok"].asBool()) { PrintJson(result); return 1; }
        }
    }
    if (a.Has("--play")) engine.Call("sim.play", Json());
    int w = a.GetInt("--width", 1600), h = a.GetInt("--height", 900);
    int frames = std::max(a.Has("--agent") || a.Has("--team") || a.Has("--chat") ? 8 : a.Has("--script") || a.Has("--players") || a.Has("--game") ? 5 : 2, a.GetInt("--frames", 3));  // the dock layout settles on the second frame
    RenderTarget rt;
    for (int i = 0; i < frames; ++i) {
        editor.Update({}, w, h, 1.0f, Engine::kFixedDt);
        if (i == 1 && (a.Has("--play") || a.Has("--game"))) editor.FocusGameView(true);  // as after pressing Play; --game: the stopped Game view (UI editing)
        if (i == 3 && a.Has("--players")) editor.FocusNetworkPanel();
        if (i == 3 && a.Has("--team")) editor.FocusTeamPanel();
        if (i == 3 && a.Has("--chat")) editor.FocusTeamChat();
        if (i == 4 && a.Has("--agent")) editor.EditAgent(a.Get("--agent") == "new" ? "" : a.Get("--agent"));  // the profile dialog
        if (i == 2 && a.Has("--script")) editor.OpenScript(a.Get("--script"));  // after the default layout settled
        if (!editor.DrawToImage(rt)) return Fail("render_failed", "cannot read back the editor frame");
    }
    std::string out = AbsolutePath(a.Get("--screenshot"));
    if (!WritePng(out, rt.ToImage())) return Fail("write_failed", "cannot write " + out);
    Json res = Json::MakeObject();
    res["ok"] = true;
    res["result"]["path"] = out;
    res["result"]["width"] = rt.width;
    res["result"]["height"] = rt.height;
    res["result"]["renderer"] = engine.Gpu()->Name();
    PrintJson(res);
    return 0;
}
#endif

int CmdEditor(const Args& a) {
    Engine engine;
    std::shared_ptr<TeamHandle> team = RegisterTeamCommands(engine.Commands());
    int code = 0;
    if (!OpenOrFail(engine, a, code)) return code;
#if OE_NATIVE_EDITOR
    if (a.Has("--screenshot")) return CmdEditorScreenshot(engine, a);
    // Native editor: one window with the panels; the API stays reachable
    // for agents (oe mcp --connect) on the same port.
    PlatformEnableHighDpi();
    std::unique_ptr<Window> window = CreatePlatformWindow("OwnEngine Editor - " + engine.ProjectName(), 1600, 900);
    if (!window) {
        return Fail("no_window", std::string("platform '") + PlatformName() + "' has no native window for the editor",
                    "Agents: `oe mcp <project>` or `oe exec`; to look at the editor headless: `oe editor <project> --screenshot shot.png`.");
    }
    std::string gpuError;
    if (!engine.EnableGpu(window.get(), &gpuError)) {
        return Fail("gpu_unavailable", "the editor needs the GPU renderer: " + gpuError, "Update the graphics driver; `oe run --renderer software` still plays the game.");
    }
    int port = a.GetInt("--port", 7777);
    HttpServer server;
    NativeEditor::Options options;
    options.layoutFile = EditorLayoutFile(engine);
    options.language = a.Get("--lang");
    if (StartServer(server, engine, port)) {
        options.serverInfo = "API http://127.0.0.1:" + std::to_string(port);
        team->host.apiPort = port;  // agents drive this editor through `oe mcp --connect`
    } else {
        OE_LOG_WARN("editor", "port %d is busy: agents cannot attach (pick another with --port)", port);
    }
    options.onFrame = team->update;  // agent turns advance every frame, whatever panel is open
    if (!a.Has("--mute")) engine.EnableAudioOutput();
    window->Maximize();
    OE_LOG_INFO("editor", "native editor (%s)", engine.Gpu()->Name());
    return RunNativeEditor(engine, *window, options);
#else
    return Fail("no_editor", "this build has no editor (no GPU backend was found when it was configured)",
                "Build on Windows, or on Linux install libegl-dev libgles-dev for headless editor screenshots.");
#endif
}

int CmdMcp(const Args& a) {
    Log::SetMinEchoLevel(LogLevel::Warn);  // keep stderr quiet for MCP hosts
    PlatformSetBinaryStdio();
    if (a.Has("--connect")) {
        int port = a.GetInt("--connect", 7777);
        const std::string agent = a.Get("--agent");  // a team agent's id: the editor shows who made each edit
        CommandCaller call = [port, agent](const std::string& command, const Json& args) { return RemoteCall(port, command, args, agent); };
        return RunMcpServer(call, std::cin, std::cout);
    }

    Engine engine;
    std::shared_ptr<TeamHandle> team = RegisterTeamCommands(engine.Commands());
    int code = 0;
    if (!OpenOrFail(engine, a, code)) return code;
    HttpServer server;
    if (a.Has("--port")) {
        if (!SetupRenderer(engine, a, nullptr, code)) return code;
        if (StartServer(server, engine, a.GetInt("--port", 7777))) team->host.apiPort = a.GetInt("--port", 7777);
    }
    std::atomic<bool> quit{false};
    // MCP I/O on a worker thread; engine work stays on the main thread.
    std::thread io([&] {
        RunMcpServer([&engine](const std::string& c, const Json& args) { return engine.PostCall(c, args).get(); }, std::cin, std::cout);
        quit = true;
    });
    MainLoop(engine, nullptr, quit, false, team.get());
    io.join();
    return 0;
}

int CmdApi(const Args& a) {
    Engine engine;
    RegisterTeamCommands(engine.Commands());  // tool commands are part of the reference
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
    // Keep the result alive: a range-for over a member of a temporary would dangle.
    Json types = engine.Call("component.types", Json());
    for (const Json& t : types["result"].items()) {
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
    std::string rel;
    try {
        rel = ImportAssetFile(engine, a.positional[1], a.Get("--to"));
    } catch (const ApiError& e) {
        return Fail(e.code, e.what(), e.hint);
    }
    std::string kind = AssetManager::KindOf(rel);
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

// Folder with the web runtime (oe_player.js + oe_player.wasm): a local
// build_web.sh / build_web.bat result first, then the prebuilt runtime/web/.
// "" when neither exists.
std::string FindWebRuntime() {
    std::string candidates[] = {JoinPath(ExecutableDirectory(), "web"), JoinPath(OE_SOURCE_DIR, "build/bin/web"), JoinPath(OE_SOURCE_DIR, "build-web/bin"),
                                // Prebuilt runtime committed to the repository (no Emscripten needed).
                                JoinPath(OE_SOURCE_DIR, "runtime/web")};
    for (const std::string& c : candidates) {
        if (FileExists(JoinPath(c, "oe_player.js")) && FileExists(JoinPath(c, "oe_player.wasm"))) return c;
    }
    return "";
}

std::string HtmlEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

// ----- oe package --android ---------------------------------------------------

std::string Env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

std::string HomeDir() {
#ifdef _WIN32
    return Env("USERPROFILE");
#else
    return Env("HOME");
#endif
}

#ifdef _WIN32
const char* kExe = ".exe";
#else
const char* kExe = "";
#endif

std::string QuoteArg(const std::string& s) {
#ifdef _WIN32
    std::string q = "\"";
    for (char c : s) q += c == '"' ? std::string("\\\"") : std::string(1, c);
    return q + "\"";
#else
    std::string q = "'";
    for (char c : s) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return q + "'";
#endif
}

// Runs an external tool (java, keytool, adb) and captures its output.
// Returns true when it exited with code 0.
bool RunTool(const std::vector<std::string>& argv, const std::string& logPath, std::string* output) {
    std::string cmd;
    for (const std::string& a : argv) cmd += (cmd.empty() ? "" : " ") + QuoteArg(a);
    cmd += " > " + QuoteArg(logPath) + " 2>&1";
#ifdef _WIN32
    cmd = "\"" + cmd + "\"";  // cmd.exe /c strips the outer quotes
#endif
    std::fflush(stdout);
    int rc = std::system(cmd.c_str());
    std::string text;
    ReadTextFile(logPath, text);
    RemoveAll(logPath);
    if (output) {
        // Keep the message, drop JVM notices and stack frames.
        output->clear();
        size_t start = 0;
        while (start < text.size()) {
            size_t end = text.find('\n', start);
            std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
            start = end == std::string::npos ? text.size() : end + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line.rfind("Picked up ", 0) == 0 || line.rfind("\tat ", 0) == 0 || line.rfind("\t... ", 0) == 0) continue;
            *output += (output->empty() ? "" : "\n") + line;
        }
        if (output->size() > 2000) *output = output->substr(0, 2000) + "...";
    }
    return rc == 0;
}

// Version-ordered compare of folder names like "34.0.0" / "35.0.0-rc1".
bool VersionLess(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit(static_cast<unsigned char>(a[i])) && std::isdigit(static_cast<unsigned char>(b[j]))) {
            long x = std::strtol(a.c_str() + i, nullptr, 10), y = std::strtol(b.c_str() + j, nullptr, 10);
            if (x != y) return x < y;
            while (i < a.size() && std::isdigit(static_cast<unsigned char>(a[i]))) ++i;
            while (j < b.size() && std::isdigit(static_cast<unsigned char>(b[j]))) ++j;
        } else {
            if (a[i] != b[j]) return a[i] < b[j];
            ++i;
            ++j;
        }
    }
    return a.size() - i < b.size() - j;
}

// The Android SDK: --sdk, ANDROID_HOME, ANDROID_SDK_ROOT, then where Android
// Studio installs it. "" when none is found.
std::string FindAndroidSdk(const Args& a) {
    std::vector<std::string> candidates = {a.Get("--sdk"), Env("ANDROID_HOME"), Env("ANDROID_SDK_ROOT")};
#ifdef _WIN32
    candidates.push_back(JoinPath(Env("LOCALAPPDATA"), "Android/Sdk"));
#else
    candidates.push_back(JoinPath(HomeDir(), "Android/Sdk"));
    candidates.push_back(JoinPath(HomeDir(), "Library/Android/sdk"));
    candidates.push_back("/usr/lib/android-sdk");  // Debian/Ubuntu packages (apksigner)
#endif
    for (const std::string& c : candidates) {
        if (!c.empty() && IsDirectory(JoinPath(c, "build-tools"))) return c;
    }
    return "";
}

// apksigner.jar of the newest build-tools.
std::string FindApksigner(const std::string& sdk) {
    if (sdk.empty()) return "";
    std::vector<std::string> versions = ListDirectories(JoinPath(sdk, "build-tools"));
    std::sort(versions.begin(), versions.end(), VersionLess);
    for (auto it = versions.rbegin(); it != versions.rend(); ++it) {
        for (const char* rel : {"lib/apksigner.jar", "apksigner.jar"}) {
            std::string jar = JoinPath(JoinPath(JoinPath(sdk, "build-tools"), *it), rel);
            if (FileExists(jar)) return jar;
        }
    }
    return "";
}

// Folder with java + keytool: JAVA_HOME, Android Studio's bundled JDK, else
// "" meaning "on the PATH" (checked by running java).
bool FindJava(std::string& binDir, const std::string& logPath) {
    std::vector<std::string> homes = {Env("JAVA_HOME")};
#ifdef _WIN32
    for (const char* pf : {"ProgramFiles", "ProgramW6432", "LOCALAPPDATA"}) {
        std::string root = Env(pf);
        if (root.empty()) continue;
        homes.push_back(JoinPath(root, "Android/Android Studio/jbr"));
        homes.push_back(JoinPath(root, "Programs/Android Studio/jbr"));
    }
#elif defined(__APPLE__)
    homes.push_back("/Applications/Android Studio.app/Contents/jbr/Contents/Home");
#else
    homes.push_back("/opt/android-studio/jbr");
    homes.push_back(JoinPath(HomeDir(), "android-studio/jbr"));
#endif
    for (const std::string& h : homes) {
        if (!h.empty() && FileExists(JoinPath(h, std::string("bin/java") + kExe))) {
            binDir = JoinPath(h, "bin");
            return true;
        }
    }
    binDir.clear();
    return RunTool({"java", "-version"}, logPath, nullptr);
}

std::string JavaTool(const std::string& binDir, const char* name) {
    return binDir.empty() ? std::string(name) : JoinPath(binDir, std::string(name) + kExe);
}

// Folder with <abi>/liboe_player.so: a local build_android.sh/.bat result
// first, then the prebuilt runtime/android/.
std::string FindAndroidRuntime(std::vector<std::string>& abis) {
    std::string candidates[] = {JoinPath(ExecutableDirectory(), "android"), JoinPath(OE_SOURCE_DIR, "build/bin/android"),
                                JoinPath(OE_SOURCE_DIR, "runtime/android")};
    for (const std::string& c : candidates) {
        abis.clear();
        for (const char* abi : {"arm64-v8a", "armeabi-v7a", "x86_64", "x86"}) {
            if (FileExists(JoinPath(JoinPath(c, abi), "liboe_player.so"))) abis.push_back(abi);
        }
        if (!abis.empty()) return c;
    }
    return "";
}

// apksigner password arguments: "pass:...", "env:VAR", "file:path" or a plain password.
std::string PassArg(const std::string& v) {
    for (const char* p : {"pass:", "env:", "file:"}) {
        if (v.rfind(p, 0) == 0) return v;
    }
    return "pass:" + v;
}

//   --android: <out>/<Name>.apk (signed; debug key unless --keystore), installable with adb or by copying it to a phone
int CmdPackageAndroid(const Args& a, const std::string& projectDir, const std::string& name, const std::vector<std::string>& files,
                      const Json& fileList) {
    Json project;
    std::string text, err;
    if (ReadTextFile(JoinPath(projectDir, "project.json"), text)) project = Json::parse(text);
    const Json& android = project["android"];

    std::vector<std::string> abis;
    std::string runtime = FindAndroidRuntime(abis);
    if (runtime.empty()) {
        return Fail("android_runtime_missing", "the Android runtime (android/<abi>/liboe_player.so) has not been built",
                    "Build it once with build_android.bat (Windows) or ./build_android.sh, which need the Android NDK "
                    "(Android Studio > SDK Manager > SDK Tools > NDK).");
    }
    if (a.Has("--abi")) {
        std::vector<std::string> wanted;
        std::string list = a.Get("--abi") + ",";
        for (size_t p = 0, q; (q = list.find(',', p)) != std::string::npos; p = q + 1) {
            std::string abi = list.substr(p, q - p);
            if (abi.empty()) continue;
            if (std::find(abis.begin(), abis.end(), abi) == abis.end()) {
                return Fail("android_runtime_missing", "no " + abi + " build of the Android runtime in " + runtime,
                            "Build it with build_android.bat Release \"" + abi + "\" (or ./build_android.sh Release " + abi + ").");
            }
            wanted.push_back(abi);
        }
        abis = wanted;
    }

    AndroidAppInfo app;
    app.network = project["network"].isObject() && project["network"]["mode"].asString("none") != "none";
    app.label = project["window"]["title"].asString(project["name"].asString(name));
    app.packageName = a.Get("--package", android["package"].asString(DefaultAndroidPackageName(name)));
    if (!IsValidAndroidPackageName(app.packageName)) {
        return Fail("invalid_package", "'" + app.packageName + "' is not a valid Android application id",
                    "Use --package com.yourname.game (lowercase letters, digits and _, at least two parts separated by dots), "
                    "or set \"android\": {\"package\": ...} in project.json.");
    }
    app.versionCode = a.GetInt("--version-code", android["versionCode"].asInt(1));
    app.versionName = a.Get("--version-name", android["versionName"].asString("1.0"));
    const Json& win = project["window"];
    std::string defOrientation = win["height"].asInt(720) > win["width"].asInt(1280) ? "portrait" : "landscape";
    app.orientation = a.Get("--orientation", android["orientation"].asString(defOrientation));
    if (app.orientation != "landscape" && app.orientation != "portrait" && app.orientation != "auto") {
        return Fail("invalid_argument", "orientation must be landscape, portrait or auto", "Use --orientation landscape.");
    }
    app.debuggable = a.Has("--debuggable");
    if (app.versionCode < 1) return Fail("invalid_argument", "versionCode must be a positive integer", "Use --version-code 1.");

    ApkContents apk;
    apk.app = app;
    for (const std::string& abi : abis) apk.nativeLibs.push_back({abi, JoinPath(JoinPath(runtime, abi), "liboe_player.so")});
    std::string icon = android["icon"].asString();
    if (!icon.empty()) {
        std::vector<unsigned char> png;
        if (!ReadBinaryFile(JoinPath(projectDir, icon), png) || png.size() < 8 || png[1] != 'P' || png[2] != 'N' || png[3] != 'G') {
            return Fail("invalid_icon", "android.icon '" + icon + "' is not a PNG file in the project", "Use a square PNG, e.g. 192x192 or 512x512.");
        }
        apk.iconPng = png;
    }

    std::string out = AbsolutePath(a.Get("--out", "dist/" + name + "-android"));
    const std::string projectAbs = AbsolutePath(projectDir);
    if (out == projectAbs || out.rfind(projectAbs + "/", 0) == 0) {
        return Fail("invalid_output", "the output folder must be outside the project", "Use --out dist/" + name + "-android.");
    }
    if (IsDirectory(out) && !ListFiles(out, "", false).empty() && ListFiles(out, ".apk", false).empty() && ListFiles(out, ".aab", false).empty()) {
        return Fail("output_not_empty", out + " exists and is not an OwnEngine Android package", "Pick another --out folder or empty it.");
    }
    CreateDirectories(out);
    const std::string pak = JoinPath(out, ".game.pak.tmp");
    double bytes = 0;
    if (!WriteGamePak(projectDir, files, pak, &err, &bytes)) return Fail("write_failed", err);
    ReadBinaryFile(pak, apk.gamePak);
    RemoveAll(pak);

    // --aab: Android App Bundle for Google Play instead of an installable APK.
    const bool aab = a.Has("--aab");
    const std::string ext = aab ? ".aab" : ".apk";
    if (aab && a.Has("--install")) {
        return Fail("invalid_argument", "--install needs an APK; an .aab is for uploading to Google Play",
                    "Drop --aab to test on a device, or turn the bundle into APKs with bundletool (build-apks / install-apks).");
    }
    const std::string apkPath = JoinPath(out, name + ext);
    const std::string unsignedPath = JoinPath(out, name + "-unsigned" + ext);
    const std::string log = JoinPath(out, ".oe-tool.log");
    if (!(aab ? WriteUnsignedAppBundle(apk, unsignedPath, &err) : WriteUnsignedApk(apk, unsignedPath, &err))) return Fail("write_failed", err);

    Json res = Json::MakeObject();
    res["ok"] = true;
    res["result"]["package"] = app.packageName;
    res["result"]["versionCode"] = app.versionCode;
    res["result"]["versionName"] = app.versionName;
    res["result"]["orientation"] = app.orientation;
    Json abiList = Json::MakeArray();
    for (const std::string& abi : abis) abiList.push(abi);
    res["result"]["abis"] = abiList;
    res["result"]["androidRuntime"] = runtime;
    res["result"]["files"] = fileList;
    res["result"]["dataBytes"] = bytes;

    if (a.Has("--unsigned")) {
        RemoveAll(apkPath);
        res["result"]["apk"] = unsignedPath;
        res["result"]["signedWith"] = "none";
        res["result"]["next"] = aab ? "Sign it with jarsigner (JDK) before uploading it." : "Sign it with apksigner (Android SDK build-tools) before installing it.";
        PrintJson(res);
        return 0;
    }

    // Signing: apksigner (SDK build-tools, runs on Java) for APKs, jarsigner (JDK) for bundles.
    const std::string sdk = FindAndroidSdk(a);
    const std::string apksigner = aab ? std::string() : FindApksigner(sdk);
    if (!aab && apksigner.empty()) {
        return Fail("android_sdk_missing", "apksigner (Android SDK build-tools) was not found" + (sdk.empty() ? std::string() : " in " + sdk),
                    "Install Android Studio (or the SDK command-line tools + build-tools), set ANDROID_HOME, or pass --sdk <folder>. "
                    "The unsigned APK is at " + unsignedPath + " (--unsigned skips signing).");
    }
    std::string javaBin;
    if (!FindJava(javaBin, log)) {
        return Fail("java_missing", std::string("Java was not found (") + (aab ? "jarsigner" : "apksigner") + " needs it)",
                    "Install Android Studio (it bundles a JDK) or a JDK 17+, or set JAVA_HOME.");
    }
    std::string keystore = a.Get("--keystore"), ksPass = a.Get("--ks-pass"), alias = a.Get("--key-alias"), keyPass = a.Get("--key-pass");
    std::string signedWith = "release";
    if (keystore.empty()) {
        // The debug key Android Studio uses (~/.android/debug.keystore): fine for
        // testing and sideloading, not for Google Play.
        signedWith = "debug";
        keystore = JoinPath(HomeDir(), ".android/debug.keystore");
        ksPass = keyPass = "android";
        alias = "androiddebugkey";
        if (!FileExists(keystore)) {
            CreateDirectories(ParentPath(keystore));
            std::string output;
            if (!RunTool({JavaTool(javaBin, "keytool"), "-genkeypair", "-keystore", keystore, "-storepass", "android", "-alias", alias,
                          "-keypass", "android", "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000", "-dname",
                          "CN=Android Debug,O=Android,C=US"},
                         log, &output)) {
                return Fail("sign_failed", "cannot create the debug keystore " + keystore + ": " + output, "Check that keytool (JDK) works.");
            }
        }
    } else {
        if (ksPass.empty()) return Fail("missing_argument", "--keystore needs --ks-pass", "Pass --ks-pass env:MY_PASSWORD (or pass:..., file:...).");
        if (alias.empty()) return Fail("missing_argument", "--keystore needs --key-alias", "Pass the alias of the key in the keystore.");
        keystore = AbsolutePath(keystore);
    }
    std::vector<std::string> sign;
    if (aab) {
        // jarsigner takes -storepass x / -storepass:env VAR / -storepass:file path.
        auto jarPass = [&](const char* flag, const std::string& v) {
            std::string p = PassArg(v);
            if (p.rfind("env:", 0) == 0) sign.insert(sign.end(), {std::string(flag) + ":env", p.substr(4)});
            else if (p.rfind("file:", 0) == 0) sign.insert(sign.end(), {std::string(flag) + ":file", p.substr(5)});
            else sign.insert(sign.end(), {flag, p.substr(5)});
        };
        sign = {JavaTool(javaBin, "jarsigner"), "-keystore", keystore};
        jarPass("-storepass", ksPass);
        if (!keyPass.empty()) jarPass("-keypass", keyPass);
        sign.insert(sign.end(), {"-sigalg", "SHA256withRSA", "-digestalg", "SHA-256", "-signedjar", apkPath, unsignedPath, alias});
    } else {
        sign = {JavaTool(javaBin, "java"), "-jar", apksigner, "sign", "--ks", keystore, "--ks-pass", PassArg(ksPass), "--ks-key-alias", alias};
        if (!keyPass.empty()) {
            sign.push_back("--key-pass");
            sign.push_back(PassArg(keyPass));
        }
        sign.insert(sign.end(), {"--out", apkPath, unsignedPath});
    }
    std::string output;
    if (!RunTool(sign, log, &output)) {
        return Fail("sign_failed", std::string(aab ? "jarsigner" : "apksigner") + " failed: " + output, "Check the keystore, its passwords and the key alias.");
    }
    RemoveAll(unsignedPath);
    RemoveAll(apkPath + ".idsig");  // v4 signature (only for incremental adb installs)
    res["result"]["apk"] = apkPath;
    res["result"]["signedWith"] = signedWith;
    res["result"]["keystore"] = keystore;
    res["result"]["next"] = "Install it with `adb install -r " + apkPath + "` (or rerun with --install), or copy it to the phone and open it. " +
                            (signedWith == "debug" ? "For Google Play, sign with your own key: --keystore my.jks --ks-pass env:PASS --key-alias key." : "");
    if (aab) {
        res["result"]["aab"] = apkPath;
        res["result"].erase("apk");
        res["result"]["next"] = signedWith == "debug"
                                    ? "Signed with the debug key, which Google Play rejects: sign with your upload key (--keystore my.jks --ks-pass env:PASS --key-alias key). Test it with bundletool build-apks."
                                    : "Upload " + apkPath + " in the Google Play Console (Play App Signing signs the APKs it delivers).";
    }

    if (a.Has("--install")) {
        std::string adb = "adb";
        if (!sdk.empty() && FileExists(JoinPath(sdk, std::string("platform-tools/adb") + kExe))) adb = JoinPath(sdk, std::string("platform-tools/adb") + kExe);
        if (!RunTool({adb, "install", "-r", apkPath}, log, &output)) {
            return Fail("install_failed", "adb install failed: " + output,
                        "Connect a phone with USB debugging enabled (or start an emulator) and check `adb devices`.");
        }
        RunTool({adb, "shell", "am", "start", "-n", app.packageName + "/android.app.NativeActivity"}, log, &output);
        res["result"]["installed"] = true;
        res["result"]["next"] = "Running on the device. Engine logs: `adb logcat -s OwnEngine`.";
    }
    PrintJson(res);
    return 0;
}

// Builds a standalone game folder:
//   desktop: <out>/<Name>.exe (the player runtime) + <out>/game/ (project files)
//   --web:   <out>/index.html + oe_player.js + oe_player.wasm + game.pak, for any static web host
//   --android: see CmdPackageAndroid
int CmdPackage(const Args& a) {
    Engine engine;
    int code = 0;
    if (!OpenOrFail(engine, a, code)) return code;
    const std::string projectDir = engine.ProjectDir();
    if (!FileExists(JoinPath(projectDir, "project.json"))) {
        return Fail("not_a_project", "no project.json in " + projectDir, "Package a project directory (created with `oe new`).");
    }
    const bool web = a.Has("--web");
    std::string name = a.Get("--name", engine.ProjectName().empty() ? "Game" : engine.ProjectName());
    for (char& c : name) {
        if (std::string("<>:\"/\\|?*").find(c) != std::string::npos || static_cast<unsigned char>(c) < 32) c = '_';
    }
    std::string out = AbsolutePath(a.Get("--out", "dist/" + name + (web ? "-web" : "")));
    const std::string projectAbs = AbsolutePath(projectDir);
    if (out == projectAbs || out.rfind(projectAbs + "/", 0) == 0) {
        return Fail("invalid_output", "the output folder must be outside the project", "Use --out dist/" + name + (web ? "-web" : "") + ".");
    }
    std::vector<std::string> files = GameFiles(projectDir);
    Json fileList = Json::MakeArray();
    for (const std::string& f : files) fileList.push(f);
    if (a.Has("--android")) return CmdPackageAndroid(a, projectDir, name, files, fileList);

    if (web) {
        std::string runtime = FindWebRuntime();
        if (runtime.empty()) {
            return Fail("web_runtime_missing", "the web runtime (web/oe_player.js + oe_player.wasm) has not been built",
                        "Build it once with build_web.bat (Windows) or ./build_web.sh, which need the Emscripten SDK (emsdk).");
        }
        std::string shell;
        if (!ReadTextFile(JoinPath(runtime, "index.html"), shell) && !ReadTextFile(JoinPath(OE_SOURCE_DIR, "tools/player/web/index.html"), shell)) {
            return Fail("web_runtime_missing", "web/index.html (the player page) is missing", "Rebuild with build.bat / build_web.bat.");
        }
        // Only reuse a folder that is empty or an earlier web package (has game.pak).
        if (IsDirectory(out) && !ListFiles(out, "", false).empty() && !FileExists(JoinPath(out, "game.pak"))) {
            return Fail("output_not_empty", out + " exists and is not an OwnEngine web package", "Pick another --out folder or empty it.");
        }
        Json project;
        std::string text;
        if (ReadTextFile(JoinPath(projectDir, "project.json"), text)) project = Json::parse(text);
        std::string title = project["window"]["title"].asString(name);
        std::string page;
        for (size_t pos = 0;;) {
            size_t at = shell.find("{{TITLE}}", pos);
            page += shell.substr(pos, at == std::string::npos ? std::string::npos : at - pos);
            if (at == std::string::npos) break;
            page += HtmlEscape(title);
            pos = at + 9;
        }
        double bytes = 0;
        std::string err;
        if (!WriteGamePak(projectDir, files, JoinPath(out, "game.pak"), &err, &bytes)) return Fail("write_failed", err);
        if (!WriteTextFile(JoinPath(out, "index.html"), page)) return Fail("write_failed", "cannot write " + JoinPath(out, "index.html"));
        for (const char* f : {"oe_player.js", "oe_player.wasm"}) {
            if (!CopyFileTo(JoinPath(runtime, f), JoinPath(out, f))) return Fail("write_failed", std::string("cannot write ") + f);
        }
        Json res = Json::MakeObject();
        res["ok"] = true;
        res["result"]["dir"] = out;
        res["result"]["index"] = JoinPath(out, "index.html");
        res["result"]["webRuntime"] = runtime;
        res["result"]["files"] = fileList;
        res["result"]["dataBytes"] = bytes;
        res["result"]["next"] = "Test locally with `oe serve " + out + "`, then upload the folder to any static web host "
                                "(itch.io: zip it and upload as an HTML game; GitHub Pages, Netlify, ...).";
        PrintJson(res);
        return 0;
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
    double bytes = 0;
    for (const std::string& rel : files) {
        std::string src = JoinPath(projectDir, rel);
        if (!CopyFileTo(src, JoinPath(gameDir, rel))) return Fail("write_failed", "cannot copy " + rel);
        std::vector<unsigned char> data;
        if (ReadBinaryFile(src, data)) bytes += static_cast<double>(data.size());
    }
    std::string exe = JoinPath(out, name + exeExt);
    if (!CopyFileTo(player, exe)) return Fail("write_failed", "cannot write " + exe, "Close the running game first.");

    Json res = Json::MakeObject();
    res["ok"] = true;
    res["result"]["exe"] = exe;
    res["result"]["gameDir"] = gameDir;
    res["result"]["files"] = fileList;
    res["result"]["dataBytes"] = bytes;
    res["result"]["next"] = "Run " + exe + " or zip the folder " + out + " to share it.";
    PrintJson(res);
    return 0;
}

// Runs an opt-in fixed-step game server without creating display/audio devices.
int CmdServeGame(const Args& a) {
    Engine engine; int code = 0;
    if (!OpenOrFail(engine, a, code)) return code;
    Json args = Json::MakeObject();
    for (const auto& flag : std::vector<std::pair<const char*, const char*>>{
             {"--port", "port"}, {"--min-players", "minPlayers"}, {"--seed", "seed"}}) {
        if (!a.Has(flag.first)) continue;
        std::string error; Json value = Json::parse(a.Get(flag.first), &error);
        if (!error.empty() || !value.isNumber()) return Fail("invalid_argument", std::string(flag.first) + " requires an integer");
        args[flag.second] = value;
    }
    uint64_t ticks = 0;
    if (a.Has("--frames")) {
        std::string error; Json value = Json::parse(a.Get("--frames"), &error); double n = value.asNumber(-1);
        if (!error.empty() || !value.isNumber() || !std::isfinite(n) || n < 1 || n > 2147483647 || std::floor(n) != n)
            return Fail("invalid_argument", "--frames requires 1..2147483647 I/O ticks");
        ticks = static_cast<uint64_t>(n);
    }
    HttpServer api;
    if (a.Has("--api-port") && !StartServer(api, engine, a.GetInt("--api-port", 7777))) return Fail("server_failed", "cannot start API server");
    Json result = RunDedicatedServer(engine, args, ticks, [](const Json& state) { PrintJson(state); });
    PrintJson(result); return result["ok"].asBool() ? 0 : 1;
}

// Serves a static web package; the game socket server is a separate process.
int CmdServe(const Args& a) {
    std::string dir = AbsolutePath(a.positional.empty() ? "." : a.positional[0]);
    if (!FileExists(JoinPath(dir, "index.html"))) {
        return Fail("not_found", "no index.html in " + dir, "Pass the folder made by `oe package --web`.");
    }
    int port = a.GetInt("--port", 8080);
    HttpServer server;
    std::string err;
    auto handler = [dir](const HttpRequest& req) {
        HttpResponse r;
        std::string path = req.path == "/" ? "/index.html" : req.path;
        bool safe = req.method == "GET" && path.find("..") == std::string::npos && path.find('\\') == std::string::npos;
        std::vector<unsigned char> data;
        if (!safe || !ReadBinaryFile(dir + path, data)) {
            r.status = 404;
            r.body = "{\"ok\":false,\"error\":{\"code\":\"not_found\"}}";
            return r;
        }
        auto ends = [&](const char* ext) { return path.size() >= std::strlen(ext) && path.compare(path.size() - std::strlen(ext), std::string::npos, ext) == 0; };
        r.contentType = ends(".html") ? "text/html; charset=utf-8" : ends(".js") ? "text/javascript" : ends(".wasm") ? "application/wasm"
                      : ends(".json") ? "application/json" : ends(".png") ? "image/png" : ends(".css") ? "text/css" : "application/octet-stream";
        r.body.assign(data.begin(), data.end());
        return r;
    };
    if (!server.Start(port, handler, &err)) return Fail("server_failed", err, "Pick another port with --port.");
    std::string url = "http://127.0.0.1:" + std::to_string(port) + "/";
    OE_LOG_INFO("serve", "serving %s at %s (Ctrl+C to quit)", dir.c_str(), url.c_str());
    if (!a.Has("--no-browser")) PlatformOpenUrl(url);
    for (;;) PlatformSleep(1.0);
}

int CmdVersion() {
    Json v = Json::MakeObject();
    v["engine"] = "OwnEngine";
    v["version"] = OE_VERSION;
    v["platform"] = PlatformName();
    v["renderer"] = "software";
    // Probe the GPU backend (headless device) so tools can tell what `--renderer gpu` would use.
    Engine engine;
    std::string err;
    if (engine.EnableGpu(nullptr, &err)) v["gpu"] = engine.Gpu()->Name();
    else v["gpu"] = "unavailable: " + err;
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
    if (cmd == "serve-game") return CmdServeGame(a);
    if (cmd == "serve") return CmdServe(a);
    return Fail("unknown_command", "unknown command '" + cmd + "'", "Run `oe help`.");
}
