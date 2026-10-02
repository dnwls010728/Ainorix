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
    if (sync_ && sync_->Active() && inWorld_ &&
        (command == "host" || command == "join" || command == "leave" || command == "kick" || command == "ready" || command == "start"))
        throw ApiError("match_control", "session controls cannot run in a synchronized game callback", "Control the lobby/session from tools; keep match updates deterministic.");
    auto state = [&] {
        if (network_) { Json out = network_->State(); out["syncImplemented"] = networkConfig_.mode != "authoritative";
            if (sync_) { out["sync"] = sync_->State(); if (replaying_) out["sync"]["frame"] = frame_; } return out; }
        Json out = Json::MakeObject(); out["mode"] = networkConfig_.mode;
        out["state"] = networkConfig_.mode == "none" ? "none" : "idle";
        out["isHost"] = false; out["isServer"] = networkConfig_.mode == "none"; out["isClient"] = false;
        out["localPlayer"] = networkConfig_.mode == "none" ? 1 : 0; out["seed"] = 0;
        out["transport"] = networkConfig_.transport; out["syncImplemented"] = networkConfig_.mode == "lockstep" || networkConfig_.mode == "rollback"; out["error"] = "";
        return out;
    };
    if (command == "state") return state();
    if (command == "desync_report") return sync_ ? sync_->Report() : Json::MakeObject();
    if (command == "start") {
        if (!network_ || !network_->Connected() || !network_->IsHost() || networkConfig_.mode == "authoritative")
            throw ApiError("network_start", "start requires a lockstep/rollback host lobby", "Host, join, and ready all players first.");
        std::vector<uint32_t> roster;
        Json players = network_->Players();
        for (const Json& player : players.items()) {
            if (!player["ready"].asBool()) throw ApiError("network_not_ready", "all players must be ready", "Call net.ready on every peer.");
            roster.push_back(static_cast<uint32_t>(player["id"].asNumber()));
        }
        BeginSessionIfNeeded(); EnsureSync(true); std::string error;
        if (!sync_->Start(roster, &error)) throw ApiError("network_start", error);
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
        if (network_) { Json stats = network_->Stats(); stats["replayMilliseconds"] = replayMilliseconds_; if (sync_) stats["sync"] = sync_->State(); return stats; }
        Json stats = Json::MakeObject(); stats["rejected"] = 0; stats["peers"] = Json::MakeArray(); return stats;
    }
    if (command == "host" || command == "join") {
        if (networkConfig_.mode == "none") throw ApiError("network_disabled", "this project has no enabled network mode",
            "Set project.json network.mode to lockstep, rollback (reference replay), or authoritative and reopen.");
        if (network_ && network_->Status() != "offline" && network_->Status() != "error")
            throw ApiError("network_active", "a session is already active", "Use net.leave and advance sim.step until offline, or sim.stop first.");
        sync_.reset(); checkpoints_.clear(); journal_.reset(); snapshots_.clear(); playerInputs_.clear(); frameInputs_.clear(); syncHashes_.clear();
        audio_->SetOutputMode(false, false); audio_->DiscardPending(); saves_.DeferFlush(false); saves_.FreezeReads(false);
        network_.reset(); networkFrame_ = 0;
        std::string error, name = args["name"].asString("Player");
        if (command == "host") {
            uint32_t seed = Number(args, "seed", 1, 0, UINT32_MAX);
            network_ = Session::Host(networkConfig_, name, seed, args["room"].asString(networkConfig_.gameId), &error);
        } else {
            uint16_t port = static_cast<uint16_t>(Number(args, "port", networkConfig_.port, 0, 65535));
            std::string address = args["address"].asString(networkConfig_.transport == "loopback" ? networkConfig_.gameId : "127.0.0.1");
            network_ = Session::Join(networkConfig_, name, address, port, &error);
        }
        if (!network_) throw ApiError("network_connect", error, "Check project network settings and net.state on the host; then advance both sessions with sim.step.");
        scripts_->SetNetworkSeed(network_->Seed());
        return state();
    }
    if (command == "leave") { if (network_) network_->Leave(); return state(); }
    std::string error;
    if (command == "rpc") {
        const Json& values = args["args"];
        Json arguments = values.isNull() ? Json::MakeArray() : values;
        std::string target = args["target"].asString("server"), name = args["name"].asString();
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
