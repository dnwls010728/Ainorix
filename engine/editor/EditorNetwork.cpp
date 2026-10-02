#include "editor/EditorInternal.h"
#include "app/Engine.h"
#include "editor/EditorText.h"

namespace oe {
Engine& NativeEditor::Impl::GameEngine() {
    Engine* peer = engine.LocalPeer(static_cast<size_t>(gamePeer));
    return peer ? *peer : engine;
}
void NativeEditor::Impl::NetworkPanel() {
    if (requestNetworkFocus) { ImGui::SetNextWindowFocus(); requestNetworkFocus = false; }
    if (!ImGui::Begin(TrId("Network").c_str(), &showNetwork)) { ImGui::End(); return; }
    if (!engine.NetworkEnabled()) {
        ImGui::TextWrapped("%s", Tr("Networking is disabled for this project."));
        ImGui::Spacing();
        ImGui::TextWrapped("%s", Tr("Add a network section to project.json and reopen the project to enable multiplayer."));
        ImGui::Spacing();
        ImGui::TextWrapped("%s", "\"network\": {\"mode\": \"authoritative\", \"transport\": \"tcp\", \"maxPlayers\": 4}");
        ImGui::End(); return;
    }
    Json state = engine.NetworkCall("state", Json::MakeObject());
    ImGui::Text("%s / %s / %s", state["mode"].asString().c_str(), state["transport"].asString().c_str(), state["state"].asString().c_str());
    if (!state["error"].asString().empty()) ImGui::TextWrapped("%s", state["error"].asString().c_str());
    Json players = Call("net.players", Json(), true)["result"];
    for (const Json& player : players.items()) {
        bool playerReady = player["ready"].asBool();
        ImGui::Text("%u  %s  %s", static_cast<unsigned>(player["id"].asNumber()), player["name"].asString().c_str(),
                    Tr(playerReady ? "Ready" : "Waiting"));
    }
    Json simulation = Call("net.stats", Json(), true)["result"]["simulation"];
    if (simulation["enabled"].asBool()) networkLatency = simulation["latencyFrames"].asInt();
    ImGui::BeginDisabled(!simulation["enabled"].asBool() && InPlaySession());
    ImGui::TextUnformatted(Tr("Latency (frames)"));
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderInt("##networkLatency", &networkLatency, 0, 30) && simulation["enabled"].asBool())
        Call("net.simulate", ObjectOf({{"latencyFrames", Json(networkLatency)}}));
    ImGui::EndDisabled();
    Json peers = Call("net.local_peers", Json(), true)["result"];
    for (const Json& peer : peers.items()) {
        ImGui::Separator();
        ImGui::Text(Tr("Player %u / frame %llu / %s"), static_cast<unsigned>(peer["peer"].asNumber() + 1),
                    static_cast<unsigned long long>(peer["frame"].asNumber()), peer["state"]["sync"]["state"].asString().c_str());
        for (const Json& connection : peer["stats"]["peers"].items())
            ImGui::TextWrapped(Tr("RTT %.1f ms / loss %u/1000\nSent %llu B / received %llu B\nPending %u"), connection["rttMs"].asNumber(),
                static_cast<unsigned>(connection["lossPermille"].asNumber()), static_cast<unsigned long long>(connection["bytesSent"].asNumber()),
                static_cast<unsigned long long>(connection["bytesReceived"].asNumber()), static_cast<unsigned>(connection["pending"].asNumber()));
        if (!peer["state"]["sync"]["error"].asString().empty()) ImGui::TextWrapped("%s", peer["state"]["sync"]["error"].asString().c_str());
        if (peer["desync"].size()) ImGui::TextWrapped("%s", peer["desync"].dump().c_str());
    }
    ImGui::End();
}
}  // namespace oe
