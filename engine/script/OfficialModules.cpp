#include "script/OfficialModules.h"

#include <algorithm>

#include "app/Engine.h"
#include "core/FileSystem.h"
#include "core/Image.h"

namespace oe {
namespace {
bool Ends(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}
std::string CanonicalSegments(const std::string& body, char separator) {
    std::string result = "oe";
    size_t start = 0;
    while (start <= body.size()) {
        size_t end = body.find(separator, start);
        if (end == std::string::npos) end = body.size();
        if (end == start || body[start] < 'a' || body[start] > 'z') return {};
        for (size_t i = start; i < end; ++i) {
            char c = body[i];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return {};
        }
        result += "." + body.substr(start, end - start);
        if (end == body.size()) return result;
        start = end + 1;
    }
    return {};
}
void Invalid(const std::string& path) {
    throw ApiError("official_name", "invalid reserved Lua spelling '" + path + "'",
                   "Use require('oe.ui') or require('oe/ui.lua'); source tools use @oe/ui.lua (lowercase, forward slashes).");
}
}  // namespace

bool IsOfficialRoot(const std::string& spelling) {
    size_t start = !spelling.empty() && spelling[0] == '@' ? 1 : 0;
    if (spelling.size() < start + 2) return false;
    char a = spelling[start], b = spelling[start + 1];
    if ((a != 'o' && a != 'O') || (b != 'e' && b != 'E')) return false;
    return spelling.size() == start + 2 || spelling[start + 2] == '.' ||
           spelling[start + 2] == '/' || spelling[start + 2] == '\\';
}

std::string ParseOfficialImport(const std::string& spelling) {
    if (!IsOfficialRoot(spelling)) return {};
    std::string name;
    if (spelling.rfind("oe.", 0) == 0 && !Ends(spelling, ".lua"))
        name = CanonicalSegments(spelling.substr(3), '.');
    else if (spelling.rfind("oe/", 0) == 0) {
        std::string body = spelling.substr(3);
        if (Ends(body, ".lua")) body.resize(body.size() - 4);
        name = CanonicalSegments(body, '/');
    }
    if (name.empty()) Invalid(spelling);
    return name;
}

const OfficialModule* FindOfficialModule(const std::string& name) {
    for (const auto& module : OfficialManifest()) if (module.name == name) return &module;
    return nullptr;
}

const OfficialModule& ResolveOfficialSource(const std::string& path) {
    if (path.rfind("@oe/", 0) != 0 || !Ends(path, ".lua")) Invalid(path);
    std::string name = CanonicalSegments(path.substr(4, path.size() - 8), '/');
    if (name.empty()) Invalid(path);
    const auto* module = FindOfficialModule(name);
    if (!module) throw ApiError("not_found", "official module not found: " + path, "Call script.list for embedded sources.");
    return *module;
}

std::string ResolveProjectScriptPath(Engine& engine, const std::string& path) {
    if ((!path.empty() && path[0] == '@') || IsOfficialRoot(path))
        throw ApiError("official_read_only", "reserved Lua source path: " + path,
                       "Read official sources using @oe/ui.lua; put project wrappers under scripts/.");
    std::string full = engine.ResolvePath(path);
    if (IsOfficialRoot(RelativePath(full, engine.ProjectDir())))
        throw ApiError("official_read_only", "project path conflicts with reserved oe/ namespace: " + path,
                       "Move project helpers under scripts/; official modules cannot be shadowed.");
    return full;
}

ScriptSource ResolveScriptSource(Engine& engine, const std::string& path) {
    if (!path.empty() && path[0] == '@') {
        const auto& module = ResolveOfficialSource(path);
        return {module.path, module.source, true};
    }
    std::string full = ResolveProjectScriptPath(engine, path), source;
    if (!ReadTextFile(full, source)) throw ApiError("not_found", "cannot read " + path, "Call script.list to see script files.");
    return {path, source, false};
}

std::string OfficialManifestDigest() {
    return OfficialManifestDigest(OfficialManifest(), kOfficialPrimitiveRevision);
}

std::string OfficialManifestDigest(std::vector<OfficialModule> modules, int primitiveRevision) {
    std::sort(modules.begin(), modules.end(), [](const OfficialModule& a, const OfficialModule& b) { return a.name < b.name; });
    std::string data = "ownengine.official-lua.v1:" + std::to_string(primitiveRevision) + ":";
    for (auto& module : modules) {
        std::string normalized;
        for (size_t i = 0; i < module.source.size(); ++i) {
            char c = module.source[i];
            if (c == '\r') {
                if (i + 1 < module.source.size() && module.source[i + 1] == '\n') ++i;
                c = '\n';
            }
            normalized += c;
        }
        module.source = std::move(normalized);
        data += std::to_string(module.name.size()) + ":" + module.name + ":";
        data += std::to_string(module.source.size()) + ":" + module.source + ":";
    }
    uint64_t hash = Fnv1a64(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    std::string result(16, '0');
    for (size_t i = result.size(); i > 0; --i) {
        result[i - 1] = "0123456789abcdef"[hash & 15]; hash >>= 4;
    }
    return result;
}
}  // namespace oe
