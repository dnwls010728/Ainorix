#pragma once
#include <memory>
#include <string>

#include "render/Renderer.h"
#include "scene/Systems.h"

namespace oe {

// Platform abstraction. Each target (Win32 today; Web, Android, iOS, macOS,
// Linux and consoles later) provides one implementation of this header.
// Everything above this layer is platform independent C++17.
class Window {
public:
    virtual ~Window() = default;
    // Processes OS events, updates `input`. Returns false once the user closed the window.
    virtual bool PumpEvents(InputState& input) = 0;
    // Shows a CPU frame (software renderer output). GPU backends present directly.
    virtual void Present(const RenderTarget& frame) = 0;
    virtual int Width() const = 0;
    virtual int Height() const = 0;
    virtual void SetTitle(const std::string& title) = 0;
};

// Returns nullptr when the platform has no windowing (headless builds).
std::unique_ptr<Window> CreatePlatformWindow(const std::string& title, int width, int height);

const char* PlatformName();
double PlatformTimeSeconds();
void PlatformSleep(double seconds);
bool PlatformOpenUrl(const std::string& url);
std::string ExecutableDirectory();
// Puts stdin/stdout in binary mode (needed for MCP's newline-delimited JSON).
void PlatformSetBinaryStdio();

}  // namespace oe
