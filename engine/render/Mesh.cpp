#include "render/Mesh.h"

#include <map>

namespace oe {

namespace {

void AddQuad(Mesh& m, Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
    uint32_t base = static_cast<uint32_t>(m.positions.size());
    m.positions.insert(m.positions.end(), {a, b, c, d});
    m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

Mesh MakeCube() {
    Mesh m;
    const float h = 0.5f;
    AddQuad(m, {-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h});      // +Z
    AddQuad(m, {h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h});  // -Z
    AddQuad(m, {h, -h, h}, {h, -h, -h}, {h, h, -h}, {h, h, h});      // +X
    AddQuad(m, {-h, -h, -h}, {-h, -h, h}, {-h, h, h}, {-h, h, -h});  // -X
    AddQuad(m, {-h, h, h}, {h, h, h}, {h, h, -h}, {-h, h, -h});      // +Y
    AddQuad(m, {-h, -h, -h}, {h, -h, -h}, {h, -h, h}, {-h, -h, h});  // -Y
    return m;
}

Mesh MakePlane() {
    Mesh m;
    const float h = 0.5f;
    AddQuad(m, {-h, 0, h}, {h, 0, h}, {h, 0, -h}, {-h, 0, -h});
    return m;
}

Mesh MakePyramid() {
    Mesh m;
    const float h = 0.5f;
    Vec3 apex(0, h, 0);
    Vec3 a(-h, -h, h), b(h, -h, h), c(h, -h, -h), d(-h, -h, -h);
    auto tri = [&](Vec3 p, Vec3 q, Vec3 r) {
        uint32_t base = static_cast<uint32_t>(m.positions.size());
        m.positions.insert(m.positions.end(), {p, q, r});
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2});
    };
    tri(a, b, apex);
    tri(b, c, apex);
    tri(c, d, apex);
    tri(d, a, apex);
    AddQuad(m, d, c, b, a);
    return m;
}

Mesh MakeSphere(int rings, int segments) {
    Mesh m;
    for (int r = 0; r <= rings; ++r) {
        float phi = kPi * static_cast<float>(r) / static_cast<float>(rings);
        for (int s = 0; s <= segments; ++s) {
            float theta = 2.0f * kPi * static_cast<float>(s) / static_cast<float>(segments);
            m.positions.push_back(Vec3(std::sin(phi) * std::cos(theta), std::cos(phi), -std::sin(phi) * std::sin(theta)) * 0.5f);
        }
    }
    const uint32_t stride = static_cast<uint32_t>(segments + 1);
    for (uint32_t r = 0; r < static_cast<uint32_t>(rings); ++r) {
        for (uint32_t s = 0; s < static_cast<uint32_t>(segments); ++s) {
            uint32_t a = r * stride + s, b = a + stride, c = b + 1, d = a + 1;
            if (r != 0) m.indices.insert(m.indices.end(), {a, b, d});
            if (r + 1 != static_cast<uint32_t>(rings)) m.indices.insert(m.indices.end(), {d, b, c});
        }
    }
    return m;
}

const std::map<std::string, Mesh>& Meshes() {
    static const std::map<std::string, Mesh> meshes = {
        {"cube", MakeCube()},
        {"plane", MakePlane()},
        {"pyramid", MakePyramid()},
        {"sphere", MakeSphere(12, 20)},
    };
    return meshes;
}

}  // namespace

const Mesh* GetBuiltinMesh(const std::string& name) {
    auto it = Meshes().find(name);
    return it == Meshes().end() ? nullptr : &it->second;
}

std::vector<std::string> BuiltinMeshNames() {
    std::vector<std::string> out;
    for (const auto& kv : Meshes()) out.push_back(kv.first);
    return out;
}

}  // namespace oe
