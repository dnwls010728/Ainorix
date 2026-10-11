#include "net/Session.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "net/Bytes.h"
#include "net/LoopbackTransport.h"

namespace oe {
namespace {
constexpr uint32_t kControl = 0x5343454f, kEnvelope = 0x5344454f;
constexpr uint32_t kAll = 0xffffffff, kOthers = 0xfffffffe;
// Unauthenticated connections are bounded separately from player slots, so idle sockets
// cannot fill the lobby; admission against maxPlayers happens at the cookie response.
constexpr size_t kMaxConnections = 64;
std::atomic<uint64_t> g_created{0}, g_polls{0};
struct Room { std::weak_ptr<LoopbackNetwork> wire; std::weak_ptr<uint64_t> clock; PeerId next = 2; };
// Late join/reconnect starts its private session frame at zero. Map it to the room's
// monotonic wire frame so existing messages do not wait for a new client to catch up.
class RoomTransport final : public ITransport {
public:
    RoomTransport(std::shared_ptr<LoopbackNetwork> network, PeerId peer, std::shared_ptr<uint64_t> clock)
        : wire_(std::move(network), peer), clock_(std::move(clock)), base_(*clock_) {}
    bool Send(PeerId peer, const uint8_t* bytes, size_t size) override { return wire_.Send(peer, bytes, size); }
    bool Poll(uint64_t frame, std::vector<TransportEvent>& events) override {
        if (frame > UINT64_MAX - base_) return false;
        uint64_t wireFrame = base_ + frame;
        *clock_ = std::max(*clock_, wireFrame);
        return wire_.Poll(wireFrame, events);
    }
private:
    LoopbackTransport wire_;
    std::shared_ptr<uint64_t> clock_;
    uint64_t base_;
};
// Tool/main-thread use only. Entries are created exclusively by explicit loopback hosts.
std::map<std::string, Room> g_rooms;
bool String(ByteWriter& writer, const std::string& value) {
    return writer.WriteBlob(reinterpret_cast<const uint8_t*>(value.data()), value.size());
}
bool String(ByteReader& reader, std::string& value, size_t limit) {
    std::vector<uint8_t> bytes;
    if (!reader.ReadBlob(bytes, limit)) return false;
    value.assign(bytes.begin(), bytes.end());
    return value.find('\0') == std::string::npos;
}
bool Text(const std::string& value, size_t limit) {
    return !value.empty() && value.size() <= limit && value.find('\0') == std::string::npos &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= 32; });
}
bool Value(const Json& value, unsigned depth) {
    if (depth > 8) return false;
    if (value.isNumber()) return std::isfinite(value.asNumber());
    if (value.isString()) return value.asString().size() <= 1024 && value.asString().find('\0') == std::string::npos;
    if (value.size() > 64) return false;
    for (const auto& item : value.items()) if (!Value(item, depth + 1)) return false;
    for (const auto& item : value.members()) if (!Text(item.first, 64) || !Value(item.second, depth + 1)) return false;
    return true;
}
// Reject deeply nested untrusted text before the recursive JSON parser touches it.
bool JsonDepth(const std::string& text) {
    int depth = 0;
    bool quoted = false, escaped = false;
    for (char c : text) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '[' || c == '{') { if (++depth > 10) return false; }
        else if (c == ']' || c == '}') { if (--depth < 0) return false; }
    }
    return depth == 0 && !quoted;
}
bool Integer(const Json& json, const char* key, uint32_t fallback, uint32_t low, uint32_t high, uint32_t& out) {
    double value = json.has(key) ? json[key].asNumber(-1) : static_cast<double>(fallback);
    if (!std::isfinite(value) || value < low || value > high || std::floor(value) != value) return false;
    out = static_cast<uint32_t>(value);
    return true;
}
}

bool ValidRpc(const std::string& name, const Json& args) {
    return Text(name, 64) && args.isArray() && args.size() <= 16 && Value(args, 0) && args.dump().size() <= 8192;
}

