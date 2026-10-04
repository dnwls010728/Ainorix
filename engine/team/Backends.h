#pragma once
#include <functional>
#include <string>
#include <vector>

#include "core/Json.h"
#include "platform/Process.h"

namespace oe {

// Agent CLI backends of the agent team (docs/TEAM.md §4): which coding-agent programs exist,
// whether each is installed on this PC, and the models it offers.

struct ModelInfo {
    std::string id;         // what the CLI's model flag takes
    std::string label;      // shown in the model combo
    bool isDefault = false; // preselected for a new agent; at most one per backend
    int contextWindow = 0;  // tokens, 0 = unknown
};

// What a backend needs to start one turn (docs/TEAM.md §5.1). The process runs in the project
// directory, so file arguments are project-relative; the prompt goes to stdin.
struct TurnRequest {
    std::string executable;        // resolved path of the CLI
    std::string model;             // empty = the CLI's default
    std::string access = "edit";   // read | edit | full, mapped to the CLI's permission system
    std::string sessionId;         // the backend's own conversation id to resume; empty = new
    std::string systemPromptFile;  // standing context, for backends with `systemPromptFlag`
    std::string mcpConfigFile;     // {"mcpServers":{"ownengine":...}} file; empty = no editor to attach to
    std::string oeExecutable;      // the same MCP server for CLIs configured by flags: `<oe> mcp --connect <apiPort>`
    int apiPort = 0;               // 0 = no editor to attach to
    std::string agentId;           // passed on as `--agent <id>` so the editor shows who made an edit
};

// One thing a CLI reported on its event stream, in the backend-neutral form TeamSession uses.
struct TurnEvent {
    enum class Kind {
        Session,   // text = the backend's conversation id
        Thinking,  // the model is generating
        Tool,      // a tool call started: text = verb ("Reading"), detail = what, itemId = the call
        Text,      // text = assistant text (the last one is the reply when Done carries none)
        Done,      // the turn ended well: text = final reply (may be empty), costUsd when known
        Failed     // the turn ended badly: text = the CLI's message
    };
    Kind kind = Kind::Thinking;
    std::string text, detail, itemId;
    bool detailIsPath = false;  // detail is a file: shown relative to the project
    double costUsd = 0.0;
};
using TurnEvents = std::vector<TurnEvent>;

// One agent CLI: how to find it, what it offers, and (when it can run turns) its adapter.
struct Backend {
    std::string id;           // "claude"; stored in team.json
    std::string displayName;  // "Claude Code"
    std::string executable;   // program name searched in PATH, or a path
    std::string installHint;  // how to install the CLI, shown for a disabled entry
    std::vector<ModelInfo> models;  // built-in catalog, used when nothing better is known
    bool supportsImages = false;    // can produce image files
    // Optional: the models the installed CLI knows on this PC (its own cache or config).
    // Returns an empty list when unknown; the catalog is used then.
    std::vector<ModelInfo> (*discoverModels)() = nullptr;
    // Turn adapter; both empty = the backend can be detected but not run yet.
    bool systemPromptFlag = false;  // false: the standing context is sent on stdin before the first prompt of a conversation
    std::function<ProcessOptions(const TurnRequest&)> buildTurn;  // argv only: no user text (platform/Process.h)
    // One line of the CLI's stdout -> events. Lines it does not understand (other event types,
    // cut or broken JSON) give nothing: a stream never fails a turn by its shape.
    std::function<void(const std::string& line, TurnEvents& out)> parseLine;
};

// Detecting: `team.backends {async:true}` has not finished looking yet.
enum class BackendStatus { Installed, NotInstalled, Error, Unavailable, Detecting };
const char* BackendStatusName(BackendStatus status);

// What detection found for one backend on this PC.
struct BackendState {
    BackendStatus status = BackendStatus::NotInstalled;
    std::string version;  // "2.1.288"; empty when the CLI printed none
    std::string path;     // resolved executable, forward slashes
    std::string message;  // why it is not Installed
    std::vector<ModelInfo> models;  // discovered, else the catalog
    bool modelsDiscovered = false;
};

// Claude Code, Codex and Gemini CLI, in the order the editor lists them.
std::vector<Backend> BuiltinBackends();

// User-level overrides so a new model or a CLI in an unusual place needs no engine rebuild:
// {"claude": {"executable": "C:/tools/claude.exe", "supportsImages": true, "models": [{"id": "opus", "label": "Opus", "default": true}]}}.
// `models` replaces the catalog and turns discovery off for that backend. A missing file is not
// an error; an invalid file changes nothing and returns false.
bool ApplyBackendOverrides(const std::string& path, std::vector<Backend>& backends, std::string* error);
// <user config>/OwnEngine/backends.json (%APPDATA% on Windows); empty where there is no such folder.
std::string BackendOverridesPath();

// Finds the executable and runs `<executable> --version` (killed after timeoutSeconds). Being
// installed does not prove the user is logged in; that shows on the first turn.
BackendState DetectBackend(const Backend& backend, double timeoutSeconds = 10.0);
// All backends at once, one thread each; the result is in the order of `backends`.
std::vector<BackendState> DetectBackends(const std::vector<Backend>& backends, double timeoutSeconds = 10.0);

// The `team.backends` entry for one backend.
Json BackendToJson(const Backend& backend, const BackendState& state);

// First dotted number in a `--version` output: "codex-cli 0.159.3" -> "0.159.3"; empty if none.
std::string ParseCliVersion(const std::string& output);
// Model ids go on a command line: [A-Za-z0-9._:[]-]{1,64}.
bool ValidModelId(const std::string& id);
// The built-in adapters (docs/TEAM.md §4): headless command lines and JSON-lines parsers.
ProcessOptions ClaudeBuildTurn(const TurnRequest& request);
void ParseClaudeLine(const std::string& line, TurnEvents& out);
ProcessOptions CodexBuildTurn(const TurnRequest& request);
void ParseCodexLine(const std::string& line, TurnEvents& out);
// Conversation ids go on a command line: [A-Za-z0-9_-]{1,64}.
bool ValidSessionId(const std::string& id);

// Models from the text of Codex's `models_cache.json`: entries with visibility "list" in
// priority order, the first one as the default. Empty for text that is not such a file.
std::vector<ModelInfo> ParseCodexModelCache(const std::string& text);

}  // namespace oe
