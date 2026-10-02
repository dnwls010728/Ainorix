// Headless platform: no window, used for servers, CI and platforms whose
// backend is not written yet. All engine features except the native game
// window work (API, MCP, editor server, software rendering to PNG).
#include <chrono>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <thread>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

#include "platform/Platform.h"

namespace oe {

std::unique_ptr<Window> CreatePlatformWindow(const std::string&, int, int) { return nullptr; }

std::unique_ptr<AudioDevice> CreateAudioDevice(int) { return nullptr; }

void PlatformEnableHighDpi() {}

std::string PlatformUserLanguage() {
    for (const char* var : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char* v = std::getenv(var);
        if (!v || !*v) continue;
        std::string code(v);
        if (code == "C" || code.rfind("C.", 0) == 0 || code == "POSIX" || code.size() < 2) return "en";
        return code.substr(0, 2);
    }
    return "en";
}

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

void PlatformAttachParentConsole() {}

void PlatformSetBinaryStdio() {}

bool PlatformReplaceFile(const std::string& from, const std::string& to, std::string* error) {
    if (std::rename(from.c_str(), to.c_str()) == 0) return true;
    if (error) *error = "cannot replace save file: " + std::string(std::strerror(errno));
    return false;
}

SaveStorage PlatformSaveStorage(const std::string& gameName) {
    const char* data = std::getenv("XDG_DATA_HOME");
    if (data && *data) return {std::string(data) + "/" + gameName, {}, {}};
    const char* home = std::getenv("HOME");
    return home && *home ? SaveStorage{std::string(home) + "/.local/share/" + gameName, {}, {}} : SaveStorage{};
}

}  // namespace oe
