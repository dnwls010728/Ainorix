#pragma once
#include <functional>
#include <map>
#include <string>

#include "core/Json.h"

namespace oe {

// Persistent, bounded task records. All access is on the team's main thread.
// A report is evidence to check, never an automatic verification of acceptance.
class TeamTasks {
public:
    void Open(const std::string& directory); // marks interrupted running records failed; never retries
    Json Create(const Json& brief, const std::string& from, const std::string& to);
    Json Get(const std::string& id) const;
    Json List() const;
    void Report(const std::string& id, const Json& report);
    void Finish(const std::string& id, const std::string& state, const std::string& summary,
                double seconds = 0.0, double costUsd = 0.0, int tools = 0);
    void Clear(); // removes readable task records; an invalid store is never overwritten
private:
    void RequireWritable() const;
    void Save(const Json& task);
    std::string directory_, error_;
    std::map<std::string, Json> tasks_;
    unsigned long long nextId_ = 1;
};

// Called on the MCP I/O thread after the nonblocking delegate command returns.
// Polls through the caller, so every engine call remains short and the editor can update.
Json AwaitTeamTask(const std::function<Json(const std::string&, const Json&)>& call, Json envelope);

} // namespace oe
