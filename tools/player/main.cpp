// OwnEngine player: the runtime that `oe package` ships as <Game>.exe.
//
// Plays the project in `game/` next to the executable (or the path given as
// the first argument) in a native window. It has no editor, no API server and
// no console window; errors are shown in a message box.
//
// Optional project.json settings:
//   "window": { "width": 1280, "height": 720, "title": "My Game", "maxRenderWidth": 1280 }

#include <algorithm>
#include <memory>
#include <string>

#include "app/Engine.h"
#include "core/FileSystem.h"
#include "core/Json.h"
#include "core/Log.h"
#include "platform/Platform.h"
#include "render/Renderer.h"

using namespace oe;

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : JoinPath(ExecutableDirectory(), "game");

    Json project;
    std::string text, err;
    if (ReadTextFile(JoinPath(dir, "project.json"), text)) project = Json::parse(text, &err);
    const Json& p = project;
    const Json& win = p["window"];
    std::string title = win["title"].asString(p["name"].asString("OwnEngine Game"));

    Engine engine;
    if (!engine.Open(dir, &err)) {
        PlatformShowError(title, "Cannot start the game.\n\n" + err + "\n\nExpected the game data in:\n" + dir);
        return 1;
    }
    std::unique_ptr<Window> window = CreatePlatformWindow(title, win["width"].asInt(1280), win["height"].asInt(720));
    if (!window) {
        PlatformShowError(title, std::string("This platform (") + PlatformName() + ") has no native window.");
        return 1;
    }
    engine.EnableAudioOutput();
    engine.Play();

    // The software renderer draws at most maxRenderWidth pixels wide; the
    // window stretches the image, so large or maximized windows stay fast.
    const int maxWidth = std::max(160, win["maxRenderWidth"].asInt(1280));
    RenderTarget frame;
    double last = PlatformTimeSeconds();
    while (window->PumpEvents(engine.Input())) {
        double now = PlatformTimeSeconds();
        engine.Tick(now - last);
        last = now;
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
        PlatformSleep(0.001);
    }
    return 0;
}
