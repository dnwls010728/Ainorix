#include "render/Mesh.h"

#include <algorithm>
#include <map>

namespace oe {

Color Texture::Sample(float u, float v) const {
    if (width <= 0 || height <= 0) return Color(1, 1, 1);
    float x = u * static_cast<float>(width) - 0.5f;
    float y = v * static_cast<float>(height) - 0.5f;
    float fx = std::floor(x), fy = std::floor(y);
    float tx = x - fx, ty = y - fy;
    auto wrap = [](int i, int n) { i %= n; return i < 0 ? i + n : i; };
    int x0 = wrap(static_cast<int>(fx), width), x1 = wrap(static_cast<int>(fx) + 1, width);
    int y0 = wrap(static_cast<int>(fy), height), y1 = wrap(static_cast<int>(fy) + 1, height);
    auto fetch = [&](int px, int py) {
        uint32_t c = texels[static_cast<size_t>(py) * static_cast<size_t>(width) + static_cast<size_t>(px)];
        return Color((c & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, ((c >> 16) & 0xFF) / 255.0f);
    };
    Color top = fetch(x0, y0) * (1 - tx) + fetch(x1, y0) * tx;
    Color bottom = fetch(x0, y1) * (1 - tx) + fetch(x1, y1) * tx;
    return top * (1 - ty) + bottom * ty;
}

void Mesh::ComputeBounds() {
    if (positions.empty()) {
        boundsMin = boundsMax = Vec3(0, 0, 0);
        return;
    }
    boundsMin = boundsMax = positions[0];
    for (const Vec3& p : positions) {
        boundsMin = Vec3(std::min(boundsMin.x, p.x), std::min(boundsMin.y, p.y), std::min(boundsMin.z, p.z));
        boundsMax = Vec3(std::max(boundsMax.x, p.x), std::max(boundsMax.y, p.y), std::max(boundsMax.z, p.z));
    }
}

void Mesh::ComputeNormals() {
    normals.assign(positions.size(), Vec3(0, 0, 0));
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const Vec3& a = positions[indices[i]];
        const Vec3& b = positions[indices[i + 1]];
        const Vec3& c = positions[indices[i + 2]];
        Vec3 n = Cross(b - a, c - a);
        for (int k = 0; k < 3; ++k) normals[indices[i + static_cast<size_t>(k)]] += n;
    }
    for (Vec3& n : normals) n = Normalize(n);
}

namespace {

void AddQuad(Mesh& m, Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
    uint32_t base = static_cast<uint32_t>(m.positions.size());
    Vec3 n = Normalize(Cross(b - a, c - a));
    m.positions.insert(m.positions.end(), {a, b, c, d});
    m.normals.insert(m.normals.end(), {n, n, n, n});
    m.uvs.insert(m.uvs.end(), {0, 1, 1, 1, 1, 0, 0, 0});
    m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

void Finish(Mesh& m) {
    Submesh s;
    s.indexCount = static_cast<uint32_t>(m.indices.size());
    m.submeshes = {s};
    m.ComputeBounds();
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
    Finish(m);
    return m;
}

Mesh MakePlane() {
    Mesh m;
    const float h = 0.5f;
    AddQuad(m, {-h, 0, h}, {h, 0, h}, {h, 0, -h}, {-h, 0, -h});
    Finish(m);
    return m;
}

Mesh MakePyramid() {
    Mesh m;
    const float h = 0.5f;
    Vec3 apex(0, h, 0);
    Vec3 a(-h, -h, h), b(h, -h, h), c(h, -h, -h), d(-h, -h, -h);
    auto tri = [&](Vec3 p, Vec3 q, Vec3 r) {
        uint32_t base = static_cast<uint32_t>(m.positions.size());
        Vec3 n = Normalize(Cross(q - p, r - p));
        m.positions.insert(m.positions.end(), {p, q, r});
        m.normals.insert(m.normals.end(), {n, n, n});
        m.uvs.insert(m.uvs.end(), {0, 1, 1, 1, 0.5f, 0});
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2});
    };
    tri(a, b, apex);
    tri(b, c, apex);
    tri(c, d, apex);
    tri(d, a, apex);
    AddQuad(m, d, c, b, a);
    Finish(m);
    return m;
}

Mesh MakeSphere(int rings, int segments) {
    Mesh m;
    for (int r = 0; r <= rings; ++r) {
        float phi = kPi * static_cast<float>(r) / static_cast<float>(rings);
        for (int s = 0; s <= segments; ++s) {
            float theta = 2.0f * kPi * static_cast<float>(s) / static_cast<float>(segments);
            Vec3 n(std::sin(phi) * std::cos(theta), std::cos(phi), -std::sin(phi) * std::sin(theta));
            m.positions.push_back(n * 0.5f);
            m.normals.push_back(n);
            m.uvs.push_back(static_cast<float>(s) / static_cast<float>(segments));
            m.uvs.push_back(static_cast<float>(r) / static_cast<float>(rings));
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
    Finish(m);
    return m;
}

const std::map<std::string, Mesh>& Meshes() {
    static const std::map<std::string, Mesh> meshes = {
        {"cube", MakeCube()},
        {"plane", MakePlane()},
        {"pyramid", MakePyramid()},
        {"sphere", MakeSphere(16, 24)},
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
