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


# TeamSession gets a structured recipient override. team.send keeps mention/lead routing.
replace_once(
    "engine/team/TeamSession.h",
    "    // `hop`: 0 for the person; for a message an agent posts itself (team.send over the API) one\n"
    "    // more than what its running turn answers. Above maxHops it is stored but starts no turn.\n"
    "    Json Send(const std::string& text, const std::string& from, const std::vector<std::string>& attachments = {}, int hop = 0);",
    "    // `hop`: 0 for the person; for a message an agent posts itself (team.send over the API) one\n"
    "    // more than what its running turn answers. Above maxHops it is stored but starts no turn.\n"
    "    // `directTo`: when non-empty, resolve exactly this agent id/name and ignore mentions in text.\n"
    "    Json Send(const std::string& text, const std::string& from, const std::vector<std::string>& attachments = {}, int hop = 0,\n"
    "              const std::string& directTo = std::string());")

replace_once(
    "engine/team/TeamSession.cpp",
    "Json TeamSession::Send(const std::string& text, const std::string& from, const std::vector<std::string>& attachments, int hop) {",
    "Json TeamSession::Send(const std::string& text, const std::string& from, const std::vector<std::string>& attachments, int hop, const std::string& directTo) {")

replace_once(
    "engine/team/TeamSession.cpp",
    "    const std::vector<AgentProfile>& roster = store_.Agents();\n"
    "    bool all = false;\n"
    "    std::vector<size_t> recipients = FindMentions(text, roster, all);\n"
    "    if (all) {\n"
    "        recipients.clear();\n"
    "        for (size_t i = 0; i < roster.size(); ++i) recipients.push_back(i);\n"
    "    } else if (recipients.empty()) {\n"
    "        for (size_t i = 0; i < roster.size(); ++i) {\n"
    "            if (roster[i].id == store_.Settings().lead) recipients.push_back(i);  // no mention: the lead\n"
    "        }\n"
    "    }\n",
    "    const std::vector<AgentProfile>& roster = store_.Agents();\n"
    "    std::vector<size_t> recipients;\n"
    "    if (!directTo.empty()) {\n"
    "        const AgentProfile* target = store_.Find(directTo);\n"
    "        if (!target) throw ApiError(\"unknown_agent\", \"no agent '\" + directTo + \"' in the team\", \"team.list shows the agents; use an id or a name.\");\n"
    "        for (size_t i = 0; i < roster.size(); ++i) {\n"
    "            if (roster[i].id == target->id) recipients.push_back(i);\n"
    "        }\n"
    "    } else {\n"
    "        bool all = false;\n"
    "        recipients = FindMentions(text, roster, all);\n"
    "        if (all) {\n"
    "            recipients.clear();\n"
    "            for (size_t i = 0; i < roster.size(); ++i) recipients.push_back(i);\n"
    "        } else if (recipients.empty()) {\n"
    "            for (size_t i = 0; i < roster.size(); ++i) {\n"
    "                if (roster[i].id == store_.Settings().lead) recipients.push_back(i);  // no mention: the lead\n"
    "            }\n"
    "        }\n"
    "    }\n")

# team.handoff now uses the structured recipient; task text is stored unchanged.
helper = '''std::string BuildTeamHandoffText(const TeamStore& store, const std::string& to, const std::string& task) {
    const AgentProfile* target = store.Find(to);
    if (!target) throw ApiError("unknown_agent", "no agent '" + to + "' in the team", "team.list shows the agents; use an id or a name.");

    bool all = false;
    const std::vector<size_t> mentioned = FindMentions(task, store.Agents(), all);
    for (size_t index : mentioned) {
        if (index >= store.Agents().size() || store.Agents()[index].id != target->id) {
            throw ApiError("ambiguous_handoff", "a handoff task cannot mention another teammate",
                           "Put context in plain text and use the `to` field as the only recipient.");
        }
    }
    if (all) {
        throw ApiError("ambiguous_handoff", "a handoff task cannot contain @all",
                       "Use one explicit recipient in the `to` field.");
    }
    return "@" + target->id + (task.empty() ? std::string() : " " + task);
}

'''
replace_once("engine/team/TeamCommands.cpp", helper, "")
replace_once(
    "engine/team/TeamCommands.cpp",
    "             \"Give exactly one teammate a task. The `to` field is the recipient; task text may mention that same teammate but no other teammate or @all.\",",
    "             \"Give exactly one teammate a task. The `to` field is the only recipient; @mentions inside task are context text and do not wake other agents.\",")
replace_once(
    "engine/team/TeamCommands.cpp",
    "                 const std::string text = BuildTeamHandoffText(state->store, a[\"to\"].asString(), a[\"task\"].asString());\n"
    "                 Json result = session.Send(text, from, attachments, hop);",
    "                 Json result = session.Send(a[\"task\"].asString(), from, attachments, hop, a[\"to\"].asString());")

replace_once(
    "engine/team/TeamCommands.h",
    "\n// Builds the single-recipient chat text used by team.handoff. The target is resolved by id/name;\n"
    "// task text may not contain another team mention because team.send routes every mention it sees.\n"
    "// Kept public so routing semantics have a small unit test independent of a real agent CLI.\n"
    "std::string BuildTeamHandoffText(const TeamStore& store, const std::string& to, const std::string& task);\n",
    "")