bool SessionConfig::Parse(const Json& project, SessionConfig& out, std::string* error) {
    SessionConfig config;
    config.gameId = project["name"].asString("Untitled");
    const Json& json = project["network"];
    auto fail = [&](const char* message) { if (error) *error = message; return false; };
    if (json.isNull()) { out = config; return true; }
    if (!json.isObject()) return fail("network must be an object; remove it for single-player");
    if (json.has("mode") && !json["mode"].isString()) return fail("network.mode must be a string");
    config.mode = json["mode"].asString("none");
    if (config.mode == "none") { out = config; return true; }
    if (config.mode != "lockstep" && config.mode != "rollback" && config.mode != "authoritative") return fail("network.mode must be none, lockstep, rollback or authoritative");
    if (!SyncConfig::Parse(json, config.sync, error)) return false;
    if (!Integer(json, "snapshotRate", 20, 1, 60, config.snapshotRate) ||
        !Integer(json, "interpolationFrames", 6, 0, 30, config.interpolationFrames) ||
        !Integer(json, "predictionFrames", 32, 8, 64, config.predictionFrames))
        return fail("snapshotRate must be 1..60, interpolationFrames 0..30, predictionFrames 8..64");
    if (json.has("playerPrefab") && !json["playerPrefab"].isString()) return fail("playerPrefab must be a project-relative path");
    config.playerPrefab = json["playerPrefab"].asString();
    if (json.has("lateJoin") && (!json["lateJoin"].isBool() || (json["lateJoin"].asBool() && config.mode != "authoritative")))
        return fail("lateJoin must be a boolean and needs mode authoritative");
    config.lateJoin = json["lateJoin"].asBool();
    if (json.has("admission") && !json["admission"].isString()) return fail("network.admission must be \"auto\" or \"script\"");
    config.admission = json["admission"].asString("auto");
    if (config.admission != "auto" && config.admission != "script") return fail("network.admission must be \"auto\" or \"script\"");
    if (config.admission == "script" && !config.lateJoin) return fail("network.admission \"script\" needs lateJoin");
    if (json.has("prediction") && (!json["prediction"].isString() || (json["prediction"].asString() != "full" && json["prediction"].asString() != "owned")))
        return fail("network.prediction must be \"full\" or \"owned\"");
    config.prediction = json["prediction"].asString("full");
    if (config.prediction == "owned" && config.mode != "authoritative") return fail("network.prediction \"owned\" needs mode authoritative");
    uint32_t timeoutSeconds = 30;
    if (!Integer(json, "timeoutSeconds", 30, 5, 120, timeoutSeconds)) return fail("network.timeoutSeconds must be 5..120");
    config.timeoutFrames = timeoutSeconds * 60;
    if (config.playerPrefab.size() > 256 || config.playerPrefab.find("..") != std::string::npos ||
        config.playerPrefab.find(':') != std::string::npos || (!config.playerPrefab.empty() && (config.playerPrefab[0] == '/' || config.playerPrefab[0] == '\\')))
        return fail("playerPrefab must stay inside the project");
    for (const char* key : {"transport", "bind", "gameId", "controlTransport"})
        if (json.has(key) && !json[key].isString()) return fail("network transport/bind/gameId/controlTransport must be strings");
    config.transport = json["transport"].asString("tcp");
    config.bind = json["bind"].asString("127.0.0.1");
    config.gameId = json["gameId"].asString(config.gameId);
    if (!Text(config.gameId, 64)) return fail("network.gameId (or project name) must contain 1..64 printable bytes");
    if (config.transport != "tcp" && config.transport != "udp" && config.transport != "websocket" && config.transport != "loopback")
        return fail("network.transport must be tcp, udp, websocket or loopback");
    if (json["controlTransport"].asString("tcp") != "tcp") return fail("controlTransport must be tcp");
    uint32_t port = 0;
    if (!Integer(json, "port", 7778, 0, 65535, port) || !Integer(json, "maxPlayers", 4, 1, 64, config.maxPlayers) ||
        !Integer(json, "tickRate", 60, 60, 60, config.tickRate))
        return fail("network.port must be 0..65535, maxPlayers 1..64, tickRate exactly 60");
    config.port = static_cast<uint16_t>(port);
    if (!ValidNetAddress({config.bind, config.port}, true)) return fail("network.bind must be canonical numeric IPv4; default is 127.0.0.1");
    out = config;
    return true;
}

namespace {
ChannelConfig ChannelsFor(const SessionConfig& config) {
    ChannelConfig channels;
    channels.timeoutFrames = config.timeoutFrames;
    return channels;
}
}  // namespace

Session::Session(const SessionConfig& config, std::unique_ptr<ITransport> wire, bool host, PeerId server,
                 const std::string& name, uint32_t seed, Random random)
    : config_(config), wire_(std::move(wire)), channels_(*this, ChannelsFor(config)), random_(std::move(random)),
      name_(name), server_(server), localPlayer_(host ? 1u : 0u), seed_(seed), host_(host) {
    ++g_created;
    SetState(host ? "lobby" : "connecting");
    if (host) {
        players_[1] = {name, false};
        Emit({SessionEvent::Type::Joined, 1}, kEveryWorld);
    } else {
        Connection connection;
        connection.name = name;
        connection.phase = "hello";
        if (!Entropy(connection.nonce)) SetState("error", "platform entropy unavailable");
        else connections_[server_] = connection;
    }
}
Session::~Session() { if (host_ && !room_.empty()) g_rooms.erase(room_); }
void Session::CloseTransport() {
    for (const auto& item : connections_) channels_.RemovePeer(item.first);
    connections_.clear();
    for (const auto& item : players_) Emit({SessionEvent::Type::Left, item.first}, item.first == 1 ? kEveryWorld : item.second.world);
    players_.clear(); localPlayer_ = 0;
    fallback_.reset(); udp_.reset(); wire_.reset(); loopback_.reset(); tcp_ = nullptr; web_ = nullptr; webServer_ = nullptr;
    if (host_ && !room_.empty()) { g_rooms.erase(room_); room_.clear(); }
}
uint64_t Session::InstancesCreated() { return g_created.load(); }
uint64_t Session::PollCalls() { return g_polls.load(); }
bool Session::Entropy(uint64_t& value) {
    uint8_t bytes[8]{};
    if (!random_ || !random_(bytes, sizeof(bytes))) return false;
    ByteReader reader(bytes, sizeof(bytes));
    return reader.ReadU64(value) && value != 0 && value != UINT64_MAX;
}

std::unique_ptr<Session> Session::Host(const SessionConfig& config, const std::string& name, uint32_t seed,
                                     const std::string& room, std::string* error) {
    auto fail = [&](const std::string& message) -> std::unique_ptr<Session> { if (error) *error = message; return nullptr; };
    if (!Text(name, 64)) return fail("player name must contain 1..64 printable bytes");
    std::unique_ptr<ITransport> wire;
    TcpTransport* tcp = nullptr; WebSocketServerTransport* webServer = nullptr;
    if (config.transport == "loopback") {
        if (!Text(room, 64)) return fail("loopback room must contain 1..64 printable bytes");
        for (auto it = g_rooms.begin(); it != g_rooms.end();) {
            if (it->second.wire.expired()) it = g_rooms.erase(it); else ++it;
        }
        if (g_rooms.count(room) || g_rooms.size() >= 64) return fail("loopback room already hosted or room limit reached");
        auto network = std::make_shared<LoopbackNetwork>();
        auto clock = std::make_shared<uint64_t>(0);
        wire = std::make_unique<RoomTransport>(network, 1, clock);
        g_rooms[room] = {network, clock, 2};
    } else if (config.transport == "websocket") {
        auto transport = std::make_unique<WebSocketServerTransport>();
        if (!transport->Listen({config.bind, config.port}, error)) return nullptr;
        webServer = transport.get(); wire = std::move(transport);
    } else {
        auto transport = std::make_unique<TcpTransport>();
        if (!transport->Listen({config.bind, config.port}, error)) return nullptr;
        tcp = transport.get();
        wire = std::move(transport);
    }
    auto session = std::make_unique<Session>(config, std::move(wire), true, 1, name, seed, NetworkRandom);
    if (config.transport == "loopback") session->loopback_ = g_rooms.at(room).wire.lock();
    session->tcp_ = tcp; session->webServer_ = webServer;
    session->room_ = config.transport == "loopback" ? room : "";
    if (config.transport == "udp") {
        session->udp_ = std::make_unique<UdpTransport>();
        if (!session->udp_->Bind({config.bind, tcp->LocalAddress().port}, error)) return nullptr;
        session->fallback_ = std::make_unique<FallbackTransport>(*session->udp_, *session->wire_, 30, true);
    }
    return session;
}

