// Headless platform: no window, used for servers, CI and platforms whose
// backend is not written yet. All engine features except the native game
// window work (API, MCP, editor server, software rendering to PNG).
#include <chrono>
#include <cstdio>
#include <thread>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

#include "platform/Platform.h"

namespace oe {

std::unique_ptr<Window> CreatePlatformWindow(const std::string&, int, int) { return nullptr; }

std::unique_ptr<AudioDevice> CreateAudioDevice(int) { return nullptr; }

void PlatformEnableHighDpi() {}

const char* PlatformName() { return "null"; }

double PlatformTimeSeconds() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

void PlatformSleep(double seconds) {
    if (seconds > 0) std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
}

bool PlatformOpenUrl(const std::string&) { return false; }

std::string ExecutableDirectory() {
#if defined(__linux__)
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        std::string p(buf, static_cast<size_t>(n));
        return p.substr(0, p.rfind('/'));
    }
#endif
    return ".";
}

void PlatformShowError(const std::string& title, const std::string& message) {
    std::fprintf(stderr, "%s: %s\n", title.c_str(), message.c_str());
}

void PlatformSetBinaryStdio() {}

}  // namespace oe
