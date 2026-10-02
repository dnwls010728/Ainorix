#include "platform/Network.h"

#include <atomic>

namespace oe {
namespace {
std::atomic<uint64_t> g_created{0}, g_live{0};
}
NetSocket::NetSocket() { ++g_created; ++g_live; }
NetSocket::~NetSocket() { --g_live; }
PlatformWebSocket::PlatformWebSocket() { ++g_created; ++g_live; }
PlatformWebSocket::~PlatformWebSocket() { --g_live; }
uint64_t PlatformNetSocketsCreated() { return g_created.load(); }
uint64_t PlatformNetSocketsLive() { return g_live.load(); }

bool ValidNetAddress(const NetAddress& address, bool allowZeroPort) {
    if ((!allowZeroPort && address.port == 0) || address.host.size() > 15) return false;
    size_t start = 0;
    for (int part = 0; part < 4; ++part) {
        size_t end = address.host.find('.', start);
        if (end == std::string::npos) end = address.host.size();
        const size_t count = end - start;
        if (count == 0 || count > 3 || (count > 1 && address.host[start] == '0')) return false;
        unsigned value = 0;
        for (size_t i = start; i < end; ++i) {
            char c = address.host[i];
            if (c < '0' || c > '9') return false;
            value = value * 10 + static_cast<unsigned>(c - '0');
        }
        if (value > 255 || (part == 3 ? end != address.host.size() : end == address.host.size())) return false;
        start = end + 1;
    }
    return true;
}
}  // namespace oe
