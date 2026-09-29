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

// Audio output: receives interleaved stereo float frames at the mixer rate.
class AudioDevice {
public:
    virtual ~AudioDevice() = default;
    virtual const char* Name() const = 0;
    virtual void Submit(const float* interleavedStereo, int frames) = 0;
};

// Returns nullptr when no audio output is available.
std::unique_ptr<AudioDevice> CreateAudioDevice(int sampleRate);

// Returns nullptr when the platform has no windowing (headless builds).
std::unique_ptr<Window> CreatePlatformWindow(const std::string& title, int width, int height);

const char* PlatformName();
double PlatformTimeSeconds();
void PlatformSleep(double seconds);
bool PlatformOpenUrl(const std::string& url);
std::string ExecutableDirectory();
// Tells the user about a fatal error (message box on desktop, stderr otherwise).
void PlatformShowError(const std::string& title, const std::string& message);
// Puts stdin/stdout in binary mode (needed for MCP's newline-delimited JSON).
void PlatformSetBinaryStdio();

}  // namespace oe
