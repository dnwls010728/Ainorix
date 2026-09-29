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
// Handlers run on the server thread and marshal engine work to the main thread.
HttpResponse HandleEditorRequest(Engine& engine, const std::string& editorDir, const HttpRequest& request);

// Locates the editor web files (next to the executable, then the source tree).
std::string FindEditorDir();

}  // namespace oe
