#pragma once

#include "core/Json.h"
#include "scene/Vfx.h"
#include "scene/Reflect.h"

namespace oe {
class Engine;

// Reflection-derived authoring schema and portable presets with bundled dependencies.
Json VfxSchema();
// Headless expression compiler/evaluator diagnostic, isolated from scene state.
Json EvaluateVfxExpressionCall(const Json& args);
Json VfxPresets(const std::string& tag);
// Create a complete preset document and install its dependencies without overwriting edits.
Json VfxPresetDocument(Engine& engine, const std::string& name);
// Materialize a bundled preset asset for linked scene placement; refuses modified destinations.
std::string VfxPresetAsset(Engine& engine, const std::string& name);
// Applies one preset to an entity atomically; the calling command supplies scene undo.
Json ApplyVfxPreset(Engine& engine, EntityId id, const Json& args);
// Read-only counts/settings and optional birth/evaluated snapshots for one asset emitter.
Json DescribeVfxEmitter(const ParticleEmitter& emitter, bool particles);
// Validate asset emitter/trail values without creating scene components; id supplies optional anchor context.
Json CheckVfxValues(Engine& engine, const ParticleEmitter* emitter, const Trail* trail, EntityId id = 0);
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
