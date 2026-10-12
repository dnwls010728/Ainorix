#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/Json.h"
#include "core/Math.h"
#include "render/Renderer.h"

namespace oe {

class Scene;
struct Mesh;

// What the navigation mesh is baked for (the scene's NavMesh component, or its defaults).
struct NavSettings {
    float agentRadius = 0.4f;  // meters
    float agentHeight = 1.8f;
    float maxSlope = 45.0f;    // degrees
    float stepHeight = 0.3f;
    float cellSize = 0.2f;     // horizontal sampling; the vertical cell is half of it
    float minRegion = 1.0f;    // square meters
};

// World-space triangles the mesh is baked from: counter-clockwise seen from their walkable side.
struct NavGeometry {
    std::vector<float> vertices;          // x, y, z per vertex
    std::vector<int> triangles;           // three vertex indices per triangle
    std::vector<unsigned char> walkable;  // per triangle: 0 = an obstacle only (NavModifier unwalkable)
    int colliders = 0, meshes = 0;        // entities that contributed
    std::vector<std::string> warnings;

    // Identity of the geometry and settings a bake depends on.
    uint64_t Hash(const NavSettings& settings) const;
};

// Resolves MeshRenderer.mesh (built-in name or model path); nullptr when it cannot be loaded.
using NavMeshLookup = std::function<std::shared_ptr<const Mesh>(const std::string&)>;

// The settings of the first NavMesh component in id order (defaults without one).
NavSettings ReadNavSettings(const Scene& scene);
// Static geometry of the scene in entity id order: Collider shapes and/or MeshRenderer models as the
// NavMesh component's `source` and `collect` say. Left out: triggers, anything with (or below) a
// RigidBody, CharacterBody or Animator, skinned models, hidden meshes and NavModifier ignore.
void CollectNavGeometry(const Scene& scene, const NavMeshLookup& meshes, NavGeometry& out);

struct NavPath {
    bool found = false;    // both ends are on the mesh
    bool partial = false;  // the goal cannot be reached: the path ends at the closest reachable point
    std::vector<Vec3> points;  // corners from the start to the end, both snapped to the mesh
    float length = 0.0f;
};

struct NavPoint {
    bool found = false;
    Vec3 point;
};

struct NavHit {
    bool valid = false;  // the start is on the mesh
    bool hit = false;    // an edge of the mesh lies between the two points
    Vec3 point;          // where the walk stops (the end when nothing is hit)
    Vec3 normal;         // horizontal normal of the edge that was hit
    float distance = 0.0f;
};

// A baked navigation mesh and the queries on it. This file's implementation is the only code that
// knows Recast and Detour. Baking and queries are deterministic for the same geometry and settings.
class Navigation {
public:
    // How far from a given point the mesh is searched when a query does not say.
    static constexpr float kSnapRange = 2.0f;

    Navigation();
    ~Navigation();
    Navigation(const Navigation&) = delete;
    Navigation& operator=(const Navigation&) = delete;

    // Replaces the mesh. False (and an empty mesh) when nothing is walkable or the world is too large.
    bool Build(const NavGeometry& geometry, const NavSettings& settings, std::string* error);
    void Clear();
    bool Baked() const;
    const NavSettings& Settings() const { return settings_; }
    // {polygons, vertices, tiles, area (m2), boundsMin, boundsMax}
    const Json& Stats() const { return stats_; }

    // The closest point of the mesh within `range` (horizontally; twice that vertically).
    NavPoint Nearest(const Vec3& point, float range = kSnapRange) const;
    // Shortest walk between two points (each snapped like Nearest).
    NavPath FindPath(const Vec3& from, const Vec3& to, float range = kSnapRange) const;
    // Walks straight from `from` toward `to` on the mesh until an edge stops it: line of walk, not of sight.
    NavHit Raycast(const Vec3& from, const Vec3& to, float range = kSnapRange) const;

    // Polygon outlines: bright along the mesh's borders, dim between polygons.
    void AppendLines(std::vector<DebugLine>& lines) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    NavSettings settings_;
    Json stats_ = Json::MakeObject();
};

}  // namespace oe
