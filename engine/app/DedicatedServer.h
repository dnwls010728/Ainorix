#pragma once
#include <functional>
#include <string>
#include <vector>
#include "core/Json.h"

namespace oe {
class Engine;
// Runs fixed 60 Hz I/O/simulation without creating a window, GPU or speaker device.
// maxTicks zero waits for SIGINT/SIGTERM; started receives the bound endpoint envelope.
Json RunDedicatedServer(Engine& engine, const Json& arguments, uint64_t maxTicks,
                        const std::function<void(const Json&)>& started = {});
// One server, several worlds: an Engine per scene of `scenes` (paths relative to the project at
// `project`, a directory or its project.json), all in one session on one port (Session worlds), stepped
// in turn by the same 60 Hz loop. A player connects once and is in world 0 (the first scene); the game
// moves it between worlds with net.move, its connection stays. The project needs an authoritative
// network with lateJoin. `prepare` is called for each
// engine before it opens its scene (launch parameters), `opened` after (a tool's API server); either
// returning false stops with its error. `started` receives each world's endpoint envelope, with
// "world" (index) and "scene". The result is {worlds: [state...], ticks}; a world that fails stops all.
Json RunDedicatedWorlds(const std::string& project, const std::vector<std::string>& scenes, const Json& arguments, uint64_t maxTicks,
                        const std::function<bool(Engine&, std::string*)>& prepare = {},
                        const std::function<bool(size_t, Engine&, std::string*)>& opened = {},
                        const std::function<void(const Json&)>& started = {});
// "a.scene.json,b.scene.json" -> the list (empty names dropped).
std::vector<std::string> SplitSceneList(const std::string& text);
// Command line `--set name=value` / `--set-file name=path` -> Engine::SetLaunchArg (Lua game.arg).
// A file's text (at most 4096 bytes, trailing whitespace removed) becomes the value, which keeps
// secrets such as service keys off the command line.
bool ApplyLaunchArg(Engine& engine, const std::string& assignment, bool fromFile, std::string* error);
}  // namespace oe
