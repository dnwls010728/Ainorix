#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/Math.h"

namespace oe {

struct Mesh {
    std::vector<Vec3> positions;
    std::vector<uint32_t> indices;  // triangle list, counter-clockwise front faces
};

// Returns nullptr for unknown names. Meshes are unit sized (fit in [-0.5, 0.5]).
const Mesh* GetBuiltinMesh(const std::string& name);
std::vector<std::string> BuiltinMeshNames();

}  // namespace oe
