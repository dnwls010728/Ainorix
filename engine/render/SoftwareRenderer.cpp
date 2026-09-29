#include <algorithm>
#include <chrono>
#include <cstring>

#include "render/Mesh.h"
#include "render/Renderer.h"
#include "scene/Components.h"

namespace oe {

void RenderTarget::Resize(int w, int h) {
    width = std::max(1, w);
    height = std::max(1, h);
    size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);
    color.assign(n, 0xFF000000u);
    depth.assign(n, 1.0f);
    ids.assign(n, kNullEntity);
}

Image RenderTarget::ToImage() const {
    Image img;
    img.width = width;
    img.height = height;
    img.rgba.resize(color.size() * 4);
    std::memcpy(img.rgba.data(), color.data(), img.rgba.size());
    return img;
}

EntityId RenderTarget::IdAt(int x, int y) const {
    if (x < 0 || y < 0 || x >= width || y >= height) return kNullEntity;
    return ids[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
}

uint64_t RenderTarget::Hash() const {
    return Fnv1a64(reinterpret_cast<const uint8_t*>(color.data()), color.size() * sizeof(uint32_t));
}

namespace {

uint32_t Pack(const Color& c) {
    auto ch = [](float v) { return static_cast<uint32_t>(Clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return ch(c.r) | (ch(c.g) << 8) | (ch(c.b) << 16) | 0xFF000000u;
}

uint32_t Blend(uint32_t dst, const Color& src, float a) {
    Color d((dst & 0xFF) / 255.0f, ((dst >> 8) & 0xFF) / 255.0f, ((dst >> 16) & 0xFF) / 255.0f);
    return Pack(d * (1.0f - a) + src * a);
}

struct Light {
    Vec3 dir;  // direction the light travels
    Color color;
};

struct ScreenVert {
    float x, y, z;
};

class Rasterizer {
public:
    Rasterizer(RenderTarget& t, const Mat4& viewProj) : t_(t), viewProj_(viewProj) {}

    // Clips a triangle (world space) against the near plane and rasterizes it.
    int DrawTriangle(const Vec3& a, const Vec3& b, const Vec3& c, uint32_t color, EntityId id) {
        Vec4 in[3] = {viewProj_ * Vec4(a, 1), viewProj_ * Vec4(b, 1), viewProj_ * Vec4(c, 1)};
        Vec4 poly[4];
        int count = 0;
        for (int i = 0; i < 3; ++i) {
            const Vec4& p = in[i];
            const Vec4& q = in[(i + 1) % 3];
            float dp = p.z + p.w, dq = q.z + q.w;  // distance to near plane (z >= -w)
            if (dp >= 0) poly[count++] = p;
            if ((dp >= 0) != (dq >= 0)) {
                float s = dp / (dp - dq);
                poly[count++] = p + (q - p) * s;
            }
        }
        if (count < 3) return 0;
        ScreenVert sv[4];
        for (int i = 0; i < count; ++i) sv[i] = ToScreen(poly[i]);
        int drawn = 0;
        for (int i = 1; i + 1 < count; ++i) drawn += Raster(sv[0], sv[i], sv[i + 1], color, id);
        return drawn;
    }

    void DrawLine(const Vec3& a, const Vec3& b, const Color& color, float alpha) {
        Vec4 p = viewProj_ * Vec4(a, 1), q = viewProj_ * Vec4(b, 1);
        float dp = p.z + p.w, dq = q.z + q.w;
        if (dp < 0 && dq < 0) return;
        if (dp < 0) p = p + (q - p) * (dp / (dp - dq));
        else if (dq < 0) q = q + (p - q) * (dq / (dq - dp));
        ScreenVert s0 = ToScreen(p), s1 = ToScreen(q);
        float dx = s1.x - s0.x, dy = s1.y - s0.y;
        int steps = static_cast<int>(std::max(std::fabs(dx), std::fabs(dy)));
        if (steps > 8192) return;
        for (int i = 0; i <= steps; ++i) {
            float tt = steps == 0 ? 0.0f : static_cast<float>(i) / static_cast<float>(steps);
            int x = static_cast<int>(s0.x + dx * tt);
            int y = static_cast<int>(s0.y + dy * tt);
            if (x < 0 || y < 0 || x >= t_.width || y >= t_.height) continue;
            float z = s0.z + (s1.z - s0.z) * tt;
            size_t idx = static_cast<size_t>(y) * static_cast<size_t>(t_.width) + static_cast<size_t>(x);
            if (z <= t_.depth[idx] + 1e-4f) t_.color[idx] = Blend(t_.color[idx], color, alpha);
        }
    }

private:
    ScreenVert ToScreen(const Vec4& c) const {
        float iw = 1.0f / c.w;
        return {(c.x * iw * 0.5f + 0.5f) * static_cast<float>(t_.width),
                (0.5f - c.y * iw * 0.5f) * static_cast<float>(t_.height),
                c.z * iw * 0.5f + 0.5f};
    }

    static float Edge(const ScreenVert& a, const ScreenVert& b, float px, float py) {
        return (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
    }

    int Raster(const ScreenVert& v0, const ScreenVert& v1, const ScreenVert& v2, uint32_t color, EntityId id) {
        float area = Edge(v0, v1, v2.x, v2.y);
        if (area >= 0) return 0;  // back face (counter-clockwise in NDC == clockwise on screen)
        int minX = std::max(0, static_cast<int>(std::floor(std::min({v0.x, v1.x, v2.x}))));
        int maxX = std::min(t_.width - 1, static_cast<int>(std::ceil(std::max({v0.x, v1.x, v2.x}))));
        int minY = std::max(0, static_cast<int>(std::floor(std::min({v0.y, v1.y, v2.y}))));
        int maxY = std::min(t_.height - 1, static_cast<int>(std::ceil(std::max({v0.y, v1.y, v2.y}))));
        if (minX > maxX || minY > maxY) return 0;
        float inv = 1.0f / area;
        for (int y = minY; y <= maxY; ++y) {
            float py = static_cast<float>(y) + 0.5f;
            size_t row = static_cast<size_t>(y) * static_cast<size_t>(t_.width);
            for (int x = minX; x <= maxX; ++x) {
                float px = static_cast<float>(x) + 0.5f;
                float w0 = Edge(v1, v2, px, py) * inv;
                float w1 = Edge(v2, v0, px, py) * inv;
                float w2 = Edge(v0, v1, px, py) * inv;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                float z = w0 * v0.z + w1 * v1.z + w2 * v2.z;
                size_t idx = row + static_cast<size_t>(x);
                if (z < 0.0f || z >= t_.depth[idx]) continue;
                t_.depth[idx] = z;
                t_.color[idx] = color;
                t_.ids[idx] = id;
            }
        }
        return 1;
    }

    RenderTarget& t_;
    Mat4 viewProj_;
};

void DrawOutline(RenderTarget& t, EntityId id) {
    const Color orange(1.0f, 0.62f, 0.1f);
    std::vector<uint8_t> mask(t.ids.size());
    for (size_t i = 0; i < t.ids.size(); ++i) mask[i] = t.ids[i] == id;
    for (int y = 0; y < t.height; ++y) {
        for (int x = 0; x < t.width; ++x) {
            size_t idx = static_cast<size_t>(y) * static_cast<size_t>(t.width) + static_cast<size_t>(x);
            if (mask[idx]) continue;
            bool edge = false;
            for (int oy = -2; oy <= 2 && !edge; ++oy) {
                for (int ox = -2; ox <= 2 && !edge; ++ox) {
                    int nx = x + ox, ny = y + oy;
                    if (nx < 0 || ny < 0 || nx >= t.width || ny >= t.height) continue;
                    edge = mask[static_cast<size_t>(ny) * static_cast<size_t>(t.width) + static_cast<size_t>(nx)] != 0;
                }
            }
            if (edge) t.color[idx] = Pack(orange);
        }
    }
}

}  // namespace

RenderStats SoftwareRenderer::Render(const Scene& scene, const RenderView& view, RenderTarget& target) {
    auto start = std::chrono::steady_clock::now();
    RenderStats stats;
    std::fill(target.color.begin(), target.color.end(), Pack(view.clearColor));
    std::fill(target.depth.begin(), target.depth.end(), 1.0f);
    std::fill(target.ids.begin(), target.ids.end(), kNullEntity);

    std::vector<Light> lights;
    Color ambient(0, 0, 0);
    for (const auto& kv : scene.Pool<DirectionalLight>()) {
        Light l;
        l.dir = Normalize(scene.WorldMatrix(kv.first).TransformDir(Vec3(0, 0, -1)));
        l.color = kv.second.color * kv.second.intensity;
        ambient = ambient + kv.second.ambient;
        lights.push_back(l);
    }
    if (lights.empty()) {
        lights.push_back({Normalize(Vec3(-0.4f, -1.0f, -0.3f)), Color(1, 1, 1)});
        ambient = Color(0.25f, 0.25f, 0.28f);
    }

    Rasterizer raster(target, view.proj * view.view);
    for (const auto& kv : scene.Pool<MeshRenderer>()) {
        const MeshRenderer& mr = kv.second;
        if (!mr.visible) continue;
        const Mesh* mesh = GetBuiltinMesh(mr.mesh);
        if (!mesh) continue;
        Mat4 world = scene.WorldMatrix(kv.first);
        std::vector<Vec3> wp(mesh->positions.size());
        for (size_t i = 0; i < wp.size(); ++i) wp[i] = world.TransformPoint(mesh->positions[i]);
        for (size_t i = 0; i + 2 < mesh->indices.size(); i += 3) {
            const Vec3& a = wp[mesh->indices[i]];
            const Vec3& b = wp[mesh->indices[i + 1]];
            const Vec3& c = wp[mesh->indices[i + 2]];
            Vec3 n = Normalize(Cross(b - a, c - a));
            Color lit = ambient;
            for (const Light& l : lights) lit = lit + l.color * std::max(0.0f, Dot(n, -l.dir));
            stats.triangles += raster.DrawTriangle(a, b, c, Pack(mr.color * lit), kv.first);
        }
        ++stats.drawnEntities;
    }

    if (view.drawGrid) {
        const Color gridColor(0.55f, 0.58f, 0.62f);
        for (int i = -20; i <= 20; ++i) {
            float f = static_cast<float>(i);
            if (i == 0) continue;
            raster.DrawLine(Vec3(f, 0, -20), Vec3(f, 0, 20), gridColor, 0.25f);
            raster.DrawLine(Vec3(-20, 0, f), Vec3(20, 0, f), gridColor, 0.25f);
        }
        raster.DrawLine(Vec3(-20, 0, 0), Vec3(20, 0, 0), Color(0.9f, 0.25f, 0.25f), 0.8f);  // X axis
        raster.DrawLine(Vec3(0, 0, -20), Vec3(0, 0, 20), Color(0.25f, 0.45f, 0.95f), 0.8f);  // Z axis
    }

    if (view.highlight != kNullEntity) DrawOutline(target, view.highlight);

    stats.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return stats;
}

bool MakeSceneView(const Scene& scene, float aspect, RenderView& out) {
    for (const auto& kv : scene.Pool<Camera>()) {
        if (!kv.second.active) continue;
        Mat4 world = scene.WorldMatrix(kv.first);
        Vec3 eye = world.TransformPoint(Vec3(0, 0, 0));
        Vec3 fwd = Normalize(world.TransformDir(Vec3(0, 0, -1)));
        Vec3 up = Normalize(world.TransformDir(Vec3(0, 1, 0)));
        out.view = Mat4::LookAt(eye, eye + fwd, up);
        out.proj = Mat4::Perspective(Radians(kv.second.fov), aspect, kv.second.nearPlane, kv.second.farPlane);
        out.eye = eye;
        out.clearColor = kv.second.clearColor;
        out.cameraEntity = kv.first;
        return true;
    }
    out = MakeLookAtView(Vec3(6, 5, 8), Vec3(0, 0, 0), 60.0f, aspect);
    return false;
}

RenderView MakeLookAtView(const Vec3& eye, const Vec3& target, float fovDeg, float aspect) {
    RenderView v;
    v.view = Mat4::LookAt(eye, target, Vec3(0, 1, 0));
    v.proj = Mat4::Perspective(Radians(fovDeg), aspect, 0.05f, 1000.0f);
    v.eye = eye;
    return v;
}

}  // namespace oe
