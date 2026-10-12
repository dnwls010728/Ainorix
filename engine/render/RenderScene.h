#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/Math.h"
#include "render/Environment.h"
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
    std::vector<std::shared_ptr<const Material>> slotMaterials;  // MeshRenderer.materials by Submesh::material; null = no entry
    // MeshRenderer.shaderUniforms applied over the material's graph uniforms; used when hasShaderUniforms.
    std::array<Vec4, ShaderGraph::kMaxUniforms> shaderUniforms{};
    bool hasShaderUniforms = false;
    Vec4 particleCustom{}; // four per-draw material attributes for unbatched particles
    Vec4 particleInput{};  // per-draw age/random/world speed for shared (unbatched) meshes
    float opacity = 1.0f;  // multiplies the material's opacity; < 1 makes it transparent
    float emission = 0.0f; // Sprite.emission: the base texture times tint * emission becomes emissive
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
// the reserved `lightDirection` uniform; `cameraPosition` is always bound to `eye` and `playing`
// to 1 or 0 (RenderView::playing).
std::vector<DrawCall> BuildDrawList(const std::vector<RenderItem>& items, const Vec3& eye, const RenderLights* lights = nullptr,
                                    bool playing = false);

// Vertex stage of a shader graph with an `offset` output, as mesh_vs runs it: calls
// fn(vertex, world position before the offset, offset) once for every vertex the draw call's
// submesh uses. Does nothing for materials without such a graph.
void ForEachVertexOffset(const RenderItem& item, const DrawCall& draw, float time,
                         const std::function<void(uint32_t, const Vec3&, const Vec3&)>& fn);
// Cull margin (world units) of a draw whose graph has an `offset` output: the graph's declared
// `maxOffset`, else kDefaultOffsetMargin; 0 for materials without such a graph.
constexpr float kDefaultOffsetMargin = 1.0f;
float OffsetMargin(const Material& material);
// The same as a per-axis range: the graph's `offsetBounds`, else -margin..+margin on every axis.
void OffsetRange(const Material& material, Vec3& lo, Vec3& hi);
// Grows lo..hi (world space) by the bounds of lit displaced items widened by their margin, so the
// shadow map both renderers fit to the scene bounds also covers displaced surfaces. Like the
// bounds margins of other engines it never runs the vertex stage on the CPU (that cost grew
// with the vertex count every frame); a graph moving vertices further than it declares can
// leave its shadow cut at the edge of the map.
void ExtendOffsetBounds(const std::vector<RenderItem>& items, const std::vector<DrawCall>& draws, Vec3& lo, Vec3& hi);
// World-space box around an item's bounds, widened by `margin` on every side.
void ItemWorldBounds(const RenderItem& item, float margin, Vec3& lo, Vec3& hi);
void ItemWorldBounds(const RenderItem& item, const Vec3& offsetLo, const Vec3& offsetHi, Vec3& lo, Vec3& hi);
// False when the world box lies entirely outside one plane of viewProj's clip volume (OpenGL clip
// space). `depth` false ignores the near and far planes (shadow casters in front of the light's
// near plane still cast).
bool BoxInFrustum(const Mat4& viewProj, const Vec3& lo, const Vec3& hi, bool depth = true);

struct RenderDirLight {
    Vec3 dir;  // direction the light travels
    Color color;
};

struct RenderPointLight {
    Vec3 pos;
    Color color;
    float range;
};

// A scene's sky environment (the Skybox component), sampled by both renderers for image-based
// lighting and the background: three colors (they already include the component's intensity), or
// an image (`map`, with `intensity`, turned by the yaw).
struct RenderEnvironment {
    bool present = false;
    Color sky{0, 0, 0};      // zenith
    Color horizon{0, 0, 0};
    Color ground{0, 0, 0};
    float reflections = 1.0f;  // strength of the environment reflection on smooth surfaces
    bool background = true;    // draw the sky behind the scene
    bool sun = false;          // draw a sun disc (needs a directional light)
    Vec3 sunDir{0, -1, 0};     // direction the sun's light travels (the disc is toward -sunDir)
    Color sunColor{0, 0, 0};   // sun disc color (the first directional light's color)
    float sunCos = 0.9995f;    // cosine of the sun disc's angular radius
    std::string texture;       // Skybox.texture; `map` is its prepared image when it loaded
    std::shared_ptr<const EnvironmentMap> map;
    float intensity = 1.0f;    // multiplies the image
    float yawCos = 1.0f, yawSin = 0.0f;  // rotation of the image around +Y
    Vec3 sh[9];                // the image's irradiance terms times intensity (see EnvIrradianceSH)
};

