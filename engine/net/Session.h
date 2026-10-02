#pragma once
#include <functional>
#include <map>
#include <memory>

#include "core/Json.h"
#include "net/Channels.h"
#include "net/SocketTransports.h"
#include "net/FallbackTransport.h"

namespace oe {

// M4 establishes a lobby; frame synchronization/replication are separate M5/M6 layers.
struct SessionConfig {
    std::string mode = "none", transport = "tcp", bind = "127.0.0.1", gameId;
    uint16_t port = 7778, version = 1;
    uint32_t maxPlayers = 4, tickRate = 60;
    // Validates all supplied settings, including unsupported rollback and fixed tick rate.
    static bool Parse(const Json& project, SessionConfig& out, std::string* error);
};

// Lobby/player and RPC notifications drained once at a simulation boundary.
struct SessionEvent {
    enum class Type { State, Joined, Left, Rpc };
    Type type = Type::State;
    uint32_t player = 0;
    uint64_t sequence = 0;
    std::string name;
    Json args = Json::MakeArray();
};

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
    std::vector<SessionEvent> DrainEvents();
    Json State() const;
    Json Players() const;
    Json Stats() const;
    bool IsHost() const { return host_; }
    bool Connected() const { return state_ == "lobby"; }
    uint32_t LocalPlayer() const { return localPlayer_; }
    uint32_t Seed() const { return seed_; }
    const std::string& Status() const { return state_; }
    // Graceful bounded shutdown; Stop/project load may destroy immediately.
    void Leave();
    bool Kick(uint32_t player, const std::string& reason, std::string* error);
    bool Ready(bool ready, std::string* error);
    // Targets: server/all/others or decimal player id; owner requires M6 and is rejected.
    bool Rpc(const std::string& target, const std::string& name, const Json& args, std::string* error);
    static uint64_t InstancesCreated();
    static uint64_t PollCalls();
private:
    struct Player { std::string name; bool ready = false; };
    struct Connection {
        uint64_t nonce = 0, cookieA = 0, cookieB = 0, started = 0, received = 0, sent = 0, closing = 0;
        uint32_t player = 0;
        uint64_t rpcSequence = 0;
        uint16_t udpPort = 0;
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
    bool RouteRpc(uint32_t origin, uint32_t target, uint64_t sequence, const std::string& name, const Json& args);
    void SetState(const std::string& state, const std::string& error = "");
    void Emit(SessionEvent event);  // bounded observer queue (256); a full observer never stalls I/O
    void CloseTransport();  // releases listeners/endpoints as soon as the terminal state is observed
    SessionConfig config_;
    std::unique_ptr<ITransport> wire_;
    TcpTransport* tcp_ = nullptr;
    WebSocketTransport* web_ = nullptr;
    std::unique_ptr<UdpTransport> udp_;
    std::unique_ptr<FallbackTransport> fallback_;
    ChannelEndpoint channels_;
    Random random_;
    std::map<PeerId, Connection> connections_;
    std::map<uint32_t, Player> players_;
    std::vector<SessionEvent> events_;
    std::map<uint32_t, uint64_t> rpcSequences_;
    std::string state_, error_, name_, room_;
    PeerId server_ = 1;
    uint32_t localPlayer_ = 0, nextPlayer_ = 2, seed_ = 0;
    uint64_t frame_ = 0, sequence_ = 0, rejected_ = 0, started_ = 0;
    bool host_ = false, polled_ = false;
};

// Bounded JSON values for RPC: 16 arguments, depth 8, finite numbers, 8 KiB serialized.
bool ValidRpc(const std::string& name, const Json& args);

}  // namespace oe
