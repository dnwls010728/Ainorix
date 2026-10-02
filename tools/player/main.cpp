// OwnEngine player: the runtime that `oe package` ships as <Game>.exe,
// (built with Emscripten) as the WebAssembly module of `oe package --web`,
// and (built with the Android NDK) as liboe_player.so of `oe package --android`.
//
// Plays the project in `game/` next to the executable (or the path given as
// the first argument) in a native window / the page's canvas / the Android
// activity. It has no editor, no API server and no console window; errors
// are shown in a message box (desktop), on the page (web) or in logcat
// (Android).
//
// Optional project.json settings:
//   "window": {
//     "width": 1280, "height": 720, "title": "My Game",
//     "renderer": "auto",      // auto (GPU, software if unavailable) | gpu | software
//     "renderScale": 1.0,      // GPU: 3D resolution relative to the window (0.25..1); UI stays sharp
//     "maxRenderWidth": 1280   // software renderer: widest image drawn, the window stretches it
//   }

#include <algorithm>
#include <exception>
#include <cstdio>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#ifdef __ANDROID__
#include <android_native_app_glue.h>

#include "app/Project.h"
#include "platform/android/AndroidApp.h"
#endif

#include "app/Engine.h"
#include "app/DedicatedServer.h"
#include "core/FileSystem.h"
#include "core/Json.h"
#include "core/Log.h"
#include "platform/Platform.h"
#include "render/GpuRenderer.h"
#include "render/Renderer.h"

using namespace oe;

namespace {

struct Player {
    Engine engine;
    std::unique_ptr<Window> window;
    bool gpu = false;
    float renderScale = 1.0f;
    int maxWidth = 1280;
    RenderTarget frame;
    double last = 0.0;

    // One iteration of the game loop. Returns false once the window closed.
    bool Frame() {
        if (!window->PumpEvents(engine.Input())) return false;
        double now = PlatformTimeSeconds();
        engine.Tick(now - last);
        last = now;
        if (gpu) {
#ifndef __EMSCRIPTEN__
            // Minimized: nothing is presented, so nothing waits for vsync either.
            if (!engine.PresentGameView(renderScale)) PlatformSleep(0.01);
#else
            engine.PresentGameView(renderScale);
#endif
            return true;
        }
        // The software renderer draws at most maxRenderWidth pixels wide; the
        // window stretches the image, so large or maximized windows stay fast.
        int w = window->Width(), h = window->Height();
        if (w > 0 && h > 0) {
            if (w > maxWidth) {
                h = std::max(1, h * maxWidth / w);
                w = maxWidth;
            }
            if (frame.width != w || frame.height != h) frame.Resize(w, h);
            engine.RenderGameView(frame);
            window->Present(frame);
        }
        return true;
    }
};

#ifdef __EMSCRIPTEN__
void WebFrame(void* arg) {
    auto* player = static_cast<Player*>(arg);
    if (!player->Frame()) emscripten_cancel_main_loop();
}
#endif

// Plays the game in `dir` until the window closes (web: starts the browser
// loop and returns). Returns the process exit code.
int RunPlayer(const std::string& dir) {
    Json project;
    std::string text, err;
    if (ReadTextFile(JoinPath(dir, "project.json"), text)) project = Json::parse(text, &err);
    const Json& p = project;
    const Json& win = p["window"];
    std::string title = win["title"].asString(p["name"].asString("OwnEngine Game"));

    // Heap allocated: on the web main() returns while the loop keeps running.
    auto* player = new Player();
    Engine& engine = player->engine;
    if (!engine.Open(dir, &err)) {
        PlatformShowError(title, "Cannot start the game.\n\n" + err + "\n\nExpected the game data in:\n" + dir);
        return 1;
    }
    try { engine.Saves().ConfigurePlayer(engine.ProjectName()); }
    catch (const std::exception& e) {
        PlatformShowError(title, std::string("Cannot open save storage.\n\n") + e.what());
        delete player;
        return 1;
    }
    player->window = CreatePlatformWindow(title, win["width"].asInt(1280), win["height"].asInt(720));
    if (!player->window) {
        PlatformShowError(title, std::string("This platform (") + PlatformName() + ") has no native window.");
        return 1;
    }

    std::string renderer = win["renderer"].asString("auto");
    if (renderer != "software") {
        player->gpu = engine.EnableGpu(player->window.get(), &err);
        if (!player->gpu && renderer == "gpu") {
            PlatformShowError(title, "This game needs GPU rendering, which is not available here.\n\n" + err);
            return 1;
        }
        if (!player->gpu) OE_LOG_WARN("render", "GPU renderer unavailable, using the software renderer: %s", err.c_str());
    }
    player->renderScale = std::clamp(win["renderScale"].asFloat(1.0f), 0.25f, 1.0f);
    player->maxWidth = std::max(160, win["maxRenderWidth"].asInt(1280));

    engine.EnableAudioOutput();
    engine.Play();
    player->last = PlatformTimeSeconds();

#ifdef __EMSCRIPTEN__
    // The browser drives the loop (requestAnimationFrame, vsync).
    emscripten_set_main_loop_arg(WebFrame, player, 0, false);
    return 0;
#else
    while (player->Frame()) {
        // With the GPU, Present() waits for vsync; the software path yields briefly.
        if (!player->gpu) PlatformSleep(0.001);
    }
    delete player;
    return 0;
#endif
}

#ifdef __ANDROID__
// The APK carries the game as assets/game.pak plus assets/game.id (a stamp
// that changes with every package). The pak is unpacked into the app's data
// folder on the first start after an install or update.
bool PrepareAndroidGame(std::string& dir, std::string& err) {
    dir = JoinPath(AndroidDataDir(), "game");
    std::vector<unsigned char> id, pak;
    if (!AndroidReadAsset("game.id", id)) {
        err = "the APK has no assets/game.id (package the game with `oe package --android`)";
        return false;
    }
    const std::string stamp(id.begin(), id.end()), stampFile = JoinPath(dir, ".oe-pak-id");
    std::string have;
    if (ReadTextFile(stampFile, have) && have == stamp && FileExists(JoinPath(dir, "project.json"))) return true;
    if (!AndroidReadAsset("game.pak", pak)) {
        err = "the APK has no assets/game.pak (package the game with `oe package --android`)";
        return false;
    }
    RemoveAll(dir);
    if (!ExtractGamePak(pak, dir, &err)) return false;
    WriteTextFile(stampFile, stamp);
    OE_LOG_INFO("player", "unpacked game data (%zu bytes) into %s", pak.size(), dir.c_str());
    return true;
}
#endif

}  // namespace

