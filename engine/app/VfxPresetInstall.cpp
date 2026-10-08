#include "app/VfxPresetBundle.h"

#include <iomanip>
#include <map>
#include <sstream>

#include "app/Engine.h"
#include "app/Vfx.h"
#include "assets/Assets.h"
#include "core/FileSystem.h"
#include "core/Image.h"

namespace oe {
namespace {
std::string BundlePrefix() {
    static const std::string prefix = [] {
        std::string bytes;
        for (const auto& file : VfxPresetFiles()) { bytes += file.path; bytes += '\0'; bytes += file.data; }
        std::ostringstream hash;
        hash << std::hex << std::setw(16) << std::setfill('0')
             << Fnv1a64(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
        return "assets/vfx-presets/" + hash.str() + "/";
    }();
    return prefix;
}

Json FindPreset(const std::string& name) {
    const Json presets = VfxPresets("");
    for (const auto& preset : presets.items()) if (preset["name"].asString() == name) return preset;
    throw ApiError("unknown_preset", "unknown VFX preset '" + name + "'", "Call vfx.presets to list names and intent.");
}

void CheckDestination(Engine& engine, const std::string& path, const std::string& data) {
    const std::string full = engine.ResolvePath(path);
    if (!FileExists(full)) return;
    std::string current;
    if (!ReadTextFile(full, current) || current != data)
        throw ApiError("preset_conflict", "preset destination has different content: " + path,
                       "Preserve your edited file; use a separate project or restore this bundled dependency before retrying.");
}

// Collect reachable dependencies while remapping references; materials also reference shader files.
void Remap(Json& value, const std::map<std::string, const VfxPresetFile*>& files, std::map<std::string, std::string>& pending) {
    if (value.isObject()) {
        for (auto& member : value.members()) Remap(member.second, files, pending);
    } else if (value.isArray()) {
        for (auto& item : value.items()) Remap(item, files, pending);
    } else if (value.isString()) {
        const auto found = files.find(value.asString());
        if (found == files.end()) return;
        const std::string path = BundlePrefix() + found->first;
        value = path;
        if (pending.count(path)) return;
        const auto& file = *found->second;
        pending[path] = file.data;
        if (file.path.size() >= 5 && file.path.substr(file.path.size() - 5) == ".json") {
            Json document = Json::parse(file.data);
            Remap(document, files, pending);
            pending[path] = document.dump(2) + "\n";
        }
    }
}
}  // namespace

Json VfxAcceptancePresets() {
    static const Json presets = [] {
        const char* names[] = {"ice_pillar", "tornado", "shield_dome", "shockwave", "rain_snow", "healing_aura"};
        const char* intents[] = {
            "An ice pillar rises from its base, glows and shatters into fragments.",
            "Wind and glowing motes spiral upward around a widening tornado.",
            "A translucent shield dome pulses with a bright rim and fades.",
            "A glowing ground ring expands outward and dissolves across its width.",
            "World-space rain falls beside drifting snow and expires at the spawn-relative floor.",
            "Green healing motes spiral upward and brighten over their individual lifetimes."};
        const char* tags[] = {"ice", "wind", "shield", "impact", "weather", "healing"};
        Json result = Json::MakeArray();
        for (int i = 0; i < 6; ++i) {
            Json preset = Json::MakeObject(); preset["name"] = names[i]; preset["intent"] = intents[i];
            preset["tags"] = Json(Json::Array{tags[i], "acceptance", "burst"});
            preset["bundled"] = true;
            for (const auto& file : VfxPresetFiles()) if (file.path == std::string("effects/") + names[i] + ".vfx.json") {
                preset["vfx"] = Json::parse(file.data);
                preset["emitters"] = preset["vfx"]["emitters"];
                preset["trails"] = preset["vfx"].has("trails") ? preset["vfx"]["trails"] : Json::MakeArray();
                break;
            }
            result.push(preset);
        }
        return result;
    }();
    return presets;
}

namespace {
struct PreparedPreset { Json document; std::map<std::string, std::string> files; };

PreparedPreset PreparePreset(const std::string& name) {
    const Json preset = FindPreset(name);
    Json document;
    if (!preset["bundled"].asBool()) {
        document["format"] = "oe.vfx"; document["version"] = 1; document["name"] = name;
        document["emitters"] = preset["emitters"]; document["trails"] = preset["trails"];
        return {ParseVfxAsset(document).json, {}};
    }
    document = preset["vfx"];
    std::map<std::string, const VfxPresetFile*> files;
    for (const auto& file : VfxPresetFiles()) files[file.path] = &file;
    std::map<std::string, std::string> pending;
    Remap(document, files, pending);
    document = ParseVfxAsset(document).json;
    return {document, pending};
}

void InstallPresetFiles(Engine& engine, const std::map<std::string, std::string>& pending) {
    // Refuse all known conflicts before installation. An I/O failure may leave unused dependencies,
    // but never publishes a partially validated effect or changes scene/document history.
    for (const auto& file : pending) CheckDestination(engine, file.first, file.second);
    for (const auto& file : pending) if (!FileExists(engine.ResolvePath(file.first))) {
        std::string error;
        if (!WriteTextFileAtomic(engine.ResolvePath(file.first), file.second, &error)) throw ApiError("write_failed", error);
        engine.Assets().Forget(file.first);
    }
}
}  // namespace

Json VfxPresetDocument(Engine& engine, const std::string& name) {
    const auto preset = PreparePreset(name);
    InstallPresetFiles(engine, preset.files);
    return preset.document;
}

std::string VfxPresetAsset(Engine& engine, const std::string& name) {
    const auto preset = PreparePreset(name);
    const Json& document = preset.document;
    const std::string path = BundlePrefix() + "effects/" + name + ".vfx.json";
    if (engine.VfxDocument(path)) throw ApiError("vfx_editing", "bundled preset asset is open in Effects", "Save a copy and close the draft first.");
    CheckDestination(engine, path, document.dump(2) + "\n");
    InstallPresetFiles(engine, preset.files);
    if (!FileExists(engine.ResolvePath(path))) SaveVfxDocument(engine, path, document);
    return path;
}
}  // namespace oe
