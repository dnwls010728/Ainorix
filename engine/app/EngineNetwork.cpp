#include "app/Engine.h"

#include <cmath>

#include "script/ScriptHost.h"

namespace oe {
namespace {
uint32_t Number(const Json& args, const char* key, uint32_t fallback, uint32_t low, uint32_t high) {
    double value = args.has(key) ? args[key].asNumber(-1) : static_cast<double>(fallback);
    if (!std::isfinite(value) || value < low || value > high || std::floor(value) != value)
        throw ApiError("invalid_argument", std::string(key) + " is outside its integer range", "Use an integer within the net.* parameter range.");
    return static_cast<uint32_t>(value);
}
}

Json Engine::NetworkCall(const std::string& command, const Json& args) {
    if (((sync_ && sync_->Active()) || (authority_ && authority_->Active())) && inWorld_ &&
        (command == "host" || command == "join" || command == "leave" || command == "kick" || command == "ready" || command == "start"))
        throw ApiError("match_control", "session controls cannot run in a synchronized game callback", "Control the lobby/session from tools; keep match updates deterministic.");
    auto state = [&] {
        if (network_) { Json out = network_->State(); out["syncImplemented"] = true; out["dedicated"] = dedicated_;
            if (dedicated_) out["isHost"] = false;
            if (authority_) out["sync"] = authority_->State();
            if (sync_) { out["sync"] = sync_->State(); if (replaying_) out["sync"]["frame"] = frame_; } return out; }
        Json out = Json::MakeObject(); out["mode"] = networkConfig_.mode;
        out["state"] = networkConfig_.mode == "none" ? "none" : "idle";
        out["isHost"] = false; out["isServer"] = networkConfig_.mode == "none"; out["isClient"] = false;
        out["localPlayer"] = networkConfig_.mode == "none" ? 1 : 0; out["seed"] = 0;
        out["maxPlayers"] = networkConfig_.maxPlayers; out["transport"] = networkConfig_.transport; out["syncImplemented"] = networkConfig_.mode != "none"; out["error"] = "";
        return out;
    };
    if (command == "local_peers") return LocalPeersState();
    if (command == "peer_call") {
        uint32_t index = Number(args, "peer", 0, 0, 7);
        Engine* peer = LocalPeer(index);
        std::string name = args["command"].asString();
        const bool allowed = name.compare(0, 6, "input.") == 0 || name == "sim.state" || name == "net.state" ||
            name == "net.players" || name == "net.stats" || name == "net.entities" || name == "net.desync_report" ||
            name == "entity.get" || name == "scene.get" || name == "game.state" || name == "script.errors" || name == "render.screenshot";
        if (!peer || !allowed || inWorld_) throw ApiError("network_peer", "invalid preview peer or unsupported command",
            "Use net.local_peers indices and input.*, state/diagnostic queries or render.screenshot from tools.");
        return peer->Call(name, args.has("args") ? args["args"] : Json::MakeObject());
    }
    if (command == "simulate") {
        if (!network_) throw ApiError("network_inactive", "fault injection requires an active loopback room", "Use net.spawn_local_peers or host a loopback room.");
        if (args.size() == 0) return network_->Simulation();
        Json previous = network_->Simulation(); LoopbackConfig config;
        config.seed = Number(args, "seed", static_cast<uint32_t>(previous["seed"].asNumber(1)), 0, UINT32_MAX);
        config.latencyFrames = Number(args, "latencyFrames", static_cast<uint32_t>(previous["latencyFrames"].asNumber()), 0, 3600);
        config.jitterFrames = Number(args, "jitterFrames", static_cast<uint32_t>(previous["jitterFrames"].asNumber()), 0, 3600);
        config.lossPermille = Number(args, "lossPermille", static_cast<uint32_t>(previous["lossPermille"].asNumber()), 0, 1000);
        config.duplicatePermille = Number(args, "duplicatePermille", static_cast<uint32_t>(previous["duplicatePermille"].asNumber()), 0, 1000);
        config.reorderFrames = Number(args, "reorderFrames", static_cast<uint32_t>(previous["reorderFrames"].asNumber()), 0, 3600);
        std::string error;
        if (!network_->Simulate(config, &error)) throw ApiError("network_transport", error, "Fault injection supports loopback rooms only.");
        return network_->Simulation();
    }
    if (command == "spawn_local_peers") {
        uint32_t count = Number(args, "count", 1, 1, 7);
        if (inWorld_ || InPlaySession() || network_ || networkConfig_.mode == "none" || count >= networkConfig_.maxPlayers)
            throw ApiError("network_preview", "preview requires an enabled idle project with enough player slots",
                "Use sim.stop first; count is additional peers (1..7), and network.maxPlayers must include the host.");
        // Open and copy the edit-time world before hosting: failures leave no active room.
        std::vector<std::unique_ptr<Engine>> peers; std::string error;
        for (uint32_t i = 0; i < count; ++i) {
            auto peer = std::make_unique<Engine>();
            if (!peer->Open(projectDir_, &error)) throw ApiError("network_preview", error);
            peer->scene_ = scene_; peer->scenePath_ = scenePath_; peers.push_back(std::move(peer));
        }
        static uint64_t roomSerial = 0;
        const std::string room = "preview-" + std::to_string(++roomSerial);
        SessionConfig config = networkConfig_; config.transport = "loopback";
        auto host = Session::Host(config, "Player 1", Number(args, "seed", 1, 0, UINT32_MAX), room, &error);
        if (!host) throw ApiError("network_preview", error);
        for (size_t i = 0; i < peers.size(); ++i) {
            auto& peer = *peers[i]; peer.previewTransport_ = peer.networkConfig_.transport;
            peer.networkConfig_.transport = "loopback";
            peer.network_ = Session::Join(config, "Player " + std::to_string(i + 2), room, 0, &error);
            if (!peer.network_) throw ApiError("network_preview", error);
            peer.Play();
        }
        previewTransport_ = networkConfig_.transport; networkConfig_.transport = "loopback";
        network_ = std::move(host); networkFrame_ = 0; localPeers_ = std::move(peers);
        autoStart_ = true; minimumPlayers_ = count;
        network_->Ready(true, &error); scripts_->SetNetworkSeed(network_->Seed()); Play();
        return LocalPeersState();
    }
    if (command == "serve") {
        if (inWorld_ || InPlaySession()) throw ApiError("network_server", "dedicated serving requires an idle engine", "Use sim.stop first.");
        uint32_t minimum = Number(args, "minPlayers", 1, 1, 63);
        if (minimum >= networkConfig_.maxPlayers) throw ApiError("network_server", "minPlayers exceeds available remote slots", "maxPlayers includes the reserved server slot 1.");
        NetworkCall("host", args);
        dedicated_ = true; autoStart_ = true; minimumPlayers_ = minimum;
        std::string error; network_->Ready(true, &error); Play();
        return NetworkCall("state", Json::MakeObject());
    }
    if (command == "state") return state();
    if (command == "entities") {
        Json out = Json::MakeArray(); if (!authority_ || !authority_->Running()) return out;
        if (network_->IsHost()) {
            for (const auto& item : authorityIds_) {
                Json entity = Json::MakeObject(); entity["netId"] = item.second; entity["id"] = item.first;
                if (const auto* policy = scene_.Get<NetSync>(item.first)) entity["owner"] = policy->owner;
                out.push(entity);
            }
        } else for (const auto& item : replicatedEntities_) {
            Json entity = Json::MakeObject(); entity["netId"] = item.first; entity["id"] = item.second;
            if (const auto* policy = scene_.Get<NetSync>(item.second)) entity["owner"] = policy->owner;
            if (!authorityWorlds_.empty()) entity["fields"] = authorityWorlds_.back().world[std::to_string(item.first)];
            out.push(entity);
        }
        return out;
    }
    if (command == "desync_report") return sync_ ? sync_->Report() : Json::MakeObject();
    if (command == "start") {
        if (!network_ || !network_->Connected() || !network_->IsHost())
            throw ApiError("network_start", "start requires a ready host lobby", "Host, join, and ready all players first.");
        std::vector<uint32_t> roster;
        Json players = network_->Players();
        for (const Json& player : players.items()) {
            if (!player["ready"].asBool()) throw ApiError("network_not_ready", "all players must be ready", "Call net.ready on every peer.");
            roster.push_back(static_cast<uint32_t>(player["id"].asNumber()));
        }
        BeginSessionIfNeeded(); std::string error;
        if (networkConfig_.mode == "authoritative") {
            EnsureAuthority(true); if (!authority_->Start(roster, &error)) throw ApiError("network_start", error);
        } else { EnsureSync(true); if (!sync_->Start(roster, &error)) throw ApiError("network_start", error); }
        network_->Seal(); return state();
    }
    if (command == "players") {
        if (network_) return network_->Players();
        Json players = Json::MakeArray();
        if (networkConfig_.mode == "none") {
            Json player = Json::MakeObject(); player["id"] = 1; player["name"] = "Local";
            player["ready"] = true; player["local"] = true; players.push(player);
        }
        return players;
    }
    if (command == "stats") {
        if (network_) { Json stats = network_->Stats(); stats["replayMilliseconds"] = replayMilliseconds_; if (sync_) stats["sync"] = sync_->State();
            if (authority_) { stats["sync"] = authority_->State(); stats["corrections"] = authorityCorrections_; stats["replicatedEntities"] = static_cast<uint64_t>(replicatedEntities_.size()); } return stats; }
        Json stats = Json::MakeObject(); stats["rejected"] = 0; stats["peers"] = Json::MakeArray(); return stats;
    }
    if (command == "host" || command == "join") {
        if (networkConfig_.mode == "none") throw ApiError("network_disabled", "this project has no enabled network mode",
            "Set project.json network.mode to lockstep, rollback, or authoritative and reopen.");
        if (network_ && network_->Status() != "offline" && network_->Status() != "error")
            throw ApiError("network_active", "a session is already active", "Use net.leave and advance sim.step until offline, or sim.stop first.");
        ResetAuthority(); sync_.reset(); checkpoints_.clear(); journal_.reset(); snapshots_.clear(); playerInputs_.clear(); frameInputs_.clear(); syncHashes_.clear();
        audio_->SetOutputMode(false, false); audio_->DiscardPending(); saves_.DeferFlush(false); saves_.FreezeReads(false);
        ResetLocalPeers(); network_.reset(); networkFrame_ = 0;
        std::string error, name = args["name"].asString("Player");
        if (command == "host") {
            uint32_t seed = Number(args, "seed", 1, 0, UINT32_MAX);
            SessionConfig config = networkConfig_; config.port = static_cast<uint16_t>(Number(args, "port", config.port, 0, 65535));
            network_ = Session::Host(config, name, seed, args["room"].asString(networkConfig_.gameId), &error);
        } else {
            uint16_t port = static_cast<uint16_t>(Number(args, "port", networkConfig_.port, 0, 65535));
            std::string address = args["address"].asString(networkConfig_.transport == "loopback" ? networkConfig_.gameId : "127.0.0.1");
            network_ = Session::Join(networkConfig_, name, address, port, &error);
        }
        if (!network_) throw ApiError("network_connect", error, "Check project network settings and net.state on the host; then advance both sessions with sim.step.");
        scripts_->SetNetworkSeed(network_->Seed());
        return state();
    }
    if (command == "leave") { ResetLocalPeers(); if (network_) network_->Leave(); return state(); }
    std::string error;
    if (command == "rpc") {
        if (authority_ && authority_->Running() && replaying_) {
            Json out = Json::MakeObject(); out["queued"] = false; out["replayed"] = true; return out;
        }
        const Json& values = args["args"];
        Json arguments = values.isNull() ? Json::MakeArray() : values;
        std::string target = args["target"].asString("server"), name = args["name"].asString();
        if (target == "owner") {
            EntityId entity = 0;
            const Json& id = args.has("entity") ? args["entity"] : (arguments.size() ? arguments[0] : Json());
            if (id.isString()) entity = scene_.FindByName(id.asString());
            else if (id.isNumber() && id.asNumber() >= 1 && id.asNumber() <= UINT32_MAX && std::floor(id.asNumber()) == id.asNumber()) entity = static_cast<EntityId>(id.asNumber());
            const auto* sync = scene_.Get<NetSync>(entity);
            if (!scene_.Exists(entity) || !sync) throw ApiError("network_owner", "owner target requires an entity with NetSync", "Pass entity in net.rpc, or the entity id/name as the first Lua RPC argument.");
            target = sync->owner ? std::to_string(sync->owner) : "server";
        }
        if (!ValidRpc(name, arguments)) throw ApiError("invalid_rpc", "RPC name/arguments exceed their bounds",
            "Use a 1..64 byte name, an array of at most 16 JSON arguments, depth <=8, and serialized size <=8192 bytes.");
        if (networkConfig_.mode == "none") {
            if (target != "server" && target != "all" && target != "others" && target != "1")
                throw ApiError("invalid_rpc_target", "target is not available in single-player", "Use server, all, others or player 1. owner requires M6.");
            if (target != "others") scripts_->DispatchRpc(1, name, arguments);
        } else if (!network_ || !network_->Rpc(target, name, arguments, &error)) {
            throw ApiError("network_rpc", error.empty() ? "no active lobby" : error, "Host/join, advance sim.step until lobby, and use a supported target.");
        }
        Json result = Json::MakeObject(); result["queued"] = networkConfig_.mode != "none"; return result;
    }
    if (command == "ready" && networkConfig_.mode == "none") return state();
    if (!network_) throw ApiError("network_inactive", "no active network session", "Use net.host or net.join first.");
    bool ok = false;
    if (command == "ready") ok = network_->Ready(args["ready"].asBool(), &error);
    else if (command == "kick") ok = network_->Kick(Number(args, "player", 0, 1, UINT32_MAX - 2), args["reason"].asString("kicked"), &error);
    if (!ok) throw ApiError("network_operation", error, "Use net.state/net.players to inspect the current lobby and try a valid operation.");
    return state();
}
}  // namespace oe
