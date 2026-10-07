#pragma once

#include "core/Json.h"
#include "scene/Reflect.h"

namespace oe {
class Engine;

// Reflection-derived authoring schema and portable, texture-independent presets.
Json VfxSchema();
Json VfxPresets(const std::string& tag);
// Strictly validates partial ParticleEmitter/Trail edits before reflection can clamp them.
// Other component types are left to their existing reflection contract.
void ValidateVfxSettings(const ComponentType& type, const void* current, const Json& values);
// Applies one preset to an entity atomically; the calling command supplies scene undo.
Json ApplyVfxPreset(Engine& engine, EntityId id, const Json& args);
// Checks a selected effect subtree without stepping or changing it.
Json CheckVfx(Engine& engine, EntityId id);
// Restarts the effect in an isolated scene and returns a contact sheet or numeric analysis.
Json PreviewVfx(Engine& engine, EntityId id, const Json& args);
Json AnalyzeVfx(Engine& engine, EntityId id, const Json& args);
// Asset authoring and linked runtime operations; arguments follow the command registry.
Json VfxAssetCall(Engine& engine, const std::string& operation, const Json& args);
// Atomic validated save used by the isolated document session.
Json SaveVfxDocument(Engine& engine, const std::string& path, const Json& document);
Json VfxRuntimeCall(Engine& engine, const std::string& operation, EntityId id, const Json& args);
// Validate a linked component before publishing an authoring edit.
void ValidateVfxSystem(Engine& engine, const void* current, const Json& values);
// Read-only check of an asset path, including effective defaults and textures.
Json CheckVfxAsset(Engine& engine, const std::string& path, const Json& params = Json::MakeObject());
}  // namespace oe
