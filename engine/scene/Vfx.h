#pragma once

#include "scene/Components.h"

namespace oe {
class AssetManager;
class PhysicsWorld;
class Scene;
struct VfxEmitterDefinition {
    std::string name;
    ParticleEmitter emitter;
    Mat4 local;  // local transform, retained exactly for exported nested/nonuniform transforms
};
struct VfxTrailDefinition {
    std::string name;
    Trail trail;
    Mat4 local;
};
struct VfxAsset {
    Json json;  // canonical ordered JSON, with default component fields omitted
    std::vector<VfxEmitterDefinition> emitters;
    std::vector<VfxTrailDefinition> trails;
    int maxParticles = 10000;
};
// Reflection for emitter asset values, intentionally absent from the scene component registry.
const ComponentType& VfxEmitterType();
// Strict reflection-based authoring validation (throws ApiError with a recovery hint).
void ValidateVfxSettings(const ComponentType& type, const void* current, const Json& values, const Json* expressionParams = nullptr);
// Parses/validates version 1, names, settings and parameter targets; throws ApiError on caller errors.
VfxAsset ParseVfxAsset(const Json& json);
// Evaluates overrides into authored settings, leaving runtime arrays empty; validates every resulting field.
void ConfigureVfx(const VfxAsset& asset, const Json& overrides, std::vector<ParticleEmitter>& emitters, std::vector<Trail>& trails);
// Loads/reconciles a linked definition, preserving birth snapshots unless the block list changes.
bool RefreshVfx(VFXSystem& system, AssetManager& assets, std::string* error = nullptr);
// Combine authored settings with transient controls without changing scene serialization.
Json EffectiveVfxParams(const VFXSystem& system);
bool VfxPlaying(const VFXSystem& system);
// Bounded system stepping and deferred completion cleanup (called by the built-in systems).
void UpdateVfxSystems(Scene& scene, AssetManager* assets, float dt, PhysicsWorld* physics = nullptr);
void UpdateVfxTrails(Scene& scene, float dt);
// True after every block has completed, or stopped emission has drained.
bool VfxFinished(const VFXSystem& system);
// Index zero keeps the legacy stream; subsequent indices have independent fixed mixes.
uint32_t VfxSeedMix(size_t index);
}  // namespace oe