// The sky color along a world direction: ground below the horizon, sky above, the horizon between.
// The same gradient runs in the GPU shader (sky_color in Shaders.glsl); keep the two identical.
inline Color SampleSky(const RenderEnvironment& e, const Vec3& dir) {
    const float y = Clamp(dir.y, -1.0f, 1.0f);
    if (y >= 0.0f) {
        const float t = std::pow(y, 0.45f);
        return e.horizon * (1.0f - t) + e.sky * t;
    }
    const float t = std::pow(-y, 0.5f);
    return e.horizon * (1.0f - t) + e.ground * t;
}

// A world direction in the image's space (the image turned around +Y). env_dir in Shaders.glsl.
inline Vec3 EnvMapDir(const RenderEnvironment& e, const Vec3& d) {
    return Vec3(e.yawCos * d.x + e.yawSin * d.z, d.y, -e.yawSin * d.x + e.yawCos * d.z);
}

// Diffuse ambient on a surface facing `n`. The three functions below are the environment branch of
// Lighting::Shade and of mesh_fs (Shaders.glsl); keep the shader identical.
inline Color EnvDiffuse(const RenderEnvironment& e, const Vec3& n) {
    if (e.map) {
        const Vec3 v = EnvIrradianceSH(e.sh, EnvMapDir(e, n));
        return Color(std::max(0.0f, v.x), std::max(0.0f, v.y), std::max(0.0f, v.z));
    }
    const float up = n.y * 0.5f + 0.5f;
    return SampleSky(e, Vec3(0, -1, 0)) * (1.0f - up) + SampleSky(e, Vec3(0, 1, 0)) * up;
}

// What a surface of roughness `r` (0.04..1) reflects along `refl`: the image's blurred level for
// that roughness, or the gradient blurred toward the sky/ground average.
inline Color EnvSpecular(const RenderEnvironment& e, const Vec3& refl, float r) {
    if (e.map) return e.map->Sample(EnvMapDir(e, refl), (r - 0.04f) / 0.96f * e.map->MaxLod()) * e.intensity;
    const Color dull = (SampleSky(e, Vec3(0, 1, 0)) + SampleSky(e, Vec3(0, -1, 0))) * 0.5f;
    return SampleSky(e, refl) * (1.0f - r) + dull * r;
}

// The sky as the background: the image, or the gradient plus a soft sun disc toward the light (the
// disc is not part of the reflection/ambient: the directional light already lights surfaces).
// Mirrors sky_fs in Shaders.glsl.
inline Color SampleSkyBackground(const RenderEnvironment& e, const Vec3& dir) {
    if (e.map) return e.map->Sample(EnvMapDir(e, dir), 0.0f) * e.intensity;
    Color c = SampleSky(e, dir);
    if (e.sun) {
        const float d = Dot(dir, e.sunDir * -1.0f);
        const float disc = Clamp((d - e.sunCos) / std::max(1e-5f, 1.0f - e.sunCos), 0.0f, 1.0f);
        c = c + e.sunColor * (disc * disc);
    }
    return c;
}

// A local reflection (the ReflectionProbe component): the surroundings as seen from `pos`, used by
// the surfaces inside the box lo..hi and fading out over `blend` meters outside it. `map` is taken
// by ActiveProbes; a probe without one is not used.
constexpr int kMaxProbes = 4;  // probes one frame uses (the nearest to the camera)
struct RenderProbe {
    EntityId id = kNullEntity;
    Vec3 pos;
    Vec3 lo, hi;
    float blend = 0.5f;
    float intensity = 1.0f;
    bool boxProjection = true;
    bool ambient = true;
    int resolution = 128;
    float nearPlane = 0.1f, farPlane = 200.0f;
    std::shared_ptr<const EnvironmentMap> map;
    Vec3 sh[9];  // the picture's irradiance terms times intensity
};

// How much of a surface point's ambient light comes from the probe: 1 inside the box, fading to 0
// `blend` meters outside it. The four functions below are the probe loop of Lighting::Shade and of
// mesh_fs (Shaders.glsl); keep the shader identical.
inline float ProbeWeight(const RenderProbe& p, const Vec3& w) {
    const float d = std::max({p.lo.x - w.x, w.x - p.hi.x, p.lo.y - w.y, w.y - p.hi.y, p.lo.z - w.z, w.z - p.hi.z});
    return 1.0f - Clamp(d / p.blend, 0.0f, 1.0f);
}

