from pathlib import Path


def read(path):
    return Path(path).read_text(encoding="utf-8")


def write(path, text):
    Path(path).write_text(text, encoding="utf-8", newline="\n")


def replace_once(path, old, new):
    text = read(path)
    if old not in text:
        raise SystemExit(f"missing patch anchor in {path}: {old[:140]!r}")
    write(path, text.replace(old, new, 1))


def replace_optional(path, old, new):
    text = read(path)
    if old in text:
        write(path, text.replace(old, new, 1))
        return True
    return False


# ---------------------------------------------------------------------------
# Team settings: reply mentions are non-executable by default.
# ---------------------------------------------------------------------------
replace_once(
    "engine/team/TeamStore.h",
    '    int maxHops = 4;        // chained agent-to-agent turns per user message, 0..16\n};',
    '    int maxHops = 4;        // explicit/automatic agent-to-agent turns per user message, 0..16\n'
    '    bool autoHandoff = false;  // finished replies wake mentioned teammates only when enabled\n};')

replace_once(
    "engine/team/TeamStore.cpp",
    '    if (settings.maxHops != defaults.maxHops) root["maxHops"] = settings.maxHops;\n',
    '    if (settings.maxHops != defaults.maxHops) root["maxHops"] = settings.maxHops;\n'
    '    if (settings.autoHandoff != defaults.autoHandoff) root["autoHandoff"] = settings.autoHandoff;\n')

replace_once(
    "engine/team/TeamStore.cpp",
    'int IntegerSetting(const Json& values, const char* key, int minimum, int maximum, int current) {\n'
    '    const Json* value = values.find(key);\n'
    '    if (!value) return current;\n'
    '    const double number = value->asNumber(-1.0);\n'
    '    if (!value->isNumber() || number != std::floor(number) || number < minimum || number > maximum) {\n'
    '        throw Invalid(std::string(key) + " must be an integer from " + std::to_string(minimum) + " to " + std::to_string(maximum));\n'
    '    }\n'
    '    return static_cast<int>(number);\n'
    '}\n',
    'int IntegerSetting(const Json& values, const char* key, int minimum, int maximum, int current) {\n'
    '    const Json* value = values.find(key);\n'
    '    if (!value) return current;\n'
    '    const double number = value->asNumber(-1.0);\n'
    '    if (!value->isNumber() || number != std::floor(number) || number < minimum || number > maximum) {\n'
    '        throw Invalid(std::string(key) + " must be an integer from " + std::to_string(minimum) + " to " + std::to_string(maximum));\n'
    '    }\n'
    '    return static_cast<int>(number);\n'
    '}\n\n'
    'bool BooleanSetting(const Json& values, const char* key, bool current) {\n'
    '    const Json* value = values.find(key);\n'
    '    if (!value) return current;\n'
    '    if (!value->isBool()) throw Invalid(std::string(key) + " must be a boolean");\n'
    '    return value->asBool(current);\n'
    '}\n')

replace_once(
    "engine/team/TeamStore.cpp",
    '        settings.maxHops = IntegerSetting(root, "maxHops", 0, 16, settings.maxHops);\n',
    '        settings.maxHops = IntegerSetting(root, "maxHops", 0, 16, settings.maxHops);\n'
    '        settings.autoHandoff = BooleanSetting(root, "autoHandoff", settings.autoHandoff);\n')

replace_once(
    "engine/team/TeamStore.cpp",
    '    settings.maxHops = IntegerSetting(values, "maxHops", 0, 16, settings.maxHops);\n',
    '    settings.maxHops = IntegerSetting(values, "maxHops", 0, 16, settings.maxHops);\n'
    '    settings.autoHandoff = BooleanSetting(values, "autoHandoff", settings.autoHandoff);\n')

replace_once(
    "engine/team/TeamStore.cpp",
    '    root["maxHops"] = settings_.maxHops;\n',
    '    root["maxHops"] = settings_.maxHops;\n'
    '    root["autoHandoff"] = settings_.autoHandoff;\n')

# ---------------------------------------------------------------------------
# TeamSession: structured single-recipient handoff + pull only addressed chat.
# ---------------------------------------------------------------------------
replace_once(
    "engine/team/TeamSession.h",
    '    Json Send(const std::string& text, const std::string& from, const std::vector<std::string>& attachments = {}, int hop = 0);',
    '    // `directTo` addresses exactly one agent (id or name) and ignores mentions in text.\n'
    '    Json Send(const std::string& text, const std::string& from, const std::vector<std::string>& attachments = {}, int hop = 0,\n'
    '              const std::string& directTo = std::string());')

