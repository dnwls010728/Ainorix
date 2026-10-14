#pragma once
#include "core/Json.h"
#include <string>
namespace oe {
class CommandRegistry;
struct Texture;
// Normalize OwnEngine or TexturePacker JSON. Rect/trim use top-left pixel coordinates;
// pivots use normalized original-canvas coordinates with Y pointing upward.
bool NormalizeSpriteAtlas(const Json& input, Json& output, std::string* error);
// Normalize a sprite asset (*.sprite.json): an atlas (texture + named frames) with pixelsPerUnit (16),
// pixelArt (true), a default pivot ([0.5, 0.5]) for frames without their own, optional normalTexture,
// the slicing settings it was cut with, and per frame an optional `collision` outline in frame-canvas
// pixels (top-left origin, Y down).
bool NormalizeSpriteAsset(const Json& input, Json& output, std::string* error);
// Resolve an atlas frame by name or zero-based index; null when absent.
const Json* FindAtlasFrame(const Json& atlas, const Json& frame);
// Frames of an image cut by `slicing`: {mode: "cellSize" (cellWidth, cellHeight) | "cellCount" (columns,
// rows) | "auto" (gap), offset: [x, y], spacing: [x, y], trim}. Grid cells that do not fit are left out.
// Each frame is {name: prefix + cell number, rect, and when trimmed size + trim}. Throws ApiError.
Json SliceSpriteImage(const Texture& image, const Json& slicing, const std::string& prefix);
void RegisterAtlasCommands(CommandRegistry& registry);
// sprite.create/get/set/slice/fit_collision/convert (api/SpriteAssetCommands.cpp).
void RegisterSpriteAssetCommands(CommandRegistry& registry);
}  // namespace oe
