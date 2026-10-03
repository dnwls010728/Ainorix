#pragma once
#include <string>

#include "render/Renderer.h"

namespace oe {
class Engine;
// Renders a static, automatically framed asset thumbnail without changing the
// active scene. Supports textures, models, materials, prefabs and scenes.
bool RenderAssetPreview(Engine& engine, const std::string& path, int size, RenderTarget& image, std::string* error);
}  // namespace oe
