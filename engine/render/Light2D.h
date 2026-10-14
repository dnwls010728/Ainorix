#pragma once
#include <cstdint>
#include <memory>
#include <vector>

#include "core/Math.h"
#include "scene/Reflect.h"

namespace oe {

class AssetManager;
class Scene;
struct Texture;

// ---------------------------------------------------------------------------
// 2D lighting shared by both renderers (docs/2D.md, "Colored lights").
//
// Light2D entities in mode "color" or "add" are rendered once into a light map: a world-space
// grid over the area their circles cover, rebuilt only when a light or an occluder changes.
// Sprites and tilemaps with light2D sample it per pixel (mesh_fs in Shaders.glsl and
// ShadeLight2D below, which must agree). Texture rows [0, rows) hold the colored light, rows
// [rows, 2 rows) the additive light; rgb = light / 2, the alpha channels the x and y of the
// main light direction (* 0.5 + 0.5). Row 0 is the bottom edge (y0).
// ---------------------------------------------------------------------------

constexpr int kMaxLights2D = 8;            // color/add lights per scene, entity id order
constexpr int kMaxOccluderEdges2D = 4096;  // occluder edges gathered, entity id order
constexpr int kMaxLightMap2D = 512;        // texels along the longer side

struct Light2DMap {
    std::shared_ptr<const Texture> texture;
    float x0 = 0, y0 = 0;          // world position of the bottom-left corner
    float cellW = 1, cellH = 1;    // world size of one texel
    int cols = 0, rows = 0;
};

// The scene's light map, or null when no sprite or tilemap is lit or no light shines.
std::shared_ptr<const Light2DMap> GetLight2DMap(const Scene& scene, AssetManager* assets);

// Color of a light2D surface: base * (ambient + colored light) + additive light. `normal` is the
// world normal with the normal map applied; without a normal map the light map is used as is.
Color ShadeLight2D(const Light2DMap* map, const Vec3& wpos, const Vec3& normal, bool normalMapped, const Color& base, const Color& ambient);

// Occluders: edges of non-trigger static Collider2D shapes (no RigidBody2D / CharacterBody2D)
// and tile collision outlines, in world XY.
struct Occluders2D {
    struct Edge {
        Vec3 a, b;
        int shape;  // index into shapes
    };
    struct Shape {
        EntityId id = kNullEntity;
        std::vector<Vec3> points;  // closed collider shapes: their outline (inside test); empty = open
        // Tile collision: solid cells (row-major) of the map in its local tile units.
        bool tilemap = false;
        Mat4 toLocal;  // world -> tile units (x = column, y = -row)
        int cols = 0, rows = 0;
        std::vector<uint8_t> solid;
    };
    std::vector<Edge> edges;
    std::vector<Shape> shapes;
};
Occluders2D GatherOccluders2D(const Scene& scene, AssetManager* assets);
// True when an occluder blocks the line from `light` to `point`. A point inside a closed occluder
// (a wall seen from above) is lit by what reaches the occluder's side that faces the light.
// `edges` (optional) limits the test to these edge indices (the ones near the light).
bool Occluded2D(const Occluders2D& occluders, const Vec3& light, const Vec3& point, const std::vector<int>* edges = nullptr);
// How much of the light reaches `point`: 1 lit, 0 in full shadow. `softness` > 0 is the light's half
// width: seven points across it (perpendicular to the line to `point`) give the soft edge; where the
// two ends and the centre agree the point is taken as fully lit or shadowed. Points of the light that
// a wall separates from its centre are left out.
float Visibility2D(const Occluders2D& occluders, const Vec3& light, float softness, const Vec3& point, const std::vector<int>* edges = nullptr);
// Indices of the edges whose bounds touch the square of `radius` around `center` (Light2D.radius + softness).
std::vector<int> OccluderEdgesNear(const Occluders2D& occluders, const Vec3& center, float radius);

}  // namespace oe
