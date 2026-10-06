#pragma once
#include <map>
#include <string>
#include <unordered_map>

#include "common/Net.h"
#include "common/Ticket.h"
#include "common/Wire.h"

namespace rpg {

struct ChannelOptions {
    int channels = 2;               // channels shown to players: 1..channels
    std::string startMap = "town";  // the map a player enters from the channel list
};

// The channel server: the hub between players choosing where to play (link 2, the first listener)
// and the game servers that run the worlds (link 4, the second listener).
//
// Everything it needs to answer is in memory: the registry of game servers, their populations and
// the entry tickets it issued. A request is answered on the I/O thread without waiting for anything.
class ChannelServer final : public FrameServer::Handler {
public:
    ChannelServer(FrameServer& server, TicketKey ticketKey, std::string linkKey, const ChannelOptions& options);
    // The listener index game servers connect to (FrameServer::AddListener).
    void SetGameListener(int listener) { gameListener_ = listener; }

    void OnOpen(FrameServer::ConnId id, const std::string& address) override;
    void OnFrame(FrameServer::ConnId id, uint16_t type, const uint8_t* payload, size_t size) override;
    void OnClose(FrameServer::ConnId id, const char* reason) override;
    void OnTick(double now) override;

    struct Counters {
        uint64_t sessions = 0, entries = 0, admitted = 0, refusedTickets = 0, refusedEntries = 0, registrations = 0;
    };
    const Counters& Stats() const { return counters_; }
    size_t GameServers() const { return games_.size(); }

private:
    struct Peer {
        bool game = false, hello = false, session = false;
        uint32_t account = 0;
        std::string name;
        uint16_t server = 0;  // a registered game server's id
    };
    struct Game {
        FrameServer::ConnId connection = 0;
        uint8_t channel = 0;
        std::string map, address;
        uint16_t port = 0, capacity = 0, players = 0, pending = 0;  // pending: entry tickets out, not yet admitted
    };
    struct Entry {
        uint32_t account = 0, expires = 0;
        uint16_t server = 0;
        std::string name;
    };
    void Error(FrameServer::ConnId id, uint16_t code, bool close);
    void ClientFrame(FrameServer::ConnId id, Peer& peer, uint16_t type, Reader& in);
    void GameFrame(FrameServer::ConnId id, Peer& peer, uint16_t type, Reader& in);
    void SendChannels(FrameServer::ConnId id);
    void BuildChannels();
    void Enter(FrameServer::ConnId id, Peer& peer, uint8_t channel);
    void Unregister(uint16_t server);
    void Forget(uint32_t nonce);  // an entry ticket is used or expired

    FrameServer& server_;
    TicketKey ticketKey_;
    std::string linkKey_;
    ChannelOptions options_;
    int gameListener_ = -1;
    std::unordered_map<FrameServer::ConnId, Peer> peers_;
    std::map<uint16_t, Game> games_;  // by server id: iteration order decides ties
    std::unordered_map<uint32_t, Entry> entries_;  // by ticket nonce
    uint16_t nextServer_ = 1;
    Writer channels_, out_;  // reused payload buffers
    Counters counters_;
    double lastBroadcast_ = 0;
};

}  // namespace rpg
