#include "app/DedicatedServer.h"

#include <algorithm>
#include <csignal>
#include <exception>
#include <memory>
#include "app/Engine.h"
#include "core/FileSystem.h"
#include "platform/Platform.h"

namespace oe {
namespace {
volatile std::sig_atomic_t g_stopServer = 0;
void StopServer(int) { g_stopServer = 1; }
}
bool ApplyLaunchArg(Engine& engine, const std::string& assignment, bool fromFile, std::string* error) {
    const size_t split = assignment.find('=');
    if (split == std::string::npos || split == 0) { if (error) *error = "expected name=value"; return false; }
    std::string value = assignment.substr(split + 1);
    if (fromFile) {
        std::string text;
        if (!ReadTextFile(value, text)) { if (error) *error = "cannot read " + value; return false; }
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '\t')) text.pop_back();
        value = std::move(text);
    }
    return engine.SetLaunchArg(assignment.substr(0, split), value, error);
}
namespace {
// Serves every engine (net.serve with its arguments) and steps them in turn until a signal, maxTicks
// or a world that stopped. Returns each world's final state, or the error that ended the run.
Json RunWorlds(const std::vector<Engine*>& engines, const Json& arguments, const std::vector<std::string>& scenes, uint64_t maxTicks,
               const std::function<void(size_t, const Json&)>& started, std::vector<Json>& states, uint64_t& ticks) {
    Json result = Json::MakeObject(); result["ok"] = true;
    // The first engine hosts the session; with several worlds the others serve their world of it.
    std::vector<Json> envelopes;
    Json served = engines[0]->Call("net.serve", arguments);
    if (!served["ok"].asBool()) return served;
    envelopes.push_back(served);
    if (engines.size() > 1) {
        try {
            std::string error;
            if (!engines[0]->SharedNetwork()->SetWorlds(scenes, &error)) throw ApiError("network_server", error);
            for (size_t i = 1; i < engines.size(); ++i) {
                engines[i]->ServeWorld(engines[0]->SharedNetwork(), static_cast<uint32_t>(i));
                envelopes.push_back(engines[i]->Call("net.state", Json::MakeObject()));
            }
            envelopes[0] = engines[0]->Call("net.state", Json::MakeObject());
        } catch (const std::exception& error) {
            for (Engine* engine : engines) engine->Stop();
            Json failed = Json::MakeObject(); failed["ok"] = false; failed["error"]["code"] = "network_server";
            failed["error"]["message"] = error.what(); return failed;
        }
    }
    // One runner per process; restore the caller's signal handlers on every exit.
    g_stopServer = 0;
    auto oldInt = std::signal(SIGINT, StopServer), oldTerm = std::signal(SIGTERM, StopServer);
    ticks = 0;
    try {
        if (started) for (size_t i = 0; i < engines.size(); ++i) started(i, envelopes[i]);
        double next = PlatformTimeSeconds();
        bool over = false;
        while (!over && !g_stopServer && (!maxTicks || ticks < maxTicks)) {
            for (Engine* engine : engines) {
                engine->RunPostedJobs(); engine->Step(1);
                Json state = engine->NetworkCall("state", Json::MakeObject());
                const std::string session = state["state"].asString(), sync = state["sync"]["state"].asString();
                if (session == "offline" || session == "idle" || session == "leaving") over = true;
                if (session == "error" || sync == "error" || sync == "desync" || sync == "stopped") {
                    result["ok"] = false; result["error"]["code"] = "network_server";
                    result["error"]["message"] = state.dump(); over = true;
                }
            }
            ++ticks;
            if (over) break;
            next += Engine::kFixedDt;
            // Drop scheduling debt after a long pause; never feed wall-clock time into simulation.
            double now = PlatformTimeSeconds(); if (next < now - 0.25) next = now;
            while (!g_stopServer && now < next) {
                PlatformSleep(std::min(0.005, next - now)); now = PlatformTimeSeconds();
            }
        }
        if (result["ok"].asBool()) for (Engine* engine : engines) {
            Json state = engine->NetworkCall("state", Json::MakeObject());
            state["frame"] = engine->Frame();
            states.push_back(std::move(state));
        }
    } catch (const std::exception& error) {
        result = Json::MakeObject(); result["ok"] = false; result["error"]["code"] = "network_server";
        result["error"]["message"] = error.what();
    }
    std::signal(SIGINT, oldInt); std::signal(SIGTERM, oldTerm);
    for (Engine* engine : engines) engine->Stop();
    return result;
}
}  // namespace

Json RunDedicatedServer(Engine& engine, const Json& arguments, uint64_t maxTicks,
                        const std::function<void(const Json&)>& started) {
    std::vector<Json> states; uint64_t ticks = 0;
    Json result = RunWorlds({&engine}, arguments, {}, maxTicks, [&](size_t, const Json& envelope) { if (started) started(envelope); }, states, ticks);
    if (result["ok"].asBool()) { result["result"] = states[0]; result["result"]["ticks"] = ticks; }
    return result;
}

std::vector<std::string> SplitSceneList(const std::string& text) {
    std::vector<std::string> out; std::string name;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i < text.size() && text[i] != ',') { name += text[i]; continue; }
        if (!name.empty()) out.push_back(name);
        name.clear();
    }
    return out;
}

Json RunDedicatedWorlds(const std::string& project, const std::vector<std::string>& scenes, const Json& arguments, uint64_t maxTicks,
                        const std::function<bool(Engine&, std::string*)>& prepare,
                        const std::function<bool(size_t, Engine&, std::string*)>& opened,
                        const std::function<void(const Json&)>& started) {
    auto fail = [](const std::string& message) {
        Json result = Json::MakeObject(); result["ok"] = false; result["error"]["code"] = "network_server";
        result["error"]["message"] = message; return result;
    };
    if (scenes.empty()) return fail("no scenes to serve");
    const std::string root = IsDirectory(project) ? project : ParentPath(project);
    std::vector<std::unique_ptr<Engine>> owned; std::vector<Engine*> engines;
    for (size_t i = 0; i < scenes.size(); ++i) {
        owned.push_back(std::make_unique<Engine>());
        std::string error;
        if (prepare && !prepare(*owned.back(), &error)) return fail(error);
        if (!owned.back()->Open(JoinPath(root, scenes[i]), &error)) return fail(error);
        if (opened && !opened(i, *owned.back(), &error)) return fail(error);
        engines.push_back(owned.back().get());
    }
    std::vector<Json> states; uint64_t ticks = 0;
    Json result = RunWorlds(engines, arguments, scenes, maxTicks, [&](size_t index, const Json& envelope) {
        if (!started) return;
        Json told = envelope; told["world"] = static_cast<int>(index); told["scene"] = scenes[index];
        started(told);
    }, states, ticks);
    if (result["ok"].asBool()) {
        Json worlds = Json::MakeArray();
        for (size_t i = 0; i < states.size(); ++i) { states[i]["scene"] = scenes[i]; worlds.push(states[i]); }
        result["result"] = Json::MakeObject(); result["result"]["worlds"] = worlds; result["result"]["ticks"] = ticks;
    }
    return result;
}
}  // namespace oe
