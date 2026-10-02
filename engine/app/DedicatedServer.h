#pragma once
#include <functional>
#include "core/Json.h"

namespace oe {
class Engine;
// Runs fixed 60 Hz I/O/simulation without creating a window, GPU or speaker device.
// maxTicks zero waits for SIGINT/SIGTERM; started receives the bound endpoint envelope.
Json RunDedicatedServer(Engine& engine, const Json& arguments, uint64_t maxTicks,
                        const std::function<void(const Json&)>& started = {});
}  // namespace oe
