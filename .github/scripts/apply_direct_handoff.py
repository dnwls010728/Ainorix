from pathlib import Path


def read(path):
    return Path(path).read_text(encoding="utf-8")


def write(path, text):
    Path(path).write_text(text, encoding="utf-8", newline="\n")


def replace_once(path, old, new):
    text = read(path)
    if old not in text:
        raise SystemExit(f"missing patch anchor in {path}: {old[:160]!r}")
    write(path, text.replace(old, new, 1))


def replace_optional(path, old, new):
    text = read(path)
    if old in text:
        write(path, text.replace(old, new, 1))
        return True
    return False


# TeamSession gets a structured recipient path. Normal team.send remains mention/lead routed.
replace_once(
    "engine/team/TeamSession.h",
    '    Json Send(const std::string& text, const std::string& from, const std::vector<std::string>& attachments = {}, int hop = 0);',
    '    // `directTo` addresses exactly one agent (id or name) and does not parse mentions in text.\n'
    '    Json Send(const std::string& text, const std::string& from, const std::vector<std::string>& attachments = {}, int hop = 0,\n'
    '              const std::string& directTo = std::string());')

replace_once(
    "engine/team/TeamSession.cpp",
    'Json TeamSession::Send(const std::string& text, const std::string& from, const std::vector<std::string>& attachments, int hop) {',
    'Json TeamSession::Send(const std::string& text, const std::string& from, const std::vector<std::string>& attachments, int hop, const std::string& directTo) {')

replace_once(
    "engine/team/TeamSession.cpp",
    '    const std::vector<AgentProfile>& roster = store_.Agents();\n'
    '    bool all = false;\n'
    '    std::vector<size_t> recipients = FindMentions(text, roster, all);\n'
    '    if (all) {\n'
    '        recipients.clear();\n'
    '        for (size_t i = 0; i < roster.size(); ++i) recipients.push_back(i);\n'
    '    } else if (recipients.empty()) {\n'
    '        for (size_t i = 0; i < roster.size(); ++i) {\n'
    '            if (roster[i].id == store_.Settings().lead) recipients.push_back(i);  // no mention: the lead\n'
    '        }\n'
    '    }\n',
    '    const std::vector<AgentProfile>& roster = store_.Agents();\n'
    '    std::vector<size_t> recipients;\n'
    '    if (!directTo.empty()) {\n'
    '        const AgentProfile* target = store_.Find(directTo);\n'
    '        if (!target) throw ApiError("unknown_agent", "no agent \'" + directTo + "\' in the team", "team.list shows the agents; use an id or a name.");\n'
    '        for (size_t i = 0; i < roster.size(); ++i) {\n'
    '            if (roster[i].id == target->id) recipients.push_back(i);\n'
    '        }\n'
    '    } else {\n'
    '        bool all = false;\n'
    '        recipients = FindMentions(text, roster, all);\n'
    '        if (all) {\n'
    '            recipients.clear();\n'
    '            for (size_t i = 0; i < roster.size(); ++i) recipients.push_back(i);\n'
    '        } else if (recipients.empty()) {\n'
    '            for (size_t i = 0; i < roster.size(); ++i) {\n'
    '                if (roster[i].id == store_.Settings().lead) recipients.push_back(i);  // no mention: the lead\n'
    '            }\n'
    '        }\n'
    '    }\n')

# team.handoff no longer encodes its recipient into human-readable chat text.
replace_once(
    "engine/team/TeamCommands.cpp",
    'std::string BuildTeamHandoffText(const TeamStore& store, const std::string& to, const std::string& task) {\n'
    '    const AgentProfile* target = store.Find(to);\n'
    '    if (!target) throw ApiError("unknown_agent", "no agent \'" + to + "\' in the team", "team.list shows the agents; use an id or a name.");\n\n'
    '    bool all = false;\n'
    '    const std::vector<size_t> mentioned = FindMentions(task, store.Agents(), all);\n'
    '    for (size_t index : mentioned) {\n'
    '        if (index >= store.Agents().size() || store.Agents()[index].id != target->id) {\n'
    '            throw ApiError("ambiguous_handoff", "a handoff task cannot mention another teammate",\n'
    '                           "Put context in plain text and use the `to` field as the only recipient.");\n'
    '        }\n'
    '    }\n'
    '    if (all) {\n'
    '        throw ApiError("ambiguous_handoff", "a handoff task cannot contain @all",\n'
    '                       "Use one explicit recipient in the `to` field.");\n'
    '    }\n'
    '    return "@" + target->id + (task.empty() ? std::string() : " " + task);\n'
    '}\n\n',
    '')