replace_once(
    "engine/team/TeamSession.cpp",
    '    // Unread chat: what others said since this agent\'s last successful turn.\n'
    '    std::vector<const ChatMessage*> unread;\n'
    '    for (const ChatMessage& message : messages_) {\n'
    '        if (message.id > runtime.seen && message.kind == "message" && message.from != agent.id) unread.push_back(&message);\n'
    '    }\n',
    '    // Push only chat explicitly addressed to this agent. Other team chat stays available\n'
    '    // through team.messages instead of being paid for in every agent turn.\n'
    '    std::vector<const ChatMessage*> unread;\n'
    '    for (const ChatMessage& message : messages_) {\n'
    '        const bool addressed = std::find(message.to.begin(), message.to.end(), agent.id) != message.to.end();\n'
    '        if (message.id > runtime.seen && message.kind == "message" && message.from != agent.id && addressed) unread.push_back(&message);\n'
    '    }\n')

replace_once(
    "engine/team/TeamSession.cpp",
    '    std::string prompt = "New messages in the team chat, oldest first. Reply to what is addressed to you.\\n";\n',
    '    std::string prompt = "New messages addressed to you in the team chat, oldest first.\\n";\n')

replace_once(
    "engine/team/TeamSession.cpp",
    '        // Agent-to-agent: teammates the reply mentions get a turn, as long as the chain that\n'
    '        // began with the person\'s message is not longer than maxHops.\n'
    '        reply.hop = turn->hop + 1;\n'
    '        const std::vector<AgentProfile>& roster = store_.Agents();\n'
    '        bool all = false;\n'
    '        std::vector<size_t> mentioned = FindMentions(reply.text, roster, all);\n'
    '        if (all) {\n'
    '            mentioned.clear();\n'
    '            for (size_t i = 0; i < roster.size(); ++i) mentioned.push_back(i);\n'
    '        }\n'
    '        mentioned.erase(std::remove_if(mentioned.begin(), mentioned.end(), [&](size_t i) { return roster[i].id == id; }), mentioned.end());\n'
    '        for (size_t i : mentioned) {\n'
    '            if (std::find(reply.to.begin(), reply.to.end(), roster[i].id) == reply.to.end()) reply.to.push_back(roster[i].id);\n'
    '        }\n',
    '        // Reply mentions are ordinary chat by default. Projects can opt into the older\n'
    '        // automatic agent-to-agent behavior with autoHandoff.\n'
    '        reply.hop = turn->hop + 1;\n'
    '        const std::vector<AgentProfile>& roster = store_.Agents();\n'
    '        std::vector<size_t> mentioned;\n'
    '        if (store_.Settings().autoHandoff) {\n'
    '            bool all = false;\n'
    '            mentioned = FindMentions(reply.text, roster, all);\n'
    '            if (all) {\n'
    '                mentioned.clear();\n'
    '                for (size_t i = 0; i < roster.size(); ++i) mentioned.push_back(i);\n'
    '            }\n'
    '            mentioned.erase(std::remove_if(mentioned.begin(), mentioned.end(), [&](size_t i) { return roster[i].id == id; }), mentioned.end());\n'
    '            for (size_t i : mentioned) {\n'
    '                if (std::find(reply.to.begin(), reply.to.end(), roster[i].id) == reply.to.end()) reply.to.push_back(roster[i].id);\n'
    '            }\n'
    '        }\n')

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

# ---------------------------------------------------------------------------
# Command surface: make the policy visible and route team.handoff structurally.
# ---------------------------------------------------------------------------
replace_optional(
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
    '"The project\'s agent team: settings (lead, maxConcurrent, maxHops), a revision that changes with every edit, and all agent profiles."',
    '"The project\'s agent team: settings (lead, maxConcurrent, maxHops, autoHandoff), a revision that changes with every edit, and all agent profiles."')

replace_once(
    "engine/team/TeamCommands.cpp",
    '                 .Opt("maxHops", "integer", "Chained agent-to-agent turns one user message may cause, 0..16 (default 4)."),\n',
    '                 .Opt("maxHops", "integer", "Chained agent-to-agent turns one user message may cause, 0..16 (default 4).")\n'
    '                 .Opt("autoHandoff", "boolean", "Automatically start teammates mentioned in an agent\'s final reply (default false). Explicit team.handoff is unaffected."),\n')

