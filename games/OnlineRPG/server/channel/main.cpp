// oe_channel: the channel server of OnlineRPG (docs/ONLINERPG_SERVERS.md, links 2 and 4).
//
//   oe_channel [--port 7002] [--bind 127.0.0.1] [--game-port 7003] [--game-bind 127.0.0.1]
//              [--key-file server.key] [--game-key-file game.key] [--channels 2] [--start-map town]
//              [--max-connections 4096]
//
// --key-file is the ticket key shared with oe_login; --game-key-file is what game servers present
// (oe serve-game ... --set-file linkkey=game.key). Either file is created when it is missing.
// Prints one JSON line on stdout when it is listening; logs go to stderr. Ctrl+C stops it.
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "channel/ChannelServer.h"
#include "common/Crypto.h"
#include "common/Log.h"
#include "common/Protocol.h"

namespace {
std::atomic<bool> g_stop{false};
void OnSignal(int) { g_stop = true; }

int Usage(const char* message) {
    std::fprintf(stderr, "oe_channel: %s\n"
                         "usage: oe_channel [--port 7002] [--bind 127.0.0.1] [--game-port 7003] [--game-bind 127.0.0.1]\n"
                         "                  [--key-file server.key] [--game-key-file game.key] [--channels 2] [--start-map town]\n"
                         "                  [--max-connections 4096]\n",
                 message);
    return 2;
}
}  // namespace

int main(int argc, char** argv) {
    rpg::ServerConfig config;
    config.port = static_cast<uint16_t>(rpg::kDefaultChannelPort);
    rpg::ChannelOptions options;
    std::string keyFile = "server.key", gameKeyFile = "game.key", gameBind = "127.0.0.1";
    uint16_t gamePort = static_cast<uint16_t>(rpg::kDefaultGameLinkPort);
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        if (i + 1 >= argc) return Usage(("missing value for " + flag).c_str());
        const char* value = argv[++i];
        char* end = nullptr;
        const long number = std::strtol(value, &end, 10);
        const bool numeric = *value && !*end && number >= 0;
        if (flag == "--bind") config.bind = value;
        else if (flag == "--game-bind") gameBind = value;
        else if (flag == "--key-file") keyFile = value;
        else if (flag == "--game-key-file") gameKeyFile = value;
        else if (flag == "--start-map") options.startMap = value;
        else if (!numeric) return Usage((flag + " needs a non-negative number (or is unknown)").c_str());
        else if (flag == "--port" && number <= 65535) config.port = static_cast<uint16_t>(number);
        else if (flag == "--game-port" && number <= 65535) gamePort = static_cast<uint16_t>(number);
        else if (flag == "--channels" && number >= 1 && number <= 200) options.channels = static_cast<int>(number);
        else if (flag == "--max-connections" && number >= 1) config.maxConnections = static_cast<size_t>(number);
        else return Usage(("unknown or out of range: " + flag).c_str());
    }

    std::string error;
    rpg::TicketKey key, gameKey;
    if (!rpg::LoadTicketKey(keyFile, true, key, &error) || !rpg::LoadTicketKey(gameKeyFile, true, gameKey, &error)) return Usage(error.c_str());
    rpg::FrameServer server;
    rpg::ChannelServer channel(server, key, rpg::Hex(gameKey.data(), gameKey.size()), options);
    if (!server.Listen(config, &error)) return Usage(error.c_str());
    const int gameListener = server.AddListener(gameBind, gamePort, &error);
    if (gameListener < 0) return Usage(error.c_str());
    channel.SetGameListener(gameListener);

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    std::printf("{\"ok\":true,\"server\":\"channel\",\"port\":%u,\"gamePort\":%u}\n", static_cast<unsigned>(server.Port()),
                static_cast<unsigned>(server.ListenerPort(gameListener)));
    std::fflush(stdout);
    rpg::Log("info", "channel server: players on %s:%u, game servers on %s:%u, %d channels", config.bind.c_str(), static_cast<unsigned>(server.Port()),
             gameBind.c_str(), static_cast<unsigned>(server.ListenerPort(gameListener)), options.channels);

    server.Run(channel, g_stop);
    const rpg::ChannelServer::Counters& done = channel.Stats();
    rpg::Log("info", "stopped: %llu sessions, %llu entries, %llu admitted, %llu tickets refused, %llu entries refused, %llu registrations",
             static_cast<unsigned long long>(done.sessions), static_cast<unsigned long long>(done.entries), static_cast<unsigned long long>(done.admitted),
             static_cast<unsigned long long>(done.refusedTickets), static_cast<unsigned long long>(done.refusedEntries),
             static_cast<unsigned long long>(done.registrations));
    return 0;
}
