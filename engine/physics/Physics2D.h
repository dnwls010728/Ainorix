#pragma once
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/Json.h"
#include "core/Math.h"
#include "scene/Reflect.h"
#include "scene/TileGrid.h"

namespace oe {

class Scene;
struct RaycastHit;

// Pair of entities; for collisions ordered (a < b), for triggers (trigger, other).
using EntityPair = std::pair<EntityId, EntityId>;

// Mirrors Collider2D / RigidBody2D / CharacterBody2D (and the collision of
// Tilemaps) into a Box2D world in the XY plane and writes the results back.
// The only code that knows Box2D. Owned by PhysicsWorld, which merges its
// events and queries with the 3D (Jolt) world. Bodies are created and updated
// in entity-id order, so simulations are deterministic.
class Physics2D {
public:
    Physics2D();
    ~Physics2D();
    Physics2D(const Physics2D&) = delete;
    Physics2D& operator=(const Physics2D&) = delete;

    // True when the scene has 2D bodies (only then a Box2D world is created).
    static bool Wanted(const Scene& scene);

    // Mirrors components into Box2D (dt == 0: query sync, no motion), moves
    // characters, steps the world and writes poses/velocities back.
    void Step(Scene& scene, float dt, std::vector<std::string>& warnings);
    void Sync(Scene& scene, std::vector<std::string>& warnings);
    void SetTilesets(const TilesetLookup* lookup) { tilesets_ = lookup; }

    // Touching pairs after the last step (collisions ordered, triggers (trigger, other)).
    std::set<EntityPair> Collisions() const;
    std::set<EntityPair> Triggers(Scene& scene) const;

    // Queries in the XY plane. The ray is the 3D ray projected on XY (hits keep its z).
    bool Raycast(const Vec3& origin, const Vec3& direction, float maxDistance, RaycastHit& out) const;
    std::vector<EntityId> OverlapCircle(const Vec3& center, float radius) const;
    void AddImpulse(EntityId id, const Vec3& impulse);
    bool Has(EntityId id) const;

    void AppendStats(Json& stats) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    const TilesetLookup* tilesets_ = nullptr;
};

// Convex pieces of a simple polygon (ear clipping, counter-clockwise output).
// Convex input comes back as one piece. Exposed for tests.
std::vector<std::vector<TilePoint>> ConvexPieces(std::vector<TilePoint> polygon);

}  // namespace oe
