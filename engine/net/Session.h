#pragma once
#include <functional>
#include <map>
#include <memory>

#include "core/Json.h"
#include "net/Channels.h"
#include "net/LoopbackTransport.h"
#include "net/sync/FrameSync.h"
#include "net/SocketTransports.h"
#include "net/WebSocketServer.h"
#include "net/FallbackTransport.h"

namespace oe {

// Authenticated lobby/RPC and match payload transport; FrameSync owns M5 synchronization.
struct SessionConfig {
    std::string mode = "none", transport = "tcp", bind = "127.0.0.1", gameId;
    uint16_t port = 7778, version = 5;
    SyncConfig sync;
    uint32_t maxPlayers = 4, tickRate = 60, snapshotRate = 20, interpolationFrames = 6, predictionFrames = 32;
    std::string playerPrefab;
    // Authoritative only: the host keeps its lobby open during the match and admits ready players into
    // the running world; a dedicated server starts at once and keeps running without players.
    bool lateJoin = false;
    // With lateJoin: "auto" admits every ready lobby player; "script" waits until the game called
    // net.admit(player), so a server can check who it is first (a ticket, a ban list).
    std::string admission = "auto";
    // network.timeoutSeconds (5..120, default 30): a peer silent this long, or a reliable message
    // unacknowledged this long, is dropped. Long enough for a stall (a window held still, a loading
    // hitch, a debugger) to recover; a client that is really gone frees its slot after it.
    uint32_t timeoutFrames = 30 * 60;
    // Authoritative clients, network.prediction: "full" (default) restores the whole world to the
    // acknowledged frame and re-simulates every pending frame on a correction; "owned" sets only the
    // client's own predicted entities to the server's state and re-runs only their scripts and
    // CharacterBody movement. "owned" is far cheaper; state that prediction depends on must then
    // live in components (scripts' own variables are not rewound).
    std::string prediction = "full";
    // Validates all supplied settings, including the bounded input schema and fixed tick rate.
    static bool Parse(const Json& project, SessionConfig& out, std::string* error);
};

// Lobby/player and RPC notifications drained once at a simulation boundary.
struct SessionEvent {
    enum class Type { State, Joined, Left, Rpc };
    Type type = Type::State;
    uint32_t player = 0;
    uint64_t sequence = 0;
    std::string name;
    Json args = Json::MakeArray();  // Rpc: the arguments. Joined: {data} when the player was moved here with data
};

// Worlds (authoritative hosts with lateJoin): one session, one port, several worlds, each simulated by
// its own engine. Every player is in one world at a time: it sees that world's roster, its match
// payloads and RPC reach only that world, and events are queued per world. A new connection is in
// world 0. Move puts a player into another world while its connection stays: the old world sees it
// leave, the new one sees it join (not ready), and the player's side is told which scene to load
// (TakeWorldChange). Match payloads carry the connection's move count, so what was still on the wire
// for the world it left is dropped on both sides. A session without SetWorlds has the one world 0.

class Session final : private ITransport {
public:
    using Random = std::function<bool(uint8_t*, size_t)>;
    // Creates sockets only on explicit host/join. Loopback rooms live only while their host lives.
    static std::unique_ptr<Session> Host(const SessionConfig& config, const std::string& name,
                                         uint32_t seed, const std::string& room, std::string* error);
    static std::unique_ptr<Session> Join(const SessionConfig& config, const std::string& name,
                                         const std::string& address, uint16_t port, std::string* error);
    // Injectable transport/entropy for deterministic headless tests. Wire must preserve peer ids.
    Session(const SessionConfig& config, std::unique_ptr<ITransport> wire, bool host, PeerId server,
            const std::string& name, uint32_t seed, Random random);
    ~Session() override;
    // Poll owns the transport, applies bounded messages and orders events by player/sequence.
    bool Advance(uint64_t frame);
    // Authenticated bounded match payloads; only host/client links are permitted.
    // reliable=false uses the unreliable channel: no retransmission, ordering or head-of-line
    // delay; a full queue simply drops the payload (authoritative snapshots and their ACKs).
    bool SendSync(uint32_t player, const std::vector<uint8_t>& bytes, bool reliable = true);
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> DrainSync(uint32_t world = 0);
    void Seal() { sealed_ = true; }
    std::vector<SessionEvent> DrainEvents(uint32_t world = 0);
    Json State() const;
    Json Players() const;
    // The roster of one world: the host and the players that are in it.
    Json Players(uint32_t world) const;
    // Host: the worlds of this session, by the scene each runs (index = world). Before anybody joins.
    bool SetWorlds(const std::vector<std::string>& scenes, std::string* error);
    const std::vector<std::string>& Worlds() const { return worlds_; }
    // Host: puts a connected player into the world that runs `scene`; `data` (bounded like RPC
    // arguments) is handed to that world with its Joined event. Each world holds maxPlayers - 1 players.
    bool Move(uint32_t player, const std::string& scene, const Json& data, std::string* error);
    // Client: the scene of the world the host moved us to, once per move. The match of the world we
    // were in is over for us; Ready says the scene is loaded.
    bool TakeWorldChange(std::string& scene);
    Json Stats() const;
    // Fault controls are shared by all endpoints in an explicit loopback room.
    bool Simulate(const LoopbackConfig& config, std::string* error);
    Json Simulation() const;
    bool IsHost() const { return host_; }
    bool Connected() const { return state_ == "lobby"; }
    uint32_t LocalPlayer() const { return localPlayer_; }
    uint32_t Seed() const { return seed_; }
    const std::string& Status() const { return state_; }
    // Graceful bounded shutdown; Stop/project load may destroy immediately.
    void Leave();
    bool Kick(uint32_t player, const std::string& reason, std::string* error);
    bool Ready(bool ready, std::string* error);
    // Targets: server/all/others or decimal player id; owner is resolved by Engine from NetSync before routing.
    bool Rpc(const std::string& target, const std::string& name, const Json& args, std::string* error) { return Rpc(0, target, name, args, error); }
    // From the host's world `world`: all/others/a player id mean the players of that world.
    bool Rpc(uint32_t world, const std::string& target, const std::string& name, const Json& args, std::string* error);
    static uint64_t InstancesCreated();
    static uint64_t PollCalls();
private:
    struct Player { std::string name; bool ready = false; uint32_t world = 0; };
    struct Connection {
        uint64_t nonce = 0, cookieA = 0, cookieB = 0, started = 0, received = 0, sent = 0, closing = 0;
        uint32_t player = 0, world = 0;
        uint64_t rpcSequence = 0;
        uint16_t udpPort = 0, moves = 0;  // moves: how often the player changed world; stamps its match payloads
        std::string name, phase = "challenge";
        bool hasSent = false;
    };
    bool Send(PeerId peer, const uint8_t* bytes, size_t size) override;
    bool Poll(uint64_t frame, std::vector<TransportEvent>& events) override;
    bool Entropy(uint64_t& value);
    void Control(PeerId peer, const std::vector<uint8_t>& bytes);
    void SendControl(PeerId peer, uint8_t kind, const Connection& connection);
    void RegisterPeer(PeerId peer, Connection& connection);
    void Drop(PeerId peer, const std::string& reason);
    void BroadcastRoster();
    void ApplyMessage(const ChannelEvent& event);
    bool SendMessage(PeerId peer, uint8_t kind, uint32_t origin, uint32_t target,
                     uint64_t sequence, const std::string& name, const Json& args);
    bool RouteRpc(uint32_t world, uint32_t origin, uint32_t target, uint64_t sequence, const std::string& name, const Json& args);
    void SetState(const std::string& state, const std::string& error = "");
    // Bounded observer queue (256 per world); a full observer never stalls I/O. kEveryWorld: all of them.
    static constexpr uint32_t kEveryWorld = 0xffffffff;
    void Emit(SessionEvent event, uint32_t world = 0);
    uint32_t WorldOf(uint32_t player) const;
    size_t WorldLoad(uint32_t world) const;  // players in it, and connections on their way in
    void CloseTransport();  // releases listeners/endpoints as soon as the terminal state is observed
    SessionConfig config_;
    std::unique_ptr<ITransport> wire_;
    std::shared_ptr<LoopbackNetwork> loopback_;
    TcpTransport* tcp_ = nullptr;
    WebSocketServerTransport* webServer_ = nullptr;
    WebSocketTransport* web_ = nullptr;
    std::unique_ptr<UdpTransport> udp_;
    std::unique_ptr<FallbackTransport> fallback_;
    ChannelEndpoint channels_;
    Random random_;
    std::map<PeerId, Connection> connections_;
    std::map<uint32_t, Player> players_;
    std::map<uint32_t, std::vector<SessionEvent>> events_;  // by world
    std::vector<std::string> worlds_;                       // host: scene of each world (empty: the one world 0)
    std::string worldChange_;                               // client: the scene of the world we were moved to
    bool worldChanged_ = false;
    std::map<uint32_t, uint64_t> rpcSequences_;
    std::string state_, error_, name_, room_;
    PeerId server_ = 1;
    uint32_t localPlayer_ = 0, nextPlayer_ = 2, seed_ = 0;
    uint64_t frame_ = 0, sequence_ = 0, rejected_ = 0, started_ = 0;
    bool host_ = false, polled_ = false, sealed_ = false;
    std::map<uint32_t, std::vector<std::pair<uint32_t, std::vector<uint8_t>>>> syncMessages_;  // by world
    size_t syncBytes_ = 0, syncCount_ = 0;
};

// Bounded JSON values for RPC: 16 arguments, depth 8, finite numbers, 8 KiB serialized.
bool ValidRpc(const std::string& name, const Json& args);

}  // namespace oe
