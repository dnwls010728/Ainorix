#include "app/DedicatedServer.h"

#include <algorithm>
#include <csignal>
#include <exception>
#include "app/Engine.h"
#include "platform/Platform.h"

namespace oe {
namespace {
volatile std::sig_atomic_t g_stopServer = 0;
void StopServer(int) { g_stopServer = 1; }
}
Json RunDedicatedServer(Engine& engine, const Json& arguments, uint64_t maxTicks,
                        const std::function<void(const Json&)>& started) {
    Json result = engine.Call("net.serve", arguments);
    if (!result["ok"].asBool()) return result;
    // One runner per process; restore the caller's signal handlers on every exit.
    g_stopServer = 0;
    auto oldInt = std::signal(SIGINT, StopServer), oldTerm = std::signal(SIGTERM, StopServer);
    uint64_t ticks = 0;
    try {
        if (started) started(result);
        double next = PlatformTimeSeconds();
        while (!g_stopServer && (!maxTicks || ticks < maxTicks)) {
            engine.RunPostedJobs(); engine.Step(1); ++ticks;
            Json state = engine.NetworkCall("state", Json::MakeObject());
            const std::string session = state["state"].asString(), sync = state["sync"]["state"].asString();
            if (session == "offline" || session == "idle" || session == "leaving") break;
            if (session == "error" || sync == "error" || sync == "desync" || sync == "stopped") {
                result["ok"] = false; result["error"]["code"] = "network_server";
                result["error"]["message"] = state.dump(); break;
            }
            next += Engine::kFixedDt;
            // Drop scheduling debt after a long pause; never feed wall-clock time into simulation.
            double now = PlatformTimeSeconds(); if (next < now - 0.25) next = now;
            while (!g_stopServer && now < next) {
                PlatformSleep(std::min(0.005, next - now)); now = PlatformTimeSeconds();
            }
        }
        if (result["ok"].asBool()) {
            result["result"] = engine.NetworkCall("state", Json::MakeObject());
            result["result"]["frame"] = engine.Frame(); result["result"]["ticks"] = ticks;
        }
    } catch (const std::exception& error) {
        result = Json::MakeObject(); result["ok"] = false; result["error"]["code"] = "network_server";
        result["error"]["message"] = error.what();
    }
    std::signal(SIGINT, oldInt); std::signal(SIGTERM, oldTerm);
    engine.Stop(); return result;
}
}  // namespace oe
