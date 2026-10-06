#pragma once
#include <functional>
#include "core/Json.h"

namespace oe {
class Engine;
// Runs fixed 60 Hz I/O/simulation without creating a window, GPU or speaker device.
// maxTicks zero waits for SIGINT/SIGTERM; started receives the bound endpoint envelope.
Json RunDedicatedServer(Engine& engine, const Json& arguments, uint64_t maxTicks,
                        const std::function<void(const Json&)>& started = {});
// Command line `--set name=value` / `--set-file name=path` -> Engine::SetLaunchArg (Lua game.arg).
// A file's text (at most 4096 bytes, trailing whitespace removed) becomes the value, which keeps
// secrets such as service keys off the command line.
bool ApplyLaunchArg(Engine& engine, const std::string& assignment, bool fromFile, std::string* error);
}  // namespace oe
