#pragma once
#include "core/Json.h"
#include "render/Mesh.h"

namespace oe {
class AssetManager;
class CommandRegistry;
struct SpriteAnimation;

// Strict reusable sprite definition: sheet or rectangle atlas, named clips and A1 markers.
bool ValidateSpriteAnimationAsset(const Json& data, std::string* error);
// Applies metadata from a model's legacy-compatible clips sidecar to loaded clips.
bool ApplyAnimationMetadata(const Json& data, Mesh& mesh, std::string* error);
// Shared asset clips take precedence over inline clips. Throws a useful API error on a broken asset.
const Json& SpriteAnimationClips(const SpriteAnimation& animation, AssetManager& assets);
// A per-instance loop override never edits a shared clip asset.
bool SpriteAnimationLoop(const SpriteAnimation& animation, const Json& clip);
// Registers animation.create/get/set (file writes, outside scene undo).
void RegisterAnimationAssetCommands(CommandRegistry& registry);
// Registers sprite sequence authoring and bounded source-frame queries.
void RegisterSpriteClipCommands(CommandRegistry& registry);
}  // namespace oe
