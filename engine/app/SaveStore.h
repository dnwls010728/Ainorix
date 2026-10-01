#pragma once
#include <map>
#include <string>

#include "core/Json.h"

namespace oe {

// Persistent JSON slots, isolated from simulation state and scene undo.
// Defaults to memory only; explicit configuration opts into filesystem I/O.
class SaveStore {
public:
    // Starts a fresh store. Empty directory means memory only.
    void Configure(const std::string& directory);
    // Slot inspection lazily loads its file; malformed files log a warning.
    Json State(const std::string& slot = "default");
    // Missing keys return the supplied default; null is a stored value.
    Json Get(const std::string& key, const Json& fallback, const std::string& slot = "default");
    // Rejects invalid slot/key names and non-finite or excessively deep JSON.
    void Set(const std::string& key, const Json& value, const std::string& slot = "default");
    // Removes a key, or clears the whole slot when key is empty.
    void Clear(const std::string& key, const std::string& slot = "default");
    // Writes one dirty slot, atomically replacing its previous file. Throws on failure.
    void Flush(const std::string& slot = "default");

private:
    struct Slot { Json data = Json::MakeObject(); bool dirty = false; };
    Slot& Load(const std::string& slot);
    std::string directory_;
    std::map<std::string, Slot> slots_;
};

}  // namespace oe
