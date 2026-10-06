// oe_login: the login server of OnlineRPG (docs/ONLINERPG_SERVERS.md, link 1).
//
//   oe_login [--port 7001] [--bind 127.0.0.1] [--db accounts.db] [--key-file server.key]
//            [--workers N] [--iterations 100000] [--login-limit 5] [--guest-limit 20]
//            [--max-connections 4096]
//
// Prints one JSON line on stdout when it is listening; logs go to stderr. Ctrl+C stops it.
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "common/Log.h"
#include "common/Protocol.h"
#include "login/LoginServer.h"

namespace {
std::atomic<bool> g_stop{false};
void OnSignal(int) { g_stop = true; }

int Usage(const char* message) {
    std::fprintf(stderr, "oe_login: %s\n"
                         "usage: oe_login [--port 7001] [--bind 127.0.0.1] [--db accounts.db] [--key-file server.key]\n"
                         "                [--workers N] [--iterations 100000] [--login-limit 5] [--guest-limit 20] [--max-connections 4096]\n",
                 message);
    return 2;
}
}  // namespace

int main(int argc, char** argv) {
    rpg::ServerConfig config;
    config.port = static_cast<uint16_t>(rpg::kDefaultLoginPort);
    rpg::LoginOptions options;
    std::string keyFile = "server.key";
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        if (i + 1 >= argc) return Usage(("missing value for " + flag).c_str());
        const char* value = argv[++i];
        char* end = nullptr;
        const long number = std::strtol(value, &end, 10);
        const bool numeric = *value && !*end && number >= 0;
        if (flag == "--bind") config.bind = value;
        else if (flag == "--db") options.database = value;
        else if (flag == "--key-file") keyFile = value;
        else if (!numeric) return Usage((flag + " needs a non-negative number (or is unknown)").c_str());
        else if (flag == "--port" && number <= 65535) config.port = static_cast<uint16_t>(number);
        else if (flag == "--workers" && number <= 256) options.workers = static_cast<size_t>(number);
        else if (flag == "--iterations" && number >= 1) options.iterations = static_cast<uint32_t>(number);
        else if (flag == "--login-limit") options.loginLimit = static_cast<uint32_t>(number);
        else if (flag == "--guest-limit") options.guestLimit = static_cast<uint32_t>(number);
        else if (flag == "--max-connections" && number >= 1) config.maxConnections = static_cast<size_t>(number);
        else return Usage(("unknown or out of range: " + flag).c_str());
    }

    std::string error;
    rpg::TicketKey key;
    if (!rpg::LoadTicketKey(keyFile, true, key, &error)) return Usage(error.c_str());
    rpg::FrameServer server;
    rpg::LoginServer login(server, key, options);
    if (!login.Open(&error)) return Usage(error.c_str());
    if (!server.Listen(config, &error)) return Usage(error.c_str());

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    std::printf("{\"ok\":true,\"server\":\"login\",\"port\":%u}\n", static_cast<unsigned>(server.Port()));
    std::fflush(stdout);
    rpg::Log("info", "login server on %s:%u, database %s, %u PBKDF2 rounds", config.bind.c_str(), static_cast<unsigned>(server.Port()),
             options.database.c_str(), static_cast<unsigned>(options.iterations));

    server.Run(login, g_stop);
    login.Stop();
    const rpg::ServerStats& net = server.Stats();
    const rpg::LoginServer::Counters& done = login.Stats();
    rpg::Log("info", "stopped: %llu connections, %llu logins, %llu registrations, %llu guests, %llu refused, %llu rate limited, %llu busy, %llu bad frames",
             static_cast<unsigned long long>(net.accepted), static_cast<unsigned long long>(done.logins), static_cast<unsigned long long>(done.registrations),
             static_cast<unsigned long long>(done.guests), static_cast<unsigned long long>(done.refused), static_cast<unsigned long long>(done.limited),
             static_cast<unsigned long long>(done.busy), static_cast<unsigned long long>(net.badFrames));
    return 0;
}
