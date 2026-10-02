#include "app/Engine.h"

namespace oe {
void Engine::ResetLocalPeers() {
    localPeers_.clear();
    if (!previewTransport_.empty()) { networkConfig_.transport = previewTransport_; previewTransport_.clear(); }
    dedicated_ = autoStart_ = false; minimumPlayers_ = 1;
}
void Engine::AdvanceLocalPeers() {
    // Each endpoint polls exactly once in stable index order per host I/O tick.
    for (auto& peer : localPeers_) {
        peer->Step(1);
        if (peer->network_ && peer->network_->Connected()) {
            bool ready = false;
            Json players = peer->network_->Players();
            for (const Json& player : players.items())
                if (player["local"].asBool()) ready = player["ready"].asBool();
            if (!ready) { std::string error; peer->network_->Ready(true, &error); }
        }
    }
}
void Engine::AutoStartNetwork() {
    if (!autoStart_ || !network_ || !network_->Connected() ||
        (sync_ && sync_->Active()) || (authority_ && authority_->Active())) return;
    Json players = network_->Players();
    if (players.size() < static_cast<size_t>(minimumPlayers_) + 1) return;
    for (const Json& player : players.items()) if (!player["ready"].asBool()) return;
    NetworkCall("start", Json::MakeObject()); autoStart_ = false;
}
Json Engine::LocalPeersState() {
    Json result = Json::MakeArray();
    for (size_t i = 0; i <= localPeers_.size(); ++i) {
        Engine& peer = *LocalPeer(i); Json item = Json::MakeObject();
        item["peer"] = static_cast<uint64_t>(i); item["frame"] = peer.Frame();
        item["state"] = peer.NetworkCall("state", Json::MakeObject());
        item["stats"] = peer.NetworkCall("stats", Json::MakeObject());
        item["desync"] = peer.NetworkCall("desync_report", Json::MakeObject()); result.push(std::move(item));
    }
    return result;
}
}  // namespace oe
