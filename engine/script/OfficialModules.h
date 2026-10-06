#pragma once
#include <string>
#include <vector>

namespace oe {
class Engine;

// Immutable generated descriptors; canonical name and virtual path never depend on the filesystem.
struct OfficialModule {
    std::string name;
    std::string path;
    std::string source;
};
const std::vector<OfficialModule>& OfficialManifest();
const OfficialModule* FindOfficialModule(const std::string& canonicalName);
// Case-insensitive root detection only; strict parsing below never lowercases module names.
bool IsOfficialRoot(const std::string& spelling);
// Empty for nonreserved project imports; invalid reserved spellings throw ApiError.
std::string ParseOfficialImport(const std::string& spelling);
// Canonical @oe/<segments>.lua only; invalid or missing sources throw ApiError.
const OfficialModule& ResolveOfficialSource(const std::string& path);
// Reject official/physical reserved paths, then use the engine's containment check.
std::string ResolveProjectScriptPath(Engine& engine, const std::string& path);
struct ScriptSource {
    std::string path;
    std::string source;
    bool embedded = false;
};
// Shared tooling resolver; does not create or execute Lua state.
ScriptSource ResolveScriptSource(Engine& engine, const std::string& path);
// Stable length-prefixed manifest fingerprint, shared by recording and match blueprints.
std::string OfficialManifestDigest();
// Pure canonicalization helper: sorts names, normalizes CRLF/CR, ignores source-path metadata.
std::string OfficialManifestDigest(std::vector<OfficialModule> modules, int primitiveRevision);
constexpr int kOfficialPrimitiveRevision = 1;
}  // namespace oe
