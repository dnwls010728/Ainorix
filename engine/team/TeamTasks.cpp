#include "team/TeamTasks.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "api/Commands.h"
#include "core/FileSystem.h"
#include "platform/Platform.h"
#include "team/TeamStore.h"

namespace oe {
namespace {
constexpr size_t kMaxTasks = 256;
constexpr size_t kMaxTaskBytes = 128 * 1024;

std::string Text(const Json& j, const char* key, size_t limit, bool required = false) {
    const Json* value = j.find(key);
    if (!value && !required) return "";
    if (!value || !value->isString() || !ValidTeamText(value->asString(), limit) || (required && value->asString().empty())) {
        throw ApiError("invalid_argument", std::string(key) + " must be UTF-8 text of at most " + std::to_string(limit) + " bytes");
    }
    return value->asString();
}

Json Paths(const Json& j, const char* key) {
    Json paths = Json::MakeArray();
    const Json* value = j.find(key);
    if (!value) return paths;
    if (!value->isArray() || value->size() > 32) throw ApiError("invalid_argument", std::string(key) + " must be at most 32 project-relative paths");
    for (const Json& item : value->items()) {
        if (!item.isString() || !ValidTeamText(item.asString(), 1024)) throw ApiError("invalid_argument", "file paths must be strings");
        std::string path = item.asString();
        std::replace(path.begin(), path.end(), '\\', '/');
        if (path.empty() || path.front() == '/' || path.find(':') != std::string::npos) throw ApiError("invalid_argument", "file paths must stay inside the project");
        for (const auto& part : std::filesystem::u8path(path)) {
            if (part == "..") throw ApiError("invalid_argument", "file paths must stay inside the project");
        }
        paths.push(path);
    }
    return paths;
}

std::string TrimOutput(std::string text) {
    if (text.size() <= 4096) return text;
    size_t end = 4093;
    while ((static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    return text.substr(0, end) + "...";
}
} // namespace

void TeamTasks::RequireWritable() const {
    if (!error_.empty()) throw ApiError("task_file_invalid", error_, "Repair the task files in .oe/team/tasks; unreadable records are never overwritten.");
    if (directory_.empty()) throw ApiError("no_project", "no task store is open");
}

void TeamTasks::Save(const Json& task) {
    RequireWritable();
    const std::string id = task["id"].asString();
    const std::string path = JoinPath(directory_, id + ".json");
    const std::string bytes = task.dump(2) + "\n";
    std::string error;
    if (FileExists(path)) {
        std::string previous, parseError;
        const auto known = tasks_.find(id);
        if (!ReadTextFile(path, previous) || previous.size() > kMaxTaskBytes || known == tasks_.end()) {
            error_ = "task file changed or is unreadable: " + id;
            throw ApiError("task_file_invalid", error_, "Repair the task file before changing it.");
        }
        const Json disk = Json::parse(previous, &parseError);
        if (!parseError.empty() || disk.dump() != known->second.dump()) {
            error_ = "task file changed or is invalid: " + id;
            throw ApiError("task_file_invalid", error_, "Reopen the task store after repairing the file.");
        }
    }
    if (bytes.size() > kMaxTaskBytes || !WriteTextFileAtomic(path, bytes, &error)) {
        error_ = "cannot write task " + id + ": " + error;
        throw ApiError("write_failed", error_, "Check that .oe/team/tasks is writable.");
    }
    tasks_[id] = task;
}

void TeamTasks::Open(const std::string& directory) {
    directory_ = directory;
    error_.clear();
    tasks_.clear();
    nextId_ = 1;
    for (const std::string& file : ListFiles(directory_, ".json", false)) {
        std::string bytes, parseError;
        if (!ReadTextFile(file, bytes) || bytes.size() > kMaxTaskBytes) { error_ = "cannot read bounded task file " + file; break; }
        Json task = Json::parse(bytes, &parseError);
        const std::string id = task["id"].asString("");
        const std::string suffix = id.size() > 5 ? id.substr(5) : "";
        unsigned long long number = 0;
        bool valid = id.rfind("task-", 0) == 0 && !suffix.empty() && suffix.size() <= 16;
        for (char c : suffix) {
            if (c < '0' || c > '9') { valid = false; break; }
            number = number * 10 + static_cast<unsigned long long>(c - '0');
        }
        const std::string state = task["state"].asString("");
        if (!parseError.empty() || !valid || number == 0 || std::filesystem::u8path(file).filename().u8string() != id + ".json" || task["version"].asInt() != 1 || !task["verified"].isBool() || task["verified"].asBool() ||
            (state != "running" && state != "done" && state != "failed" && state != "timed_out" && state != "cancelled") || tasks_.size() >= kMaxTasks) {
            error_ = "invalid task file " + file; break;
        }
        tasks_[id] = std::move(task);
        nextId_ = std::max(nextId_, number + 1);
    }
    if (!error_.empty()) return;
    for (auto& entry : tasks_) {
        if (entry.second["state"].asString() != "running") continue;
        Json interrupted = entry.second;
        interrupted["state"] = "failed";
        interrupted["summary"] = "The hosting session ended before the task finished; it was not retried.";
        Save(interrupted);
    }
}

Json TeamTasks::Create(const Json& brief, const std::string& from, const std::string& to) {
    RequireWritable();
    if (tasks_.size() >= kMaxTasks) throw ApiError("tasks_full", "at most 256 stored tasks", "Use team.clear after keeping the evidence you need.");
    Json task = Json::MakeObject();
    task["version"] = 1;
    task["id"] = "task-" + std::to_string(nextId_);
    task["from"] = from;
    task["to"] = to;
    task["goal"] = Text(brief, "goal", 16384, true);
    task["acceptance"] = Text(brief, "acceptance", 8192);
    task["files"] = Paths(brief, "files");
    task["context"] = Text(brief, "context", 16384);
    const Json* duration = brief.find("maxDurationMs");
    const double milliseconds = duration ? duration->asNumber(-1) : 1800000;
    if ((duration && !duration->isNumber()) || milliseconds != std::floor(milliseconds) || milliseconds < 100 || milliseconds > 7200000) {
        throw ApiError("invalid_argument", "maxDurationMs must be an integer from 100 to 7200000");
    }
    task["maxDurationMs"] = milliseconds;
    if (brief.has("deadlineAt")) task["deadlineAt"] = brief["deadlineAt"];
    task["state"] = "running";
    task["verified"] = false;
    task["reportReceived"] = false;
    task["unverified"] = true;
    Save(task);
    ++nextId_;
    return task;
}

Json TeamTasks::Get(const std::string& id) const {
    RequireWritable();
    auto found = tasks_.find(id);
    if (found == tasks_.end()) throw ApiError("unknown_task", "no task '" + id + "'", "team.tasks lists the stored tasks.");
    return found->second;
}

Json TeamTasks::List() const {
    RequireWritable();
    Json list = Json::MakeArray();
    for (const auto& entry : tasks_) list.push(entry.second);
    return list;
}

void TeamTasks::Report(const std::string& id, const Json& report) {
    Json task = Get(id);
    if (task["state"].asString() != "running" || task["reportReceived"].asBool()) throw ApiError("task_closed", "the task cannot accept another report");
    task["summary"] = Text(report, "summary", 8192, true);
    if (!report.has("changed")) throw ApiError("invalid_argument", "changed is required (use an empty array when no files changed)");
    task["changed"] = Paths(report, "changed");
    const Json& evidence = report["evidence"];
    if (!evidence.isArray() || evidence.size() == 0 || evidence.size() > 16) throw ApiError("invalid_argument", "evidence must contain 1..16 command/result/output records");
    Json records = Json::MakeArray();
    for (const Json& item : evidence.items()) {
        Json record = Json::MakeObject();
        record["command"] = Text(item, "command", 4096, true);
        if (!item.has("result") || item["result"].dump().size() > 1024) throw ApiError("invalid_argument", "evidence.result must be a bounded command result or exit code");
        record["result"] = item["result"];
        record["output"] = TrimOutput(Text(item, "output", 32768, true));
        records.push(std::move(record));
    }
    task["evidence"] = std::move(records);
    task["reportReceived"] = true;
    task["unverified"] = false; // evidence was submitted; verified remains false until acceptance is checked
    Save(task); // remains running until the CLI also finishes successfully
}

void TeamTasks::Finish(const std::string& id, const std::string& state, const std::string& summary, double seconds, double costUsd, int tools) {
    Json task = Get(id);
    if (task["state"].asString() != "running") return;
    task["state"] = state;
    if (!summary.empty()) task["summary"] = TrimOutput(summary);
    task["seconds"] = seconds;
    task["costUsd"] = costUsd;
    task["tools"] = tools;
    Save(task);
}

void TeamTasks::Clear() {
    RequireWritable();
    if (!RemoveAll(directory_)) throw ApiError("write_failed", "cannot clear task files");
    tasks_.clear(); // retain nextId so a blocked caller never sees an id reused
}

Json AwaitTeamTask(const std::function<Json(const std::string&, const Json&)>& call, Json envelope) {
    if (!envelope["ok"].asBool()) return envelope;
    const std::string id = envelope["result"]["id"].asString("");
    if (id.empty()) return envelope;
    Json args = Json::MakeObject();
    args["id"] = id;
    while (envelope["ok"].asBool() && envelope["result"]["state"].asString() == "running") {
        PlatformSleep(0.05);
        envelope = call("team.task", args);
    }
    return envelope;
}

} // namespace oe
