#pragma once
#include <functional>
#include <iosfwd>
#include <string>

#include "core/Json.h"

namespace oe {

// Model Context Protocol server over stdio (newline-delimited JSON-RPC 2.0).
// Every engine command becomes an MCP tool ("entity.create" -> "entity_create").
// render.screenshot results are returned as MCP image content so the agent
// can see the frame directly.
//
// `call` runs a command and returns the standard envelope
// {"ok":bool,"result":...,"error":...}; it may forward to a running editor.
using CommandCaller = std::function<Json(const std::string& command, const Json& args)>;

// Blocks until stdin closes. Returns a process exit code.
int RunMcpServer(const CommandCaller& call, std::istream& in, std::ostream& out);

}  // namespace oe
