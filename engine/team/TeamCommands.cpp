#include "team/TeamCommands.h"

#include <chrono>
#include <future>
#include <memory>

#include "app/Engine.h"
#include "core/Log.h"
#include "platform/Platform.h"
#include "team/TeamStore.h"

namespace oe {
namespace {

// State shared by the team commands of one registry (one tool session).
struct TeamState {
    std::vector<Backend> backends;
    std::vector<BackendState> detected;  // parallel to `backends` once `hasDetected`
    bool hasDetected = false;
    std::future<std::vector<BackendState>> detecting;  // valid while a background detection runs
    std::shared_ptr<TeamHandle> handle = std::make_shared<TeamHandle>();
    TeamStore store;
    TeamSession session{store, backends, handle->host};  // after store and backends: it refers to them

    // The team of the engine's project, read again when the project or the file changed.
    TeamStore& Store(Engine& e) {
        if (e.ProjectDir().empty()) throw ApiError("no_project", "no project is open", "Open a project directory first; a team belongs to a project.");
        if (store.ProjectDir() != e.ProjectDir()) {
            std::string error;
            if (!store.Open(e.ProjectDir(), &error)) OE_LOG_WARN("team", "%s", error.c_str());
        } else {
            store.Refresh();
        }
        return store;
    }

    // Takes the result of a background detection: when it is ready, or (wait) blocking until it is.
    void CollectDetection(bool wait) {
        if (!detecting.valid()) return;
        if (!wait && detecting.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        detected = detecting.get();
        hasDetected = true;
        session.RefreshBackends();
    }

    // The store and the session of the engine's project, with agent output handled up to now.
    TeamSession& Session(Engine& e) {
        Store(e);
        session.Update();
        return session;
    }

    // Default model of a backend: what the installed CLI reported when detection already ran
    // (it is not started for this), else the built-in catalog.
    TeamStore::BackendLookup Lookup() const {
        return [this](const std::string& id, std::string& defaultModel) {
            for (size_t i = 0; i < backends.size(); ++i) {
                if (backends[i].id != id) continue;
                for (const ModelInfo& model : hasDetected ? detected[i].models : backends[i].models) {
                    if (model.isDefault) defaultModel = model.id;
                }
                return true;
            }
            return false;
        };
    }
};

void Register(CommandRegistry& r, const char* name, const char* summary, Json params, std::function<Json(Engine&, const Json&)> fn) {
    Command c;
    c.name = name;
    c.summary = summary;
    c.params = std::move(params);
    c.mutates = false;  // the team is not part of the scene: no undo step, no scene revision
    c.run = std::move(fn);
    r.Add(std::move(c));
}

const char* const kAgentRef = "Agent id or name.";

}  // namespace

std::shared_ptr<TeamHandle> RegisterTeamCommands(CommandRegistry& registry) {
    std::vector<Backend> backends = BuiltinBackends();
    std::string error;
    if (!ApplyBackendOverrides(BackendOverridesPath(), backends, &error)) OE_LOG_WARN("team", "ignoring backend overrides: %s", error.c_str());
    return RegisterTeamCommands(registry, std::move(backends));
}

std::shared_ptr<TeamHandle> RegisterTeamCommands(CommandRegistry& registry, std::vector<Backend> backends) {
    auto state = std::make_shared<TeamState>();
    state->backends = std::move(backends);
    // The commands own the state; the handle must not keep it alive past the registry.
    state->handle->update = [weak = std::weak_ptr<TeamState>(state)] {
        if (std::shared_ptr<TeamState> team = weak.lock()) team->session.Update();
    };

    Register(registry, "team.backends",
             "Agent CLIs (Claude Code, Codex, ...) with whether each is installed on this PC, its version, path, models and install hint. "
             "Detection runs once per session (it starts `<cli> --version`, a few seconds in total).",
             Params()
                 .Opt("refresh", "boolean", "Detect again, e.g. after installing a CLI (default false).")
                 .Opt("async", "boolean", "Do not wait: detection runs in the background and entries report status \"detecting\" until a later call finds it done "
                                          "(what the editor does, so its window never freezes; default false)."),
             [state](Engine&, const Json& a) {
                 const bool async = a["async"].asBool(false);
                 state->CollectDetection(!async);
                 if ((!state->hasDetected || a["refresh"].asBool(false)) && !state->detecting.valid()) {
                     if (async) {
                         state->detecting = std::async(std::launch::async, [backends = state->backends] { return DetectBackends(backends); });
                     } else {
                         state->detected = DetectBackends(state->backends);
                         state->hasDetected = true;
                         state->session.RefreshBackends();
                     }
                 }
                 const bool busy = state->detecting.valid();
                 Json list = Json::MakeArray();
                 for (size_t i = 0; i < state->backends.size(); ++i) {
                     BackendState pending;
                     pending.status = BackendStatus::Detecting;
                     pending.models = state->backends[i].models;
                     list.push(BackendToJson(state->backends[i], busy ? pending : state->detected[i]));
                 }
                 return list;
             });

    Register(registry, "team.presets", "Names of the built-in profile pictures (use as avatar \"preset:<name>\").", Params(), [](Engine&, const Json&) {
        Json list = Json::MakeArray();
        for (const std::string& preset : TeamAvatarPresets()) list.push(preset);
        return list;
    });

    Register(registry, "team.list", "The project's agent team: settings (lead, maxConcurrent, maxHops), a revision that changes with every edit, and all agent profiles.",
             Params(), [state](Engine& e, const Json&) {
                 TeamStore& store = state->Store(e);
                 Json result = store.ToJson();
                 if (!store.LoadError().empty()) result["error"] = store.LoadError();  // the file is invalid: shown empty, not editable
                 return result;
             });

    Register(registry, "team.get", "One agent profile.", Params().Req("id", "string", kAgentRef), [state](Engine& e, const Json& a) {
        TeamStore& store = state->Store(e);
        const AgentProfile* agent = store.Find(a["id"].asString());
        if (!agent) throw ApiError("unknown_agent", "no agent '" + a["id"].asString() + "' in the team", "team.list shows the agents; use an id or a name.");
        return store.AgentToJson(*agent);
    });

    Register(registry, "team.add",
             "Add an agent to the team (at most 16). The id is generated from the name and never changes; the first agent becomes the lead. "
             "Stored in .oe/team/team.json, local to this checkout.",
             Params()
                 .Req("name", "string", "Display name, 1..40 characters, unique.")
                 .Opt("description", "string", "One or two lines: what the agent is for (shown in the roster and told to teammates), at most 400 bytes.")
                 .Opt("instructions", "string", "Standing instructions added to the agent's system prompt, at most 16 KiB.")
                 .Opt("backend", "string", "Agent CLI id from team.backends (default: the first one).")
                 .Opt("model", "string", "Model id of that backend; \"\" = the CLI's default (default: the backend's default model).")
                 .Opt("access", "string", "read | edit (default) | full: what the agent may do without asking.")
                 .Opt("avatar", "string", "\"preset:<name>\" from team.presets (default: the first preset no teammate uses)."),
             [state](Engine& e, const Json& a) {
                 TeamStore& store = state->Store(e);
                 Json values = a;
                 if (!values.has("backend") && !state->backends.empty()) values["backend"] = state->backends.front().id;
                 return store.AgentToJson(store.Add(values, state->Lookup()));
             });

    Register(registry, "team.update",
             "Change some fields of an agent (partial update). Changing the backend without a model resets the model to the new backend's default.",
             Params().Req("id", "string", kAgentRef).Req("values", "object", "Fields to change: name, description, instructions, backend, model, access, avatar (\"preset:<name>\")."),
             [state](Engine& e, const Json& a) {
                 TeamStore& store = state->Store(e);
                 return store.AgentToJson(store.Update(a["id"].asString(), a["values"], state->Lookup()));
             });

    Register(registry, "team.remove", "Remove an agent and its uploaded picture. If it was the lead, the team has no lead afterwards.",
             Params().Req("id", "string", kAgentRef), [state](Engine& e, const Json& a) {
                 Json result = Json::MakeObject();
                 const std::string id = state->Store(e).Remove(a["id"].asString());
                 state->session.Forget(id);  // stops its turn, drops its conversation
                 result["removed"] = id;
                 return result;
             });

    Register(registry, "team.move", "Move an agent to another place in the roster (the order agents are listed and given turns).",
             Params().Req("id", "string", kAgentRef).Req("index", "integer", "New position, 0 = first; larger than the team = last."), [state](Engine& e, const Json& a) {
                 if (a["index"].asNumber() < 0) throw ApiError("invalid_argument", "index must not be negative");
                 TeamStore& store = state->Store(e);
                 store.Move(a["id"].asString(), static_cast<size_t>(a["index"].asNumber()));
                 return store.ToJson();
             });

    Register(registry, "team.avatar",
             "Set an agent's profile picture: a built-in preset, or an image file that is center-cropped to a square and stored as a 256x256 PNG in .oe/team/avatars/.",
             Params()
                 .Req("id", "string", kAgentRef)
                 .Opt("preset", "string", "Preset name from team.presets.")
                 .Opt("source", "string", "Path of a .png/.jpg/.jpeg file (may be outside the project; it is only read). At most 16 MiB and 8192 px per side."),
             [state](Engine& e, const Json& a) {
                 TeamStore& store = state->Store(e);
                 if (a.has("preset") == a.has("source")) throw ApiError("invalid_argument", "pass exactly one of preset and source", "team.presets lists the presets.");
                 const std::string id = a["id"].asString();
                 return store.AgentToJson(a.has("preset") ? store.SetAvatarPreset(id, a["preset"].asString()) : store.SetAvatarFile(id, a["source"].asString()));
             });

    Register(registry, "team.settings", "Read or change the team settings; without arguments it only reads them.",
             Params()
                 .Opt("lead", "string", "Agent (id or name) that receives messages without a mention; \"\" = nobody.")
                 .Opt("maxConcurrent", "integer", "Agent turns that may run at the same time, 1..8 (default 3).")
                 .Opt("maxHops", "integer", "Chained agent-to-agent turns one user message may cause, 0..16 (default 4)."),
             [state](Engine& e, const Json& a) {
                 TeamStore& store = state->Store(e);
                 if (a.size() > 0) store.SetSettings(a);
                 Json result = Json::MakeObject();
                 result["lead"] = store.Settings().lead;
                 result["maxConcurrent"] = store.Settings().maxConcurrent;
                 result["maxHops"] = store.Settings().maxHops;
                 return result;
             });

    Register(registry, "team.send",
             "Post a message to the team chat and give a turn to every agent it addresses: @id or @name mentions, @all, otherwise the lead. "
             "A turn runs the agent's CLI as a child process; follow it with team.state and read the reply with team.messages.",
             Params()
                 .Req("text", "string", "The message, at most 32 KiB (may be empty when files are attached). Mention agents as @id or @name.")
                 .OptWith("attachments", Json::parse(R"J({"type":"array","items":{"type":"string"},"maxItems":8,"description":"Files the agents should look at: paths on this PC or relative to the project, at most 8, 32 MiB each. A file outside the project is copied into .oe/team/attachments/."})J"))
                 .Opt("from", "string", "Sender: \"user\" (default) or an agent id.")
                 .Opt("wait", "number", "Block up to this many seconds until the addressed agents have finished, then also return the new messages and the state "
                                        "(for one-shot `oe exec`; default 0 = return at once)."),
             [state](Engine& e, const Json& a) {
                 TeamSession& session = state->Session(e);
                 std::vector<std::string> attachments;
                 for (const Json& path : a["attachments"].items()) {
                     if (!path.isString()) throw ApiError("invalid_argument", "attachments must be file paths (strings)");
                     attachments.push_back(path.asString());
                 }
                 // A team agent that posts through the API (oe mcp/exec --connect --agent <id>) speaks as
                 // itself, whatever `from` says, and its message continues the chain of its running
                 // turn: agents cannot reset the hop count by calling the command.
                 std::string from = a["from"].asString("user");
                 int hop = 0;
                 if (!e.RemoteCallAgent().empty() && state->store.Find(e.RemoteCallAgent()) && state->store.Find(e.RemoteCallAgent())->id == e.RemoteCallAgent()) {
                     from = e.RemoteCallAgent();
                     hop = session.TurnHop(from) + 1;
                 }
                 Json result = session.Send(a["text"].asString(), from, attachments, hop);
                 const double wait = a["wait"].asNumber(0.0);
                 if (wait > 0.0) {
                     std::vector<std::string> ids;
                     for (const char* key : {"started", "queued"}) {
                         for (const Json& id : result[key].items()) ids.push_back(id.asString());
                     }
                     const double deadline = PlatformTimeSeconds() + wait;
                     while (!ids.empty() && session.Busy(ids) && PlatformTimeSeconds() < deadline) {
                         PlatformSleep(0.01);
                         session.Update();
                     }
                     result["replies"] = session.Messages(static_cast<uint64_t>(result["message"]["id"].asNumber()), true, 500)["messages"];
                     result["state"] = session.State();
                 }
                 return result;
             });

    Register(registry, "team.cancel", "Stop an agent's running turn (its process tree is killed) and drop its waiting message; the chat gets a notice.",
             Params().Opt("id", "string", kAgentRef).Opt("all", "boolean", "Stop every agent."), [state](Engine& e, const Json& a) {
                 TeamSession& session = state->Session(e);
                 const bool all = a["all"].asBool(false);
                 if (all == a.has("id")) throw ApiError("invalid_argument", "pass an agent id, or all:true");
                 Json cancelled = Json::MakeArray();
                 for (const std::string& id : session.Cancel(a["id"].asString(""), all)) cancelled.push(id);
                 Json result = Json::MakeObject();
                 result["cancelled"] = std::move(cancelled);
                 return result;
             });

    Register(registry, "team.state",
             "Live state of every agent: offline | idle | queued | thinking | working | error, the activity line, the task, elapsed seconds and tool count. "
             "`revision` changes whenever the roster, a state or the chat changed.",
             Params(), [state](Engine& e, const Json&) { return state->Session(e).State(); });

    Register(registry, "team.messages", "Team chat messages, oldest first: {messages, first, last} (first = oldest id still in the log, 0 when it is empty). kind is message, notice or error; an agent's reply carries its turn summary.",
             Params().Opt("after", "integer", "Only messages with a larger id (poll with the previous `last`).").Opt("limit", "integer", "At most this many, 1..500 (default 100)."),
             [state](Engine& e, const Json& a) {
                 const int limit = a["limit"].asInt(100);
                 if (limit < 1 || limit > 500 || a["after"].asNumber(0.0) < 0.0) throw ApiError("invalid_argument", "limit must be 1..500 and after must not be negative");
                 return state->Session(e).Messages(static_cast<uint64_t>(a["after"].asNumber(0.0)), a.has("after"), limit);
             });

    Register(registry, "team.clear", "Stop every turn, empty the chat log and forget the agents' stored conversations. Profiles are kept.", Params(),
             [state](Engine& e, const Json&) {
                 state->Session(e).Clear();
                 return Json::MakeObject();
             });

    return state->handle;
}

}  // namespace oe
