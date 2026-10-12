#pragma once
#include <set>
#include <string>
#include <vector>

#include "core/Json.h"

namespace oe {

// Static checks of Lua scripts, without running them (script.check,
// script.params, the native editor's script panel and inspector).

struct LuaDiagnostic {
    int line = 0;          // 1-based; 0 = unknown
    std::string severity;  // "error" (does not compile) or "warning"
    std::string message;
};

// Compiles `source` (nothing runs) and reports the syntax error, if any.
// When it compiles, walks the bytecode for global variable use: assigning a
// global ("missing local?") and reading a global that is neither in
// `knownGlobals` (the sandbox API) nor assigned in this file ("typo?").
std::vector<LuaDiagnostic> CheckLuaSource(const std::string& source, const std::string& chunkName, const std::set<std::string>& knownGlobals);

// A parameter a script reads from its Script component's params, found in
// the source: `self.params.speed or 5`, `local p = self.params ... p.speed or 5`.
struct ScriptParam {
    std::string name;
    std::string type;          // "number", "boolean", "string", "vec3", "array", "object" or "any" (no default)
    Json defaultValue;         // null when the script gives none
    std::vector<std::string> options;  // string values the script compares it with (enum-like)
    std::string description;   // trailing comment of the line that reads it
    int line = 0;
};

std::vector<ScriptParam> InferScriptParams(const std::string& source);

// Lua source as the bytecode a packaged game ships (lua_dump with debug information, so errors keep
// their file and line). `chunkName` is what messages call the file, e.g. "@scripts/player.lua".
// The same bytes load on every platform the engine builds for (little-endian, 64-bit numbers).
// `strip` leaves out debug information (line numbers, local and upvalue names, the chunk name), so a
// decompiler recovers less; error messages from the script then have no file or line. False with the
// compiler's message when the source does not compile.
bool CompileLuaBytecode(const std::string& source, const std::string& chunkName, std::string& bytecode, std::string* error, bool strip = false);

}  // namespace oe
