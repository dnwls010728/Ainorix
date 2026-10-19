#pragma once
#include <cstdint>
#include <array>
#include <memory>
#include <string>
#include <vector>

#include "core/Math.h"
#include "render/ColorSpace.h"
#include "render/ShaderGraph.h"
#include "scene/AnimationEvents.h"

namespace oe {

struct Light2DMap;

// RGBA8 image (0xAABBGGRR texels, rows top to bottom).
struct Texture {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> texels;
    uint32_t version = 0;  // bumped when texels change in place (font atlases grow)
    // A color image whose texels are sRGB-encoded (the default). In a color slot (base, emissive, the graph
    // `texture` node, decals) of a linear project it is decoded per texel before filtering, as a GPU SRGB8A8
    // texture is. false (`<image>.texture.json` {"sRGB": false}, Unity's "sRGB (Color Texture)" off): data such as
    // flow or noise maps, used as stored in any slot. Data slots (normal, metallic-roughness, occlusion) never decode.
    bool srgb = true;
    // Bilinear sample with repeat wrapping; (0,0) is the top-left corner.
    Color Sample(float u, float v) const;
    // Same with alpha (0..1), optional nearest-texel sampling (pixel art) and `decode`: the four texels are turned
    // from sRGB to linear (SrgbToLinear) before they are blended, as the GPU samples an SRGB8A8 texture.
    Color Sample(float u, float v, bool nearest, float* alpha, bool decode = false) const;
    // Whether a color slot decodes this texture under `linear` lighting (RenderLights::linear).
    bool Decoded(bool linear) const { return linear && srgb; }
};

// How a surface looks (glTF 2.0 metallic-roughness model). Shared by both
// renderers; loaded from models (their materials) and from .mat.json files.
enum class AlphaMode { Opaque, Mask, Blend };

// Largest Material::outlineWidth in world units (also the cull margin of the world outline graph) and in
// pixels (outlineUnits "pixels").
constexpr float kMaxOutlineWidth = 0.25f;
constexpr float kMaxOutlinePixels = 16.0f;
// Longest Material::nextPass chain a draw follows (the material itself not counted).
constexpr int kMaxMaterialPasses = 4;

struct Material {
    std::string name;  // model materials: the name in the source file (the slot MeshRenderer.materials replaces)
    Color baseColor{1, 1, 1};
    float opacity = 1.0f;                                     // alpha of the base color
    std::shared_ptr<const Texture> baseTexture;               // multiplies baseColor/opacity
    float metallic = 0.0f;
    float roughness = 0.7f;
    std::shared_ptr<const Texture> metallicRoughnessTexture;  // G = roughness, B = metallic (multiplied)
    std::shared_ptr<const Texture> normalTexture;             // tangent space, +Y up (OpenGL / glTF)
    float normalScale = 1.0f;
    std::shared_ptr<const Texture> occlusionTexture;          // R, darkens ambient light
    float occlusionStrength = 1.0f;
    Color emissive{0, 0, 0};
    float emissiveIntensity = 1.0f;
    std::shared_ptr<const Texture> emissiveTexture;           // multiplies emissive
    AlphaMode alphaMode = AlphaMode::Opaque;
    float alphaCutoff = 0.5f;  // Mask: texels below are discarded
    bool doubleSided = false;  // `cull: "none"`: both faces are drawn
    bool cullFront = false;    // `cull: "front"`: only back faces are drawn (inverted hulls, inside views)
    bool castShadows = true;   // false: never in the shadow maps (whatever MeshRenderer.castShadows says)
    bool smoothNormals = false;  // vertex normals averaged at shared positions (Mesh::SmoothNormals): closed outline shells
    bool additive = false;     // Blend only: add color * alpha to the target (glows) instead of mixing
    bool unlit = false;
    bool pixelArt = false;     // nearest-neighbour sampling
    bool fog = true;           // PostProcess fog applies (false: sky domes, backdrops)
    float tiling[2] = {1, 1};  // uv = uv * tiling + offset
    float offset[2] = {0, 0};
    std::string shaderPath;                                  // project-relative surface graph asset
    std::shared_ptr<const ShaderGraph> shader;                // null retains the standard material
    std::array<Vec4, ShaderGraph::kMaxUniforms> shaderUniforms{};  // validated graph defaults/overrides
    // Outline (inverted hull): > 0 draws the surface again after the opaque ones, pushed out along the
    // vertex normals by this many world units (or screen pixels with outlinePixels), front faces culled,
    // unlit in outlineColor. Opaque and cut-out materials only; 0..kMaxOutlineWidth (kMaxOutlinePixels).
    float outlineWidth = 0.0f;
    Color outlineColor{0, 0, 0};
    bool outlinePixels = false;  // outlineUnits "pixels": the same thickness on screen at any distance
    // `nextPass`: a material drawn on the same submesh right after this one (Godot's next_pass), e.g. an
    // outline shell with `cull: "front"` and a graph offset, or a glow. Chains up to kMaxMaterialPasses.
    std::string nextPassPath;
    std::shared_ptr<const Material> nextPass;
    // Sprite/Tilemap light2D (unlit surfaces only): shaded by this 2D light map with ambient2D;
    // normalTexture is then the sprite's normal map and flip2D undoes the sprite's flips.
    std::shared_ptr<const Light2DMap> light2D;
    bool light2DOn = false;
    Color ambient2D;
    float flip2D[2] = {1, 1};
    // Sprite.mask: alpha 0 outside the opaque texels (nearest) of the mask frame; found = false hides all.
    bool mask2DOn = false, mask2DFound = false;
    Vec4 mask2DX, mask2DY, mask2DUv;
    std::shared_ptr<const Texture> mask2DTexture;
    float mask2DCutoff = 0.5f;

