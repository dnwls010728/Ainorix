#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace oe {

enum class LogLevel { Debug, Info, Warn, Error };

const char* ToString(LogLevel level);

struct LogEntry {
    uint64_t seq = 0;
    double time = 0.0;  // seconds since engine start
    LogLevel level = LogLevel::Info;
    std::string category;
    std::string message;
};

// Thread-safe ring buffer log. Every entry gets a monotonically increasing
// sequence number so tools can poll "everything since seq N".
class Log {
public:
    static void Write(LogLevel level, const std::string& category, const std::string& message);
    static std::vector<LogEntry> Since(uint64_t seq, size_t maxEntries = 500);
    static uint64_t LastSeq();
    // Echo entries to stderr (default on). stdout is never used so it stays
    // free for machine-readable output (CLI results, MCP protocol).
    static void SetEcho(bool enabled);
    static void SetMinEchoLevel(LogLevel level);
};

std::string Format(const char* fmt, ...);

}  // namespace oe

#define OE_LOG_DEBUG(cat, ...) ::oe::Log::Write(::oe::LogLevel::Debug, cat, ::oe::Format(__VA_ARGS__))
#define OE_LOG_INFO(cat, ...) ::oe::Log::Write(::oe::LogLevel::Info, cat, ::oe::Format(__VA_ARGS__))
#define OE_LOG_WARN(cat, ...) ::oe::Log::Write(::oe::LogLevel::Warn, cat, ::oe::Format(__VA_ARGS__))
#define OE_LOG_ERROR(cat, ...) ::oe::Log::Write(::oe::LogLevel::Error, cat, ::oe::Format(__VA_ARGS__))
