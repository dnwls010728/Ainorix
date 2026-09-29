#pragma once
#include <string>

#include "api/HttpServer.h"

namespace oe {

class Engine;

// HTTP routes shared by the web editor and external tools:
//   GET  /                  editor UI (static files from editorDir)
//   POST /api/call          {"command": "...", "args": {...}} -> envelope
//   GET  /api/commands      command list with schemas
//   GET  /api/frame.png     rendered frame (?w,h,eye=x,y,z,target=x,y,z,fov,grid,colliders,sel,game)
//   WS   /api/stream        viewport stream: each text message {w,h,eye,target,fov,grid,colliders,sel,game,quality}
//                           is answered with one binary JPEG frame; the first message from the
//                           server is {"type":"hello","renderer":...,"gpu":bool}
// Frames come from the display renderer (GPU when enabled, else software).
// Handlers run on the server thread and marshal engine work to the main thread.
HttpResponse HandleEditorRequest(Engine& engine, const std::string& editorDir, const HttpRequest& request);

// WebSocket side of the routes above (register with HttpServer::OnWebSocket).
bool AcceptEditorStream(const HttpRequest& request, int* status);
void RunEditorStream(Engine& engine, const HttpRequest& request, WebSocket& socket);

// Registers the editor routes and the viewport stream on `server`.
bool StartEditorServer(HttpServer& server, Engine& engine, int port, std::string* error);

// Locates the editor web files (next to the executable, then the source tree).
std::string FindEditorDir();

}  // namespace oe
