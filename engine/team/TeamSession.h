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

namespace oe {

// Turns, agent states and the team chat of one project (docs/TEAM.md §5). A turn is one run of
// an agent's CLI as a child process; a reader thread per turn queues its output lines and
// Update() handles them on the calling (main) thread. Nothing here touches the simulation.

// What the hosting tool tells the team (set through the handle RegisterTeamCommands returns).
struct TeamHost {
    int apiPort = 0;                    // the tool's HTTP API port agents attach to with `oe mcp --connect`; 0 = none
    std::string oeExecutable;           // the `oe` program that serves MCP for agents; default: next to the running program
    double turnTimeoutSeconds = 1800.0; // a turn running longer is stopped and reported as an error
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
    // Executable paths are looked up once; call after a CLI was installed.
    void RefreshBackends() { executables_.clear(); ++revision_; }

    // {revision, running, agents: [{id, state, activity, task, elapsed, tools, error?, hint?}]}.
    Json State();
    // Messages with id > after (all = the newest `limit`), oldest first: {messages, first, last},
    // where first/last are the oldest and newest ids the log holds.
    Json Messages(uint64_t after, bool hasAfter, int limit);
    // True while one of these agents has a turn running or waiting (empty = any agent).
    bool Busy(const std::vector<std::string>& ids);

private:
    struct Turn;
    struct Runtime {
        std::unique_ptr<Turn> turn;
        bool pending = false;       // has unread messages addressed to it and waits for a turn
        std::string session;        // the backend's conversation id
        std::string sessionBackend; // backend that conversation belongs to
        uint64_t seen = 0;          // chat messages up to this id were given to the agent
        std::string lastError, lastHint;
    };

    const Backend* FindBackend(const std::string& id) const;
    const std::string& Executable(const Backend& backend);
    // Why the agent cannot take a turn now; empty when it can.
    std::string OfflineReason(const AgentProfile& agent, std::string* hint);
    ChatMessage& Append(ChatMessage message);
    void Notice(const std::string& text, const std::string& about);
    void Schedule();
    void StartTurn(const AgentProfile& agent, Runtime& runtime);
    void HandleLine(Turn& turn, const std::string& line, bool isStderr);
    void FinishTurn(const std::string& id, Runtime& runtime);
    void StopTurn(Runtime& runtime);
    void FailTurn(const std::string& id, Runtime& runtime, const std::string& message, const std::string& hint);
    std::string StandingContext(const AgentProfile& agent) const;
    void SaveSessions();
    // png/jpg files in .oe/team/images/ and assets/ with their modification times: compared
    // before and after a turn to find the images it produced.
    std::map<std::string, int64_t> ScanImages() const;
    ChatAttachment Describe(const std::string& relativePath) const;
    std::string Directory() const { return store_.Directory(); }

    TeamStore& store_;
    const std::vector<Backend>& backends_;
    const TeamHost& host_;
    std::string projectDir_;
    std::deque<ChatMessage> messages_;
    uint64_t lastMessageId_ = 0;
    std::map<std::string, Runtime> agents_;        // by agent id
    std::map<std::string, std::string> executables_;  // backend id -> resolved path ("" = not found)
    std::map<std::string, int64_t> claimedImages_;    // images already attached to a reply (parallel turns see the same folders)
    uint64_t revision_ = 1;
};

Json ChatMessageToJson(const ChatMessage& message);
// Agents a chat text addresses, as indices into `agents`: "@id" or "@name" (ASCII case-insensitive,
// the longest match wins, not inside a word). `all` is set by "@all".
std::vector<size_t> FindMentions(const std::string& text, const std::vector<AgentProfile>& agents, bool& all);

}  // namespace oe
