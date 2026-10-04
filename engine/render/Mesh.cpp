#include "render/Mesh.h"

#include <algorithm>
#include <cmath>
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

Color Texture::Sample(float u, float v, bool nearest, float* alpha) const {
    if (width <= 0 || height <= 0) {
        if (alpha) *alpha = 1.0f;
        return Color(1, 1, 1);
    }
    auto wrap = [](int i, int n) { i %= n; return i < 0 ? i + n : i; };
    auto texel = [&](int px, int py) { return texels[static_cast<size_t>(py) * static_cast<size_t>(width) + static_cast<size_t>(px)]; };
    if (nearest) {
        uint32_t c = texel(wrap(static_cast<int>(std::floor(u * static_cast<float>(width))), width),
                           wrap(static_cast<int>(std::floor(v * static_cast<float>(height))), height));
        if (alpha) *alpha = static_cast<float>(c >> 24) / 255.0f;
        return Color((c & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, ((c >> 16) & 0xFF) / 255.0f);
    }
    float x = u * static_cast<float>(width) - 0.5f;
    float y = v * static_cast<float>(height) - 0.5f;
    float fx = std::floor(x), fy = std::floor(y);
    float tx = x - fx, ty = y - fy;
    int x0 = wrap(static_cast<int>(fx), width), x1 = wrap(static_cast<int>(fx) + 1, width);
    int y0 = wrap(static_cast<int>(fy), height), y1 = wrap(static_cast<int>(fy) + 1, height);
    float r = 0, g = 0, b = 0, a = 0;
    auto add = [&](int px, int py, float w) {
        uint32_t c = texel(px, py);
        r += w * static_cast<float>(c & 0xFF);
        g += w * static_cast<float>((c >> 8) & 0xFF);
        b += w * static_cast<float>((c >> 16) & 0xFF);
        a += w * static_cast<float>(c >> 24);
    };
    add(x0, y0, (1 - tx) * (1 - ty));
    add(x1, y0, tx * (1 - ty));
    add(x0, y1, (1 - tx) * ty);
    add(x1, y1, tx * ty);
    if (alpha) *alpha = a / 255.0f;
    return Color(r / 255.0f, g / 255.0f, b / 255.0f);
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

void Mesh::ComputeTangents() {
    const size_t n = positions.size();
    std::vector<Vec3> du(n, Vec3(0, 0, 0)), dv(n, Vec3(0, 0, 0));  // dP/du, dP/dv
    if (uvs.size() >= n * 2) {
        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
            uint32_t a = indices[i], b = indices[i + 1], c = indices[i + 2];
            Vec3 e1 = positions[b] - positions[a], e2 = positions[c] - positions[a];
            float du1 = uvs[b * 2] - uvs[a * 2], dv1 = uvs[b * 2 + 1] - uvs[a * 2 + 1];
            float du2 = uvs[c * 2] - uvs[a * 2], dv2 = uvs[c * 2 + 1] - uvs[a * 2 + 1];
            float r = du1 * dv2 - du2 * dv1;
            if (std::fabs(r) < 1e-12f) continue;
            float f = 1.0f / r;
            Vec3 t = (e1 * dv2 - e2 * dv1) * f;
            Vec3 s = (e2 * du1 - e1 * du2) * f;
            for (uint32_t k : {a, b, c}) {
                du[k] += t;
                dv[k] += s;
            }
        }
    }
    tangents.resize(n);
    for (size_t i = 0; i < n; ++i) {
        Vec3 nrm = i < normals.size() ? normals[i] : Vec3(0, 1, 0);
        Vec3 t = du[i] - nrm * Dot(nrm, du[i]);
        if (Length(t) < 1e-8f) t = Cross(std::fabs(nrm.y) < 0.99f ? Vec3(0, 1, 0) : Vec3(1, 0, 0), nrm);
        t = Normalize(t);
        // Normal maps store +Y towards the top of the image, i.e. decreasing v.
        float w = Dot(Cross(nrm, t), dv[i]) < 0.0f ? 1.0f : -1.0f;
        tangents[i] = Vec4(t.x, t.y, t.z, w);
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
    m.ComputeTangents();
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

// The plane split into cells x cells quads sharing vertices: enough vertices for a shader
// graph's `offset` output (waves, wind) to bend it. Same size, facing and uv as MakePlane.
Mesh MakePlaneGrid(int cells) {
    Mesh m;
    for (int z = 0; z <= cells; ++z) {
        for (int x = 0; x <= cells; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(cells), v = static_cast<float>(z) / static_cast<float>(cells);
            m.positions.push_back(Vec3(u - 0.5f, 0, v - 0.5f));
            m.normals.push_back(Vec3(0, 1, 0));
            m.uvs.push_back(u);
            m.uvs.push_back(v);
        }
    }
    const uint32_t stride = static_cast<uint32_t>(cells + 1);
    for (uint32_t z = 0; z < static_cast<uint32_t>(cells); ++z) {
        for (uint32_t x = 0; x < static_cast<uint32_t>(cells); ++x) {
            const uint32_t a = z * stride + x, b = a + 1, c = a + stride + 1, d = a + stride;
            m.indices.insert(m.indices.end(), {d, c, b, d, b, a});
        }
    }
    Finish(m);
    return m;
}

// 1x1 quad in the XY plane facing +Z (sprites); uv (0,0) = top-left.
Mesh MakeQuad() {
    Mesh m;
    const float h = 0.5f;
    AddQuad(m, {-h, -h, 0}, {h, -h, 0}, {h, h, 0}, {-h, h, 0});
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
        {"plane64", MakePlaneGrid(64)},
        {"pyramid", MakePyramid()},
        {"quad", MakeQuad()},
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
