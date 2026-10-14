#pragma once
#include "core/Json.h"
#include <string>
namespace oe {
class CommandRegistry;
// Normalize OwnEngine or TexturePacker JSON. Rect/trim use top-left pixel coordinates;
// pivots use normalized original-canvas coordinates with Y pointing upward.
bool NormalizeSpriteAtlas(const Json& input, Json& output, std::string* error);
// Resolve an atlas frame by name or zero-based index; null when absent.
const Json* FindAtlasFrame(const Json& atlas, const Json& frame);
void RegisterAtlasCommands(CommandRegistry& registry);
}  // namespace oe
