// Turn adapters of the built-in backends (docs/TEAM.md §4): the headless command line of each
// agent CLI and the parser of its JSON-lines event stream. Formats were recorded from
// Claude Code 2.1.288 (`-p --output-format stream-json --verbose`) and codex-cli 0.159.3
// (`exec --json`); the fixtures are in tests/tests.cpp (TeamStreamParsers).
#include "team/Backends.h"

namespace oe {
namespace {

TurnEvent Event(TurnEvent::Kind kind, std::string text = std::string()) {
    TurnEvent event;
    event.kind = kind;
    event.text = std::move(text);
    return event;
}

TurnEvent Tool(const std::string& itemId, std::string verb, std::string detail = std::string(), bool isPath = false) {
    TurnEvent event = Event(TurnEvent::Kind::Tool, std::move(verb));
    event.itemId = itemId;
    event.detail = std::move(detail);
    event.detailIsPath = isPath;
    return event;
}

// "component_set" -> "component.set": MCP tool names replace the dot of a command with '_'.
std::string EngineCommand(std::string tool) {
    const size_t underscore = tool.find('_');
    if (underscore != std::string::npos) tool[underscore] = '.';
    return tool;
}

// The entity or agent an engine command is about, when its arguments name one.
std::string EngineTarget(const Json& arguments) {
    for (const char* key : {"id", "name", "path"}) {
        const Json* value = arguments.find(key);
        if (value && value->isString()) return value->asString();
        if (value && value->isNumber()) return std::to_string(static_cast<long long>(value->asNumber()));
    }
    return std::string();
}

TurnEvent ClaudeTool(const Json& item) {
    const std::string name = item["name"].asString(""), id = item["id"].asString("");
    const Json& input = item["input"];
    if (name == "Read") return Tool(id, "Reading", input["file_path"].asString(""), true);
    if (name == "Edit" || name == "Write" || name == "MultiEdit") return Tool(id, "Editing", input["file_path"].asString(""), true);
    if (name == "NotebookEdit") return Tool(id, "Editing", input["notebook_path"].asString(""), true);
    if (name == "Grep" || name == "Glob") return Tool(id, "Searching", input["pattern"].asString(""));
    if (name == "Bash" || name == "PowerShell") return Tool(id, "Running", input["command"].asString(""));
    if (name == "WebSearch") return Tool(id, "Searching the web", input["query"].asString(""));
    if (name == "WebFetch") return Tool(id, "Fetching", input["url"].asString(""));
    if (name == "Task" || name == "Agent") return Tool(id, "Delegating", input["description"].asString(""));
    const std::string engine = "mcp__ownengine__";
    if (name.rfind(engine, 0) == 0) return Tool(id, "Engine:", EngineCommand(name.substr(engine.size())) + (EngineTarget(input).empty() ? "" : " " + EngineTarget(input)));
    return Tool(id, "Using", name);
}

}  // namespace

bool ValidSessionId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (char c : id) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

ProcessOptions ClaudeBuildTurn(const TurnRequest& request) {
    ProcessOptions options;
    options.executable = request.executable;
    options.arguments = {"-p", "--output-format", "stream-json", "--verbose"};
    auto add = [&](std::initializer_list<std::string> arguments) { options.arguments.insert(options.arguments.end(), arguments); };
    if (!request.model.empty()) add({"--model", request.model});
    if (!request.systemPromptFile.empty()) add({"--append-system-prompt-file", request.systemPromptFile});
    // docs/TEAM.md §6: a headless turn cannot ask, so the mode decides what is refused.
    add({"--permission-mode", request.access == "read" ? "plan" : request.access == "full" ? "bypassPermissions" : "acceptEdits"});
    if (!request.mcpConfigFile.empty()) {
        // Strict: the agent drives this editor, not another engine server from the user's own config.
        add({"--mcp-config", request.mcpConfigFile, "--strict-mcp-config"});
        if (request.access == "edit") add({"--allowedTools", "mcp__ownengine"});
    }
    if (!request.sessionId.empty()) add({"--resume", request.sessionId});
    return options;
}

void ParseClaudeLine(const std::string& line, TurnEvents& out) {
    std::string error;
    const Json j = Json::parse(line, &error);
    if (!error.empty() || !j.isObject()) return;
    const std::string type = j["type"].asString("");
    if (type == "system") {
        if (j["subtype"].asString("") == "init" && j["session_id"].isString()) out.push_back(Event(TurnEvent::Kind::Session, j["session_id"].asString()));
    } else if (type == "assistant") {
        const bool subagent = j["parent_tool_use_id"].isString();  // a delegated agent's words are not the reply
        for (const Json& item : j["message"]["content"].items()) {
            const std::string kind = item["type"].asString("");
            if (kind == "thinking") out.push_back(Event(TurnEvent::Kind::Thinking));
            else if (kind == "text" && !subagent) out.push_back(Event(TurnEvent::Kind::Text, item["text"].asString("")));
            else if (kind == "tool_use") out.push_back(ClaudeTool(item));
        }
    } else if (type == "user") {
        out.push_back(Event(TurnEvent::Kind::Thinking));  // a tool result went back to the model
    } else if (type == "result") {
        if (j["session_id"].isString()) out.push_back(Event(TurnEvent::Kind::Session, j["session_id"].asString()));
        const std::string text = j["result"].asString("");
        if (!j["is_error"].asBool(false) && j["subtype"].asString("") == "success") {
            TurnEvent done = Event(TurnEvent::Kind::Done, text);
            done.costUsd = j["total_cost_usd"].asNumber(0.0);
            out.push_back(std::move(done));
        } else {
            out.push_back(Event(TurnEvent::Kind::Failed, text.empty() ? "the turn ended with " + j["subtype"].asString("an error") : text));
        }
    }
}

ProcessOptions CodexBuildTurn(const TurnRequest& request) {
    ProcessOptions options;
    options.executable = request.executable;
    options.arguments = {"exec"};
    auto add = [&](std::initializer_list<std::string> arguments) { options.arguments.insert(options.arguments.end(), arguments); };
    const bool resume = !request.sessionId.empty();
    if (resume) add({"resume"});
    add({"--json", "--skip-git-repo-check"});
    if (!request.model.empty()) add({"-m", request.model});
    // Values are TOML; an npm-installed Codex is a .cmd shim that refuses double quotes, so
    // strings use TOML's single-quoted form. `exec resume` has no -s flag: the config key works for both.
    add({"-c", std::string("sandbox_mode='") + (request.access == "read" ? "read-only" : request.access == "full" ? "danger-full-access" : "workspace-write") + "'"});
    if (request.apiPort > 0 && !request.oeExecutable.empty()) {
        add({"-c", "mcp_servers.ownengine.command='" + request.oeExecutable + "'"});
        add({"-c", "mcp_servers.ownengine.args=['mcp','--connect','" + std::to_string(request.apiPort) + "'" +
                       (request.agentId.empty() ? "" : ",'--agent','" + request.agentId + "'") + "]"});
        // A headless turn cannot be asked: without this every engine tool call is refused.
        // A read-only agent keeps the default, so it cannot change the scene through the editor.
        if (request.access != "read") add({"-c", "mcp_servers.ownengine.default_tools_approval_mode='approve'"});
    }
    if (resume) add({request.sessionId});
    add({"-"});  // the prompt is read from stdin
    return options;
}

void ParseCodexLine(const std::string& line, TurnEvents& out) {
    std::string error;
    const Json j = Json::parse(line, &error);
    if (!error.empty() || !j.isObject()) return;
    const std::string type = j["type"].asString("");
    if (type == "thread.started") {
        if (j["thread_id"].isString()) out.push_back(Event(TurnEvent::Kind::Session, j["thread_id"].asString()));
    } else if (type == "turn.started") {
        out.push_back(Event(TurnEvent::Kind::Thinking));
    } else if (type == "item.started" || type == "item.completed") {
        const Json& item = j["item"];
        const std::string kind = item["type"].asString(""), id = item["id"].asString("");
        const bool completed = type == "item.completed";
        // A tool is reported when it starts and again when it ends (same id; TeamSession counts
        // it once), then the model continues.
        if (kind == "agent_message") {
            if (completed) out.push_back(Event(TurnEvent::Kind::Text, item["text"].asString("")));
        } else if (kind == "reasoning") {
            out.push_back(Event(TurnEvent::Kind::Thinking));
        } else if (kind == "command_execution") {
            out.push_back(Tool(id, "Running", item["command"].asString("")));
        } else if (kind == "file_change") {
            const Json& changes = item["changes"];
            out.push_back(Tool(id, "Editing", changes.size() > 0 && changes.isArray() ? changes[0]["path"].asString("") : std::string(), true));
        } else if (kind == "mcp_tool_call") {
            const std::string tool = item["tool"].asString("");
            if (item["server"].asString("") == "ownengine") out.push_back(Tool(id, "Engine:", EngineCommand(tool) + (EngineTarget(item["arguments"]).empty() ? "" : " " + EngineTarget(item["arguments"]))));
            else out.push_back(Tool(id, "Using", item["server"].asString("") + " " + tool));
        } else if (kind == "web_search") {
            out.push_back(Tool(id, "Searching the web", item["query"].asString("")));
        } else if (kind == "error") {
            return;  // a warning inside the turn; a failed turn ends with turn.failed
        } else {
            return;
        }
        if (completed && kind != "agent_message" && kind != "reasoning") out.push_back(Event(TurnEvent::Kind::Thinking));
    } else if (type == "turn.completed") {
        out.push_back(Event(TurnEvent::Kind::Done));
    } else if (type == "turn.failed") {
        out.push_back(Event(TurnEvent::Kind::Failed, j["error"]["message"].asString("the turn failed")));
    } else if (type == "error") {
        out.push_back(Event(TurnEvent::Kind::Failed, j["message"].asString("the CLI reported an error")));
    }
}

}  // namespace oe
