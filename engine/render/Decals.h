#pragma once
#include <array>
#include <memory>
#include <vector>
#include "render/Mesh.h"
#include "scene/Scene.h"

namespace oe {
class AssetManager;
struct RenderView;
constexpr int kMaxDecals = 16;
constexpr int kDecalTileSize = 256, kDecalAtlasSize = kDecalTileSize * 4;
constexpr int kDecalDataStart = 152, kDecalDataStride = 7;

// Ordered color projector; coordinates in worldToBox are within [-0.5,0.5] on all axes.
struct RenderDecal {
    EntityId id = kNullEntity;
    Mat4 worldToBox;
    Vec3 normal;
    Color color;
    float opacity = 1, edgeFade = 0.1f, normalThreshold = 0.2f;
    int order = 0;
    std::shared_ptr<const Texture> texture;  // Null means a solid white image multiplied by color.
};
// Visible enabled boxes, order then entity id; first sixteen, invalid transforms skipped.
std::vector<RenderDecal> GatherDecals(const Scene& scene, AssetManager* assets, const RenderView& view);
// Shared shader-equivalent projection coverage; uv is local X / reversed local Z.
float DecalCoverage(const RenderDecal& decal, const Vec3& position, const Vec3& normal, float& u, float& v);
// Blend decals into base color in order. Atlas tiles include a duplicated one-pixel clamp border.
Color ApplyDecals(const std::vector<RenderDecal>& decals, const Texture* atlas, const Vec3& position, const Vec3& normal, Color base);

// Rebuild only when the selected textures/versions change. At most sixteen owners and one 1024-square atlas.
class DecalAtlas {
public:
    std::shared_ptr<const Texture> Prepare(const std::vector<RenderDecal>& decals);
private:
    std::shared_ptr<Texture> atlas_;
    std::array<std::shared_ptr<const Texture>, kMaxDecals> sources_{};
    std::array<uint32_t, kMaxDecals> versions_{};
    size_t count_ = 0;
};
}  // namespace oe