replace_once(
    "engine/team/TeamCommands.cpp",
    '                 result["maxHops"] = store.Settings().maxHops;\n'
    '                 return result;\n',
    '                 result["maxHops"] = store.Settings().maxHops;\n'
    '                 result["autoHandoff"] = store.Settings().autoHandoff;\n'
    '                 return result;\n')

replace_once(
    "engine/team/TeamCommands.cpp",
    '             "Give exactly one teammate a task. The `to` field is the recipient; task text may mention that same teammate but no other teammate or @all.",\n',
    '             "Give exactly one teammate a task. The `to` field is the only recipient; @mentions inside task are context text and do not wake other agents.",\n')

replace_once(
    "engine/team/TeamCommands.cpp",
    '                 const std::string text = BuildTeamHandoffText(state->store, a["to"].asString(), a["task"].asString());\n'
    '                 Json result = session.Send(text, from, attachments, hop);\n',
    '                 Json result = session.Send(a["task"].asString(), from, attachments, hop, a["to"].asString());\n')

replace_optional(
    "engine/team/TeamCommands.h",
    '\n// Builds the single-recipient chat text used by team.handoff. The target is resolved by id/name;\n'
    '// task text may not contain another team mention because team.send routes every mention it sees.\n'
    '// Kept public so routing semantics have a small unit test independent of a real agent CLI.\n'
    'std::string BuildTeamHandoffText(const TeamStore& store, const std::string& to, const std::string& task);\n',
    '')
replace_optional("engine/team/TeamCommands.h", '#include <string>\n', '')

# ---------------------------------------------------------------------------
# Focused no-CLI test: settings + true direct routing even when task has mentions.
# ---------------------------------------------------------------------------
write("tests/team_efficiency_tests.cpp", r'''// Focused tests for low-cost agent-team routing; no real agent CLI is required.
#include <cstdio>
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
}  // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path project = fs::temp_directory_path() / "ownengine_team_efficiency_tests";
    std::error_code ec;
    fs::remove_all(project, ec);
    fs::create_directories(project, ec);
    CHECK(!ec);

    TeamStore store;
    std::string error;
    CHECK(store.Open(project.u8string(), &error));
    TeamStore::BackendLookup lookup = [](const std::string& backend, std::string& defaultModel) {
        defaultModel.clear();
        return backend == "fake";
    };
    store.Add(Agent("Core"), lookup);
    store.Add(Agent("QA"), lookup);

    CHECK(!store.Settings().autoHandoff);
    CHECK(!store.ToJson()["autoHandoff"].asBool());
    Json settings = Json::MakeObject();
    settings["autoHandoff"] = true;
    store.SetSettings(settings);
    CHECK(store.Settings().autoHandoff && store.ToJson()["autoHandoff"].asBool());
    settings["autoHandoff"] = false;
    store.SetSettings(settings);

    // No backend executable is needed to test routing: Send stores the addressed message before
    // reporting the target offline. directTo must win over every mention in the task text.
    const std::vector<Backend> backends;
    TeamHost host;
    TeamSession session(store, backends, host);
    session.Open();
    Json routed = session.Send("Review this; @qa and @all are context only.", "user", {}, 0, "Core");
    CHECK(routed["message"]["to"].size() == 1);
    CHECK(routed["message"]["to"][0].asString() == "core");

    bool unknown = false;
    try {
        session.Send("Do it", "user", {}, 0, "missing");
    } catch (const ApiError& e) {
        unknown = e.code == "unknown_agent";
    }
    CHECK(unknown);

    CommandRegistry commands;
    RegisterTeamCommands(commands, {});
    const Command* handoff = commands.Find("team.handoff");
    CHECK(handoff != nullptr);
    const Command* teamSettings = commands.Find("team.settings");
    CHECK(teamSettings != nullptr);
    if (handoff) {
        CHECK(handoff->params["properties"]["to"].isObject());
        CHECK(handoff->params["properties"]["task"].isObject());
    }
    if (teamSettings) CHECK(teamSettings->params["properties"]["autoHandoff"].isObject());

    fs::remove_all(project, ec);
    if (failures == 0) {
        std::printf("team efficiency tests passed\n");
        return 0;
    }
    std::printf("team efficiency tests failed: %d\n", failures);
    return 1;
}
''')