std::unique_ptr<Session> Session::Join(const SessionConfig& config, const std::string& name,
                                     const std::string& address, uint16_t port, std::string* error) {
    std::unique_ptr<ITransport> wire;
    TcpTransport* tcp = nullptr;
    WebSocketTransport* web = nullptr;
    if (!Text(name, 64)) { if (error) *error = "player name must contain 1..64 printable bytes"; return nullptr; }
    if (config.transport == "loopback") {
        auto it = g_rooms.find(address);
        auto network = it == g_rooms.end() ? nullptr : it->second.wire.lock();
        if (!network || it->second.next == std::numeric_limits<PeerId>::max()) {
            if (error) *error = "loopback room not found; use net.host in this process first";
            return nullptr;
        }
        try { wire = std::make_unique<RoomTransport>(network, it->second.next++, it->second.clock.lock()); }
        catch (const std::invalid_argument&) {
            if (error) *error = "loopback endpoint limit reached; leave/stop a peer before joining";
            return nullptr;
        }
    } else if (config.transport == "websocket") {
        auto transport = std::make_unique<WebSocketTransport>();
        if (!transport->Connect(1, address, error)) return nullptr;
        web = transport.get();
        wire = std::move(transport);
    } else {
        auto transport = std::make_unique<TcpTransport>();
        if (!transport->Connect(1, {address, port}, error)) return nullptr;
        tcp = transport.get();
        wire = std::move(transport);
    }
    auto session = std::make_unique<Session>(config, std::move(wire), false, 1, name, 0, NetworkRandom);
    if (config.transport == "loopback") session->loopback_ = g_rooms.at(address).wire.lock();
    session->tcp_ = tcp;
    session->web_ = web;
    if (session->state_ == "error") { if (error) *error = session->error_; return nullptr; }
    if (config.transport == "udp") {
        session->udp_ = std::make_unique<UdpTransport>();
        if (!session->udp_->Bind({config.bind, 0}, error)) return nullptr;
        session->fallback_ = std::make_unique<FallbackTransport>(*session->udp_, *session->wire_, 30, true);
    }
    return session;
}

void Session::SetState(const std::string& state, const std::string& error) {
    if (state_ != state) Emit({SessionEvent::Type::State, 0, 0, state}, kEveryWorld);
    state_ = state;
    error_ = error;
}
void Session::Emit(SessionEvent event, uint32_t world) {
    if (world == kEveryWorld) {
        for (uint32_t i = 0; i < std::max<size_t>(1, worlds_.size()); ++i) Emit(event, i);
        return;
    }
    auto& queue = events_[world];
    if (queue.size() >= 256) { ++rejected_; return; }
    queue.push_back(std::move(event));
}
uint32_t Session::WorldOf(uint32_t player) const {
    auto it = players_.find(player);
    return it == players_.end() ? 0 : it->second.world;
}
size_t Session::WorldLoad(uint32_t world) const {
    size_t load = 0;
    for (const auto& item : connections_) if (item.second.player && item.second.world == world) ++load;
    return load;
}
bool Session::SetWorlds(const std::vector<std::string>& scenes, std::string* error) {
    auto fail = [&](const char* message) { if (error) *error = message; return false; };
    if (!host_ || config_.mode != "authoritative" || !config_.lateJoin) return fail("worlds need an authoritative host with network.lateJoin");
    if (!connections_.empty() || !worlds_.empty()) return fail("worlds are set once, before anybody joins");
    if (scenes.empty() || scenes.size() > 64) return fail("1..64 worlds");
    for (size_t i = 0; i < scenes.size(); ++i) {
        if (!Text(scenes[i], 256)) return fail("a world's scene is a path of 1..256 printable bytes");
        for (size_t j = 0; j < i; ++j) if (scenes[j] == scenes[i]) return fail("each world runs another scene");
    }
    worlds_ = scenes;
    return true;
}
bool Session::Move(uint32_t player, const std::string& scene, const Json& data, std::string* error) {
    auto fail = [&](const char* message) { if (error) *error = message; return false; };
    if (!host_ || worlds_.empty()) return fail("only the host of a session with worlds moves players");
    const auto found = std::find(worlds_.begin(), worlds_.end(), scene);
    if (found == worlds_.end()) return fail("no world runs that scene");
    const uint32_t world = static_cast<uint32_t>(found - worlds_.begin());
    auto it = players_.find(player);
    if (player == 1 || it == players_.end()) return fail("no such player");
    if (it->second.world == world) return fail("the player is in that world");
    if (!Value(data, 0) || data.dump().size() > 8192) return fail("move data exceeds its bounds (8 KiB, depth 8)");
    if (WorldLoad(world) + 1 >= config_.maxPlayers) return fail("that world is full");
    for (auto& item : connections_) if (item.second.player == player && item.second.phase == "active") {
        Connection& connection = item.second;
        const uint16_t moves = static_cast<uint16_t>(connection.moves + 1);
        if (!SendMessage(item.first, 8, 1, player, moves, "world", Json(scene))) return fail("the player's send queue is full");
        Emit({SessionEvent::Type::Left, player}, connection.world);
        connection.world = it->second.world = world; connection.moves = moves;
        it->second.ready = false;
        Json joined = Json::MakeObject(); joined["data"] = data;
        Emit({SessionEvent::Type::Joined, player, 0, "", joined}, world);
        BroadcastRoster();
        return true;
    }
    return fail("the player is not connected");
}
bool Session::TakeWorldChange(std::string& scene) {
    if (!worldChanged_) return false;
    worldChanged_ = false; scene = worldChange_; return true;
}

