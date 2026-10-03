#include "app/SaveStore.h"

#include <cmath>

#include "api/Commands.h"
#include "core/FileSystem.h"
#include "core/Log.h"
#include "platform/Platform.h"

namespace oe {
namespace {

void ValidateValue(const Json& value, int depth = 0) {
    if (depth > 32 || (value.isNumber() && !std::isfinite(value.asNumber())))
        throw ApiError("invalid_save_value", "save value must be finite JSON with at most 32 nested levels",
                       "Use numbers, strings, booleans, null, arrays or objects.");
    for (const auto& item : value.items()) ValidateValue(item, depth + 1);
    for (const auto& member : value.members()) ValidateValue(member.second, depth + 1);
}

void ValidateKey(const std::string& key) {
    if (key.empty() || key.find('\0') != std::string::npos)
        throw ApiError("invalid_save_key", "save key must be nonempty and contain no NUL",
                       "Use a nonempty string such as highScore.");
}

}  // namespace

void SaveStore::Configure(const std::string& directory) {
    storage_ = {};
    directory_ = directory.empty() ? "" : AbsolutePath(directory);
    slots_.clear(); deferred_.clear(); frozenReads_ = deferFlush_ = false;
}

void SaveStore::ConfigurePlayer(const std::string& gameName) {
    std::string safe;
    const char* hex = "0123456789abcdef";
    for (unsigned char c : gameName) {
        if (c >= 32 && c != '<' && c != '>' && c != ':' && c != '"' && c != '/' && c != '\\' &&
            c != '|' && c != '?' && c != '*' && c != '%' && c != '.' && c != ' ') safe += static_cast<char>(c);
        else { safe += '%'; safe += hex[c >> 4]; safe += hex[c & 15]; }
    }
    if (safe.empty()) safe = "OwnEngine";
    Configure("");
    storage_ = PlatformSaveStorage(safe);
    directory_ = storage_.directory;
    if (directory_.empty() && (!storage_.read || !storage_.write))
        throw ApiError("save_unavailable", "platform save storage is unavailable", "Check the user's data directory configuration.");
}

SaveStore::Slot& SaveStore::Load(const std::string& slot) {
    bool valid = !slot.empty() && slot.size() <= 64;
    for (char c : slot)
        valid = valid && ((c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '_' || c == '-');
    if (!valid) throw ApiError("invalid_save_slot", "invalid save slot name",
                              "Use 1-64 lowercase ASCII letters, digits, underscores or hyphens.");
    auto inserted = slots_.emplace(slot, Slot{});
    Slot& result = inserted.first->second;
    if (inserted.second && !frozenReads_ && (!directory_.empty() || storage_.read)) {
        std::string path = JoinPath(directory_, "slot-" + slot + ".json"), text;
        if (storage_.read || FileExists(path)) {
            std::string error;
            Json data;
            bool read = storage_.read ? storage_.read(slot, text, &error) : ReadTextFile(path, text);
            if (read && storage_.read && text.empty()) return result;
            if (read) data = Json::parse(text, &error);
            else if (error.empty()) error = "cannot read file";
            if (error.empty() && data.isObject()) {
                try {
                    for (const auto& member : data.members()) { ValidateKey(member.first); ValidateValue(member.second); }
                    result.data = std::move(data);
                }
                catch (const ApiError& e) { error = e.what(); }
            } else if (error.empty()) error = "expected a JSON object";
            if (!error.empty()) OE_LOG_WARN("save", "Ignoring invalid save %s: %s", path.c_str(), error.c_str());
        }
    }
    return result;
}

Json SaveStore::State(const std::string& slot) {
    Slot& value = Load(slot);
    Json out = Json::MakeObject();
    out["mode"] = storage_.read ? "localStorage" : directory_.empty() ? "memory" : "directory";
    out["slot"] = slot;
    out["data"] = value.data;
    out["dirty"] = value.dirty;
    return out;
}

Json SaveStore::Get(const std::string& key, const Json& fallback, const std::string& slot) {
    ValidateKey(key);
    const Json* value = Load(slot).data.find(key);
    return value ? *value : fallback;
}

void SaveStore::Set(const std::string& key, const Json& value, const std::string& slot) {
    ValidateKey(key);
    ValidateValue(value);
    Slot& target = Load(slot);
    if (!target.data.has(key) || target.data[key] != value) {
        target.data[key] = value;
        target.dirty = true;
    }
}

void SaveStore::Clear(const std::string& key, const std::string& slot) {
    if (!key.empty()) ValidateKey(key);
    Slot& target = Load(slot);
    if (key.empty()) {
        if (target.data.size() > 0) { target.data = Json::MakeObject(); target.dirty = true; }
    } else if (target.data.erase(key)) target.dirty = true;
}

void SaveStore::FreezeReads(bool freeze) {
    if (freeze && !frozenReads_ && !directory_.empty()) {
        for (const auto& path : ListFiles(directory_, ".json", false)) {
            std::string name = RelativePath(path, directory_);
            if (name.size() <= 10 || name.compare(0, 5, "slot-") != 0) continue;
            try { Load(name.substr(5, name.size() - 10)); }
            catch (const ApiError& error) { OE_LOG_WARN("save", "Ignoring snapshot slot %s: %s", name.c_str(), error.what()); }
        }
    }
    frozenReads_ = freeze;
}

void SaveStore::DeferFlush(bool defer) {
    const bool commit = deferFlush_ && !defer;
    deferFlush_ = defer;
    if (!commit) return;
    std::set<std::string> slots;
    slots.swap(deferred_);
    for (const auto& slot : slots) {
        try { Flush(slot); }
        catch (const ApiError& error) { OE_LOG_WARN("save", "Deferred flush of slot %s failed: %s", slot.c_str(), error.what()); }
    }
}

void SaveStore::Flush(const std::string& slot) {
    if (deferFlush_) { Load(slot); deferred_.insert(slot); return; }
    Slot& value = Load(slot);
    if (!value.dirty) return;
    if (!directory_.empty() || storage_.write) {
        std::string path = JoinPath(directory_, "slot-" + slot + ".json"), error;
        std::string text = value.data.dump(2) + "\n";
        bool written = storage_.write ? storage_.write(slot, text, &error) : WriteTextFileAtomic(path, text, &error);
        if (!written)
            throw ApiError("save_write_failed", error, "Check the save directory permissions and available space; retry save.flush.");
    }
    value.dirty = false;
}

}  // namespace oe
