#pragma once
#include <string>

#include "api/HttpServer.h"

namespace oe {

class Engine;

// The command API over HTTP (localhost only), for agents and tools while an
// engine runs (`oe editor`, `oe run --port`, `oe mcp --port`):
//   POST /api/call      {"command": "...", "args": {...}} -> {"ok":..., "result"|"error"}
//   GET  /api/commands  command list with schemas
// `oe mcp --connect <port>` turns it into an MCP server. Handlers run on the
// server thread and hand the work to the engine's main thread.
HttpResponse HandleApiRequest(Engine& engine, const HttpRequest& request);

// Serves HandleApiRequest on 127.0.0.1:port.
bool StartApiServer(HttpServer& server, Engine& engine, int port, std::string* error);

}  // namespace oe
