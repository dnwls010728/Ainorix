#pragma once
#include <memory>
#include <string>
#include <vector>

#include "core/Json.h"
#include "core/Math.h"
#include "render/Renderer.h"
#include "scene/Reflect.h"

namespace oe {

class Scene;

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
// Jolt PhysicsSystem and writes the simulated results back. The Jolt world
// lives for one play session (Reset() discards it); bodies are created and
// updated in entity-id order so simulations are deterministic.
//
// This is the only place that knows about Jolt: swapping the backend (or
// adding a 2D one) means reimplementing this class.
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<std::string> warnings_;
};

// Wireframes of every collider/character (green solid, yellow trigger,
// cyan character). Computed from components, so it works without simulating.
void AppendColliderLines(const Scene& scene, std::vector<DebugLine>& lines);

}  // namespace oe