    bool Blended() const { return alphaMode == AlphaMode::Blend; }
};

// A range of indices drawn with one material.
struct Submesh {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    int material = -1;  // index into Mesh::materials; -1 = the default material
    // Models with more joints than one GPU draw can address (Mesh::kDrawJoints): the joints this
    // range uses. No vertex is shared with another range then. Empty = the whole palette.
    std::vector<uint16_t> palette;
};

// Immutable glTF hierarchy; rotations use quaternion x/y/z/w, not Euler angles.
struct ModelNode {
    std::string name;  // source node/bone name; may be empty or repeated
    int parent = -1;
    Vec3 translation;
    Vec4 rotation{0, 0, 0, 1};
    Vec3 scale{1, 1, 1};
    Mat4 matrix;
    bool usesMatrix = false;
};

// One palette entry maps a node's current world transform to model coordinates.
struct ModelJoint {
    int node = -1;
    Mat4 inverseBind;
};

struct SkinVertex {
    std::array<uint16_t, 4> joints{};
    std::array<float, 4> weights{};  // normalized; zero total means identity
};

// A named deformation of a model (FBX blend shape, glTF morph target): offsets added to some of its
// vertices, scaled by a weight (MeshRenderer.blendShapes), before skinning. Faces: blink, mouth shapes.
struct BlendShape {
    std::string name;
    std::vector<uint32_t> vertices;  // ascending indices into Mesh::positions
    std::vector<Vec3> positions;     // offset per listed vertex, model space
    std::vector<Vec3> normals;       // offset per listed vertex (zero where the source has none)
};

enum class AnimationPath { Translation, Rotation, Scale };
enum class AnimationInterpolation { Linear, Step, CubicSpline };

struct AnimationChannel {
    int node = -1;
    AnimationPath path = AnimationPath::Translation;
    AnimationInterpolation interpolation = AnimationInterpolation::Linear;
    std::vector<float> times;  // strictly increasing seconds
    std::vector<Vec4> values;  // cubic: in tangent, value, out tangent per key
};

struct AnimationClip {
    std::string name;
    float duration = 0;
    std::vector<AnimationChannel> channels;
    std::vector<AnimationMarker> events;  // from <model>.events.json; never embedded in the model
    std::string displayName;  // optional label; name remains the stable playback key
    float loopStart = 0, loopEnd = -1;  // seconds; negative end means full duration
    bool metadataSidecar = false;  // events authored through the A3 sidecar instead of legacy events.json
    bool sidecarEvents = false;  // clips.json events override the legacy events.json entry
    Json tracks = Json::MakeArray();  // clips.json property tracks (scene/PropertyTracks.h), applied while the clip plays
};

struct Mesh {
    static constexpr size_t kMaxJoints = 1024;  // palette entries of one model
    static constexpr size_t kDrawJoints = 64;   // matrices in the GPU vertex uniforms of one draw
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;      // one per position
    std::vector<float> uvs;         // two per position
    std::vector<Vec4> colors;       // optional RGBA per vertex; empty means white
    // `colors` hold linear values (glTF COLOR_0, by its specification) instead of sRGB-authored ones (FBX, sprite
    // tints, 2D skeleton slots): linear lighting uses them as they are rather than decoding them (VertexColor).
    bool linearColors = false;
    // Vertex i's color as the renderers light it (`linear`: RenderLights::linear); white without colors.
    Vec4 VertexColor(size_t i, bool linear) const {
        return i < colors.size() ? SceneColor(colors[i], linear && !linearColors) : Vec4(1, 1, 1, 1);
    }
    std::vector<Vec4> particleCustom; // optional four per-particle material attributes
    std::vector<Vec4> particleData; // optional normalized age/random/world speed per vertex
    std::vector<Vec4> tangents;     // xyz tangent (+u), w: bitangent sign; from ComputeTangents
    std::vector<uint32_t> indices;  // triangle list, counter-clockwise front faces
    std::vector<Submesh> submeshes;
    std::vector<Material> materials;
    std::vector<std::shared_ptr<const Texture>> textures;  // images embedded in / referenced by the model
    Vec3 boundsMin, boundsMax;
    std::vector<ModelNode> nodes;      // glTF source order, including joint ancestors
    std::vector<ModelJoint> joints;    // flattened skins and rigid animated nodes
    std::vector<SkinVertex> skin;      // one per position for models with a palette
    std::vector<AnimationClip> clips;
    std::vector<BlendShape> blendShapes;  // shapes of the same name in several parts of a model are one entry
    // Filled by FinishSkeleton when a model is loaded (empty for meshes built in code, which are then
    // walked the slow way): every node with the nodes below it, and the default pose in model space.
    std::vector<std::vector<int>> subtrees;
    std::vector<Mat4> restPose;
    // With Submesh::palette: each vertex's joints as places in its range's palette (what the GPU addresses).
    std::vector<std::array<uint8_t, 4>> drawJoints;