void Session::SendControl(PeerId peer, uint8_t kind, const Connection& connection) {
    ByteWriter writer(1024);
    writer.WriteU32(kControl); writer.WriteU8(kind); writer.WriteU16(config_.version);
    writer.WriteU64(connection.nonce); writer.WriteU64(connection.cookieA); writer.WriteU64(connection.cookieB);
    writer.WriteU32(config_.tickRate); String(writer, config_.mode); String(writer, config_.gameId);
    String(writer, kind == 1 || connection.name.empty() ? name_ : connection.name);
    writer.WriteU16(udp_ ? udp_->LocalAddress().port : 0);
    writer.WriteU32(seed_); writer.WriteU32(connection.player);
    if (writer.Ok()) wire_->Send(peer, writer.Data().data(), writer.Data().size());
}

void Session::RegisterPeer(PeerId peer, Connection& connection) {
    channels_.AddPeer(peer);
    connection.received = frame_;
    if (udp_ && connection.udpPort != 0) {
        NetAddress address = tcp_->PeerAddress(peer);
        address.port = connection.udpPort;
        if (!udp_->AddPeer(peer, address)) connection.udpPort = 0;
        else if (!fallback_->AddPeer(peer, connection.cookieA)) { udp_->RemovePeer(peer); connection.udpPort = 0; }
    }
}

void Session::Control(PeerId peer, const std::vector<uint8_t>& bytes) {
    ByteReader reader(bytes.data(), bytes.size());
    uint32_t magic = 0, tick = 0, seed = 0, player = 0;
    uint8_t kind = 0;
    uint16_t version = 0, port = 0;
    uint64_t nonce = 0, cookieA = 0, cookieB = 0;
    std::string mode, game, name;
    if (bytes.size() > 1024 || !reader.ReadU32(magic) || magic != kControl || !reader.ReadU8(kind) || kind < 1 || kind > 6 ||
        !reader.ReadU16(version) || !reader.ReadU64(nonce) || !nonce || !reader.ReadU64(cookieA) || !reader.ReadU64(cookieB) ||
        !reader.ReadU32(tick) || !String(reader, mode, 32) || !String(reader, game, 64) || !String(reader, name, 64) ||
        !reader.ReadU16(port) || !reader.ReadU32(seed) || !reader.ReadU32(player) || reader.Remaining() || !Text(name, 64)) {
        ++rejected_; return;
    }
    if (version != config_.version || tick != config_.tickRate || mode != config_.mode || game != config_.gameId) {
        ++rejected_;
        if (!host_) SetState("error", "session protocol/game/mode/tick mismatch; use matching project settings and engine builds");
        else {
            if (!connections_.count(peer) && config_.transport == "loopback" && !sealed_ && kind == 1 && connections_.size() < kMaxConnections) {
                Connection connection; connection.started = connection.received = frame_; connections_[peer] = connection;
            }
            if (!connections_.count(peer)) return;
            Connection& connection = connections_.at(peer);
            connection.nonce = nonce;
            SendControl(peer, 6, connection);
            connection.phase = "reject";
            connection.closing = frame_;
        }
        return;
    }
    if (host_ && kind == 1) {
        auto it = connections_.find(peer);
        if (it == connections_.end()) {
            // Native peer ids are allocated by bounded TCP accept, never by UDP packets.
            if (sealed_ || config_.transport != "loopback" || connections_.size() >= kMaxConnections * std::max<size_t>(1, worlds_.size())) { ++rejected_; return; }
            it = connections_.emplace(peer, Connection{}).first;
            // Both clocks start now: a host that has run for a while must not see a new loopback peer as silent since frame 0.
            it->second.started = it->second.received = frame_;
        }
        Connection& connection = it->second;
        if (connection.nonce && connection.nonce != nonce) { ++rejected_; return; }
        if (!connection.nonce) {
            connection.nonce = nonce; connection.name = name; connection.udpPort = port;
            if (!Entropy(connection.cookieA) || !Entropy(connection.cookieB)) { Drop(peer, "platform entropy unavailable"); return; }
        }
        SendControl(peer, connection.player ? 4 : 2, connection);
        return;
    }
    auto it = connections_.find(peer);
    if (it == connections_.end()) { ++rejected_; return; }
    Connection& connection = it->second;
    if (nonce != connection.nonce) { ++rejected_; return; }
    if (!host_ && kind == 2 && connection.phase == "hello" && cookieA && cookieB) {
        connection.cookieA = cookieA; connection.cookieB = cookieB; connection.udpPort = port;
        connection.phase = "response"; connection.hasSent = false;
        SendControl(peer, 3, connection);
    } else if (cookieA != connection.cookieA || cookieB != connection.cookieB) { ++rejected_; }
    else if (host_ && kind == 3 && connection.phase == "challenge") {
        if (nextPlayer_ >= kOthers || (config_.mode == "authoritative" && nextPlayer_ > static_cast<uint32_t>(INT32_MAX))) { Drop(peer, "player ids exhausted; restart host"); return; }
        // A newcomer is in world 0, and each world holds maxPlayers - 1 players.
        if (WorldLoad(0) + 1 >= config_.maxPlayers) { Drop(peer, "lobby is full"); return; }
        connection.player = nextPlayer_++;
        connection.phase = "welcome"; connection.hasSent = false;
        RegisterPeer(peer, connection);
        SendControl(peer, 4, connection);
    } else if (!host_ && kind == 4 && player >= 2 && player < kOthers) {
        if (connection.phase == "response") {
            connection.player = player; localPlayer_ = player; seed_ = seed;
            RegisterPeer(peer, connection);
            connection.phase = "active";
            SetState("lobby");
        }
        if (connection.phase == "active" && connection.player == player) SendControl(peer, 5, connection);
    } else if (host_ && kind == 5 && connection.phase == "welcome" && connection.player == player) {
        connection.phase = "active";
        players_[player] = {connection.name, false};
        Emit({SessionEvent::Type::Joined, player});
        BroadcastRoster();
    } else if (!host_ && kind == 6) { SetState("error", "host rejected session; match protocol/game/mode/tick settings"); }
}

