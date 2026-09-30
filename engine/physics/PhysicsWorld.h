#pragma once
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/Json.h"
#include "core/Math.h"
#include "render/Renderer.h"
#include "scene/Reflect.h"
#include "scene/TileGrid.h"

namespace oe {

class Scene;
class Physics2D;

struct PhysicsEvent {
    enum class Kind { CollisionEnter, CollisionExit, TriggerEnter, TriggerExit };
    Kind kind;
    EntityId a = kNullEntity;  // for triggers: the trigger volume
    EntityId b = kNullEntity;
};
const char* ToString(PhysicsEvent::Kind kind);

struct RaycastHit {
    bool hit = false;
    EntityId entity = kNullEntity;
    Vec3 point;
    Vec3 normal;
    float distance = 0.0f;
};

struct ContactPair {
    EntityId a = kNullEntity;
    EntityId b = kNullEntity;
    bool trigger = false;
};

// Mirrors the scene's Collider / RigidBody / CharacterBody components into a
// Jolt PhysicsSystem (3D) and Collider2D / RigidBody2D / CharacterBody2D into
// a Box2D world (2D, engine/physics/Physics2D) and writes the simulated
// results back. Each world is created only when the scene has bodies for it;
// tilemaps collide in both. Events and queries merge both worlds. The worlds
// live for one play session (Reset() discards them); bodies are created and
// updated in entity-id order so simulations are deterministic.
//
// This file is the only place that knows about Jolt, Physics2D.cpp the only
// one that knows Box2D.
class PhysicsWorld {
public:
    static constexpr float kGravity = -9.81f;

    PhysicsWorld();
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    // Advances the simulation by dt and returns collision/trigger events
    // (sorted, deterministic) that happened during the step.
    std::vector<PhysicsEvent> Step(Scene& scene, float dt);
    void Reset();

    // Queries (also valid outside simulation: the world is synced first).
    RaycastHit Raycast(Scene& scene, const Vec3& origin, const Vec3& direction, float maxDistance);
    std::vector<EntityId> OverlapSphere(Scene& scene, const Vec3& center, float radius);
    std::vector<ContactPair> Contacts() const;
    void AddImpulse(EntityId id, const Vec3& impulse);

    Json Stats() const;
    const std::vector<std::string>& Warnings() const { return warnings_; }

    // Resolves Tilemap.tileset files (*.tileset.json) for tile collision.
    void SetTilesets(TilesetLookup lookup);

private:
    Physics2D* World2D(Scene& scene);  // created on demand, nullptr without 2D bodies
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::unique_ptr<Physics2D> world2d_;
    TilesetLookup tilesets_;
    std::set<std::pair<EntityId, EntityId>> prevCollisions_, prevTriggers_;
    std::vector<std::string> warnings_;
};

// Wireframes of every collider/character, 3D and 2D (green solid, yellow
// trigger, cyan character, orange one-way). Computed from components, so it
// works without simulating. `tilesets` resolves Tilemap tileset files.
void AppendColliderLines(const Scene& scene, std::vector<DebugLine>& lines, const TilesetLookup* tilesets = nullptr);

}  // namespace oe
