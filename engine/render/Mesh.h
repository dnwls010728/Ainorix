#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/Math.h"

namespace oe {

// RGBA8 image (0xAABBGGRR texels, rows top to bottom).
struct Texture {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> texels;
    uint32_t version = 0;  // bumped when texels change in place (font atlases grow)
    // Bilinear sample with repeat wrapping; (0,0) is the top-left corner.
    Color Sample(float u, float v) const;
    // Same with alpha (0..1) and optional nearest-texel sampling (pixel art).
    Color Sample(float u, float v, bool nearest, float* alpha) const;
};

// How a surface looks (glTF 2.0 metallic-roughness model). Shared by both
// renderers; loaded from models (their materials) and from .mat.json files.
enum class AlphaMode { Opaque, Mask, Blend };

struct Material {
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
    bool doubleSided = false;
    bool unlit = false;
    bool pixelArt = false;     // nearest-neighbour sampling
    float tiling[2] = {1, 1};  // uv = uv * tiling + offset
    float offset[2] = {0, 0};

    bool Blended() const { return alphaMode == AlphaMode::Blend; }
};

// A range of indices drawn with one material.
struct Submesh {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    int material = -1;  // index into Mesh::materials; -1 = the default material
};

struct Mesh {
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;      // one per position
    std::vector<float> uvs;         // two per position
    std::vector<Vec4> tangents;     // xyz tangent (+u), w: bitangent sign; from ComputeTangents
    std::vector<uint32_t> indices;  // triangle list, counter-clockwise front faces
    std::vector<Submesh> submeshes;
    std::vector<Material> materials;
    std::vector<std::shared_ptr<const Texture>> textures;  // images embedded in / referenced by the model
    Vec3 boundsMin, boundsMax;

    size_t TriangleCount() const { return indices.size() / 3; }
    void ComputeBounds();
    // Fills `normals` from face normals (area weighted) when a source has none.
    void ComputeNormals();
    // Per-vertex tangents from the UVs (for normal maps).
    void ComputeTangents();
};

// Returns nullptr for unknown names. Meshes are unit sized (fit in [-0.5, 0.5]).
const Mesh* GetBuiltinMesh(const std::string& name);
std::vector<std::string> BuiltinMeshNames();

}  // namespace oe