bool Session::Send(PeerId peer, const uint8_t* bytes, size_t size) {
    auto it = connections_.find(peer);
    if (it == connections_.end() || !it->second.player || size > kNetPacketBytes) return false;
    ByteWriter writer(1200);
    writer.WriteU32(kEnvelope); writer.WriteU64(it->second.cookieA); writer.WriteU64(it->second.cookieB);
    writer.WriteBlob(bytes, size);
    if (!writer.Ok()) return false;
    // Unproven UDP never carries control/handshake data; raw control always stays on TCP.
    if (fallback_ && it->second.udpPort) return fallback_->Send(peer, writer.Data().data(), writer.Data().size());
    return wire_->Send(peer, writer.Data().data(), writer.Data().size());
}

bool Session::Poll(uint64_t frame, std::vector<TransportEvent>& events) {
    std::vector<TransportEvent> incoming;
    bool ok = fallback_ ? fallback_->Poll(frame, incoming) : wire_->Poll(frame, incoming);
    if (!ok) return false;
    std::map<PeerId, size_t> controls;
    for (const auto& event : incoming) {
        if (event.type == TransportEvent::Type::Connected) {
            if (host_) {
                if (connections_.size() >= kMaxConnections * std::max<size_t>(1, worlds_.size()) || (state_ != "lobby" || sealed_)) { if (tcp_) tcp_->Disconnect(event.peer); if (webServer_) webServer_->Disconnect(event.peer); continue; }
                Connection connection; connection.started = frame_; connection.received = frame_;
                connections_.emplace(event.peer, connection);
            }
            continue;
        }
        if (event.type == TransportEvent::Type::Disconnected) { Drop(event.peer, event.error.empty() ? "connection closed" : event.error); continue; }
        ByteReader reader(event.bytes.data(), event.bytes.size());
        uint32_t magic = 0;
        if (!reader.ReadU32(magic)) { ++rejected_; continue; }
        if (magic == kControl) {
            if (event.datagram || ++controls[event.peer] > 8) ++rejected_; else Control(event.peer, event.bytes);
            continue;
        }
        auto it = connections_.find(event.peer);
        uint64_t a = 0, b = 0;
        std::vector<uint8_t> bytes;
        if (magic != kEnvelope || it == connections_.end() || !it->second.player || !reader.ReadU64(a) || !reader.ReadU64(b) ||
            a != it->second.cookieA || b != it->second.cookieB || !reader.ReadBlob(bytes, kNetPacketBytes) || reader.Remaining()) {
            ++rejected_; continue;
        }
        it->second.received = frame_;
        events.push_back({event.peer, std::move(bytes)});
    }
    return true;
}

void Session::Drop(PeerId peer, const std::string& reason) {
    auto it = connections_.find(peer);
    if (it == connections_.end()) return;
    uint32_t player = it->second.player;
    if (players_.erase(player)) Emit({SessionEvent::Type::Left, player}, it->second.world);
    channels_.RemovePeer(peer);
    if (udp_) udp_->RemovePeer(peer);
    if (fallback_) fallback_->RemovePeer(peer);
    if (tcp_) tcp_->Disconnect(peer);
    if (web_) web_->Disconnect(peer);
    if (webServer_) webServer_->Disconnect(peer);
    connections_.erase(it);
    if (!host_) {
        for (const auto& item : players_) Emit({SessionEvent::Type::Left, item.first});
        players_.clear(); localPlayer_ = 0;
        SetState(state_ == "leaving" ? "offline" : "error", state_ == "leaving" ? "" : reason);
    } else if (state_ == "lobby") BroadcastRoster();
}

bool Session::SendMessage(PeerId peer, uint8_t kind, uint32_t origin, uint32_t target, uint64_t sequence,
                          const std::string& name, const Json& args) {
    ByteWriter writer(9000);
    writer.WriteU8(kind); writer.WriteU32(origin); writer.WriteU32(target); writer.WriteU64(sequence);
    String(writer, name); String(writer, args.dump());
    return writer.Ok() && channels_.Send(peer, NetChannel::ReliableOrdered, writer.Data().data(), writer.Data().size());
}

void Session::BroadcastRoster() {
    // Each player is told who is in its world (and the host).
    std::map<uint32_t, Json> rosters;
    for (const auto& item : players_) {
        Json player = Json::MakeObject(); player["id"] = item.first; player["name"] = item.second.name; player["ready"] = item.second.ready;
        for (uint32_t world = 0; world < std::max<size_t>(1, worlds_.size()); ++world) {
            if (item.first != 1 && item.second.world != world) continue;
            if (!rosters.count(world)) rosters[world] = Json::MakeArray();
            rosters[world].push(player);
        }
    }
    for (const auto& item : connections_)
        if (item.second.phase == "active" && !SendMessage(item.first, 2, 1, 0, 0, "", rosters[item.second.world])) {
            // A slow peer must not block others; close it on the next Advance pass.
            connections_.at(item.first).phase = "overflow";
        }
}