replace_once(
    "engine/team/TeamCommands.cpp",
    '             "Give exactly one teammate a task. The `to` field is the recipient; task text may mention that same teammate but no other teammate or @all.",\n',
    '             "Give exactly one teammate a task. The `to` field is the only recipient; @mentions inside task are context text and never add recipients.",\n')

replace_once(
    "engine/team/TeamCommands.cpp",
    '                 const std::string text = BuildTeamHandoffText(state->store, a["to"].asString(), a["task"].asString());\n'
    '                 Json result = session.Send(text, from, attachments, hop);\n',
    '                 Json result = session.Send(a["task"].asString(), from, attachments, hop, a["to"].asString());\n')

replace_once(
    "engine/team/TeamCommands.h",
    '\n// Builds the single-recipient chat text used by team.handoff. The target is resolved by id/name;\n'
    '// task text may not contain another team mention because team.send routes every mention it sees.\n'
    '// Kept public so routing semantics have a small unit test independent of a real agent CLI.\n'
    'std::string BuildTeamHandoffText(const TeamStore& store, const std::string& to, const std::string& task);\n',
    '')
replace_optional("engine/team/TeamCommands.h", '#include <string>\n', '')

# Focused no-CLI coverage for the lean switch and direct single-recipient routing.
write("tests/team_efficiency_tests.cpp", r'''// Focused tests for agent-team routing that do not need a real agent CLI.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "api/Commands.h"
#include "core/Json.h"
#include "team/TeamCommands.h"
#include "team/TeamSession.h"
#include "team/TeamStore.h"

using namespace oe;

namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; } } while (0)

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

    // The backend is intentionally absent. Send still stores/routs the message before reporting
    // the selected agent offline, which is enough to prove direct routing without a paid CLI turn.
    const std::vector<Backend> backends;
    TeamHost host;
    TeamSession session(store, backends, host);
    session.Open();
    Json routed = session.Send("Review this; @qa and @all are context only.", "user", {}, 0, "Core");
    CHECK(routed["message"]["text"].asString() == "Review this; @qa and @all are context only.");
    CHECK(routed["message"]["to"].size() == 1);
    CHECK(routed["message"]["to"][0].asString() == "core");

    bool rejected = false;
    try {
        session.Send("Do it", "user", {}, 0, "missing");
    } catch (const ApiError& e) {
        rejected = e.code == "unknown_agent";
    }
    CHECK(rejected);

    // Normal chat routing is unchanged: a mention still addresses the mentioned teammate.
    Json chat = session.Send("@qa check this", "user");
    CHECK(chat["message"]["to"].size() == 1 && chat["message"]["to"][0].asString() == "qa");

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
''')

replace_once(
    "docs/TEAM_EFFICIENCY.md",
    'The command resolves `to` by agent id or name and sends one addressed turn through the existing `TeamSession`, so attachment handling, offline reporting, session resume, concurrency and `maxHops` stay unchanged. To guarantee a single recipient with the current mention-based `TeamSession::Send` router, the task may mention the selected teammate but rejects another teammate mention or `@all` with `ambiguous_handoff`.\n\n'
    'This is a compatibility step toward separating human-readable chat from executable tasks. It gives lean turns an explicit execution signal without changing the normal `team.send` path.',
    'The command resolves `to` by agent id or name and passes it to `TeamSession::Send` as a structured direct recipient. The stored task stays exactly as written: `@qa`, `@all`, or any other mention inside `task` is context text and cannot add recipients. Attachment handling, offline reporting, session resume, concurrency and `maxHops` stay unchanged. Normal `team.send` keeps its existing mention/lead routing.')

replace_once(
    "docs/TEAM_EFFICIENCY.md",
    'The runner configures the normal repository root and injects only the focused test target through `CMAKE_PROJECT_TOP_LEVEL_INCLUDES`; this preserves the root `CMAKE_SOURCE_DIR` assumptions used by the engine build. The test verifies the lean environment switch, id/name resolution, one-recipient routing text, rejection of another teammate / `@all`, unknown-agent handling, and registration of the `team.handoff` command.',
    'The runner configures the normal repository root and injects only the focused test target through `CMAKE_PROJECT_TOP_LEVEL_INCLUDES`; this preserves the root `CMAKE_SOURCE_DIR` assumptions used by the engine build. The test verifies the lean environment switch, structured one-recipient routing even when task text contains `@qa` / `@all`, unknown-agent handling, unchanged normal chat mention routing, and registration of the `team.handoff` command.')

replace_once(
    "docs/TEAM_EFFICIENCY.md",
    '- split human-readable `ChatMessage` from executable `AgentTask`, so explicit routing no longer needs to encode the recipient as an `@mention` in stored chat text;\n',
    '- consider a fuller `AgentTask` type later if task metadata grows beyond the structured `directTo` route now used by `team.handoff`;\n')

print("direct handoff routing patch applied")