    size_t TriangleCount() const { return indices.size() / 3; }
    void ComputeBounds();
    // Fills `normals` from face normals (area weighted) when a source has none.
    void ComputeNormals();
    // `normals` averaged over the vertices at the same position (split by hard edges or uv seams), so a
    // shell pushed out along them stays closed (Material::smoothNormals). Where they cancel out, the own normal.
    std::vector<Vec3> SmoothNormals() const;
    // Per-vertex tangents from the UVs (for normal maps).
    void ComputeTangents();
    // With more than kDrawJoints joints: cuts every submesh into ranges that each use at most
    // kDrawJoints of them (Submesh::palette), duplicating the vertices two ranges would share.
    void SplitSkinPalettes();
    // Lists the nodes below every node (after the hierarchy is complete).
    void FinishSkeleton();
    // The shape of that name, added when new.
    BlendShape& BlendShapeNamed(const std::string& name);
    // What a renderer draws with shapes applied: this mesh's geometry, materials and skin (not its
    // nodes, clips and shapes) with `weights` (shape index, weight) added to positions and normals.
    std::shared_ptr<Mesh> Morphed(const std::vector<std::pair<int, float>>& weights) const;
};

// Returns nullptr for unknown names. Meshes are unit sized (fit in [-0.5, 0.5]).
const Mesh* GetBuiltinMesh(const std::string& name);
std::vector<std::string> BuiltinMeshNames();

}  // namespace oe
