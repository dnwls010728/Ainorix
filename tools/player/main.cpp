// OwnEngine player: the runtime that `oe package` ships as <Game>.exe, and
// (built with Emscripten) as the WebAssembly module of `oe package --web`.
//
// Plays the project in `game/` next to the executable (or the path given as
// the first argument) in a native window / the page's canvas. It has no
// editor, no API server and no console window; errors are shown in a
// message box (desktop) or on the page (web).
//
// Optional project.json settings:
//   "window": {
//     "width": 1280, "height": 720, "title": "My Game",
//     "renderer": "auto",      // auto (GPU, software if unavailable) | gpu | software
//     "renderScale": 1.0,      // GPU: 3D resolution relative to the window (0.25..1); UI stays sharp
//     "maxRenderWidth": 1280   // software renderer: widest image drawn, the window stretches it
//   }

#include <algorithm>
#include <memory>
#include <string>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "app/Engine.h"
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

}  // namespace

int main(int argc, char** argv) {
#ifdef __EMSCRIPTEN__
    std::string dir = argc > 1 ? argv[1] : "/game";  // the page loader unpacks game.pak there
#else
    std::string dir = argc > 1 ? argv[1] : JoinPath(ExecutableDirectory(), "game");
#endif

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
