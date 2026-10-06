#include "channel/ChannelServer.h"

#include "common/Crypto.h"
#include "common/Log.h"
#include "common/Protocol.h"

namespace rpg {
namespace {
constexpr double kClientIdleSeconds = 30, kGameIdleSeconds = 5, kBroadcastSeconds = 2;
}

ChannelServer::ChannelServer(FrameServer& server, TicketKey ticketKey, std::string linkKey, const ChannelOptions& options)
    : server_(server), ticketKey_(std::move(ticketKey)), linkKey_(std::move(linkKey)), options_(options) {
    if (options_.channels < 1) options_.channels = 1;
    if (options_.channels > 200) options_.channels = 200;
}

void ChannelServer::OnOpen(FrameServer::ConnId id, const std::string&) {
    Peer& peer = peers_[id];
    peer.game = server_.Listener(id) == gameListener_;
    server_.SetIdleSeconds(id, peer.game ? kGameIdleSeconds : kClientIdleSeconds);
}

void ChannelServer::OnClose(FrameServer::ConnId id, const char* reason) {
    auto it = peers_.find(id);
    if (it == peers_.end()) return;
    if (it->second.game && it->second.server) {
        auto game = games_.find(it->second.server);
        // A replaced registration keeps its id with the new connection: only the owner removes it.
        if (game != games_.end() && game->second.connection == id) {
            Log("info", "game server %u (channel %u, %s) is offline: %s", static_cast<unsigned>(it->second.server),
                static_cast<unsigned>(game->second.channel), game->second.map.c_str(), reason);
            games_.erase(game);
            BuildChannels();
        }
    }
    peers_.erase(it);
}

void ChannelServer::Error(FrameServer::ConnId id, uint16_t code, bool close) {
    out_.Clear();
    out_.U16(code);
    server_.Send(id, MsgError, out_.Data(), out_.Size());
    if (close) server_.Close(id, "refused", true);
}

void ChannelServer::OnFrame(FrameServer::ConnId id, uint16_t type, const uint8_t* payload, size_t size) {
    auto it = peers_.find(id);
    if (it == peers_.end()) return;
    Peer& peer = it->second;
    Reader in(payload, size);
    if (!peer.hello) {
        uint16_t version = 0;
        uint8_t role = 0;
        std::string key;
        in.U16(version);
        in.U8(role);
        if (peer.game) in.Str8(key, 128);
        if (type != MsgHello || !in.End() || version != kProtocolVersion || role != (peer.game ? RoleGame : RoleClient)) { Error(id, ErrVersion, true); return; }
        if (peer.game && (key.size() != linkKey_.size() || !ConstantTimeEqual(key.data(), linkKey_.data(), key.size()))) {
            Log("warn", "a game server from %s presented a wrong link key", server_.Address(id).c_str());
            Error(id, ErrTicket, true);
            return;
        }
        peer.hello = true;
        return;
    }
    if (peer.game) GameFrame(id, peer, type, in);
    else ClientFrame(id, peer, type, in);
}

// ----- players choosing a channel (link 2) ------------------------------------

void ChannelServer::ClientFrame(FrameServer::ConnId id, Peer& peer, uint16_t type, Reader& in) {
    if (type == MsgPing && in.End()) return;  // receiving it was the point
    if (type == MsgSession && !peer.session) {
        std::string text, name, proof;
        in.Str8(text, 64);
        in.Str8(name, 64);
        in.Str8(proof, 64);
        if (!in.End()) { Error(id, ErrBadRequest, true); return; }
        Ticket ticket;
        if (!OpenTicket(text, ticketKey_, Ticket::Session, UnixNow(), ticket) || !CheckNameProof(ticketKey_, ticket.account, name, proof)) {
            ++counters_.refusedTickets;
            Error(id, ErrTicket, true);
            return;
        }
        peer.session = true;
        peer.account = ticket.account;
        peer.name = name;
        ++counters_.sessions;
        SendChannels(id);
        return;
    }
    if (type == MsgEnter && peer.session) {
        uint8_t channel = 0;
        in.U8(channel);
        if (!in.End()) { Error(id, ErrBadRequest, true); return; }
        Enter(id, peer, channel);
        return;
    }
    Error(id, ErrBadRequest, true);
}

void ChannelServer::BuildChannels() {
    channels_.Clear();
    channels_.U8(static_cast<uint8_t>(options_.channels));
    for (int channel = 1; channel <= options_.channels; ++channel) {
        uint32_t players = 0, capacity = 0;
        bool online = false;
        for (const auto& game : games_) {
            if (game.second.channel != channel) continue;
            online = true;
            players += game.second.players;
            capacity += game.second.capacity;
        }
        channels_.U8(static_cast<uint8_t>(channel)).U16(static_cast<uint16_t>(players)).U16(static_cast<uint16_t>(capacity)).U8(online ? 1 : 0);
    }
}

void ChannelServer::SendChannels(FrameServer::ConnId id) {
    if (!channels_.Size()) BuildChannels();
    server_.Send(id, MsgChannels, channels_.Data(), channels_.Size());
}

void ChannelServer::Enter(FrameServer::ConnId id, Peer& peer, uint8_t channel) {
    // The start map's server of the wanted channel, or (0) the emptiest one that has room.
    Game* chosen = nullptr;
    uint16_t chosenId = 0;
    bool exists = false;
    for (auto& entry : games_) {
        Game& game = entry.second;
        if (game.map != options_.startMap || (channel && game.channel != channel)) continue;
        exists = true;
        const uint32_t load = static_cast<uint32_t>(game.players) + game.pending;
        if (load >= game.capacity) continue;
        if (!chosen || load < static_cast<uint32_t>(chosen->players) + chosen->pending) { chosen = &game; chosenId = entry.first; }
    }
    if (!chosen) {
        ++counters_.refusedEntries;
        Error(id, exists ? ErrChannelFull : ErrChannelOffline, false);  // the player may pick another channel
        return;
    }
    Ticket ticket;
    ticket.kind = Ticket::Entry;
    ticket.account = peer.account;
    ticket.server = chosenId;
    ticket.expires = UnixNow() + Ticket::kEntrySeconds;
    do {
        if (!RandomBytes(&ticket.nonce, sizeof(ticket.nonce))) { Error(id, ErrInternal, true); return; }
    } while (entries_.count(ticket.nonce));
    Entry& issued = entries_[ticket.nonce];
    issued.account = peer.account;
    issued.expires = ticket.expires;
    issued.server = chosenId;
    issued.name = peer.name;
    ++chosen->pending;
    ++counters_.entries;
    out_.Clear();
    out_.Str8(chosen->address).U16(chosen->port).Str8(SealTicket(ticket, ticketKey_)).U8(chosen->channel).Str8(chosen->map);
    server_.Send(id, MsgEnterResult, out_.Data(), out_.Size());
    server_.Close(id, "entered", true);
}

void ChannelServer::Forget(uint32_t nonce) {
    auto it = entries_.find(nonce);
    if (it == entries_.end()) return;
    auto game = games_.find(it->second.server);
    if (game != games_.end() && game->second.pending) --game->second.pending;
    entries_.erase(it);
}

// ----- game servers (link 4) --------------------------------------------------

void ChannelServer::Unregister(uint16_t server) {
    auto it = games_.find(server);
    if (it == games_.end()) return;
    const FrameServer::ConnId old = it->second.connection;
    games_.erase(it);
    auto peer = peers_.find(old);
    if (peer != peers_.end()) peer->second.server = 0;
    server_.Close(old, "replaced");
}

void ChannelServer::GameFrame(FrameServer::ConnId id, Peer& peer, uint16_t type, Reader& in) {
    if (type == MsgGameRegister) {
        Game game;
        in.U8(game.channel);
        in.Str8(game.map, 32);
        in.Str8(game.address, 64);
        in.U16(game.port);
        in.U16(game.capacity);
        if (!in.End() || peer.server || game.channel < 1 || game.channel > options_.channels || game.map.empty() || game.address.empty() || !game.port ||
            game.capacity < 1 || game.capacity > 63) { Error(id, ErrBadRequest, true); return; }
        // One server per channel and map: a newcomer for the same place replaces the old one
        // (a restarted server whose old link has not timed out yet).
        for (const auto& entry : games_)
            if (entry.second.channel == game.channel && entry.second.map == game.map) { Unregister(entry.first); break; }
        while (!nextServer_ || games_.count(nextServer_)) ++nextServer_;
        peer.server = nextServer_++;
        game.connection = id;
        Log("info", "game server %u registered: channel %u, %s, %s:%u, %u players at most", static_cast<unsigned>(peer.server),
            static_cast<unsigned>(game.channel), game.map.c_str(), game.address.c_str(), static_cast<unsigned>(game.port), static_cast<unsigned>(game.capacity));
        games_[peer.server] = std::move(game);
        ++counters_.registrations;
        BuildChannels();
        out_.Clear();
        out_.U16(peer.server);
        server_.Send(id, MsgGameRegistered, out_.Data(), out_.Size());
        return;
    }
    auto registered = games_.find(peer.server);
    if (!peer.server || registered == games_.end()) { Error(id, ErrNotRegistered, true); return; }
    if (type == MsgHeartbeat) {
        uint16_t players = 0;
        in.U16(players);
        if (!in.End()) { Error(id, ErrBadRequest, true); return; }
        registered->second.players = players;
    } else if (type == MsgAdmit) {
        uint32_t player = 0;
        std::string text;
        in.U32(player);
        in.Str8(text, 64);
        if (!in.End()) { Error(id, ErrBadRequest, true); return; }
        // Signed by us, not expired, issued for this very server, and not used before.
        Ticket ticket;
        uint16_t code = ErrTicket;
        uint32_t account = 0;
        std::string name;
        if (OpenTicket(text, ticketKey_, Ticket::Entry, UnixNow(), ticket) && ticket.server == peer.server) {
            auto issued = entries_.find(ticket.nonce);
            if (issued != entries_.end() && issued->second.account == ticket.account && issued->second.server == peer.server) {
                code = 0;
                account = ticket.account;
                name = issued->second.name;
                Forget(ticket.nonce);
            }
        }
        if (code) ++counters_.refusedTickets;
        else ++counters_.admitted;
        out_.Clear();
        out_.U32(player).U16(code).U32(account).Str8(name);
        server_.Send(id, MsgAdmitResult, out_.Data(), out_.Size());
    } else if (type == MsgLeft) {
        uint32_t account = 0;
        in.U32(account);
        if (!in.End()) { Error(id, ErrBadRequest, true); return; }
    } else {
        Error(id, ErrBadRequest, true);
    }
}

void ChannelServer::OnTick(double now) {
    const uint32_t today = UnixNow();
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.expires >= today) { ++it; continue; }
        const uint32_t nonce = it->first;
        ++it;
        Forget(nonce);
    }
    if (now - lastBroadcast_ < kBroadcastSeconds) return;
    lastBroadcast_ = now;
    BuildChannels();
    for (const auto& peer : peers_)
        if (!peer.second.game && peer.second.session) server_.Send(peer.first, MsgChannels, channels_.Data(), channels_.Size());
}

}  // namespace rpg