#ifdef __ANDROID__
// NativeActivity entry point (android_native_app_glue), on its own thread.
void android_main(android_app* app) {
    AndroidSetApp(app);
    std::string dir, err;
    if (PrepareAndroidGame(dir, err)) RunPlayer(dir);
    else PlatformShowError("OwnEngine", "Cannot start the game.\n\n" + err);
    // Close the activity (and wait for it) instead of leaving a frozen window.
    AndroidFinish();
}
#else
int main(int argc, char** argv) {
#ifdef __EMSCRIPTEN__
    std::string dir = argc > 1 ? argv[1] : "/game";  // the page loader unpacks game.pak there
#else
    std::string dir = JoinPath(ExecutableDirectory(), "game");
    bool server = false; Json arguments = Json::MakeObject(); uint64_t ticks = 0; std::string argumentError;
    for (int i = 1; i < argc; ++i) {
        std::string flag = argv[i];
        if (flag == "--server") server = true;
        else if (flag == "--port" || flag == "--min-players" || flag == "--seed" || flag == "--frames") {
            if (++i >= argc) { argumentError = flag + " needs a value"; break; }
            std::string error; Json value = Json::parse(argv[i], &error); double n = value.asNumber(-1);
            if (!error.empty() || !value.isNumber() || !std::isfinite(n) || n < 0 || n > UINT32_MAX || std::floor(n) != n) {
                argumentError = flag + " needs a nonnegative integer"; break;
            }
            if (flag == "--frames") { if (n < 1 || n > 2147483647) argumentError = "--frames requires 1..2147483647 ticks"; ticks = static_cast<uint64_t>(n); }
            else arguments[flag == "--port" ? "port" : flag == "--seed" ? "seed" : "minPlayers"] = value;
        } else if (flag.compare(0, 2, "--") == 0) argumentError = "unknown argument: " + flag;
        else dir = flag;
    }
    if (server || !argumentError.empty()) {
        PlatformAttachParentConsole();
        Engine engine; std::string error; Json result;
        if (!argumentError.empty() || !engine.Open(dir, &error)) {
            result["ok"] = false; result["error"]["code"] = "player_server";
            result["error"]["message"] = argumentError.empty() ? error : argumentError;
        } else result = RunDedicatedServer(engine, arguments, ticks);
        std::string text = result.dump(); std::fputs(text.c_str(), stdout); std::fputc('\n', stdout); std::fflush(stdout);
        return result["ok"].asBool() ? 0 : 1;
    }
#endif
    return RunPlayer(dir);
}
#endif
