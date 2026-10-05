// Focused tests for agent-team routing that do not need a real agent CLI.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "api/Commands.h"
#include "core/Json.h"
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

Json Agent(const char* name) {
    Json values = Json::MakeObject();
    values["name"] = name;
    values["backend"] = "fake";
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

    fs::remove_all(project, ec);
    if (failures == 0) {
        std::printf("team efficiency tests passed\n");
        return 0;
    }
    std::printf("team efficiency tests failed: %d\n", failures);
    return 1;
}