bool Session::RouteRpc(uint32_t world, uint32_t origin, uint32_t target, uint64_t sequence, const std::string& name, const Json& args) {
    auto selected = [&](uint32_t player) { return target == kAll || (target == kOthers && player != origin) ||
        (target == 0 && player == 1) || target == player; };
    // Only the players of the world the call comes from are reached.
    auto reached = [&](const Connection& connection) { return connection.phase == "active" && connection.world == world && selected(connection.player); };
    // Check all destinations before queuing any, keeping the command atomic under backpressure.
    for (const auto& item : connections_) if (reached(item.second) &&
        channels_.PendingMessages(item.first) >= ChannelEndpoint::kMaxQueuedMessages) return false;
    if (selected(localPlayer_)) {
        if (events_[world].size() >= 256) return false;
        Emit({SessionEvent::Type::Rpc, origin, sequence, name, args}, world);
    }
    for (const auto& item : connections_) if (reached(item.second))
        if (!SendMessage(item.first, 1, origin, target, sequence, name, args)) return false;
    return true;
}

void Session::ApplyMessage(const ChannelEvent& event) {
    auto it = connections_.find(event.peer);
    if (it == connections_.end()) return;
    if (event.type != ChannelEvent::Type::Message) { Drop(event.peer, "channel disconnected or reliable send timed out"); return; }
    if (!event.bytes.empty() && event.bytes[0] == 7) {
        if (it->second.phase != "active" || event.bytes.size() < 3 || event.bytes.size() > 60003 || syncCount_ >= 256 * std::max<size_t>(1, worlds_.size()) ||
            syncBytes_ + event.bytes.size() > 4 * 1024 * 1024) { ++rejected_; return; }
        // Sent for the world the player was in before it was moved: that match is over for it.
        if (static_cast<uint16_t>(event.bytes[1] | (event.bytes[2] << 8)) != it->second.moves) return;
        uint32_t sender = host_ ? it->second.player : 1;
        syncMessages_[host_ ? it->second.world : 0].emplace_back(sender, std::vector<uint8_t>(event.bytes.begin() + 3, event.bytes.end()));
        syncBytes_ += event.bytes.size(); ++syncCount_; return;
    }
    ByteReader reader(event.bytes.data(), event.bytes.size());
    uint8_t kind = 0; uint32_t origin = 0, target = 0; uint64_t sequence = 0;
    std::string name, text, parseError;
    if (!reader.ReadU8(kind) || !reader.ReadU32(origin) || !reader.ReadU32(target) || !reader.ReadU64(sequence) ||
        !String(reader, name, 64) || !String(reader, text, 8192) || reader.Remaining() || !JsonDepth(text)) { ++rejected_; return; }
    Json args = Json::parse(text, &parseError);
    if (!parseError.empty()) { ++rejected_; return; }
    Connection& connection = it->second;
    if (connection.phase != "active" && kind != 5) { ++rejected_; return; }
    if (kind == 1) {
        if (sealed_ && config_.mode != "authoritative") { ++rejected_; return; }
        if (!ValidRpc(name, args) || !sequence || (target != 0 && target != kAll && target != kOthers && !players_.count(target))) { ++rejected_; return; }
        if (host_) {
            if (origin != connection.player || sequence <= connection.rpcSequence || !RouteRpc(connection.world, origin, target, sequence, name, args)) { ++rejected_; }
            else connection.rpcSequence = sequence;
        } else if (players_.count(origin) && sequence > rpcSequences_[origin] && events_[0].size() < 256) {
            rpcSequences_[origin] = sequence;
            Emit({SessionEvent::Type::Rpc, origin, sequence, name, args});
        }
        else ++rejected_;
    } else if (kind == 2 && !host_) {
        if (!args.isArray() || args.size() > config_.maxPlayers) { ++rejected_; return; }
        std::map<uint32_t, Player> players;
        for (const auto& item : args.items()) {
            uint32_t id = 0;
            if (!Integer(item, "id", 0, 1, kOthers - 1, id) || !Text(item["name"].asString(), 64) ||
                !item["ready"].isBool() || players.count(id)) { ++rejected_; return; }
            players[id] = {item["name"].asString(), item["ready"].asBool()};
        }
        if (!players.count(1) || !players.count(localPlayer_)) { ++rejected_; return; }
        for (const auto& item : players_) if (!players.count(item.first)) Emit({SessionEvent::Type::Left, item.first});
        for (const auto& item : players) if (!players_.count(item.first)) Emit({SessionEvent::Type::Joined, item.first});
        players_ = std::move(players);
        for (auto sequenceIt = rpcSequences_.begin(); sequenceIt != rpcSequences_.end();) {
            if (!players_.count(sequenceIt->first)) sequenceIt = rpcSequences_.erase(sequenceIt); else ++sequenceIt;
        }
    } else if (kind == 3 && host_ && !sealed_ && args.isBool()) {
        players_.at(connection.player).ready = args.asBool(); BroadcastRoster();
    } else if (kind == 4 && host_) {
        std::string ignored; Kick(connection.player, "left", &ignored);
    } else if (kind == 5 && !host_) {
        SetState("leaving"); Drop(event.peer, name);
    } else if (kind == 8 && !host_ && origin == 1 && target == localPlayer_ && args.isString() && Text(args.asString(), 256) && sequence && sequence <= 0xffff) {
        // Moved to another world: what the old one still sent is dropped, and the lobby is open again
        // for our Ready (the scene is loaded).
        connection.moves = static_cast<uint16_t>(sequence);
        syncMessages_.clear(); syncBytes_ = 0; syncCount_ = 0; sealed_ = false;
        worldChange_ = args.asString(); worldChanged_ = true;
    } else if (kind != 6) ++rejected_;
}