// Where the picture is read for a reflection leaving `wpos` along `refl`: with box projection the
// direction from the capture position to the point where the reflected ray leaves the box, so the
// reflection of a wall stays on the wall as the viewer moves.
inline Vec3 ProbeDir(const RenderProbe& p, const Vec3& wpos, const Vec3& refl) {
    if (!p.boxProjection) return refl;
    const Vec3 rd(std::fabs(refl.x) < 1e-6f ? 1e-6f : refl.x, std::fabs(refl.y) < 1e-6f ? 1e-6f : refl.y,
                  std::fabs(refl.z) < 1e-6f ? 1e-6f : refl.z);
    const float tx = std::max((p.hi.x - wpos.x) / rd.x, (p.lo.x - wpos.x) / rd.x);
    const float ty = std::max((p.hi.y - wpos.y) / rd.y, (p.lo.y - wpos.y) / rd.y);
    const float tz = std::max((p.hi.z - wpos.z) / rd.z, (p.lo.z - wpos.z) / rd.z);
    const float t = std::max(std::min(tx, std::min(ty, tz)), 0.0f);
    const Vec3 hit = wpos + refl * t - p.pos;
    return Dot(hit, hit) > 1e-12f ? Normalize(hit) : refl;
}

inline Color ProbeSpecular(const RenderProbe& p, const Vec3& wpos, const Vec3& refl, float r) {
    return p.map->Sample(ProbeDir(p, wpos, refl), (r - 0.04f) / 0.96f * p.map->MaxLod()) * p.intensity;
}

inline Color ProbeDiffuse(const RenderProbe& p, const Vec3& n) {
    const Vec3 v = EnvIrradianceSH(p.sh, n);
    return Color(std::max(0.0f, v.x), std::max(0.0f, v.y), std::max(0.0f, v.z));
}

struct RenderLights {
    Color ambient{0, 0, 0};
    Color groundAmbient{0, 0, 0};  // hemisphere: ambient seen by downward faces (sum over lights)
    bool hemisphere = false;       // a directional light asked for hemisphere ambient
    RenderEnvironment env;         // the Skybox component, if any
    std::vector<RenderProbe> probes;   // every ReflectionProbe, in entity id order, without pictures (see ActiveProbes)
    std::vector<RenderDirLight> dirs;  // never empty: a default light is added when the scene has none
    std::vector<RenderPointLight> points;
    bool shadows = false;  // the first directional light casts shadows
    float shadowStrength = 0.75f;
    float shadowDistance = 0.0f;  // > 0: the shadow map covers the camera's view up to this distance
};

// The camera view orients particle billboards; identity faces +Z for callers without a camera.
std::vector<RenderItem> GatherRenderItems(const Scene& scene, AssetManager* assets, const Mat4& cameraView = Mat4{});
// `assets` (optional) loads the image of a textured Skybox; without it the Skybox keeps its colors.
RenderLights GatherRenderLights(const Scene& scene, AssetManager* assets = nullptr);

// The reflection probes a frame uses: those whose box (with its fade) is in view, the nearest
// kMaxProbes to the eye, smallest box first (a small probe inside a large one wins where they
// overlap), each with its picture. A probe that has no picture yet gets one here (CaptureProbe),
// kept in the asset manager until AssetManager::InvalidateProbes. Empty without `assets`, and for a
// scene other than the one the pictures belong to (AssetManager::SetProbeScene).
std::vector<RenderProbe> ActiveProbes(const Scene& scene, AssetManager* assets, const RenderLights& lights,
                                      const Mat4& viewProj, const Vec3& eye);
// The picture of one probe (taken now if it has none): the scenery around its position drawn by the
// software renderer on six faces, as an equirectangular environment in world axes. null without assets.
std::shared_ptr<const EnvironmentMap> CaptureProbe(const Scene& scene, AssetManager* assets, const RenderProbe& probe);
// False for what a probe's picture leaves out: entities that move while the game runs (a RigidBody,
// character, animated model, effect or trail on it or on an ancestor), which would stay in the
// reflections as ghosts where they stood when the picture was taken.
bool ProbeShows(const Scene& scene, EntityId id);

// Orthographic view-projection of the first directional light, fitted to a
// bounding box of the shadow casters and receivers.
struct ShadowFit {
    Mat4 viewProj;
    float texelWorld = 0.01f;  // world size of one shadow map texel (size = mapSize)
};
ShadowFit FitShadow(const Vec3& lo, const Vec3& hi, const Vec3& lightDir, int mapSize);
// DirectionalLight.shadowDistance > 0 (shadow distance as in other engines): fitted to the part of
// the camera's view frustum up to `distance` (the receivers that can be seen), with the light's
// depth range stretched toward the light over the scene box lo..hi so every caster between the
// light and that part still casts. The map moves in whole texels, so shadows do not shimmer while
// the camera moves. Falls back to FitShadow when the scene box is the smaller of the two.
ShadowFit FitShadowToView(const Mat4& view, const Mat4& proj, float distance, const Vec3& lo, const Vec3& hi,
                          const Vec3& lightDir, int mapSize);

}  // namespace oe