# ---------------------------------------------------------------------------
# Documentation. Keep these replacements tolerant because the compatibility
# handoff patch already updated some wording.
# ---------------------------------------------------------------------------
for path in ("AGENTS.md", "CLAUDE.md"):
    replace_optional(
        path,
        '`TeamSession` (a turn = the agent\'s CLI as a child process; routing by `@mention`/lead, states, `chat.jsonl`, conversation resume, cancel), `TeamCommands` (`team.backends`, `team.list/get/add/update/remove/move/avatar/presets/settings`, `team.send/cancel/state/messages/clear`;',
        '`TeamSession` (a turn = the agent\'s CLI as a child process; routing by `@mention`/lead or explicit handoff, states, `chat.jsonl`, conversation resume, cancel), `TeamCommands` (`team.backends`, `team.list/get/add/update/remove/move/avatar/presets/settings`, `team.send/handoff/cancel/state/messages/clear`;')

replace_optional(
    "docs/TEAM.md",
    '  "maxConcurrent": 3,\n  "maxHops": 4,\n',
    '  "maxConcurrent": 3,\n  "maxHops": 4,\n  "autoHandoff": false,\n')
replace_optional(
    "docs/TEAM.md",
    '| `maxHops` | 0..16, default 4 |\n',
    '| `maxHops` | 0..16, default 4; safety cap for agent-originated handoff chains |\n'
    '| `autoHandoff` | false by default. When true, mentions in a finished agent reply automatically start those teammates; explicit `team.handoff` works either way |\n')
replace_optional(
    "docs/TEAM.md",
    '3. The prompt given to the CLI = the chat messages (kind `message`, not its own) since that\n   agent\'s previous successful turn, bounded to the last 40 messages or 32 KiB, each as\n   `[Sender] text`.',
    '3. The prompt given to the CLI = only chat messages addressed to that agent (kind `message`, not its own) since that\n   agent\'s previous successful turn, bounded to the last 40 messages or 32 KiB, each as\n   `[Sender] text`. Unrelated team chat is not pushed into every turn; an attached agent can query `team.messages` when it needs more history.')

replace_optional(
    "docs/TEAM_EFFICIENCY.md",
    'oe exec <project> team.settings \'{"lead":"content","maxConcurrent":2,"maxHops":0}\'\n',
    'oe exec <project> team.settings \'{"lead":"content","maxConcurrent":2,"maxHops":2,"autoHandoff":false}\'\n')
replace_optional(
    "docs/TEAM_EFFICIENCY.md",
    '`maxHops: 0` still lets the person call any agent with `@core`, `@qa`, `@art`, or `team.handoff`; it prevents an agent turn from automatically starting another agent. Raise it temporarily when an agent itself must continue a short specialist chain.',
    '`autoHandoff: false` makes mentions in an agent reply non-executable. Use `team.handoff {to, task}` when a specialist really must continue. `maxHops` remains a short safety cap for explicit agent-originated chains rather than the on/off switch.')
replace_optional(
    "docs/TEAM_EFFICIENCY.md",
    'The command resolves `to` by agent id or name and sends one addressed turn through the existing `TeamSession`, so attachment handling, offline reporting, session resume, concurrency and `maxHops` stay unchanged. To guarantee a single recipient with the current mention-based router, the task may mention the selected teammate but rejects another teammate mention or `@all` with `ambiguous_handoff`.',
    'The command resolves `to` by agent id or name and routes directly to that one agent through `TeamSession`. Mentions inside `task` are context text only, so `@qa` or `@all` cannot accidentally wake extra agents. Attachment handling, offline reporting, session resume, concurrency and `maxHops` stay unchanged.')
replace_optional(
    "docs/TEAM_EFFICIENCY.md",
    'This is the first compatibility step toward separating human-readable chat from executable tasks. `team.send` and reply mentions keep their existing behavior for now; the next routing milestone makes automatic reply handoff opt-in and removes unrelated unread chat from prompts.',
    '`team.send` keeps mention/lead routing for messages from the person. Agent replies do not automatically wake mentioned teammates unless `autoHandoff` is enabled, and each turn receives only unread messages addressed to that agent.')
replace_optional(
    "docs/TEAM_EFFICIENCY.md",
    '4. Keep `maxHops` at `0` for normal work; enable short chains only for a specific task.',
    '4. Keep `autoHandoff` false for normal work; keep `maxHops` small as a safety cap for explicit chains.')

print("agent-team efficiency milestone 2 patch applied")
