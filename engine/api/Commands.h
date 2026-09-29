#pragma once
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/Json.h"

namespace oe {

class Engine;

// Error returned to API callers. `code` is a stable machine-readable string,
// `hint` tells the caller (often an AI agent) how to fix the request.
struct ApiError : std::runtime_error {
    ApiError(std::string code_, const std::string& message, std::string hint_ = "")
        : std::runtime_error(message), code(std::move(code_)), hint(std::move(hint_)) {}
    std::string code;
    std::string hint;
};

// One API command. The same registry is exposed through the CLI (`oe exec`),
// the HTTP API used by the editor, and the MCP server used by AI agents.
struct Command {
    std::string name;     // "entity.create"
    std::string summary;  // one line, imperative
    Json params;          // JSON schema of the arguments object
    bool mutates = false; // true: changes the scene and records an undo step
    std::function<Json(Engine&, const Json& args)> run;
};

class CommandRegistry {
public:
    void Add(Command command);
    const Command* Find(const std::string& name) const;
    const std::vector<Command>& All() const { return commands_; }

private:
    std::vector<Command> commands_;
};

// Tiny builder for argument schemas.
class Params {
public:
    Params();
    Params& Req(const char* name, const char* type, const char* doc);
    Params& Opt(const char* name, const char* type, const char* doc);
    Params& OptWith(const char* name, Json schema);
    Params& ReqWith(const char* name, Json schema);
    operator Json() const { return schema_; }

private:
    Params& Prop(const char* name, const char* type, const char* doc, bool required);
    Json schema_;
};

// Checks required properties, primitive types and unknown properties.
void ValidateArgs(const Command& command, const Json& args);

void RegisterBuiltinCommands(CommandRegistry& registry);

}  // namespace oe
