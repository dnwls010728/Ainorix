#include "team/TeamSession.h"
#include "team/TeamContext.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>

#include "api/Commands.h"
#include "assets/Assets.h"
#include "core/FileSystem.h"
#include "core/Log.h"
#include "platform/Platform.h"

namespace oe {
namespace {

constexpr size_t kMaxStoredMessages = 2000;   // chat.jsonl is trimmed to this when a project opens
constexpr size_t kMaxSendBytes = 32 * 1024;   // one message typed into the chat
constexpr size_t kMaxReplyBytes = 64 * 1024;  // an agent's reply as stored
constexpr size_t kMaxPromptMessages = 40;     // unread messages handed to an agent ...
constexpr size_t kMaxPromptBytes = 32 * 1024; // ... and their total size (the oldest are dropped)
constexpr size_t kMaxSteps = 100;
constexpr size_t kMaxActivityBytes = 80;
constexpr size_t kMaxSendAttachments = 8;
constexpr uintmax_t kMaxAttachmentBytes = 32 * 1024 * 1024;
constexpr size_t kMaxTurnImages = 16;         // images one reply may carry
constexpr size_t kMaxScannedImages = 4000;    // files looked at when searching for a turn's images
constexpr double kExitGraceSeconds = 5.0;     // after the final event, before the process tree is killed

std::string Lower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

// Part of a word for mention boundaries: ASCII letters, digits, '_', '-' and any non-ASCII byte.
bool IsWordByte(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return u >= 0x80 || (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || u == '_' || u == '-';
}

// Cuts at a character boundary and marks the cut.
std::string Shorten(const std::string& text, size_t maxBytes) {
    if (text.size() <= maxBytes) return text;
    size_t end = maxBytes > 3 ? maxBytes - 3 : 0;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    return text.substr(0, end) + "...";
}

std::string FirstLine(const std::string& text) {
    const size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return std::string();
    const size_t end = text.find_first_of("\r\n", start);
    return text.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

std::string NowUtc() {
    const std::time_t now = std::time(nullptr);
    char buffer[32] = "";
    if (const std::tm* utc = std::gmtime(&now)) std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", utc);
    return buffer;
}

// "Reading scripts/player.lua", "Running `build.bat`", "Engine: component.set Player".
std::string ActivityLine(const TurnEvent& event, const std::string& projectDir) {
    std::string detail = FirstLine(event.detail);
    if (event.detailIsPath) {
        std::replace(detail.begin(), detail.end(), '\\', '/');
        std::string root = projectDir;
        std::replace(root.begin(), root.end(), '\\', '/');
        if (!root.empty() && root.back() != '/') root += '/';
        if (detail.size() > root.size() && Lower(detail.substr(0, root.size())) == Lower(root)) detail = detail.substr(root.size());
    }
    if (detail.empty()) return event.text;
    if (event.text == "Running") return "Running `" + Shorten(detail, kMaxActivityBytes) + "`";
    return event.text + " " + Shorten(detail, kMaxActivityBytes);
}

bool IsImagePath(const std::string& path) {
    const std::string lowered = Lower(path);
    for (const char* extension : {".png", ".jpg", ".jpeg"}) {
        const size_t length = std::strlen(extension);
        if (lowered.size() > length && lowered.compare(lowered.size() - length, length, extension) == 0) return true;
    }
    return false;
}

const AgentProfile* FindById(const TeamStore& store, const std::string& id) {
    for (const AgentProfile& agent : store.Agents()) {
        if (agent.id == id) return &agent;
    }
    return nullptr;
}

std::string BlockedMentionNotice(const TeamStore& store, const std::string& from, const std::vector<size_t>& targets) {
    const AgentProfile* caller = FindById(store, from);
    std::string names, callableNames;
    for (size_t i : targets) {
        const AgentProfile& target = store.Agents()[i];
        names += (names.empty() ? "" : ", ") + target.name;
        if (caller && target.id != from && (from == store.Settings().lead ||
            std::find(caller->canCall.begin(), caller->canCall.end(), target.id) != caller->canCall.end())) {
            callableNames += (callableNames.empty() ? "" : ", ") + target.name;
        }
    }
    std::string notice = "Not handed on to " + names + ": this chain already has " + std::to_string(store.Settings().maxHops) +
        " agent-to-agent turns (maxHops). Enable with team.settings {maxHops: 2}, or mention them yourself to continue.";
    if (!callableNames.empty()) notice += " To hand work to " + callableNames + ", call team_delegate; mentions are not delivered.";
    return notice;
}

void CheckSessionsFile(const std::string& path) {
    if (!FileExists(path)) return;
    std::error_code ec; const auto size = std::filesystem::file_size(std::filesystem::u8path(path), ec);
    std::string bytes, error;
    if (ec || size > 1024 * 1024 || !ReadTextFile(path, bytes)) throw ApiError("session_file_invalid", "cannot read sessions.json", "Repair .oe/team/sessions.json before resetting or saving conversations.");
    const Json j = Json::parse(bytes, &error);
    if (!error.empty() || !j.isObject()) throw ApiError("session_file_invalid", "invalid sessions.json", "Repair the file; unreadable sessions are never overwritten.");
    for (const auto& member : j.members()) {
        const Json& row = member.second;
        const double seen = row["seen"].asNumber(0);
        if (!row.isObject() || (row.has("seen") && !row["seen"].isNumber()) || seen < 0 || seen > 9007199254740991.0 || std::floor(seen) != seen ||
            (row.has("session") && (!row["session"].isString() || !ValidSessionId(row["session"].asString()))) ||
            (row.has("backend") && !row["backend"].isString())) throw ApiError("session_file_invalid", "invalid saved conversation", "Repair sessions.json before changing it.");
    }
}

bool MessageFromJson(const Json& j, ChatMessage& message) {
    if (!j.isObject() || !j["id"].isNumber() || !j["from"].isString() || !j["text"].isString()) return false;
    message.id = static_cast<uint64_t>(std::max(0.0, j["id"].asNumber()));
    message.time = j["time"].asString("");
    message.from = j["from"].asString();
    for (const Json& to : j["to"].items()) message.to.push_back(to.asString(""));
    message.kind = j["kind"].asString("message");
    message.hop = std::max(0, j["hop"].asInt(0));
    message.text = j["text"].asString();
    message.hint = j["hint"].asString("");
    if (const Json* turn = j.find("turn")) {
        message.hasTurn = true;
        message.seconds = (*turn)["seconds"].asNumber(0.0);
        message.tools = (*turn)["tools"].asInt(0);
        message.costUsd = (*turn)["costUsd"].asNumber(0.0);
        message.costKnown = (*turn)["costKnown"].asBool(false);
        message.usage = TokenUsageFromJson((*turn)["usage"]);
        for (const Json& step : (*turn)["steps"].items()) message.steps.push_back(step.asString(""));
    }
    for (const Json& item : j["attachments"].items()) {
        ChatAttachment attachment;
        attachment.type = item["type"].asString("file");
        attachment.path = item["path"].asString("");
        attachment.width = item["width"].asInt(0);
        attachment.height = item["height"].asInt(0);
        // Only paths inside the project are ever shown or opened.
        if (!attachment.path.empty() && attachment.path.find("..") == std::string::npos && attachment.path.find(':') == std::string::npos && attachment.path[0] != '/') {
            message.attachments.push_back(std::move(attachment));
        }
    }
    return message.id > 0;
}

}  // namespace

Json ChatMessageToJson(const ChatMessage& message) {
    Json j = Json::MakeObject();
    j["id"] = message.id;
    j["time"] = message.time;
    j["from"] = message.from;
    Json to = Json::MakeArray();
    for (const std::string& id : message.to) to.push(id);
    j["to"] = std::move(to);
    j["kind"] = message.kind;
    if (message.hop > 0) j["hop"] = message.hop;
    j["text"] = message.text;
    if (!message.hint.empty()) j["hint"] = message.hint;
    if (!message.attachments.empty()) {
        Json list = Json::MakeArray();
        for (const ChatAttachment& attachment : message.attachments) {
            Json item = Json::MakeObject();
            item["type"] = attachment.type;
            item["path"] = attachment.path;
            if (attachment.width > 0 && attachment.height > 0) {
                item["width"] = attachment.width;
                item["height"] = attachment.height;
            }
            list.push(std::move(item));
        }
        j["attachments"] = std::move(list);
    }
    if (message.hasTurn) {
        Json turn = Json::MakeObject();
        turn["seconds"] = std::round(message.seconds * 10.0) / 10.0;
        turn["tools"] = message.tools;
        turn["costKnown"] = message.costKnown;
        if (message.costKnown) turn["costUsd"] = message.costUsd;
        turn["usage"] = TokenUsageToJson(message.usage);
        if (!message.steps.empty()) {
            Json steps = Json::MakeArray();
            for (const std::string& step : message.steps) steps.push(step);
            turn["steps"] = std::move(steps);
        }
        j["turn"] = std::move(turn);
    }
    return j;
}

std::vector<size_t> FindMentions(const std::string& text, const std::vector<AgentProfile>& agents, bool& all) {
    all = false;
    std::vector<size_t> found;
    const std::string lowered = Lower(text);
    for (size_t at = lowered.find('@'); at != std::string::npos; at = lowered.find('@', at + 1)) {
        if (at > 0 && IsWordByte(lowered[at - 1])) continue;  // an e-mail address, not a mention
        size_t bestLength = 0, bestAgent = agents.size();
        auto consider = [&](const std::string& candidate, size_t agent) {
            const size_t end = at + 1 + candidate.size();
            if (candidate.size() <= bestLength || lowered.compare(at + 1, candidate.size(), candidate) != 0) return;
            if (end < lowered.size() && IsWordByte(lowered[end])) return;
            bestLength = candidate.size();
            bestAgent = agent;
        };
        for (size_t i = 0; i < agents.size(); ++i) {
            consider(agents[i].id, i);
            consider(Lower(agents[i].name), i);
        }
        const size_t agentLength = bestLength;
        consider("all", agents.size());
        if (bestLength == 0) continue;
        if (bestAgent == agents.size() && bestLength > agentLength) all = true;
        else if (std::find(found.begin(), found.end(), bestAgent) == found.end()) found.push_back(bestAgent);
    }
    return found;
}

struct TeamSession::Turn {
    std::unique_ptr<Process> process;
    std::thread thread;  // writes the prompt, then queues output lines until the streams end
    std::mutex mutex;
    std::vector<std::pair<std::string, bool>> lines;  // (line, isStderr); guarded by mutex
    bool eof = false;                                  // guarded by mutex
    double lastOutputAt = 0.0;                        // guarded by mutex; any stdout/stderr line

    const Backend* backend = nullptr;
    double startedAt = 0.0, doneAt = -1.0, eofAt = -1.0;
    bool done = false, working = false, timedOut = false;
    bool idleTimedOut = false;
    std::string task, replyTo, activity = "Starting", finalText, lastText, failure, sessionId;
    std::string taskId; // empty for a resumed chat turn
    std::vector<std::string> childTasks; // cancelled with the delegating turn
    double maxDurationSeconds = 1800.0;
    double deadlineAt = 0.0;
    int hop = 0;  // of the message this turn answers
    double costUsd = 0.0;
    bool costKnown = false, costCumulative = false, resumed = false;
    TokenUsage usage;
    std::string agentId, model;
    int tools = 0;
    std::set<std::string> toolIds;
    std::vector<std::string> steps;
    std::deque<std::string> stderrTail;
    uint64_t seenUpTo = 0;
    std::map<std::string, int64_t> imagesBefore;  // ScanImages() when the turn started
};

TeamSession::TeamSession(TeamStore& store, const std::vector<Backend>& backends, const TeamHost& host) : store_(store), backends_(backends), host_(host) {}

TeamSession::~TeamSession() {
    for (auto& entry : agents_) StopTurn(entry.second);
}

void TeamSession::Open() {
    for (auto& entry : agents_) StopTurn(entry.second);
    agents_.clear();
    messages_.clear();
    lastMessageId_ = 0;
    projectDir_ = store_.ProjectDir();
    usage_.Open(JoinPath(Directory(), "usage.jsonl"));
    tasks_.Open(JoinPath(Directory(), "tasks"));
    ++revision_;

    std::string text;
    const std::string chatFile = JoinPath(Directory(), "chat.jsonl");
    if (ReadTextFile(chatFile, text)) {
        size_t lines = 0;
        for (size_t start = 0; start < text.size();) {
            size_t end = text.find('\n', start);
            if (end == std::string::npos) end = text.size();
            std::string parseError;
            ChatMessage message;
            if (end > start && MessageFromJson(Json::parse(text.substr(start, end - start), &parseError), message) && parseError.empty()) {
                lastMessageId_ = std::max(lastMessageId_, message.id);
                messages_.push_back(std::move(message));
                if (messages_.size() > kMaxStoredMessages) messages_.pop_front();
            }
            ++lines;
            start = end + 1;
        }
        if (lines > kMaxStoredMessages) {  // trim the file once, when the project opens
            std::string trimmed, error;
            for (const ChatMessage& message : messages_) trimmed += ChatMessageToJson(message).dump() + "\n";
            if (!WriteTextFileAtomic(chatFile, trimmed, &error)) OE_LOG_WARN("team", "cannot trim %s: %s", chatFile.c_str(), error.c_str());
        }
    }
    if (ReadTextFile(JoinPath(Directory(), "sessions.json"), text)) {
        std::string parseError;
        const Json root = Json::parse(text, &parseError);
        if (parseError.empty() && root.isObject()) {
            for (const auto& member : root.members()) {
                if (!FindById(store_, member.first)) continue;
                Runtime& runtime = agents_[member.first];
                const std::string session = member.second["session"].asString("");
                if (ValidSessionId(session)) runtime.session = session;
                runtime.sessionBackend = member.second["backend"].asString("");
                runtime.contextTokens = usage_.LastContext(runtime.sessionBackend, runtime.session);
                runtime.seen = static_cast<uint64_t>(std::max(0.0, member.second["seen"].asNumber(0.0)));
            }
        }
    }
    RemoveAll(JoinPath(Directory(), "tmp"));  // prompt files and MCP configs of earlier runs
}

const Backend* TeamSession::FindBackend(const std::string& id) const {
    for (const Backend& backend : backends_) {
        if (backend.id == id) return &backend;
    }
    return nullptr;
}

const std::string& TeamSession::Executable(const Backend& backend) {
    auto found = executables_.find(backend.id);
    if (found == executables_.end()) found = executables_.emplace(backend.id, PlatformFindExecutable(backend.executable)).first;
    return found->second;
}

std::string TeamSession::OfflineReason(const AgentProfile& agent, std::string* hint) {
    const Backend* backend = FindBackend(agent.backend);
    if (!backend) {
        if (hint) *hint = "Choose a backend from team.backends with team.update.";
        return "unknown backend '" + agent.backend + "'";
    }
    if (!PlatformProcessSupported()) return "this platform cannot start agent CLIs yet";
    if (!backend->buildTurn || !backend->parseLine) return backend->displayName + " cannot run turns yet";
    if (Executable(*backend).empty()) {
        if (hint) *hint = "Install it with `" + backend->installHint + "`, then call team.backends {refresh:true}.";
        return backend->displayName + " is not installed (" + backend->executable + " was not found in PATH)";
    }
    return std::string();
}

ChatMessage& TeamSession::Append(ChatMessage message) {
    message.id = ++lastMessageId_;
    message.time = NowUtc();
    message.text = Shorten(message.text, kMaxReplyBytes);
    CreateDirectories(Directory());
    std::ofstream file(std::filesystem::u8path(JoinPath(Directory(), "chat.jsonl")), std::ios::binary | std::ios::app);
    file << ChatMessageToJson(message).dump() << "\n";
    if (!file) OE_LOG_WARN("team", "cannot append to %s", JoinPath(Directory(), "chat.jsonl").c_str());
    messages_.push_back(std::move(message));
    if (messages_.size() > kMaxStoredMessages) messages_.pop_front();
    ++revision_;
    return messages_.back();
}

void TeamSession::Notice(const std::string& text, const std::string& about) {
    ChatMessage notice;
    notice.from = about.empty() ? "system" : about;
    notice.kind = "notice";
    notice.text = text;
    Append(std::move(notice));
}

void TeamSession::SaveSessions() {
    try { CheckSessionsFile(JoinPath(Directory(), "sessions.json")); }
    catch (const ApiError& e) { OE_LOG_WARN("team", "cannot save sessions: %s", e.what()); return; }
    Json root = Json::MakeObject();
    for (const auto& entry : agents_) {
        if (entry.second.session.empty() && entry.second.seen == 0) continue;
        Json j = Json::MakeObject();
        if (!entry.second.session.empty()) {
            j["backend"] = entry.second.sessionBackend;
            j["session"] = entry.second.session;
        }
        j["seen"] = entry.second.seen;
        root[entry.first] = std::move(j);
    }
    std::string error;
    const std::string file = JoinPath(Directory(), "sessions.json");
    if (!WriteTextFileAtomic(file, root.dump(2) + "\n", &error)) OE_LOG_WARN("team", "cannot write %s: %s", file.c_str(), error.c_str());
}

std::string TeamSession::StandingContext(const AgentProfile& agent) const {
    std::vector<AgentProfile> callable;
    for (const AgentProfile& mate : store_.Agents()) {
        if (mate.id != agent.id && (agent.id == store_.Settings().lead ||
            std::find(agent.canCall.begin(), agent.canCall.end(), mate.id) != agent.canCall.end())) callable.push_back(mate);
    }
    return BuildTeamStandingContext(agent, std::move(callable), projectDir_);
}

void TeamSession::FailTurn(const std::string& id, Runtime& runtime, const std::string& message, const std::string& hint) {
    runtime.pending = false;
    runtime.lastError = message;
    runtime.lastHint = hint;
    ChatMessage error;
    error.from = id;
    error.to = {"user"};
    error.kind = "error";
    error.text = message;
    error.hint = hint;
    Append(std::move(error));
}

void TeamSession::StartTurn(const AgentProfile& agent, Runtime& runtime, const Json* task) {
    if (!task) runtime.pending = false;
    auto fail = [&](const std::string& message, const std::string& hint) {
        if (!task) FailTurn(agent.id, runtime, message, hint);
        else {
            tasks_.Finish((*task)["id"].asString(), "failed", message);
            runtime.lastError = message;
            runtime.lastHint = hint;
        }
    };
    std::string hint;
    const std::string offline = OfflineReason(agent, &hint);
    if (!offline.empty()) return fail(agent.name + " cannot take a turn: " + offline + ".", hint);
    const Backend& backend = *FindBackend(agent.backend);

    // Delivery uses persisted recipients, including the message that scheduled this turn.
    // Unrelated history stays available through team.messages rather than entering every prompt.
    std::vector<const ChatMessage*> unread;
    size_t omitted = 0;
    const uint64_t seenUpTo = lastMessageId_;
    ChatMessage taskMessage;
    if (task) {
        taskMessage.from = (*task)["from"].asString();
        taskMessage.text = (*task)["goal"].asString();
        unread.push_back(&taskMessage);
    }
    for (const ChatMessage& message : messages_) {
        if (task) continue;
        if (message.id <= runtime.seen || message.kind != "message" || message.from == agent.id) continue;
        if (std::find(message.to.begin(), message.to.end(), agent.id) != message.to.end()) unread.push_back(&message);
        else ++omitted;
    }
    if (unread.empty()) return;
    if (unread.size() > kMaxPromptMessages) {
        omitted += unread.size() - kMaxPromptMessages;
        unread.erase(unread.begin(), unread.end() - static_cast<std::ptrdiff_t>(kMaxPromptMessages));
    }
    size_t bytes = 0;
    for (const ChatMessage* message : unread) bytes += message->text.size();
    while (unread.size() > 1 && bytes > kMaxPromptBytes) {
        bytes -= unread.front()->text.size();
        ++omitted;
        unread.erase(unread.begin());
    }
    std::string prompt = "New messages in the team chat, oldest first. Reply to what is addressed to you.\n";
    for (const ChatMessage* message : unread) {
        const AgentProfile* sender = FindById(store_, message->from);
        prompt += "\n[" + (message->from == "user" ? std::string("User") : sender ? sender->name : message->from) + "] " + message->text + "\n";
        for (const ChatAttachment& attachment : message->attachments) prompt += "  Attached " + attachment.type + ": " + attachment.path + "\n";
    }
    if (task) prompt = "Task brief (one step; preserve the exact requirements):\n" + task->dump(2) + "\n";

    const ChatMessage trigger = *unread.back();

    if (!task && omitted > 0 && host_.apiPort > 0) {
        prompt += "\n" + std::to_string(omitted) + " chat messages omitted. Read team_messages when earlier context is needed.\n";
    }

    if (!task && store_.Settings().rotateAboveInputTokens > 0 && !runtime.session.empty() &&
        runtime.contextTokens >= static_cast<uint64_t>(store_.Settings().rotateAboveInputTokens)) {
        runtime.session.clear(); runtime.sessionBackend.clear(); runtime.contextTokens = 0;
        Notice(agent.name + " started a fresh conversation: the input-size estimate reached rotateAboveInputTokens.", agent.id);
        SaveSessions();
    }
    if (!task && runtime.sessionBackend != backend.id) runtime.session.clear();  // a conversation belongs to the CLI that made it
    const std::string tmp = JoinPath(Directory(), "tmp"), relativeTmp = ".oe/team/tmp/";
    CreateDirectories(tmp);
    TurnRequest request;
    request.executable = Executable(backend);
    request.model = agent.model;
    request.access = agent.access;
    request.inheritUserTools = store_.Settings().inheritUserTools;
    request.sessionId = task ? "" : runtime.session;
    try {
        const Json baseline = usage_.Baseline(backend.id, request.sessionId);
        if (agent.maxBudgetUsd > 0) {
            if (!request.sessionId.empty() && baseline.isNull()) {
                runtime.session.clear(); request.sessionId.clear();
                Notice(agent.name + " started a fresh conversation because its budget baseline is unknown.", agent.id);
            }
            request.maxBudgetUsd = agent.maxBudgetUsd + (request.sessionId.empty() ? 0 : baseline["reportedCostUsd"].asNumber());
        }
    } catch (const ApiError& e) { return fail(e.what(), e.hint); }
    const std::string context = StandingContext(agent);
    if (task) {
        prompt += "\nTask protocol: work only on this task. On failure report the exact failure and stop; do not retry or invent work. "
                  "Report changed files and command, result/exit code and trimmed output. Never claim success without verifying output.\n";
        prompt += host_.apiPort > 0 ? "You MUST call the team_report tool to submit {id,summary,changed,evidence:[{command,result,output}]}. "
                                     "Do not print report JSON in the final answer. Use the task id. Do not post to team chat.\n" :
                                     "No API is attached. Return final text; the harness stores it unverified without posting to chat.\n";
    } else {
        prompt += "\nChat protocol: reply only to addressed messages; final text is posted to chat.\n";
        if (agent.id == store_.Settings().lead || !agent.canCall.empty()) {
            prompt += "Use team_delegate for one task step, passing exact requirements and acceptance verbatim; add no constraints. "
                      "Check the returned evidence yourself: done is not verified.\n";
            if (store_.Settings().maxHops == 0) {
                prompt += "An @mention does not hand work to a teammate; only team_delegate does. "
                          "To give a teammate a task, call team_delegate; do not answer with a mention.\n";
            }
        }
    }
    prompt += host_.apiPort > 0 ? "\nConnection: ownengine MCP controls this project. Use its tools.\n" :
                                 "\nConnection: no API attached; edit files or use oe exec.\n";

    if (backend.systemPromptFlag) {
        if (!WriteTextFileAtomic(JoinPath(tmp, agent.id + ".system.md"), context, nullptr)) return fail( "cannot write the prompt file in .oe/team/tmp/.", "Check that the project folder is writable.");
        request.systemPromptFile = relativeTmp + agent.id + ".system.md";
    } else if (task || runtime.session.empty()) {
        prompt = context + "\n---\n\n" + prompt;  // the CLI has no system prompt flag: said once, at the start of the conversation
    }
    std::string oe = host_.oeExecutable;
    if (oe.empty()) {
        oe = JoinPath(ExecutableDirectory(), "oe.exe");
        if (!FileExists(oe)) oe = JoinPath(ExecutableDirectory(), "oe");
    }
    Json config = Json::MakeObject();
    config["mcpServers"] = Json::MakeObject();
    if (host_.apiPort > 0) {
        if (!FileExists(oe)) return fail( "Cannot configure the editor MCP server: oe executable is missing.", "Set TeamHost::oeExecutable to the built oe executable.");
        Json server = Json::MakeObject();
        server["command"] = oe;
        Json arguments = Json::MakeArray();
        for (const std::string& argument : {std::string("mcp"), std::string("--connect"), std::to_string(host_.apiPort), std::string("--agent"), agent.id}) arguments.push(argument);
        server["args"] = std::move(arguments);
        config["mcpServers"]["ownengine"] = std::move(server);
        request.oeExecutable = oe;
        request.apiPort = host_.apiPort;
        request.agentId = agent.id;
    }
    // An empty config is intentional: lean Claude turns must not load unrelated user servers.
    if (!WriteTextFileAtomic(JoinPath(tmp, agent.id + ".mcp.json"), config.dump(2) + "\n", nullptr)) {
        return fail( "Cannot write the MCP config in .oe/team/tmp/.", "Check that the project folder is writable.");
    }
    request.mcpConfigFile = relativeTmp + agent.id + ".mcp.json";
    ProcessOptions options = backend.buildTurn(request);
    options.workingDirectory = projectDir_;
    options.environment.emplace_back("OE_TEAM_AGENT", agent.id);
    options.environment.emplace_back("OE_TEAM_HOP", std::to_string(trigger.hop));
    if (task) {
        options.environment.emplace_back("OE_TEAM_TASK", (*task)["id"].asString());
        if (!WriteTextFileAtomic(JoinPath(tmp, agent.id + ".task.md"), context + "\n" + prompt, nullptr)) return fail("Cannot write task prompt.", "Check that .oe/team/tmp is writable.");
    }
    options.pipeStdin = true;

    auto turn = std::make_unique<Turn>();
    std::string error;
    turn->process = PlatformStartProcess(options, &error);
    if (!turn->process) return fail("Cannot start " + backend.displayName + ": " + error, "Call team.backends {refresh:true} to check the installation.");
    turn->backend = &backend;
    turn->agentId = agent.id; turn->model = agent.model; turn->resumed = !request.sessionId.empty();
    turn->startedAt = PlatformTimeSeconds();
    turn->lastOutputAt = turn->startedAt;
    turn->task = Shorten(FirstLine(trigger.text), 120);
    turn->replyTo = trigger.from;
    turn->hop = trigger.hop;
    turn->seenUpTo = seenUpTo;
    turn->maxDurationSeconds = task ? std::min(host_.turnTimeoutSeconds, (*task)["maxDurationMs"].asNumber() / 1000.0) : host_.turnTimeoutSeconds;
    turn->taskId = task ? (*task)["id"].asString() : "";
    turn->deadlineAt = task && (*task).has("deadlineAt") ? (*task)["deadlineAt"].asNumber() : turn->startedAt + turn->maxDurationSeconds;
    CreateDirectories(JoinPath(Directory(), "images"));  // where agents are told to save pictures
    turn->imagesBefore = ScanImages();
    Turn* raw = turn.get();
    turn->thread = std::thread([raw, prompt] {
        std::string writeError;
        raw->process->Write(prompt, &writeError);  // a CLI that left early shows as its own failed exit
        raw->process->CloseInput();
        std::string line;
        bool isStderr = false;
        while (raw->process->ReadLine(line, isStderr)) {
            std::lock_guard<std::mutex> lock(raw->mutex);
            raw->lines.emplace_back(line, isStderr);
            raw->lastOutputAt = PlatformTimeSeconds();
        }
        std::lock_guard<std::mutex> lock(raw->mutex);
        raw->eof = true;
    });
    runtime.turn = std::move(turn);
    runtime.lastError.clear();
    runtime.lastHint.clear();
    ++revision_;
}

void TeamSession::HandleLine(Turn& turn, const std::string& line, bool isStderr) {
    if (isStderr) {
        if (!FirstLine(line).empty()) turn.stderrTail.push_back(Shorten(line, 300));
        if (turn.stderrTail.size() > 8) turn.stderrTail.pop_front();
        return;
    }
    TurnEvents events;
    turn.backend->parseLine(line, events);
    for (const TurnEvent& event : events) {
        switch (event.kind) {
            case TurnEvent::Kind::Session:
                if (ValidSessionId(event.text)) turn.sessionId = event.text;
                break;
            case TurnEvent::Kind::Text:
                turn.lastText = event.text;
                [[fallthrough]];  // the model is generating
            case TurnEvent::Kind::Thinking:
                turn.working = false;
                turn.activity = "Thinking";
                break;
            case TurnEvent::Kind::Tool:
                if (!event.itemId.empty() && !turn.toolIds.insert(event.itemId).second) break;  // the same call, reported again when it ended
                ++turn.tools;
                turn.working = true;
                turn.activity = ActivityLine(event, projectDir_);
                if (turn.steps.size() < kMaxSteps) turn.steps.push_back(turn.activity);
                break;
            case TurnEvent::Kind::Done:
                turn.done = true;
                turn.doneAt = PlatformTimeSeconds();
                turn.finalText = event.text;
                turn.costUsd = event.costUsd;
                turn.costKnown = event.costKnown; turn.costCumulative = event.costCumulative; turn.usage = event.usage;
                turn.failure.clear();
                break;
            case TurnEvent::Kind::Failed:
                turn.done = false;
                turn.failure = event.text;
                if (event.usage.known) turn.usage = event.usage;
                if (event.costKnown) { turn.costUsd = event.costUsd; turn.costKnown = true; turn.costCumulative = event.costCumulative; }
                break;
        }
        ++revision_;
    }
}

Json TeamSession::RecordTurn(const Turn& turn, const std::string& state) {
    Json record = Json::MakeObject();
    record["version"] = 1; record["time"] = NowUtc(); record["date"] = record["time"].asString().substr(0, 10);
    record["agent"] = turn.agentId; record["backend"] = turn.backend->id; record["model"] = turn.model;
    record["session"] = turn.sessionId; record["state"] = state;
    if (!turn.taskId.empty()) record["taskId"] = turn.taskId;
    record["tokensKnown"] = turn.usage.known;
    record["contextTokens"] = turn.usage.contextTokens; record["contextEstimated"] = turn.usage.contextEstimated;
    record["inputTokens"] = turn.usage.inputTokens; record["outputTokens"] = turn.usage.outputTokens;
    record["cacheReadTokens"] = turn.usage.cacheReadTokens; record["cacheCreationTokens"] = turn.usage.cacheCreationTokens;
    record["reportedCostKnown"] = turn.costKnown; record["reportedCostUsd"] = turn.costUsd;
    record["costKnown"] = false; record["costUsd"] = 0;
    try {
        if (turn.costKnown && !turn.costCumulative) { record["costKnown"] = true; record["costUsd"] = turn.costUsd; }
        else if (turn.costKnown && !turn.sessionId.empty()) {
            const Json baseline = usage_.Baseline(turn.backend->id, turn.sessionId);
            if (!baseline.isNull() && turn.costUsd >= baseline["reportedCostUsd"].asNumber()) {
                record["costKnown"] = true; record["costUsd"] = turn.costUsd - baseline["reportedCostUsd"].asNumber();
            } else if (baseline.isNull() && !turn.resumed) { record["costKnown"] = true; record["costUsd"] = turn.costUsd; }
        }
        usage_.Append(record);
    } catch (const ApiError& e) { OE_LOG_WARN("team", "cannot record usage: %s", e.what()); }
    return record;
}

Json TeamSession::Usage(const std::string& agent, const std::string& date) const { return usage_.Read(agent, date == "today" ? NowUtc().substr(0, 10) : date); }

void TeamSession::StopTurn(Runtime& runtime) {
    if (!runtime.turn) return;
    const auto children = runtime.turn->childTasks;
    const std::string taskId = runtime.turn->taskId;
    runtime.turn->process->Kill();
    runtime.turn->thread.join();
    for (const auto& line : runtime.turn->lines) HandleLine(*runtime.turn, line.first, line.second);
    RecordTurn(*runtime.turn, "cancelled");
    runtime.turn.reset();
    for (const std::string& id : children) CancelTask(id);
    if (!taskId.empty()) {
        try { tasks_.Finish(taskId, "cancelled", "The worker turn was cancelled."); }
        catch (const ApiError& e) { OE_LOG_WARN("team", "cannot persist task cancellation: %s", e.what()); }
    }
    ++revision_;
}

void TeamSession::FinishTurn(const std::string& id, Runtime& runtime) {
    std::unique_ptr<Turn> turn = std::move(runtime.turn);
    turn->thread.join();
    int exitCode = 0;
    turn->process->Running(&exitCode);
    const double seconds = PlatformTimeSeconds() - turn->startedAt;
    const bool pending = runtime.pending;
    const Json accounting = RecordTurn(*turn, turn->timedOut && !turn->idleTimedOut ? "timed_out" : turn->done && exitCode == 0 && !turn->timedOut ? "done" : "failed");
    if (!turn->taskId.empty()) {
        try {
            const Json task = tasks_.Get(turn->taskId);
            std::string state = "failed", summary;
            if (turn->timedOut && !turn->idleTimedOut) { state = "timed_out"; summary = "The task exceeded its duration limit; it was not retried."; }
            else if (!turn->done || exitCode != 0) summary = !turn->failure.empty() ? turn->failure : "Worker exited with code " + std::to_string(exitCode) + " before successful completion.";
            else {
                state = "done";
                if (!task["reportReceived"].asBool()) summary = !turn->finalText.empty() ? turn->finalText : turn->lastText;
            }
            tasks_.Finish(turn->taskId, state, summary, seconds, accounting["costUsd"].asNumber(), turn->tools);
            if (state != "done") {
                for (const std::string& child : turn->childTasks) CancelTask(child);
                runtime.lastError = summary;
                runtime.lastHint = "Check the task evidence and fix the cause before delegating another task.";
            }
        } catch (const ApiError& e) {
            for (const std::string& child : turn->childTasks) CancelTask(child);
            runtime.lastError = e.what();
            runtime.lastHint = e.hint;
            OE_LOG_WARN("team", "cannot persist task result: %s", e.what());
        }
        ++revision_;
        return; // worker text and session id never enter chat or resume state
    }
    if (turn->done && !turn->timedOut) {
        ChatMessage reply;
        reply.from = id;
        reply.to = {turn->replyTo};
        reply.text = !turn->finalText.empty() ? turn->finalText : !turn->lastText.empty() ? turn->lastText : "(no reply)";
        reply.hasTurn = true;
        reply.seconds = seconds;
        reply.tools = turn->tools;
        reply.costUsd = accounting["costUsd"].asNumber();
        reply.costKnown = accounting["costKnown"].asBool();
        reply.usage = turn->usage;
        reply.steps = turn->steps;
        // Images the turn made: new or changed png/jpg files in .oe/team/images/ and assets/.
        for (const auto& image : ScanImages()) {
            const auto before = turn->imagesBefore.find(image.first);
            const auto claimed = claimedImages_.find(image.first);
            if ((before != turn->imagesBefore.end() && before->second == image.second) || (claimed != claimedImages_.end() && claimed->second == image.second)) continue;
            if (reply.attachments.size() >= kMaxTurnImages) break;
            claimedImages_[image.first] = image.second;
            reply.attachments.push_back(Describe(image.first));
        }
        // Agent-to-agent: teammates the reply mentions get a turn, as long as the chain that
        // began with the person's message is not longer than maxHops.
        reply.hop = turn->hop + 1;
        const std::vector<AgentProfile>& roster = store_.Agents();
        bool all = false;
        std::vector<size_t> mentioned = FindMentions(reply.text, roster, all);
        if (all) {
            mentioned.clear();
            for (size_t i = 0; i < roster.size(); ++i) mentioned.push_back(i);
        }
        mentioned.erase(std::remove_if(mentioned.begin(), mentioned.end(), [&](size_t i) { return roster[i].id == id; }), mentioned.end());
        for (size_t i : mentioned) {
            if (std::find(reply.to.begin(), reply.to.end(), roster[i].id) == reply.to.end()) reply.to.push_back(roster[i].id);
        }
        const int replyHop = reply.hop;
        Append(std::move(reply));
        runtime.session = turn->sessionId;
        runtime.contextTokens = turn->usage.contextTokens;
        runtime.sessionBackend = turn->backend->id;
        runtime.seen = std::max(runtime.seen, turn->seenUpTo);
        runtime.lastError.clear();
        runtime.lastHint.clear();
        if (!mentioned.empty() && replyHop > store_.Settings().maxHops) {
            Notice(BlockedMentionNotice(store_, id, mentioned), id);
        } else {
            for (size_t i : mentioned) {
                std::string hint;
                const std::string reason = OfflineReason(roster[i], &hint);
                if (!reason.empty()) {
                    Notice(roster[i].name + " is offline: " + reason + "." + (hint.empty() ? "" : " " + hint), roster[i].id);
                    continue;
                }
                agents_[roster[i].id].pending = true;  // Update() schedules it right after this
            }
        }
    } else {
        for (const std::string& child : turn->childTasks) CancelTask(child);
        std::string message = turn->idleTimedOut ? "The turn was stopped after " + std::to_string(static_cast<int>(host_.idleTimeoutSeconds)) + " s without output (idle timeout)." :
                              turn->timedOut ? "The turn was stopped after " + std::to_string(static_cast<int>(seconds)) + " s without finishing." :
                              !turn->failure.empty() ? turn->failure : turn->backend->displayName + " exited with code " + std::to_string(exitCode) + " before finishing the turn.";
        if (turn->failure.empty()) {
            for (const std::string& line : turn->stderrTail) message += "\n" + line;
        }
        runtime.session.clear();  // a broken or expired conversation must not fail every later turn
        FailTurn(id, runtime, message, "Run `" + turn->backend->executable + "` once in a terminal to check that it works and that you are logged in.");
        runtime.pending = pending;  // messages that arrived meanwhile still get their turn
    }
    SaveSessions();
    ++revision_;
}

void TeamSession::Update() {
    if (store_.ProjectDir() != projectDir_) Open();
    for (auto it = agents_.begin(); it != agents_.end();) {
        if (FindById(store_, it->first)) {
            ++it;
            continue;
        }
        StopTurn(it->second);  // the agent left the team (also from another process)
        it = agents_.erase(it);
    }
    const double now = PlatformTimeSeconds();
    for (auto& entry : agents_) {
        Runtime& runtime = entry.second;
        if (!runtime.turn) continue;
        Turn& turn = *runtime.turn;
        std::vector<std::pair<std::string, bool>> lines;
        bool eof = false;
        double lastOutputAt = turn.startedAt;
        {
            std::lock_guard<std::mutex> lock(turn.mutex);
            lines.swap(turn.lines);
            eof = turn.eof;
            lastOutputAt = turn.lastOutputAt;
        }
        for (const auto& line : lines) HandleLine(turn, line.first, line.second);
        if (eof) {
            // A CLI may close its output a moment before it exits: its exit code is read once
            // it has exited (or was killed after a short wait), not at the end of the stream.
            if (turn.eofAt < 0.0) turn.eofAt = now;
            if (!turn.process->Running(nullptr)) FinishTurn(entry.first, runtime);
            else if (now - turn.eofAt > kExitGraceSeconds) turn.process->Kill();
        } else if (turn.done && now - turn.doneAt > kExitGraceSeconds) {
            turn.process->Kill();  // the CLI is done but something it started keeps the pipes open
        } else if (!turn.timedOut && !turn.done &&
                   (now >= turn.deadlineAt || now - lastOutputAt > host_.idleTimeoutSeconds)) {
            turn.timedOut = true;
            turn.idleTimedOut = now < turn.deadlineAt;
            turn.process->Kill();
        }
    }
    Schedule();
}

void TeamSession::Schedule() {
    int running = 0;
    for (const auto& entry : agents_) running += entry.second.turn ? 1 : 0;
    for (const AgentProfile& agent : store_.Agents()) {  // roster order decides who goes first
        auto found = agents_.find(agent.id);
        if (found == agents_.end() || !found->second.pending || found->second.turn) continue;
        if (running >= store_.Settings().maxConcurrent) break;
        StartTurn(agent, found->second);
        running += found->second.turn ? 1 : 0;
    }
}

std::map<std::string, int64_t> TeamSession::ScanImages() const {
    std::map<std::string, int64_t> images;
    for (const std::string& folder : {JoinPath(Directory(), "images"), JoinPath(projectDir_, "assets")}) {
        for (const char* extension : {".png", ".jpg", ".jpeg"}) {
            for (const std::string& file : ListFiles(folder, extension, true)) {
                if (images.size() >= kMaxScannedImages) return images;
                images[RelativePath(file, projectDir_)] = FileModifiedTime(file);
            }
        }
    }
    return images;
}

ChatAttachment TeamSession::Describe(const std::string& relativePath) const {
    ChatAttachment attachment;
    attachment.path = relativePath;
    if (!IsImagePath(relativePath)) return attachment;
    attachment.type = "image";
    std::error_code ec;
    const std::string full = JoinPath(projectDir_, relativePath);
    std::vector<unsigned char> bytes;
    if (std::filesystem::file_size(std::filesystem::u8path(full), ec) <= kMaxAttachmentBytes && !ec && ReadBinaryFile(full, bytes)) {
        ImageDimensions(bytes.data(), bytes.size(), attachment.width, attachment.height);
    }
    return attachment;
}

int TeamSession::TurnHop(const std::string& id) const {
    const auto found = agents_.find(id);
    return found != agents_.end() && found->second.turn ? found->second.turn->hop : 0;
}

Json TeamSession::Send(const std::string& text, const std::string& from, const std::vector<std::string>& attachments, int hop) {
    auto sender = agents_.find(from);
    if (sender != agents_.end() && sender->second.turn && !sender->second.turn->taskId.empty()) {
        throw ApiError("task_chat_forbidden", "workers report through team.report, not team chat", "Call team.report with your task id and evidence.");
    }
    if (text.find_first_not_of(" \t\r\n") == std::string::npos && attachments.empty()) throw ApiError("invalid_argument", "the message is empty");
    if (attachments.size() > kMaxSendAttachments) throw ApiError("invalid_argument", "a message carries at most 8 attachments");
    if (!ValidTeamText(text, kMaxSendBytes)) throw ApiError("invalid_argument", "a chat message must be UTF-8 text of at most 32768 bytes");
    if (from != "user" && !FindById(store_, from)) throw ApiError("unknown_agent", "no agent '" + from + "' in the team", "`from` is \"user\" or an agent id.");
    if (!store_.LoadError().empty()) throw ApiError("team_file_invalid", store_.LoadError(), "Repair or delete .oe/team/team.json.");

    const std::vector<AgentProfile>& roster = store_.Agents();
    bool all = false;
    std::vector<size_t> recipients = FindMentions(text, roster, all);
    if (all) {
        recipients.clear();
        for (size_t i = 0; i < roster.size(); ++i) recipients.push_back(i);
    } else if (recipients.empty()) {
        for (size_t i = 0; i < roster.size(); ++i) {
            if (roster[i].id == store_.Settings().lead) recipients.push_back(i);  // no mention: the lead
        }
    }
    recipients.erase(std::remove_if(recipients.begin(), recipients.end(), [&](size_t i) { return roster[i].id == from; }), recipients.end());

    ChatMessage message;
    message.from = from;
    message.text = text;
    message.hop = std::max(0, hop);
    for (size_t i : recipients) message.to.push_back(roster[i].id);
    // Attachments: a file of the project is referred to in place; one from elsewhere is copied
    // into .oe/team/attachments/ (agents work inside the project directory).
    std::string root = Lower(AbsolutePath(projectDir_));
    std::replace(root.begin(), root.end(), '\\', '/');
    if (!root.empty() && root.back() != '/') root += '/';
    for (size_t index = 0; index < attachments.size(); ++index) {
        const std::string& given = attachments[index];
        const bool absolute = given.size() > 1 && (given[1] == ':' || given[0] == '/');
        std::string full = AbsolutePath(absolute ? given : JoinPath(projectDir_, given));
        std::replace(full.begin(), full.end(), '\\', '/');
        std::error_code ec;
        const uintmax_t bytes = std::filesystem::file_size(std::filesystem::u8path(full), ec);
        if (given.empty() || ec || IsDirectory(full)) throw ApiError("not_found", "cannot read attachment '" + given + "'", "Attach a file, by a path on this PC or one relative to the project.");
        if (bytes > kMaxAttachmentBytes) throw ApiError("invalid_argument", "attachment '" + given + "' is larger than 32 MiB");
        std::string relative;
        if (Lower(full).compare(0, root.size(), root) == 0) {
            relative = full.substr(root.size());
        } else {
            std::string name = full.substr(full.find_last_of('/') + 1);
            for (char& c : name) {
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) c = '_';
            }
            if (name.size() > 64) name = name.substr(name.size() - 64);
            relative = ".oe/team/attachments/" + std::to_string(lastMessageId_ + 1) + "-" + std::to_string(index + 1) + "-" + name;
            if (!CopyFileTo(full, JoinPath(projectDir_, relative))) throw ApiError("write_failed", "cannot copy attachment '" + given + "' into the project");
        }
        message.attachments.push_back(Describe(relative));
    }
    Json result = Json::MakeObject();
    result["message"] = ChatMessageToJson(Append(std::move(message)));

    Json started = Json::MakeArray(), queued = Json::MakeArray(), offline = Json::MakeArray();
    std::vector<std::string> waiting;
    // An agent's own message is part of the chain its turn belongs to: past maxHops nobody is called.
    if (hop > store_.Settings().maxHops && !recipients.empty()) {
        Notice(BlockedMentionNotice(store_, from, recipients), from);
        Schedule();
        result["started"] = std::move(started);
        result["queued"] = std::move(queued);
        result["offline"] = std::move(offline);
        return result;
    }
    for (size_t i : recipients) {
        const AgentProfile& agent = roster[i];
        std::string hint;
        const std::string reason = OfflineReason(agent, &hint);
        if (!reason.empty()) {
            offline.push(agent.id);
            Notice(agent.name + " is offline: " + reason + "." + (hint.empty() ? "" : " " + hint), agent.id);
            continue;
        }
        Runtime& runtime = agents_[agent.id];
        runtime.pending = true;
        if (runtime.turn) queued.push(agent.id);  // delivered when its current turn ends
        else waiting.push_back(agent.id);
    }
    if (recipients.empty()) Notice("Nobody was addressed: mention an agent with @name, or choose a lead with team.settings.", "");
    Schedule();
    for (const std::string& id : waiting) {
        const Runtime& runtime = agents_[id];
        if (runtime.turn) started.push(id);
        else if (runtime.pending) queued.push(id);  // no free slot yet; a turn that could not start is in the chat as an error
    }
    result["started"] = std::move(started);
    result["queued"] = std::move(queued);
    result["offline"] = std::move(offline);
    return result;
}

std::vector<std::string> TeamSession::Cancel(const std::string& idOrName, bool all) {
    std::vector<std::string> cancelled;
    for (const AgentProfile& agent : store_.Agents()) {
        if (!all && agent.id != idOrName && Lower(agent.name) != Lower(idOrName)) continue;
        auto found = agents_.find(agent.id);
        if (found == agents_.end() || (!found->second.turn && !found->second.pending)) continue;
        StopTurn(found->second);
        found->second.pending = false;
        Notice(agent.name + " was stopped.", agent.id);
        cancelled.push_back(agent.id);
    }
    if (!all && cancelled.empty() && !store_.Find(idOrName)) {
        throw ApiError("unknown_agent", "no agent '" + idOrName + "' in the team", "team.list shows the agents; use an id or a name.");
    }
    return cancelled;
}

void TeamSession::Forget(const std::string& id) {
    auto found = agents_.find(id);
    if (found == agents_.end()) return;
    StopTurn(found->second);
    agents_.erase(found);
    SaveSessions();
    RemoveAll(JoinPath(JoinPath(Directory(), "tmp"), id + ".system.md"));
    RemoveAll(JoinPath(JoinPath(Directory(), "tmp"), id + ".mcp.json"));
    ++revision_;
}

void TeamSession::Reset(const std::string& idOrName) {
    const AgentProfile* agent = store_.Find(idOrName);
    if (!agent) throw ApiError("unknown_agent", "no agent '" + idOrName + "'", "team.list shows the agents.");
    Runtime& runtime = agents_[agent->id];
    if (runtime.turn || runtime.pending) throw ApiError("agent_busy", agent->name + " is busy", "Cancel that agent or wait before resetting its conversation.");
    CheckSessionsFile(JoinPath(Directory(), "sessions.json"));
    runtime.session.clear(); runtime.sessionBackend.clear(); runtime.contextTokens = 0;
    SaveSessions(); ++revision_;
}

void TeamSession::Clear() {
    for (auto& entry : agents_) StopTurn(entry.second);
    agents_.clear();
    tasks_.Clear();
    messages_.clear();
    RemoveAll(JoinPath(Directory(), "chat.jsonl"));
    RemoveAll(JoinPath(Directory(), "sessions.json"));
    ++revision_;  // message ids keep counting, so a client polling `after` never sees an old id again
}

bool TeamSession::Busy(const std::vector<std::string>& ids) {
    for (const auto& entry : agents_) {
        if (!entry.second.turn && !entry.second.pending) continue;
        if (ids.empty() || std::find(ids.begin(), ids.end(), entry.first) != ids.end()) return true;
    }
    return false;
}

Json TeamSession::Delegate(const Json& brief, const std::string& from) {
    if (!store_.LoadError().empty()) throw ApiError("team_file_invalid", store_.LoadError(), "Repair .oe/team/team.json first.");
    std::string targetName = brief["to"].asString("");
    if (!targetName.empty() && targetName.front() == '@') targetName.erase(0, 1);
    const AgentProfile* target = store_.Find(targetName);
    if (!target) throw ApiError("unknown_agent", "no delegate target", "team.list shows the agents; pass to as an id or name.");
    if (from != "user") {
        const AgentProfile* caller = FindById(store_, from);
        if (!caller) throw ApiError("unknown_agent", "unknown task sender");
        auto active = agents_.find(from);
        if (active == agents_.end() || !active->second.turn) throw ApiError("agent_idle", "only a running agent turn may delegate", "Send the agent a chat message first.");
        if (from != store_.Settings().lead && std::find(caller->canCall.begin(), caller->canCall.end(), target->id) == caller->canCall.end()) {
            throw ApiError("delegate_forbidden", from + " may not call " + target->id, "Set the caller's canCall profile, or delegate from the lead.");
        }
    }
    if (from == target->id) throw ApiError("delegate_cycle", "an agent cannot delegate to itself");
    Runtime& worker = agents_[target->id];
    if (worker.turn || worker.pending) throw ApiError("agent_busy", target->name + " is already busy", "Choose an idle worker or wait for its turn to finish.");
    int running = 0;
    for (const auto& entry : agents_) running += entry.second.turn ? 1 : 0;
    if (running >= store_.Settings().maxConcurrent) throw ApiError("team_busy", "no free worker slot", "Increase maxConcurrent, or delegate after another turn finishes.");
    Json boundedBrief = brief;
    const double now = PlatformTimeSeconds();
    double deadline = now + std::min(host_.turnTimeoutSeconds, brief["maxDurationMs"].asNumber(1800000) / 1000.0);
    if (from != "user") deadline = std::min(deadline, agents_[from].turn->deadlineAt);
    if (deadline <= now) throw ApiError("task_deadline", "the caller's deadline has expired", "Do not retry automatically.");
    boundedBrief["deadlineAt"] = deadline;
    Json task = tasks_.Create(boundedBrief, from, target->id);
    if (from != "user") agents_[from].turn->childTasks.push_back(task["id"].asString());
    StartTurn(*target, worker, &task);
    return tasks_.Get(task["id"].asString());
}

Json TeamSession::Report(const Json& report, const std::string& from) {
    const std::string id = report["id"].asString("");
    const Json task = tasks_.Get(id);
    auto active = agents_.find(from);
    if (from != task["to"].asString() || active == agents_.end() || !active->second.turn || active->second.turn->taskId != id) {
        throw ApiError("report_forbidden", "only the assigned running worker may report this task", "Use team.report from that worker's attributed MCP connection.");
    }
    if (PlatformTimeSeconds() >= active->second.turn->deadlineAt) throw ApiError("task_deadline", "the task deadline has expired", "Do not retry automatically.");
    tasks_.Report(id, report);
    ++revision_;
    return tasks_.Get(id);
}

void TeamSession::CancelTask(const std::string& id) {
    for (auto& entry : agents_) {
        if (entry.second.turn && entry.second.turn->taskId == id) { StopTurn(entry.second); return; }
    }
    try { tasks_.Finish(id, "cancelled", "The delegating turn was cancelled."); }
    catch (const ApiError& e) { OE_LOG_WARN("team", "cannot persist task cancellation: %s", e.what()); }
}

Json TeamSession::State() {
    const double now = PlatformTimeSeconds();
    Json list = Json::MakeArray();
    int running = 0;
    for (const AgentProfile& agent : store_.Agents()) {
        const auto found = agents_.find(agent.id);
        const Runtime* runtime = found == agents_.end() ? nullptr : &found->second;
        Json j = Json::MakeObject();
        j["id"] = agent.id;
        std::string state = "idle", activity, task, error, hint;
        double elapsed = 0.0;
        int tools = 0;
        if (runtime && runtime->turn) {
            const Turn& turn = *runtime->turn;
            state = turn.working ? "working" : "thinking";
            activity = turn.activity;
            task = turn.task;
            elapsed = now - turn.startedAt;
            tools = turn.tools;
            ++running;
        } else if (runtime && runtime->pending) {
            state = "queued";
            activity = "Waiting for a free slot";
        } else if (!(error = OfflineReason(agent, &hint)).empty()) {
            state = "offline";
        } else if (runtime && !runtime->lastError.empty()) {
            state = "error";
            error = runtime->lastError;
            hint = runtime->lastHint;
        }
        j["state"] = state;
        j["activity"] = activity;
        j["task"] = task;
        j["elapsed"] = std::round(elapsed * 10.0) / 10.0;
        j["tools"] = tools;
        if (!error.empty()) j["error"] = error;
        if (!hint.empty()) j["hint"] = hint;
        list.push(std::move(j));
    }
    Json result = Json::MakeObject();
    result["revision"] = revision_ + store_.Revision();
    result["running"] = running;
    result["agents"] = std::move(list);
    return result;
}

Json TeamSession::Messages(uint64_t after, bool hasAfter, int limit) {
    std::vector<const ChatMessage*> selected;
    for (const ChatMessage& message : messages_) {
        if (!hasAfter || message.id > after) selected.push_back(&message);
    }
    const size_t count = static_cast<size_t>(std::max(1, limit));
    if (selected.size() > count) {
        // After an id: the oldest unseen ones first (the caller pages forward); otherwise the newest.
        if (hasAfter) selected.resize(count);
        else selected.erase(selected.begin(), selected.end() - static_cast<std::ptrdiff_t>(count));
    }
    Json list = Json::MakeArray();
    for (const ChatMessage* message : selected) list.push(ChatMessageToJson(*message));
    Json result = Json::MakeObject();
    result["messages"] = std::move(list);
    result["first"] = messages_.empty() ? uint64_t(0) : messages_.front().id;  // 0: the log is empty (a viewer drops what it kept)
    result["last"] = lastMessageId_;
    return result;
}

}  // namespace oe
