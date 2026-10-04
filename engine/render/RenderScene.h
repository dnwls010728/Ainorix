#pragma once
#include <functional>
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
    // MeshRenderer.shaderUniforms applied over the material's graph uniforms; used when hasShaderUniforms.
    std::array<Vec4, ShaderGraph::kMaxUniforms> shaderUniforms{};
    bool hasShaderUniforms = false;
    float opacity = 1.0f;  // multiplies the material's opacity; < 1 makes it transparent
    bool blend = false;    // force alpha blending (sprites without a cutoff)
    bool additive = false; // blended additively (Sprite/ParticleEmitter blend: "add"); implies blend
    // 2D draw order of transparent items: layer, then order, before distance to the eye (Sprite.layer/order).
    int sortLayer = 0, sortOrder = 0;
    bool unlit = false;
    bool flat = false;
    bool castShadows = true;
    bool doubleSided = false;  // draw and shadow both faces whatever the material says (shadow-casting sprites)
    bool error = false;  // missing asset, drawn as an unlit magenta cube
    bool pickable = true;  // false: never written to the entity-id buffer (Darkness2D overlay)
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
// transparent ones by sort layer and order, then back to front (by distance of
// the item's bounds center to the eye, ties by id): the order both renderers use.
struct DrawCall {
    size_t item = 0;
    size_t submesh = 0;
    Material material;
    bool blend = false;
};
struct RenderLights;
// `lights` (optional) supplies the first directional light for shader graphs that declare
// the reserved `lightDirection` uniform; `cameraPosition` is always bound to `eye`.
std::vector<DrawCall> BuildDrawList(const std::vector<RenderItem>& items, const Vec3& eye, const RenderLights* lights = nullptr);

// Vertex stage of a shader graph with an `offset` output, as mesh_vs runs it: calls
// fn(vertex, world position before the offset, offset) once for every vertex the draw call's
// submesh uses. Does nothing for materials without such a graph.
void ForEachVertexOffset(const RenderItem& item, const DrawCall& draw, float time,
                         const std::function<void(uint32_t, const Vec3&, const Vec3&)>& fn);
// Grows lo..hi (world space) to the places graph offsets move the vertices of lit items to, so
// the shadow map both renderers fit to the scene bounds also covers displaced surfaces.
void ExtendOffsetBounds(const std::vector<RenderItem>& items, const std::vector<DrawCall>& draws, float time, Vec3& lo, Vec3& hi);

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