# Focused test: direct routing ignores mentions in task text and resolves id/name.
old = '''    CHECK(BuildTeamHandoffText(store, "Core", "Review the renderer") == "@core Review the renderer");
    CHECK(BuildTeamHandoffText(store, "core", "Ask @core to inspect it") == "@core Ask @core to inspect it");

    bool rejected = false;
    try {
        BuildTeamHandoffText(store, "Core", "Then ask @qa too");
    } catch (const ApiError& e) {
        rejected = e.code == "ambiguous_handoff";
    }
    CHECK(rejected);

    rejected = false;
    try {
        BuildTeamHandoffText(store, "Core", "Ask @all");
    } catch (const ApiError& e) {
        rejected = e.code == "ambiguous_handoff";
    }
    CHECK(rejected);

    rejected = false;
    try {
        BuildTeamHandoffText(store, "missing", "Do it");
    } catch (const ApiError& e) {
        rejected = e.code == "unknown_agent";
    }
    CHECK(rejected);
'''
new = '''    const std::vector<Backend> noBackends;
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
'''
replace_once("tests/team_efficiency_tests.cpp", old, new)

# Main command-path regression test required by docs/DESIGN.md.
anchor = '''    // Agent to agent: a teammate mentioned in a reply gets a turn; the chain a person's message
    // starts is cut after maxHops such turns.
'''
insert = '''    // Explicit handoff is structural: mentions in task text are context, not extra recipients.
    sent = Call(e, "team.handoff", R"J({"to":"Jun","task":"Review @mina and @all as context."})J");
    CHECK(sent["ok"].asBool() && sent["result"]["message"]["text"].asString() == "Review @mina and @all as context.");
    CHECK(sent["result"]["message"]["to"].size() == 1 && sent["result"]["message"]["to"][0].asString() == "jun");
    CHECK(sent["result"]["started"].size() == 1 && sent["result"]["started"][0].asString() == "jun" && until(settled));
    CHECK(fileText("jun.prompt.txt").find("[User] Review @mina and @all as context.") != std::string::npos);

'''
replace_once("tests/tests.cpp", anchor, insert + anchor)

# Documentation: handoff no longer encodes routing in chat text.
replace_once(
    "docs/TEAM_EFFICIENCY.md",
    "The command resolves `to` by agent id or name and sends one addressed turn through the existing `TeamSession`, so attachment handling, offline reporting, session resume, concurrency and `maxHops` stay unchanged. To guarantee a single recipient with the current mention-based `TeamSession::Send` router, the task may mention the selected teammate but rejects another teammate mention or `@all` with `ambiguous_handoff`.",
    "The command resolves `to` by agent id or name and passes it as a structured recipient to `TeamSession::Send`. The task text is stored unchanged, so `@qa`, `@all`, or any other mention inside the task is context only and cannot widen the recipient set. Attachment handling, offline reporting, session resume, concurrency and `maxHops` stay unchanged.")
replace_once(
    "docs/TEAM_EFFICIENCY.md",
    "The runner configures the normal repository root and injects only the focused test target through `CMAKE_PROJECT_TOP_LEVEL_INCLUDES`; this preserves the root `CMAKE_SOURCE_DIR` assumptions used by the engine build. The test verifies the lean environment switch, id/name resolution, one-recipient routing text, rejection of another teammate / `@all`, unknown-agent handling, and registration of the `team.handoff` command.",
    "The runner configures the normal repository root and injects only the focused test target through `CMAKE_PROJECT_TOP_LEVEL_INCLUDES`; this preserves the root `CMAKE_SOURCE_DIR` assumptions used by the engine build. The test verifies the lean environment switch, id/name resolution, structured one-recipient routing even when task text contains other mentions or `@all`, unknown-agent handling, and registration of the `team.handoff` command.")
replace_once(
    "docs/TEAM_EFFICIENCY.md",
    "- split human-readable `ChatMessage` from executable `AgentTask`, so explicit routing no longer needs to encode the recipient as an `@mention` in stored chat text;",
    "- split human-readable `ChatMessage` from executable `AgentTask`, so scheduling, budgets and permissions can be attached to a task independently of the stored chat message;")

replace_once(
    "docs/TEAM.md",
    "| `team.send` | `{text, attachments?, from?, wait?}` | `{message, started: [ids], queued: [ids], offline: [ids]}`; with `wait` also `replies` and `state` |\n",
    "| `team.send` | `{text, attachments?, from?, wait?}` | `{message, started: [ids], queued: [ids], offline: [ids]}`; with `wait` also `replies` and `state` |\n"
    "| `team.handoff` | `{to, task, attachments?, from?, wait?}` | same delivery result as `team.send`, but exactly one `to` agent is addressed; mentions in `task` are context text only |\n")
replace_once(
    "docs/TEAM.md",
    "  activity of a running turn, queueing behind a busy agent and behind `maxConcurrent`, cancel,\n",
    "  activity of a running turn, structured single-recipient `team.handoff` (task mentions do not widen routing), queueing behind a busy agent and behind `maxConcurrent`, cancel,\n")

# Keep the agent instruction mirrors identical and command map current.
for path in ("AGENTS.md", "CLAUDE.md"):
    replace_once(
        path,
        "`TeamSession` (a turn = the agent's CLI as a child process; routing by `@mention`/lead, states, `chat.jsonl`, conversation resume, cancel), `TeamCommands` (`team.backends`, `team.list/get/add/update/remove/move/avatar/presets/settings`, `team.send/cancel/state/messages/clear`;",
        "`TeamSession` (a turn = the agent's CLI as a child process; routing by `@mention`/lead or explicit single-recipient handoff, states, `chat.jsonl`, conversation resume, cancel), `TeamCommands` (`team.backends`, `team.list/get/add/update/remove/move/avatar/presets/settings`, `team.send/handoff/cancel/state/messages/clear`;")

print("direct handoff patch applied")
