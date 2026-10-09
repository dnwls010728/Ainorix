#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/Json.h"
#include "team/Backends.h"
#include "team/TeamStore.h"
#include "team/TeamUsage.h"
#include "team/TeamTasks.h"

namespace oe {

// Turns, agent states and the team chat of one project (docs/TEAM.md §5). A turn is one run of
// an agent's CLI as a child process; a reader thread per turn queues its output lines and
// Update() handles them on the calling (main) thread. Nothing here touches the simulation.

// What the hosting tool tells the team (set through the handle RegisterTeamCommands returns).
struct TeamHost {
    int apiPort = 0;                    // the tool's HTTP API port agents attach to with `oe mcp --connect`; 0 = none
    std::string oeExecutable;           // the `oe` program that serves MCP for agents; default: next to the running program
    double turnTimeoutSeconds = 1800.0; // a turn running longer is stopped and reported as an error
    double idleTimeoutSeconds = 600.0;  // stopped after this much silence on stdout and stderr
};

// A file that travels with a chat message: one the person attached, or an image an agent's
// turn produced. `path` is project-relative (attachments from outside are copied into
// `.oe/team/attachments/`).
struct ChatAttachment {
    std::string type = "file";  // image | file
    std::string path;
    int width = 0, height = 0;  // images, when the header could be read
};

// One line of `.oe/team/chat.jsonl` (docs/TEAM.md §5.2).
struct ChatMessage {
    uint64_t id = 0;
    std::string time;             // UTC, ISO 8601
    std::string from;             // "user" or an agent id
    std::vector<std::string> to;  // agent ids, or "user"
    std::string kind = "message"; // message | notice | error
    std::string text, hint;       // hint: how to fix an error
    int hop = 0;                  // 0 = from the person; an agent's reply is one more than what it answered
    bool hasTurn = false;         // an agent's reply: how the turn went
    double seconds = 0.0, costUsd = 0.0;
    bool costKnown = false;
    TokenUsage usage;
    int tools = 0;
    std::vector<std::string> steps;  // the turn's tool activity lines, bounded
    std::vector<ChatAttachment> attachments;
};

class TeamSession {
public:
    TeamSession(TeamStore& store, const std::vector<Backend>& backends, const TeamHost& host);
    ~TeamSession();  // stops every turn
    TeamSession(const TeamSession&) = delete;
    TeamSession& operator=(const TeamSession&) = delete;

    // Loads the chat log and stored conversations of the store's project; stops running turns.
    void Open();
    // Handles agent output, ends finished turns and starts waiting ones. Call once per frame;
    // every command below also calls it.
    void Update();
    // Posts a message and gives a turn to every agent it addresses: @id / @name mentions,
    // @all, otherwise the lead. Throws ApiError for an invalid text or sender.
    // Result: {message, started: [ids], queued: [ids], offline: [ids]}.
    // `attachments`: up to 8 files (any path on this PC, at most 32 MiB each) the agents are
    // pointed to; with attachments the text may be empty.
    // `hop`: 0 for the person; for a message an agent posts itself (team.send over the API) one
    // more than what its running turn answers. Above maxHops it is stored but starts no turn.
    Json Send(const std::string& text, const std::string& from, const std::vector<std::string>& attachments = {}, int hop = 0);
    // Hop of the message the agent's running turn answers; 0 when it has no turn.
    int TurnHop(const std::string& id) const;
    // Stops an agent's turn and drops its waiting message; `all` for everyone. Returns the ids stopped.
    std::vector<std::string> Cancel(const std::string& idOrName, bool all);
    // The agent left the team: stops its turn and forgets its conversation.
    void Forget(const std::string& id);
    // Stops every turn, empties the chat log and forgets all conversations.
    void Clear();
    // Drop one idle agent's backend conversation; chat, usage and other agents stay intact.
    void Reset(const std::string& idOrName);
    // Executable paths are looked up once; call after a CLI was installed.
    void RefreshBackends() { executables_.clear(); ++revision_; }

    // {revision, running, agents: [{id, state, activity, task, elapsed, tools, error?, hint?}]}.
    Json State();
    // Persisted agent/day totals in UTC; includes unknown-count turns explicitly.
    Json Usage(const std::string& agent = "", const std::string& date = "") const;
    // Messages with id > after (all = the newest `limit`), oldest first: {messages, first, last},
    // where first/last are the oldest and newest ids the log holds.
    Json Messages(uint64_t after, bool hasAfter, int limit);
    // True while one of these agents has a turn running or waiting (empty = any agent).
    bool Busy(const std::vector<std::string>& ids);
    // Nonblocking task entry point; sender is the authenticated remote agent or "user".
    Json Delegate(const Json& brief, const std::string& from);
    Json Report(const Json& report, const std::string& from);
    Json Task(const std::string& id) const { return tasks_.Get(id); }
    Json Tasks() const { return tasks_.List(); }