bool Session::SendSync(uint32_t player, const std::vector<uint8_t>& bytes, bool reliable) {
    if (!Connected() || bytes.empty() || bytes.size() > 60000 || player == localPlayer_) return false;
    for (const auto& connection : connections_) {
        if (connection.second.phase != "active" || (host_ ? connection.second.player != player : player != 1)) continue;
        std::vector<uint8_t> payload{7, static_cast<uint8_t>(connection.second.moves & 0xff), static_cast<uint8_t>(connection.second.moves >> 8)};
        payload.insert(payload.end(), bytes.begin(), bytes.end());
        return channels_.Send(connection.first, reliable ? NetChannel::ReliableOrdered : NetChannel::Unreliable, payload.data(), payload.size());
    }
    return false;
}
std::vector<std::pair<uint32_t, std::vector<uint8_t>>> Session::DrainSync(uint32_t world) {
    auto it = syncMessages_.find(world);
    if (it == syncMessages_.end()) return {};
    auto messages = std::move(it->second); syncMessages_.erase(it);
    for (const auto& message : messages) { syncBytes_ -= std::min(syncBytes_, message.second.size() + 3); --syncCount_; }
    return messages;
}

bool Session::Advance(uint64_t frame) {
    if (polled_ && frame < frame_) return false;
    if (polled_ && frame == frame_) return true;
    ++g_polls; frame_ = frame; polled_ = true;
    if (state_ == "offline" || state_ == "error") { CloseTransport(); return true; }
    std::vector<ChannelEvent> incoming;
    if (!channels_.Poll(frame, incoming)) { SetState("error", "transport poll failed"); CloseTransport(); return false; }
    if (state_ == "error") {
        std::string reason = error_;
        std::vector<PeerId> peers; for (const auto& item : connections_) peers.push_back(item.first);
        for (PeerId peer : peers) Drop(peer, reason);
        CloseTransport();
        return true;
    }
    std::sort(incoming.begin(), incoming.end(), [&](const ChannelEvent& a, const ChannelEvent& b) {
        auto player = [&](PeerId peer) { auto it = connections_.find(peer); return it == connections_.end() ? 0u : it->second.player; };
        return std::make_pair(player(a.peer), a.sequence) < std::make_pair(player(b.peer), b.sequence);
    });
    for (const auto& event : incoming) ApplyMessage(event);
    std::vector<std::pair<PeerId, std::string>> drop;
    for (auto& item : connections_) {
        Connection& connection = item.second;
        if (connection.phase == "reject") {
            if (frame_ - connection.closing >= 6) drop.emplace_back(item.first, "session mismatch");
        } else if (connection.phase == "closing") {
            if (!channels_.PendingMessages(item.first) || frame_ - connection.closing >= 30) drop.emplace_back(item.first, "left");
        } else if (connection.phase == "overflow" || frame_ - connection.received >= config_.timeoutFrames ||
                   (connection.phase != "active" && frame_ - connection.started >= 300)) drop.emplace_back(item.first, "session timeout or queue overflow");
        else if (connection.phase != "active") {
            if (!connection.hasSent || frame_ - connection.sent >= 6) {
                if (host_ && connection.nonce) SendControl(item.first, connection.player ? 4 : 2, connection);
                else if (!host_) SendControl(item.first, connection.phase == "hello" ? 1 : 3, connection);
                connection.sent = frame_; connection.hasSent = true;
            }
        } else if (frame_ % 60 == 0) SendMessage(item.first, 6, localPlayer_, 0, 0, "", Json());
    }
    for (const auto& item : drop) Drop(item.first, item.second);
    if (state_ == "leaving" && (connections_.empty() || frame_ - started_ >= 30)) {
        std::vector<PeerId> peers; for (const auto& item : connections_) peers.push_back(item.first);
        for (PeerId peer : peers) Drop(peer, "left");
        players_.clear(); localPlayer_ = 0; SetState("offline");
    }
    if (state_ == "offline" || state_ == "error") CloseTransport();
    return true;
}

