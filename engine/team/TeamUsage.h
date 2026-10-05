#pragma once
#include <string>
#include <vector>
#include "core/Json.h"

namespace oe {
// Bounded, atomic JSON-lines ledger; invalid/external changes are never overwritten.
class TeamUsage {
public:
    void Open(const std::string& path);
    void Append(const Json& record);
    // Last reported cumulative bill for this session, including unknown-delta records.
    Json Baseline(const std::string& backend, const std::string& session) const;
    Json Read(const std::string& agent = "", const std::string& date = "") const;
private:
    std::string path_, bytes_, error_;
    std::vector<Json> records_;
};
}  // namespace oe
