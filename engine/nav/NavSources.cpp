#include "nav/Navigation.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

#include "core/Image.h"
#include "render/Mesh.h"
#include "scene/Components.h"
#include "scene/Scene.h"

namespace oe {

namespace {

// How an entity takes part, inherited by its children.
enum class Part { Normal, Unwalkable, Ignored };

int AddVertex(NavGeometry& out, const Vec3& p) {
    out.vertices.push_back(p.x);
    out.vertices.push_back(p.y);
    out.vertices.push_back(p.z);
    return static_cast<int>(out.vertices.size() / 3) - 1;
}

Vec3 VertexAt(const NavGeometry& out, int index) {
    const size_t i = static_cast<size_t>(index) * 3;
    return Vec3(out.vertices[i], out.vertices[i + 1], out.vertices[i + 2]);
}

// A triangle of a convex shape around `inside`, wound so that it faces away from it.
void AddOutward(NavGeometry& out, int a, int b, int c, const Vec3& inside, bool walkable) {
    const Vec3 pa = VertexAt(out, a), pb = VertexAt(out, b), pc = VertexAt(out, c);
    const Vec3 normal = Cross(pb - pa, pc - pa);
    if (Dot(normal, normal) < 1e-12f) return;  // the collapsed quads at a sphere's poles
    if (Dot(normal, (pa + pb + pc) / 3.0f - inside) < 0) std::swap(b, c);
    out.triangles.push_back(a);
    out.triangles.push_back(b);
    out.triangles.push_back(c);
    out.walkable.push_back(walkable ? 1 : 0);
}

// Position + rotation of an entity with its scale taken out, as the physics shapes use it.
Mat4 RigidWorld(const Scene& scene, EntityId id, Vec3* scale) {
    const Mat4 m = scene.WorldMatrix(id);
    const Vec3 s(Length(Vec3(m.at(0, 0), m.at(1, 0), m.at(2, 0))), Length(Vec3(m.at(0, 1), m.at(1, 1), m.at(2, 1))),
                 Length(Vec3(m.at(0, 2), m.at(1, 2), m.at(2, 2))));
    *scale = s;
    Mat4 r = m;
    for (int row = 0; row < 3; ++row) {
        r.at(row, 0) = s.x > 1e-6f ? m.at(row, 0) / s.x : 0;
        r.at(row, 1) = s.y > 1e-6f ? m.at(row, 1) / s.y : 0;
        r.at(row, 2) = s.z > 1e-6f ? m.at(row, 2) / s.z : 0;
    }
    return r;
}

void AddBox(NavGeometry& out, const Mat4& m, const Vec3& center, const Vec3& half, bool walkable) {
    int v[8];
    for (int i = 0; i < 8; ++i)
        v[i] = AddVertex(out, m.TransformPoint(center + Vec3(i & 1 ? half.x : -half.x, i & 2 ? half.y : -half.y, i & 4 ? half.z : -half.z)));
    const Vec3 inside = m.TransformPoint(center);
    const int quads[6][4] = {{0, 1, 3, 2}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 3, 7, 5}};
    for (const auto& q : quads) {
        AddOutward(out, v[q[0]], v[q[1]], v[q[2]], inside, walkable);
        AddOutward(out, v[q[0]], v[q[2]], v[q[3]], inside, walkable);
    }
}

// A capsule along local Y (a sphere when `half` is 0) as rings of quads.
void AddCapsule(NavGeometry& out, const Mat4& m, const Vec3& center, float radius, float half, bool walkable) {
    const int kStacks = 8, kSlices = 12;
    std::vector<int> rings;
    auto ring = [&](float phi, float offset) {
        for (int s = 0; s < kSlices; ++s) {
            const float theta = 2.0f * kPi * static_cast<float>(s) / static_cast<float>(kSlices);
            rings.push_back(AddVertex(out, m.TransformPoint(center + Vec3(radius * std::sin(phi) * std::cos(theta), offset + radius * std::cos(phi),
                                                                         radius * std::sin(phi) * std::sin(theta)))));
        }
    };
    for (int i = 0; i <= kStacks / 2; ++i) ring(kPi * static_cast<float>(i) / static_cast<float>(kStacks), half);
    for (int i = kStacks / 2; i <= kStacks; ++i) ring(kPi * static_cast<float>(i) / static_cast<float>(kStacks), -half);
    const Vec3 inside = m.TransformPoint(center);
    const int count = static_cast<int>(rings.size()) / kSlices;
    for (int r = 0; r + 1 < count; ++r) {
        for (int s = 0; s < kSlices; ++s) {
            const int a = rings[static_cast<size_t>(r * kSlices + s)], b = rings[static_cast<size_t>(r * kSlices + (s + 1) % kSlices)];
            const int c = rings[static_cast<size_t>((r + 1) * kSlices + (s + 1) % kSlices)], d = rings[static_cast<size_t>((r + 1) * kSlices + s)];
            AddOutward(out, a, b, c, inside, walkable);
            AddOutward(out, a, c, d, inside, walkable);
        }
    }
}

void HashBytes(uint64_t& hash, const void* data, size_t size) {
    if (!size) return;
    const uint64_t part = Fnv1a64(static_cast<const uint8_t*>(data), size);
    uint8_t both[16];
    std::memcpy(both, &hash, 8);
    std::memcpy(both + 8, &part, 8);
    hash = Fnv1a64(both, sizeof(both));
}

}  // namespace

uint64_t NavGeometry::Hash(const NavSettings& settings) const {
    uint64_t hash = 0;
    const float values[] = {settings.agentRadius, settings.agentHeight, settings.maxSlope, settings.stepHeight, settings.cellSize, settings.minRegion};
    HashBytes(hash, values, sizeof(values));
    HashBytes(hash, vertices.data(), vertices.size() * sizeof(float));
    HashBytes(hash, triangles.data(), triangles.size() * sizeof(int));
    HashBytes(hash, walkable.data(), walkable.size());
    return hash;
}

