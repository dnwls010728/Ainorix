#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "render/Renderer.h"
#include "scene/Systems.h"

namespace oe {

class GpuDevice;

// Keys reported by WindowEvent (layout independent: letters and digits are
// the characters on the key, like the game's InputState names).
enum class WindowKey : int {
    None = 0,
    Tab, Left, Right, Up, Down, PageUp, PageDown, Home, End, Insert, Delete, Backspace, Space, Enter, Escape,
    Shift, Control, Alt, Super,
    A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Minus, Equal, LeftBracket, RightBracket, Backslash, Semicolon, Apostrophe, Comma, Period, Slash, GraveAccent,
    KeypadEnter,
    Count
};

// Raw window input for tools that need the whole keyboard, text entry and
// every mouse button (the native editor). Games read InputState instead.
struct WindowEvent {
    enum class Type { MouseMove, MouseButton, MouseWheel, Key, Text, Focus, Close, DropFile };
    Type type = Type::MouseMove;
    float x = 0, y = 0;      // MouseMove: client pixels; MouseWheel: notches (y > 0 = away from the user)
    int button = 0;          // MouseButton: 0 left, 1 right, 2 middle
    bool down = false;       // MouseButton / Key: pressed; Focus: gained
    WindowKey key = WindowKey::None;
    uint32_t codepoint = 0;  // Text: one Unicode character (IME results included)
    bool ctrl = false, shift = false, alt = false, super = false;  // modifier state (Key, MouseButton)
    std::string path;        // DropFile: UTF-8 path of a file dropped on the window
};

enum class WindowCursor { Arrow, TextInput, ResizeAll, ResizeNS, ResizeEW, ResizeNESW, ResizeNWSE, Hand, NotAllowed, Hidden };

// Platform abstraction. Each target (Win32 today; Web, Android, iOS, macOS,
// Linux and consoles later) provides one implementation of this header.
// Everything above this layer is platform independent C++17.
class Window {
public:
    virtual ~Window() = default;
    // Processes OS events, updates `input`. Returns false once the user closed the window.
    virtual bool PumpEvents(InputState& input) = 0;
    // Shows a CPU frame (software renderer output). With a GPU device bound to
    // the window (CreateGpuDevice), GpuRenderer::RenderToWindow presents instead.
    virtual void Present(const RenderTarget& frame) = 0;
    virtual int Width() const = 0;
    virtual int Height() const = 0;
    virtual void SetTitle(const std::string& title) = 0;
    // OS handle for graphics APIs (HWND on Win32); nullptr where unused.
    virtual void* NativeHandle() const { return nullptr; }

    // ----- Tool windows (native editor) --------------------------------------
    // Event mode: every input event is also queued as a WindowEvent (read with
    // TakeEvents), Escape no longer closes the window, and the close button
    // queues a Close event instead of ending PumpEvents (the tool decides).
    virtual void SetEventMode(bool on) {}
    virtual std::vector<WindowEvent> TakeEvents() { return {}; }
    virtual void SetCursor(WindowCursor cursor) {}
    // Pixels per 96-DPI unit of the monitor the window is on (1 = 100 %).
    virtual float DpiScale() const { return 1.0f; }
    virtual void Maximize() {}
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

// Creates the graphics device for GpuRenderer (render/GpuDevice.h): bound to
// `window` so frames can be presented in it, or headless (window == nullptr)
// for offscreen rendering (editor viewport, GPU screenshots). Returns nullptr
// and sets `error` when no GPU backend is available; callers then fall back
// to the software renderer. Only one device may exist at a time.
std::unique_ptr<GpuDevice> CreateGpuDevice(Window* window, std::string* error);

// The user's interface language as an ISO 639-1 code ("en", "ko", "ja", ...),
// "en" when unknown.
std::string PlatformUserLanguage();
// Opts the process into per-monitor DPI awareness (sharp tool UI on high-DPI
// screens). Call before creating windows; games leave it off.
void PlatformEnableHighDpi();

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