std::vector<SessionEvent> Session::DrainEvents(uint32_t world) {
    std::vector<SessionEvent> events;
    events.swap(events_[world]);
    std::stable_sort(events.begin(), events.end(), [](const SessionEvent& a, const SessionEvent& b) {
        return std::make_pair(a.player, a.sequence) < std::make_pair(b.player, b.sequence);
    });
    return events;
}
Json Session::Players() const {
    Json players = Json::MakeArray();
    for (const auto& item : players_) {
        Json player = Json::MakeObject(); player["id"] = item.first; player["name"] = item.second.name;
        player["ready"] = item.second.ready; player["local"] = item.first == localPlayer_; players.push(std::move(player));
    }
    return players;
}
Json Session::Players(uint32_t world) const {
    Json players = Json::MakeArray();
    for (const auto& item : players_) {
        if (item.first != 1 && item.second.world != world) continue;
        Json player = Json::MakeObject(); player["id"] = item.first; player["name"] = item.second.name;
        player["ready"] = item.second.ready; player["local"] = item.first == localPlayer_; players.push(std::move(player));
    }
    return players;
}
Json Session::State() const {
    Json state = Json::MakeObject(); state["mode"] = config_.mode; state["state"] = state_; state["transport"] = config_.transport;
    state["isHost"] = host_; state["isServer"] = host_; state["isClient"] = !host_; state["localPlayer"] = localPlayer_;
    state["seed"] = seed_; state["tickRate"] = config_.tickRate; state["maxPlayers"] = config_.maxPlayers;
    state["port"] = tcp_ ? tcp_->LocalAddress().port : webServer_ ? webServer_->LocalAddress().port : 0; state["room"] = room_; state["error"] = error_;
    state["syncImplemented"] = true;
    if (!worlds_.empty()) { state["worlds"] = Json::MakeArray(); for (const auto& scene : worlds_) state["worlds"].push(scene); }
    return state;
}
bool Session::Simulate(const LoopbackConfig& config, std::string* error) {
    if (!loopback_) { if (error) *error = "fault injection requires an active loopback room"; return false; }
    loopback_->Configure(config); return true;
}
Json Session::Simulation() const {
    Json out = Json::MakeObject(); out["enabled"] = loopback_ != nullptr;
    if (!loopback_) return out;
    const auto& c = loopback_->Configuration();
    out["seed"] = c.seed; out["latencyFrames"] = c.latencyFrames; out["jitterFrames"] = c.jitterFrames;
    out["lossPermille"] = c.lossPermille; out["duplicatePermille"] = c.duplicatePermille; out["reorderFrames"] = c.reorderFrames;
    out["pendingMessages"] = static_cast<uint64_t>(loopback_->PendingMessages());
    out["pendingBytes"] = static_cast<uint64_t>(loopback_->PendingBytes()); out["dropped"] = loopback_->DroppedMessages(); return out;
}
Json Session::Stats() const {
    Json stats = Json::MakeObject(), peers = Json::MakeArray(); stats["rejected"] = rejected_;
    for (const auto& item : connections_) {
        const ChannelStats* source = channels_.Stats(item.first);
        Json peer = Json::MakeObject(); peer["player"] = item.second.player; peer["phase"] = item.second.phase;
        peer["rttMs"] = source ? source->rttFrames * 1000.0 / config_.tickRate : 0;
        peer["lossPermille"] = source ? source->LossPermille() : 0;
        peer["bytesSent"] = source ? source->bytesSent : 0; peer["bytesReceived"] = source ? source->bytesReceived : 0;
        peer["pending"] = static_cast<uint64_t>(channels_.PendingMessages(item.first));
        peer["route"] = fallback_ && fallback_->Selected(item.first) == FallbackTransport::Route::Udp ? "udp" : "tcp";
        if (config_.transport == "loopback" || config_.transport == "websocket") peer["route"] = config_.transport;
        peers.push(std::move(peer));
    }
    stats["peers"] = std::move(peers); stats["simulation"] = Simulation(); return stats;
}
void Session::Leave() {
    if (state_ == "offline" || state_ == "leaving") return;
    if (state_ == "error") { SetState("offline"); CloseTransport(); return; }
    SetState("leaving"); started_ = frame_;
    for (auto& item : connections_) {
        SendMessage(item.first, host_ ? 5 : 4, localPlayer_, 0, 0, "left", Json());
        item.second.phase = "closing"; item.second.closing = frame_;
    }
}
bool Session::Kick(uint32_t player, const std::string& reason, std::string* error) {
    if (!host_ || player == 1 || !players_.count(player) || !Text(reason, 64)) {
        if (error) *error = "only the host can kick an existing remote player; reason is 1..64 printable bytes";
        return false;
    }
    for (auto& item : connections_) if (item.second.player == player) {
        // An unresponsive peer cannot be told; close it on the next Advance instead of failing.
        if (!SendMessage(item.first, 5, 1, player, 0, reason, Json())) { item.second.phase = "overflow"; continue; }
        item.second.phase = "closing"; item.second.closing = frame_;
    }
    const uint32_t world = WorldOf(player);
    players_.erase(player); Emit({SessionEvent::Type::Left, player}, world); BroadcastRoster(); return true;
}
bool Session::Ready(bool ready, std::string* error) {
    if (!Connected() || sealed_) { if (error) *error = "join an unsealed lobby first"; return false; }
    if (host_) { players_.at(1).ready = ready; BroadcastRoster(); return true; }
    if (SendMessage(server_, 3, localPlayer_, 0, 0, "", Json(ready))) return true;
    if (error) *error = "peer send queue is full";
    return false;
}
bool Session::Rpc(uint32_t world, const std::string& target, const std::string& name, const Json& args, std::string* error) {
    auto fail = [&](const char* message) { if (error) *error = message; return false; };
    if (!Connected() || (sealed_ && config_.mode != "authoritative")) return fail("RPC requires a lobby or authoritative match; use synchronized input during lockstep/rollback matches");
    if (!ValidRpc(name, args)) return fail("RPC requires a name of 1..64 bytes and at most 16 bounded JSON arguments (8 KiB)");
    uint32_t destination = 0;
    if (target == "all") destination = kAll;
    else if (target == "others") destination = kOthers;
    else if (target != "server") {
        uint64_t value = 0;
        if (target.empty() || target.size() > 10) return fail("target must be server, all, others or an existing player id; Engine resolves owner entities");
        for (char c : target) { if (c < '0' || c > '9') return fail("target must be server, all, others or an existing player id; Engine resolves owner entities"); value = value * 10 + static_cast<unsigned>(c - '0'); }
        if (value >= kOthers || !players_.count(static_cast<uint32_t>(value)) || (host_ && value != 1 && WorldOf(static_cast<uint32_t>(value)) != world))
            return fail("target player is not in this lobby");
        destination = static_cast<uint32_t>(value);
    }
    if (sequence_ == std::numeric_limits<uint64_t>::max()) return fail("RPC sequence exhausted; reconnect");
    uint64_t sequence = sequence_ + 1;
    bool ok = host_ ? RouteRpc(world, localPlayer_, destination, sequence, name, args) : SendMessage(server_, 1, localPlayer_, destination, sequence, name, args);
    if (!ok) return fail("RPC send/event queue is full; advance simulation and retry");
    sequence_ = sequence; return true;
}
}  // namespace oe
