#pragma once

#include "core/Json.h"

namespace oe {
class Engine;
// Read-only presentation queries. Named request schema is shared by Lua and registry commands.
Json QueryCoordinates(Engine& engine, const std::string& operation, const Json& request);
}  // namespace oe
