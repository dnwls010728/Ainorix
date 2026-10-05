#include "team/TeamUsage.h"

#include <cmath>
#include <filesystem>
#include <map>

#include "api/Commands.h"
#include "core/FileSystem.h"

namespace oe {
namespace {
constexpr size_t kMaxBytes = 16 * 1024 * 1024, kMaxRecords = 20000;
bool ValidCount(const Json& value) {
    const double n = value.asNumber(-1);
    return value.isNumber() && std::isfinite(n) && n >= 0 && n <= 1e12 && std::floor(n) == n;
}
bool ValidRecord(const Json& j) {
    if (!j.isObject() || j["version"].asInt() != 1 || !j["agent"].isString() || !j["backend"].isString() ||
        !j["date"].isString() || j["date"].asString().size() != 10 || !j["tokensKnown"].isBool() || !j["costKnown"].isBool() || !j["session"].isString() || !j["reportedCostKnown"].isBool()) return false;
    for (const char* k : {"inputTokens", "cacheReadTokens", "cacheCreationTokens", "outputTokens"}) if (!ValidCount(j[k])) return false;
    const double reported = j["reportedCostUsd"].asNumber(-1);
    if (!j["reportedCostUsd"].isNumber() || !std::isfinite(reported) || reported < 0 || reported > 1e6) return false;
    const double cost = j["costUsd"].asNumber(-1);
    return j["costUsd"].isNumber() && std::isfinite(cost) && cost >= 0 && cost <= 1e6 &&
           j["cacheReadTokens"].asNumber() + j["cacheCreationTokens"].asNumber() <= j["inputTokens"].asNumber();
}
Json Totals() {
    Json j = Json::MakeObject();
    for (const char* k : {"turns", "reportedTurns", "costReportedTurns", "inputTokens", "cacheReadTokens", "cacheCreationTokens", "outputTokens", "costUsd"}) j[k] = 0;
    return j;
}
void Add(Json& total, const Json& record) {
    total["turns"] = total["turns"].asNumber() + 1;
    total["reportedTurns"] = total["reportedTurns"].asNumber() + (record["tokensKnown"].asBool() ? 1 : 0);
    total["costReportedTurns"] = total["costReportedTurns"].asNumber() + (record["costKnown"].asBool() ? 1 : 0);
    for (const char* k : {"inputTokens", "cacheReadTokens", "cacheCreationTokens", "outputTokens", "costUsd"}) total[k] = total[k].asNumber() + record[k].asNumber();
}
}  // namespace
void TeamUsage::Open(const std::string& path) {
    path_ = path; bytes_.clear(); error_.clear(); records_.clear();
    if (!FileExists(path_)) return;
    std::error_code ec;
    const auto size = std::filesystem::file_size(std::filesystem::u8path(path_), ec);
    if (ec || size > kMaxBytes || !ReadTextFile(path_, bytes_)) { error_ = "cannot read bounded usage ledger"; return; }
    for (size_t start = 0; start < bytes_.size();) {
        size_t end = bytes_.find('\n', start); if (end == std::string::npos) end = bytes_.size();
        if (end > start) {
            std::string error; Json j = Json::parse(bytes_.substr(start, end - start), &error);
            if (!error.empty() || !ValidRecord(j) || records_.size() >= kMaxRecords) { error_ = "invalid usage ledger"; return; }
            records_.push_back(std::move(j));
        }
        start = end + 1;
    }
}
void TeamUsage::Append(const Json& record) {
    if (!error_.empty()) throw ApiError("usage_file_invalid", error_, "Repair .oe/team/usage.jsonl; it is never overwritten when unreadable.");
    if (!ValidRecord(record)) throw ApiError("invalid_argument", "invalid usage record");
    std::string disk;
    if (FileExists(path_) ? (!ReadTextFile(path_, disk) || disk != bytes_) : !bytes_.empty()) {
        error_ = "usage ledger changed outside this session";
        throw ApiError("usage_file_invalid", error_, "Reopen the project after checking .oe/team/usage.jsonl.");
    }
    const std::string next = bytes_ + (bytes_.empty() || bytes_.back() == '\n' ? "" : "\n") + record.dump() + "\n";
    if (records_.size() >= kMaxRecords || next.size() > kMaxBytes) throw ApiError("usage_full", "usage ledger reached its bound", "Archive usage.jsonl and reopen the project.");
    std::string error;
    if (!WriteTextFileAtomic(path_, next, &error)) {
        error_ = "cannot write usage ledger: " + error;
        throw ApiError("write_failed", error_, "Check .oe/team/usage.jsonl permissions.");
    }
    bytes_ = next; records_.push_back(record);
}
Json TeamUsage::Baseline(const std::string& backend, const std::string& session) const {
    if (!error_.empty()) throw ApiError("usage_file_invalid", error_, "Repair usage.jsonl and reopen the project.");
    for (auto it = records_.rbegin(); it != records_.rend(); ++it) {
        if ((*it)["backend"].asString() == backend && (*it)["session"].asString() == session && (*it)["reportedCostKnown"].asBool()) return *it;
    }
    return Json();
}
Json TeamUsage::Read(const std::string& agent, const std::string& date) const {
    if (!error_.empty()) throw ApiError("usage_file_invalid", error_, "Repair .oe/team/usage.jsonl and reopen the project.");
    if (!date.empty()) {
        bool valid = date.size() == 10 && date[4] == '-' && date[7] == '-';
        for (size_t i = 0; i < date.size(); ++i) if (i != 4 && i != 7 && (date[i] < '0' || date[i] > '9')) valid = false;
        if (!valid) throw ApiError("invalid_argument", "date must be YYYY-MM-DD in UTC");
    }
    std::map<std::string, Json> agents, days;
    Json total = Totals();
    for (const Json& record : records_) {
        const std::string id = record["agent"].asString(), day = record["date"].asString();
        if ((!agent.empty() && id != agent) || (!date.empty() && day != date)) continue;
        if (!agents.count(id)) { agents[id] = Totals(); agents[id]["id"] = id; }
        if (!days.count(day)) { days[day] = Totals(); days[day]["date"] = day; }
        Add(total, record); Add(agents[id], record); Add(days[day], record);
    }
    Json out = Json::MakeObject(); out["total"] = total; out["agents"] = Json::MakeArray(); out["days"] = Json::MakeArray();
    for (const auto& entry : agents) out["agents"].push(entry.second);
    for (const auto& entry : days) out["days"].push(entry.second);
    return out;
}
}  // namespace oe