NavSettings ReadNavSettings(const Scene& scene) {
    NavSettings out;
    const auto& pool = scene.Pool<NavMesh>();
    if (pool.empty()) return out;
    const NavMesh& nav = pool.begin()->second;
    out.agentRadius = Clamp(nav.agentRadius, 0.0f, 10.0f);
    out.agentHeight = Clamp(nav.agentHeight, 0.1f, 50.0f);
    out.maxSlope = Clamp(nav.maxSlope, 0.0f, 89.0f);
    out.stepHeight = Clamp(nav.stepHeight, 0.0f, 10.0f);
    out.cellSize = Clamp(nav.cellSize, 0.05f, 5.0f);
    out.minRegion = Clamp(nav.minRegion, 0.0f, 10000.0f);
    return out;
}

void CollectNavGeometry(const Scene& scene, const NavMeshLookup& meshes, NavGeometry& out) {
    out = NavGeometry();
    std::string source = "colliders", collect = "all";
    EntityId root = kNullEntity;
    if (!scene.Pool<NavMesh>().empty()) {
        root = scene.Pool<NavMesh>().begin()->first;
        source = scene.Pool<NavMesh>().begin()->second.source;
        collect = scene.Pool<NavMesh>().begin()->second.collect;
        if (scene.Pool<NavMesh>().size() > 1) out.warnings.push_back("several NavMesh components: only the first in id order is used");
    }
    const bool useColliders = source != "meshes", useMeshes = source == "meshes" || source == "all";

    // Per entity: how it takes part, whether something moves it, and whether it is under the root.
    // Parents may have higher ids than their children, so each entity walks its own chain.
    struct Info { Part part = Part::Normal; bool moving = false; bool inside = false; };
    std::map<EntityId, Info> infos;
    for (const auto& kv : scene.Entities()) {
        Info info;
        info.inside = collect != "children" || root == kNullEntity;
        int depth = 0;
        for (EntityId at = kv.first; at != kNullEntity && depth < 256; ++depth) {
            if (const NavModifier* modifier = scene.Get<NavModifier>(at)) {
                // The closest modifier wins, and ignoring wins over everything further up.
                if (info.part == Part::Normal) info.part = modifier->mode == "unwalkable" ? Part::Unwalkable : Part::Ignored;
            }
            if (scene.Get<RigidBody>(at) || scene.Get<CharacterBody>(at) || scene.Get<Animator>(at)) info.moving = true;
            if (at == root) info.inside = true;
            const EntityRecord* record = scene.Record(at);
            at = record ? record->parent : kNullEntity;
        }
        infos[kv.first] = info;
    }

    for (const auto& kv : scene.Entities()) {
        const EntityId id = kv.first;
        const Info& info = infos[id];
        if (info.part == Part::Ignored || info.moving || !info.inside) continue;
        const bool walkable = info.part == Part::Normal;
        const Collider* collider = useColliders ? scene.Get<Collider>(id) : nullptr;
        if (collider && !collider->isTrigger) {
            Vec3 scale;
            const Mat4 m = RigidWorld(scene, id, &scale);
            const Vec3 center = Mul(collider->center, scale);
            if (collider->shape == "sphere") {
                AddCapsule(out, m, center, collider->radius * std::max(scale.x, std::max(scale.y, scale.z)), 0.0f, walkable);
            } else if (collider->shape == "capsule") {
                const float radius = collider->radius * std::max(scale.x, scale.z);
                AddCapsule(out, m, center, radius, std::max(0.001f, 0.5f * collider->height * scale.y - radius), walkable);
            } else {
                AddBox(out, m, center, Mul(collider->size, scale) * 0.5f, walkable);
            }
            ++out.colliders;
        }
        const MeshRenderer* renderer = useMeshes ? scene.Get<MeshRenderer>(id) : nullptr;
        if (renderer && renderer->visible && !renderer->parentPose && meshes) {
            const std::shared_ptr<const Mesh> mesh = meshes(renderer->mesh);
            if (!mesh) {
                out.warnings.push_back("mesh '" + renderer->mesh + "' of '" + kv.second.name + "' could not be loaded and is left out");
                continue;
            }
            if (!mesh->skin.empty() || mesh->indices.size() < 3) continue;  // posed by bones: not ground
            const Mat4 world = scene.WorldMatrix(id);
            // A mirrored entity turns its triangles over; turn them back so their tops stay walkable.
            const Vec3 x(world.at(0, 0), world.at(1, 0), world.at(2, 0)), y(world.at(0, 1), world.at(1, 1), world.at(2, 1)),
                z(world.at(0, 2), world.at(1, 2), world.at(2, 2));
            const bool mirrored = Dot(Cross(x, y), z) < 0;
            const int base = static_cast<int>(out.vertices.size() / 3);
            for (const Vec3& p : mesh->positions) AddVertex(out, world.TransformPoint(p));
            for (size_t i = 0; i + 2 < mesh->indices.size(); i += 3) {
                const int a = base + static_cast<int>(mesh->indices[i]);
                int b = base + static_cast<int>(mesh->indices[i + 1]), c = base + static_cast<int>(mesh->indices[i + 2]);
                if (mirrored) std::swap(b, c);
                out.triangles.push_back(a);
                out.triangles.push_back(b);
                out.triangles.push_back(c);
                out.walkable.push_back(walkable ? 1 : 0);
            }
            ++out.meshes;
        }
    }
}

}  // namespace oe
