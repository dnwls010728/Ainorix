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

// A range of indices drawn with one material.
struct Submesh {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    Color baseColor{1, 1, 1};
    int texture = -1;  // index into Mesh::textures
};

struct Mesh {
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;      // one per position
    std::vector<float> uvs;         // two per position
    std::vector<uint32_t> indices;  // triangle list, counter-clockwise front faces
    std::vector<Submesh> submeshes;
    std::vector<std::shared_ptr<const Texture>> textures;
    Vec3 boundsMin, boundsMax;

    size_t TriangleCount() const { return indices.size() / 3; }
    void ComputeBounds();
    // Fills `normals` from face normals (area weighted) when a source has none.
    void ComputeNormals();
};

// Returns nullptr for unknown names. Meshes are unit sized (fit in [-0.5, 0.5]).
const Mesh* GetBuiltinMesh(const std::string& name);
std::vector<std::string> BuiltinMeshNames();

}  // namespace oe