    // Direct conversations (docs/TEAM.md §7.4): one agent CLI and model chosen per conversation,
    // outside the roster and the team chat; the editor's "Ask an agent" bubbles keep one per
    // entity and CLI. `thread` is [a-z0-9_-]{1,64}; it resumes the CLI's own conversation.
    // `context` goes before the text in the prompt only (what the person points at). Throws
    // ApiError for an unknown or missing CLI, a busy thread or invalid text. Result: DirectState.
    Json Ask(const std::string& thread, const std::string& backend, const std::string& model, const std::string& text,
             const std::string& context);
    // {thread, backend, model, running, activity, elapsed, tools, revision, messages (oldest first)};
    // an unknown thread has no messages.
    Json DirectState(const std::string& thread);
    // Stops the thread's running turn; false when none was running.
    bool CancelDirect(const std::string& thread);
    // Stored threads: [{thread, backend, model, messages, running}].
    Json DirectThreads();

private:
    struct Turn;
    struct Runtime {
        std::unique_ptr<Turn> turn;
        bool pending = false;       // has unread messages addressed to it and waits for a turn
        std::string session;        // the backend's conversation id
        std::string sessionBackend; // backend that conversation belongs to
        uint64_t contextTokens = 0; // saved context estimate, used only when rotation is enabled
        uint64_t seen = 0;          // chat messages up to this id were given to the agent
        std::string lastError, lastHint;
    };

    struct Direct {
        std::string backend, model;             // the CLI and model of the next turn
        std::string session, sessionBackend;    // that CLI's conversation, resumed by the next turn
        std::vector<ChatMessage> messages;      // the conversation as shown, bounded
        uint64_t lastId = 0;
        std::unique_ptr<Turn> turn;
    };

    // Reads output a turn queued and stops one past its time; true when it is ready to finish.
    bool PumpTurn(Turn& turn, double now);
    Direct& LoadDirect(const std::string& thread);
    void SaveDirect(const std::string& thread, const Direct& direct);
    ChatMessage& AppendDirect(Direct& direct, ChatMessage message);
    void FinishDirectTurn(const std::string& thread, Direct& direct);
    void StopDirectTurn(Direct& direct);
    const Backend* FindBackend(const std::string& id) const;
    const std::string& Executable(const Backend& backend);
    // Why the agent cannot take a turn now; empty when it can.
    std::string OfflineReason(const AgentProfile& agent, std::string* hint);
    ChatMessage& Append(ChatMessage message);
    void Notice(const std::string& text, const std::string& about);
    void Schedule();
    void StartTurn(const AgentProfile& agent, Runtime& runtime, const Json* task = nullptr);
    void CancelTask(const std::string& id); // also cancels tasks delegated by this worker
    void HandleLine(Turn& turn, const std::string& line, bool isStderr);
    void FinishTurn(const std::string& id, Runtime& runtime);
    void StopTurn(Runtime& runtime);
    Json RecordTurn(const Turn& turn, const std::string& state);
    void FailTurn(const std::string& id, Runtime& runtime, const std::string& message, const std::string& hint);
    std::string StandingContext(const AgentProfile& agent) const;
    void SaveSessions();
    // png/jpg files in .oe/team/images/ and assets/ with their modification times: compared
    // before and after a turn to find the images it produced.
    std::map<std::string, int64_t> ScanImages() const;
    ChatAttachment Describe(const std::string& relativePath) const;
    std::string Directory() const { return store_.Directory(); }

    TeamUsage usage_;
    TeamStore& store_;
    const std::vector<Backend>& backends_;
    const TeamHost& host_;
    TeamTasks tasks_;
    std::string projectDir_;
    std::deque<ChatMessage> messages_;
    uint64_t lastMessageId_ = 0;
    std::map<std::string, Runtime> agents_;        // by agent id
    std::map<std::string, Direct> directs_;        // direct conversations by thread, loaded when used
    std::map<std::string, std::string> executables_;  // backend id -> resolved path ("" = not found)
    std::map<std::string, int64_t> claimedImages_;    // images already attached to a reply (parallel turns see the same folders)
    uint64_t revision_ = 1;
};

Json ChatMessageToJson(const ChatMessage& message);
// Agents a chat text addresses, as indices into `agents`: "@id" or "@name" (ASCII case-insensitive,
// the longest match wins, not inside a word). `all` is set by "@all".
std::vector<size_t> FindMentions(const std::string& text, const std::vector<AgentProfile>& agents, bool& all);

}  // namespace oe
