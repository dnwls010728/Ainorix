#include "core/Log.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>

namespace oe {

namespace {
struct LogState {
    std::mutex mutex;
    std::deque<LogEntry> entries;
    uint64_t nextSeq = 1;
    bool echo = true;
    LogLevel minEcho = LogLevel::Info;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};
LogState& State() {
    static LogState s;
    return s;
}
constexpr size_t kMaxEntries = 4096;
}  // namespace

const char* ToString(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "debug";
        case LogLevel::Info: return "info";
        case LogLevel::Warn: return "warn";
        case LogLevel::Error: return "error";
    }
    return "info";
}

void Log::Write(LogLevel level, const std::string& category, const std::string& message) {
    LogState& s = State();
    std::lock_guard<std::mutex> lock(s.mutex);
    LogEntry e;
    e.seq = s.nextSeq++;
    e.time = std::chrono::duration<double>(std::chrono::steady_clock::now() - s.start).count();
    e.level = level;
    e.category = category;
    e.message = message;
    if (s.echo && level >= s.minEcho) {
        std::fprintf(stderr, "[%8.3f] %-5s %s: %s\n", e.time, ToString(level), category.c_str(), message.c_str());
    }
    s.entries.push_back(std::move(e));
    if (s.entries.size() > kMaxEntries) s.entries.pop_front();
}

std::vector<LogEntry> Log::Since(uint64_t seq, size_t maxEntries) {
    LogState& s = State();
    std::lock_guard<std::mutex> lock(s.mutex);
    std::vector<LogEntry> out;
    for (const LogEntry& e : s.entries) {
        if (e.seq > seq) out.push_back(e);
    }
    if (out.size() > maxEntries) out.erase(out.begin(), out.end() - static_cast<std::ptrdiff_t>(maxEntries));
    return out;
}

uint64_t Log::LastSeq() {
    LogState& s = State();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.nextSeq - 1;
}

void Log::SetEcho(bool enabled) {
    LogState& s = State();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.echo = enabled;
}

void Log::SetMinEchoLevel(LogLevel level) {
    LogState& s = State();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.minEcho = level;
}

std::string Format(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int len = std::vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);
    std::string out;
    if (len > 0) {
        out.resize(static_cast<size_t>(len) + 1);
        std::vsnprintf(out.data(), out.size(), fmt, args);
        out.resize(static_cast<size_t>(len));
    }
    va_end(args);
    return out;
}

}  // namespace oe
