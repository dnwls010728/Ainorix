// Focused tests for agent-team routing that do not need a real agent CLI.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "api/Commands.h"
#include "core/FileSystem.h"
#include "core/Json.h"
#include "platform/Platform.h"
#include "team/TeamCommands.h"
#include "team/TeamStore.h"

using namespace oe;

namespace {

int failures = 0;

#define CHECK(condition)                                                                 \
    do {                                                                                 \
        if (!(condition)) {                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);            \
            ++failures;                                                                  \
        }                                                                                \
    } while (0)

Json Agent(const char* name, const char* backend = "fake") {
    Json values = Json::MakeObject();
    values["name"] = name;
    values["backend"] = backend;
    return values;
}

void SetLeanEnv(const char* value) {
#if defined(_WIN32)
    _putenv_s("OE_TEAM_LEAN", value ? value : "");
#else
    if (value && *value) setenv("OE_TEAM_LEAN", value, 1);
    else unsetenv("OE_TEAM_LEAN");
#endif
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path project = fs::temp_directory_path() / "ownengine_team_efficiency_tests";
    std::error_code ec;
    fs::remove_all(project, ec);
    fs::create_directories(project, ec);
    CHECK(!ec);

    const char* originalLean = std::getenv("OE_TEAM_LEAN");
    const std::string savedLean = originalLean ? originalLean : "";
    const bool hadLean = originalLean != nullptr;
    SetLeanEnv(nullptr);
    CHECK(!LeanTeamTurnsEnabled());
    SetLeanEnv("1");
    CHECK(LeanTeamTurnsEnabled());
    SetLeanEnv("false");
    CHECK(!LeanTeamTurnsEnabled());
    SetLeanEnv("on");
    CHECK(LeanTeamTurnsEnabled());
    SetLeanEnv(hadLean ? savedLean.c_str() : nullptr);

    TeamStore store;
    std::string error;
    CHECK(store.Open(project.u8string(), &error));
    TeamStore::BackendLookup lookup = [](const std::string& backend, std::string& defaultModel) {
        defaultModel.clear();
        return backend == "fake";
    };
    store.Add(Agent("Core"), lookup);
    store.Add(Agent("QA"), lookup);

    const std::vector<Backend> noBackends;
    const TeamHost host;
    TeamSession session(store, noBackends, host);
    session.Open();
    Json routed = session.Send("Review with @qa and @all as context", "user", {}, 0, "Core");
    CHECK(routed["message"]["text"].asString() == "Review with @qa and @all as context");
    CHECK(routed["message"]["to"].size() == 1 && routed["message"]["to"][0].asString() == "core");
    CHECK(routed["offline"].size() == 1 && routed["offline"][0].asString() == "core");
    routed = session.Send("QA only", "user", {}, 0, "qa");
    CHECK(routed["message"]["to"].size() == 1 && routed["message"]["to"][0].asString() == "qa");

    bool rejected = false;
    try {
        session.Send("Do it", "user", {}, 0, "missing");
    } catch (const ApiError& e) {
        rejected = e.code == "unknown_agent";
    }
    CHECK(rejected);

    CommandRegistry commands;
    RegisterTeamCommands(commands, {});
    const Command* handoff = commands.Find("team.handoff");
    CHECK(handoff != nullptr);
    if (handoff) {
        CHECK(handoff->params["required"].size() == 2);
        CHECK(handoff->params["properties"]["to"].isObject());
        CHECK(handoff->params["properties"]["task"].isObject());
    }

#if defined(_WIN32)
    if (PlatformProcessSupported()) {
        // A tiny stand-in CLI captures stdin and emits one recorded Claude result. This exercises
        // TeamSession's real child-process path without login, network, or paid model calls.
        const fs::path turnProject = project / "turns";
        const fs::path fixture = project / "fixtures";
        fs::create_directories(turnProject, ec);
        CHECK(!ec);
        fs::create_directories(fixture, ec);
        CHECK(!ec);

        auto fake = [&](const char* id, const char* answer) {
            Backend backend;
            backend.id = id;
            backend.displayName = std::string("Fake ") + id;
            backend.executable = (fixture / (std::string(id) + ".cmd")).u8string();
            backend.systemPromptFlag = true;
            backend.buildTurn = [](const TurnRequest& request) {
                ProcessOptions options;
                options.executable = request.executable;
                return options;
            };
            backend.parseLine = &ParseClaudeLine;
            const std::string stream = std::string(R"J({"type":"result","subtype":"success","is_error":false,"result":")J") + answer +
                                       R"J(","session_id":"lean-session"})J" + "\n";
            CHECK(WriteTextFile((fixture / (std::string(id) + ".jsonl")).u8string(), stream));
            const std::string script = "@echo off\nfindstr \"^\" > \"%~dp0%OE_TEAM_AGENT%.prompt.txt\"\ntype \"%~dp0" + std::string(id) + ".jsonl\"\n";
            CHECK(WriteTextFile(backend.executable, script));
            return backend;
        };

        std::vector<Backend> turnBackends = {
            fake("ping", "Ping finished; @pong is only mentioned in text."),
            fake("pong", "Pong finished."),
        };
        TeamStore turnStore;
        CHECK(turnStore.Open(turnProject.u8string(), &error));
        TeamStore::BackendLookup turnLookup = [](const std::string& backend, std::string& defaultModel) {
            defaultModel.clear();
            return backend == "ping" || backend == "pong";
        };
        turnStore.Add(Agent("Ping", "ping"), turnLookup);
        turnStore.Add(Agent("Pong", "pong"), turnLookup);

        SetLeanEnv("1");
        {
            TeamSession turns(turnStore, turnBackends, host);
            turns.Open();
            auto untilIdle = [&] {
                const double end = PlatformTimeSeconds() + 10.0;
                while (PlatformTimeSeconds() < end) {
                    turns.Update();
                    if (!turns.Busy({})) return true;
                    PlatformSleep(0.01);
                }
                turns.Update();
                return !turns.Busy({});
            };

            Json sent = turns.Send("@pong unrelated-only-for-pong", "user");
            CHECK(sent["started"].size() == 1 && sent["started"][0].asString() == "pong");
            CHECK(untilIdle());
            sent = turns.Send("@ping lean-target-only-for-ping", "user");
            CHECK(sent["started"].size() == 1 && sent["started"][0].asString() == "ping");
            CHECK(untilIdle());

            std::string pingPrompt, pongPrompt;
            CHECK(ReadTextFile((fixture / "ping.prompt.txt").u8string(), pingPrompt));
            CHECK(ReadTextFile((fixture / "pong.prompt.txt").u8string(), pongPrompt));
            CHECK(pingPrompt.find("lean-target-only-for-ping") != std::string::npos);
            CHECK(pingPrompt.find("unrelated-only-for-pong") == std::string::npos);
            CHECK(pongPrompt.find("unrelated-only-for-pong") != std::string::npos);
            CHECK(pongPrompt.find("Ping finished; @pong") == std::string::npos);

            const Json chat = turns.Messages(0, false, 100)["messages"];
            int pingReplies = 0, pongReplies = 0;
            for (const Json& message : chat.items()) {
                if (message["from"].asString() == "ping") ++pingReplies;
                if (message["from"].asString() == "pong") ++pongReplies;
            }
            CHECK(pingReplies == 1 && pongReplies == 1);
            CHECK(chat.size() == 4);
            if (chat.size() == 4) {
                CHECK(chat[3]["from"].asString() == "ping");
                CHECK(chat[3]["to"].size() == 1 && chat[3]["to"][0].asString() == "user");
            }
        }
        SetLeanEnv(hadLean ? savedLean.c_str() : nullptr);
    }
#endif

    fs::remove_all(project, ec);
    if (failures == 0) {
        std::printf("team efficiency tests passed\n");
        return 0;
    }
    std::printf("team efficiency tests failed: %d\n", failures);
    return 1;
}
