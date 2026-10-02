#pragma once
#include <memory>
#include <vector>

#include "core/Math.h"
#include "render/Mesh.h"
#include "scene/Scene.h"

namespace oe {

class AssetManager;

// What a frame draws, collected from the scene the same way for every
// renderer so the software and GPU backends agree on meshes, materials and
// lights. Items are in entity id order.
struct RenderItem {
    EntityId id = kNullEntity;
    std::shared_ptr<const Mesh> mesh;  // built-in meshes use a non-owning pointer (use_count 0)
    Mat4 world;
    Mat4 normalMatrix;
    std::vector<Mat4> joints;  // model-space pose palette, shared with GPU uniforms
    Vec3 boundsMin, boundsMax;  // model-space bounds of the current pose
    Color tint;
    std::shared_ptr<const Texture> textureOverride;    // MeshRenderer.texture; null = the material's own
    std::shared_ptr<const Material> materialOverride;  // MeshRenderer.material; null = the mesh's materials
    float opacity = 1.0f;  // multiplies the material's opacity; < 1 makes it transparent
    bool blend = false;    // force alpha blending (sprites without a cutoff)
    bool unlit = false;
    bool flat = false;
    bool castShadows = true;
    bool error = false;  // missing asset, drawn as an unlit magenta cube
    // 2D (Sprite / Tilemap): texture coordinates become uv * uvScale + uvOffset,
    // texels with alpha below alphaCutoff are discarded (0 = off), and
    // pointSample picks the nearest texel (pixel art).
    float uvOffset[2] = {0, 0};
    float uvScale[2] = {1, 1};
    float alphaCutoff = 0.0f;
    bool pointSample = false;

    // Material of a submesh with this item's overrides applied (tint, texture,
    // opacity, unlit, sprite cutoff and sampling).
    Material SubmeshMaterial(const Submesh& sub) const;
    // Four normalized influences; static/zero-weight vertices use identity.
    Mat4 VertexSkinMatrix(size_t vertex) const;
};

// One submesh to draw. Opaque and cut-out surfaces first (item order), then
// transparent ones back to front (by distance of the item's bounds center to
// the eye, ties by id): the order both renderers use.
struct DrawCall {
    size_t item = 0;
    size_t submesh = 0;
    Material material;
    bool blend = false;
};
std::vector<DrawCall> BuildDrawList(const std::vector<RenderItem>& items, const Vec3& eye);

struct RenderDirLight {
    Vec3 dir;  // direction the light travels
    Color color;
};

struct RenderPointLight {
    Vec3 pos;
    Color color;
    float range;
};

struct RenderLights {
    Color ambient{0, 0, 0};
    std::vector<RenderDirLight> dirs;  // never empty: a default light is added when the scene has none
    std::vector<RenderPointLight> points;
    bool shadows = false;  // the first directional light casts shadows
    float shadowStrength = 0.75f;
};

// The camera view orients particle billboards; identity faces +Z for callers without a camera.
std::vector<RenderItem> GatherRenderItems(const Scene& scene, AssetManager* assets, const Mat4& cameraView = Mat4{});
RenderLights GatherRenderLights(const Scene& scene);

// Orthographic view-projection of the first directional light, fitted to a
// bounding box of the shadow casters and receivers.
struct ShadowFit {
    Mat4 viewProj;
    float texelWorld = 0.01f;  // world size of one shadow map texel (size = mapSize)
};
ShadowFit FitShadow(const Vec3& lo, const Vec3& hi, const Vec3& lightDir, int mapSize);

}  // namespace oe
