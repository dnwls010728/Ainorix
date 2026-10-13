#pragma once
#include "core/Json.h"

namespace oe {
class Engine;
class CommandRegistry;
// Validate the bounded controller format; evaluation belongs to oe.animator in Lua.
bool ValidateAnimationController(const Json& values, std::string* error);
// Read a validated project controller (also works from packaged files).
Json ReadAnimationController(Engine& engine, const std::string& path);
// Register controller asset commands; file edits never alter scene undo/revision.
void RegisterAnimationControllerCommands(CommandRegistry& registry);
}  // namespace oe
