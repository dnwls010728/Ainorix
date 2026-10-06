#pragma once
#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace rpg {

// Logs go to stderr; stdout carries only the one JSON line a server prints when it is ready.
inline void Log(const char* level, const char* format, ...) {
    char when[32];
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    std::strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &utc);
    char line[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    std::fprintf(stderr, "%s %s %s\n", when, level, line);
}

}  // namespace rpg
