#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <thread>

#include "assets/Assets.h"
#include "render/Mesh.h"
#include "render/RenderScene.h"
#include "render/Renderer.h"
#include "render/UI.h"
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

// ----- Lighting ------------------------------------------------------------------

// Depth map rendered from the first directional light (orthographic).
struct ShadowMap {
    bool enabled = false;
    int size = 1024;
    std::vector<float> depth;
    Mat4 viewProj;
    float strength = 0.75f;
    float texelWorld = 0.01f;

    // 1 = fully lit, 0 = fully shadowed (3x3 PCF).
    float Lit(const Vec3& wpos, const Vec3& normal) const {
        if (!enabled) return 1.0f;
        Vec4 c = viewProj * Vec4(wpos + normal * (texelWorld * 1.5f), 1.0f);
        float u = c.x * 0.5f + 0.5f, v = 0.5f - c.y * 0.5f, z = c.z * 0.5f + 0.5f;
        if (u < 0 || u > 1 || v < 0 || v > 1 || z > 1) return 1.0f;
        int cx = static_cast<int>(u * static_cast<float>(size)), cy = static_cast<int>(v * static_cast<float>(size));
        int lit = 0;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                int x = std::clamp(cx + dx, 0, size - 1), y = std::clamp(cy + dy, 0, size - 1);
                lit += z - 0.002f <= depth[static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)];
            }
        }
        float fraction = static_cast<float>(lit) / 9.0f;
        return 1.0f - strength * (1.0f - fraction);
    }
};

struct Lighting {
    Color ambient{0, 0, 0};
    std::vector<RenderDirLight> dirs;
    std::vector<RenderPointLight> points;
    ShadowMap shadow;
    Vec3 eye;

    // Metallic-roughness shading (GGX / Smith / Schlick) in the engine's
    // lighting convention: light colors already include pi, so a white
    // diffuse surface lit head-on by a light of color c shows c (as before
    // PBR). Mirrors mesh_fs in shaders/Shaders.glsl. `ng` (geometric normal)
    // offsets the shadow lookup.
    Color Shade(const Vec3& wpos, const Vec3& n, const Vec3& ng, const Color& base, float metallic, float roughness, float ao) const {

        Vec3 v = Normalize(eye - wpos);
        float ndv = std::max(Dot(n, v), 1e-4f);
        Color diffuse = base * (1.0f - metallic);
        Color f0 = Color(0.04f, 0.04f, 0.04f) * (1.0f - metallic) + base * metallic;
        float r = Clamp(roughness, 0.04f, 1.0f);
        float a2 = r * r * r * r;
        float k = (r + 1.0f) * (r + 1.0f) / 8.0f;
        float gv = ndv / (ndv * (1.0f - k) + k);
        // Ambient: diffuse plus reflected surroundings, approximated by a
        // hemisphere that is brighter above (sky) than below (ground).
        Vec3 refl = n * (2.0f * Dot(n, v)) - v;
        float sky = 0.6f + 1.6f * (refl.y * 0.5f + 0.5f);
        float fe = std::pow(1.0f - ndv, 5.0f);
        Color fr(std::max(1.0f - r, f0.r), std::max(1.0f - r, f0.g), std::max(1.0f - r, f0.b));
        Color fenv = f0 + (fr + f0 * -1.0f) * fe;
        Color c = ambient * (diffuse + fenv * (sky * (1.0f - 0.75f * r))) * ao;
        auto add = [&](const Vec3& l, const Color& radiance) {
            float ndl = Dot(n, l);
            if (ndl <= 0.0f) return;
            Vec3 h = Normalize(l + v);
            float ndh = std::max(Dot(n, h), 0.0f), vdh = std::max(Dot(v, h), 0.0f);
            float d = ndh * ndh * (a2 - 1.0f) + 1.0f;
            float D = a2 / (kPi * d * d);
            float gl = ndl / (ndl * (1.0f - k) + k);
            float fw = std::pow(1.0f - vdh, 5.0f);
            Color F = f0 * (1.0f - fw) + Color(fw, fw, fw);
            float spec = D * gv * gl / (4.0f * ndv * ndl + 1e-4f) * kPi;
            c = c + (diffuse + F * spec) * radiance * ndl;
        };
        for (size_t i = 0; i < dirs.size(); ++i) {
            float lit = i == 0 ? shadow.Lit(wpos, ng) : 1.0f;
            if (lit > 0.0f) add(-dirs[i].dir, dirs[i].color * lit);
        }
        for (const RenderPointLight& p : points) {
            Vec3 d = p.pos - wpos;
            float dist = Length(d);
            if (dist >= p.range || dist < 1e-5f) continue;
            float fall = 1.0f - dist / p.range;
            add(d / dist, p.color * (fall * fall));
        }
        return c;
    }
};

// ----- Rasterization ---------------------------------------------------------------

struct Vtx {
    Vec4 clip;
    Vec3 wpos;
    Vec3 nrm;
    Vec4 tan;
    float u = 0, v = 0;
};

Vtx LerpVtx(const Vtx& a, const Vtx& b, float t) {
    Vtx r;
    r.clip = a.clip + (b.clip - a.clip) * t;
    r.wpos = a.wpos + (b.wpos - a.wpos) * t;
    r.nrm = a.nrm + (b.nrm - a.nrm) * t;
    r.tan = a.tan + (b.tan - a.tan) * t;
    r.u = a.u + (b.u - a.u) * t;
    r.v = a.v + (b.v - a.v) * t;
    return r;
}

// A draw call's material plus the per-item bits the rasterizer needs.
struct RasterMaterial {
    const Material* m = nullptr;
    bool flat = false;
    bool blend = false;
    EntityId id = kNullEntity;
    float uvOffset[2] = {0, 0};  // item uv rect (sprite frames) applied before the material tiling
    float uvScale[2] = {1, 1};
};

enum class Cull { Back, Front, None };

// Screen-space triangle setup shared by the color and depth-only passes.
struct Screen {
    float x, y, z, invW;
};

// Rasterizes into rows [yMin, yMax). Several rasterizers over disjoint row
// bands can run in parallel: every pixel is owned by one band and sees the
// triangles in the same order, so the image is identical to a single thread.
class Rasterizer {
public:
    Rasterizer(int width, int height, float* depth, uint32_t* color, EntityId* ids, int yMin = 0, int yMax = -1)
        : w_(width), h_(height), yMin_(yMin), yMax_(yMax < 0 ? height : yMax), depth_(depth), color_(color), ids_(ids) {}

    // Clips against the near plane, then rasterizes. `lighting` null = depth only.
    int Draw(const Vtx in[3], const RasterMaterial& mat, const Lighting* lighting, Cull cull) {
        Vtx poly[4];
        int count = 0;
        for (int i = 0; i < 3; ++i) {
            const Vtx& p = in[i];
            const Vtx& q = in[(i + 1) % 3];
            float dp = p.clip.z + p.clip.w, dq = q.clip.z + q.clip.w;
            if (dp >= 0) poly[count++] = p;
            if ((dp >= 0) != (dq >= 0)) poly[count++] = LerpVtx(p, q, dp / (dp - dq));
        }
        if (count < 3) return 0;
        Vec3 faceN = Normalize(Cross(in[1].wpos - in[0].wpos, in[2].wpos - in[0].wpos));
        int drawn = 0;
        for (int i = 1; i + 1 < count; ++i) drawn += Raster(poly[0], poly[i], poly[i + 1], mat, lighting, cull, faceN);
        return drawn;
    }

    void Line(const Vec4& p0, const Vec4& q0, const Color& color, float alpha) {
        Vec4 p = p0, q = q0;
        float dp = p.z + p.w, dq = q.z + q.w;
        if (dp < 0 && dq < 0) return;
        if (dp < 0) p = p + (q - p) * (dp / (dp - dq));
        else if (dq < 0) q = q + (p - q) * (dq / (dq - dp));
        Screen s0 = ToScreen(p), s1 = ToScreen(q);
        float dx = s1.x - s0.x, dy = s1.y - s0.y;
        int steps = static_cast<int>(std::max(std::fabs(dx), std::fabs(dy)));
        if (steps > 8192) return;
        for (int i = 0; i <= steps; ++i) {
            float t = steps == 0 ? 0.0f : static_cast<float>(i) / static_cast<float>(steps);
            int x = static_cast<int>(s0.x + dx * t), y = static_cast<int>(s0.y + dy * t);
            if (x < 0 || y < yMin_ || x >= w_ || y >= yMax_) continue;
            float z = s0.z + (s1.z - s0.z) * t;
            size_t idx = static_cast<size_t>(y) * static_cast<size_t>(w_) + static_cast<size_t>(x);
            if (z <= depth_[idx] + 1e-4f) color_[idx] = Blend(color_[idx], color, alpha);
        }
    }

private:
    Screen ToScreen(const Vec4& c) const {
        float iw = 1.0f / c.w;
        return {(c.x * iw * 0.5f + 0.5f) * static_cast<float>(w_), (0.5f - c.y * iw * 0.5f) * static_cast<float>(h_), c.z * iw * 0.5f + 0.5f, iw};
    }

    static float Edge(const Screen& a, const Screen& b, float px, float py) { return (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x); }

    int Raster(const Vtx& a, const Vtx& b, const Vtx& c, const RasterMaterial& mat, const Lighting* lighting, Cull cull, const Vec3& faceN) {
        Screen s0 = ToScreen(a.clip), s1 = ToScreen(b.clip), s2 = ToScreen(c.clip);
        float area = Edge(s0, s1, s2.x, s2.y);
        if (area == 0 || (cull == Cull::Back && area > 0) || (cull == Cull::Front && area < 0)) return 0;
        const bool backFace = area > 0;  // only reached with Cull::None (double-sided materials)
        int minX = std::max(0, static_cast<int>(std::floor(std::min({s0.x, s1.x, s2.x}))));
        int maxX = std::min(w_ - 1, static_cast<int>(std::ceil(std::max({s0.x, s1.x, s2.x}))));
        int minY = std::max(yMin_, static_cast<int>(std::floor(std::min({s0.y, s1.y, s2.y}))));
        int maxY = std::min(yMax_ - 1, static_cast<int>(std::ceil(std::max({s0.y, s1.y, s2.y}))));
        if (minX > maxX || minY > maxY) return 1;  // front facing, just not in this band
        const float inv = 1.0f / std::fabs(area);
        // Edge functions are evaluated with each edge's endpoints in a fixed
        // order, so two triangles sharing an edge compute exactly opposite
        // values there: every pixel center belongs to exactly one of them (on
        // the edge itself, the one on the edge's positive side). No gaps, and
        // blended surfaces are not drawn twice along their diagonals.
        struct EdgeEq {
            Screen a, b;
            float side;  // +1 / -1: sign of the edge function inside the triangle
        };
        auto edgeEq = [](const Screen& p, const Screen& q, const Screen& third) {
            bool keep = p.y < q.y || (p.y == q.y && p.x < q.x);
            EdgeEq e{keep ? p : q, keep ? q : p, 1.0f};
            e.side = Edge(e.a, e.b, third.x, third.y) > 0 ? 1.0f : -1.0f;
            return e;
        };
        const EdgeEq e0 = edgeEq(s1, s2, s0), e1 = edgeEq(s2, s0, s1), e2 = edgeEq(s0, s1, s2);
        auto weight = [](const EdgeEq& e, float px, float py, float* w) {
            float v = Edge(e.a, e.b, px, py) * e.side;
            *w = v;
            return v > 0 || (v == 0 && e.side > 0);
        };
        for (int y = minY; y <= maxY; ++y) {
            float py = static_cast<float>(y) + 0.5f;
            size_t row = static_cast<size_t>(y) * static_cast<size_t>(w_);
            for (int x = minX; x <= maxX; ++x) {
                float px = static_cast<float>(x) + 0.5f;
                float w0, w1, w2;
                if (!weight(e0, px, py, &w0) || !weight(e1, px, py, &w1) || !weight(e2, px, py, &w2)) continue;
                w0 *= inv;
                w1 *= inv;
                w2 *= inv;
                float z = w0 * s0.z + w1 * s1.z + w2 * s2.z;
                size_t idx = row + static_cast<size_t>(x);
                if (z < 0.0f || z >= depth_[idx]) continue;
                if (!lighting) {
                    depth_[idx] = z;
                    continue;
                }
                // Perspective-correct attribute weights.
                float p0 = w0 * s0.invW, p1 = w1 * s1.invW, p2 = w2 * s2.invW;
                float norm = 1.0f / (p0 + p1 + p2);
                p0 *= norm;
                p1 *= norm;
                p2 *= norm;
                const Material& m = *mat.m;
                float u = ((a.u * p0 + b.u * p1 + c.u * p2) * mat.uvScale[0] + mat.uvOffset[0]) * m.tiling[0] + m.offset[0];
                float v = ((a.v * p0 + b.v * p1 + c.v * p2) * mat.uvScale[1] + mat.uvOffset[1]) * m.tiling[1] + m.offset[1];
                Color base = m.baseColor;
                float alpha = m.opacity;
                if (m.baseTexture) {
                    float ta = 1.0f;
                    base = base * m.baseTexture->Sample(u, v, m.pixelArt, &ta);
                    alpha *= ta;
                }
                if (m.alphaMode == AlphaMode::Mask && alpha < m.alphaCutoff) continue;  // cut out: no depth, color or id
                if (!mat.blend) depth_[idx] = z;
                Color out = base;
                if (!m.unlit) {
                    Vec3 wpos = a.wpos * p0 + b.wpos * p1 + c.wpos * p2;
                    Vec3 ng = mat.flat ? faceN : Normalize(a.nrm * p0 + b.nrm * p1 + c.nrm * p2);
                    if (backFace) ng = -ng;
                    Vec3 n = ng;
                    if (m.normalTexture) {
                        Vec4 t4 = a.tan * p0 + b.tan * p1 + c.tan * p2;
                        Vec3 t = Vec3(t4.x, t4.y, t4.z);
                        t = t - ng * Dot(ng, t);
                        if (Length(t) > 1e-6f) {
                            t = Normalize(t);
                            Vec3 bt = Cross(ng, t) * (t4.w < 0.0f ? -1.0f : 1.0f);
                            Color nt = m.normalTexture->Sample(u, v, m.pixelArt, nullptr);
                            float nx = (nt.r * 2.0f - 1.0f) * m.normalScale, ny = (nt.g * 2.0f - 1.0f) * m.normalScale, nz = nt.b * 2.0f - 1.0f;
                            n = Normalize(t * nx + bt * ny + ng * nz);
                        }
                    }
                    float metallic = m.metallic, roughness = m.roughness, ao = 1.0f;
                    if (m.metallicRoughnessTexture) {
                        Color mr = m.metallicRoughnessTexture->Sample(u, v, m.pixelArt, nullptr);
                        roughness *= mr.g;
                        metallic *= mr.b;
                    }
                    if (m.occlusionTexture) ao = 1.0f + m.occlusionStrength * (m.occlusionTexture->Sample(u, v, m.pixelArt, nullptr).r - 1.0f);
                    out = lighting->Shade(wpos, n, ng, base, metallic, roughness, ao);
                }
                Color em = m.emissive * m.emissiveIntensity;
                if (m.emissiveTexture) em = em * m.emissiveTexture->Sample(u, v, m.pixelArt, nullptr);
                out = out + em;
                if (mat.blend) {
                    alpha = Clamp(alpha, 0.0f, 1.0f);
                    color_[idx] = Blend(color_[idx], out, alpha);
                    if (alpha >= 0.5f) ids_[idx] = mat.id;
                } else {
                    color_[idx] = Pack(out);
                    ids_[idx] = mat.id;
                }
            }
        }
        return 1;
    }

    int w_, h_, yMin_, yMax_;
    float* depth_;
    uint32_t* color_;
    EntityId* ids_;
};

int g_maxRenderThreads = 16;

// Runs fn(bandIndex, y0, y1) over horizontal bands on worker threads.
void ParallelBands(int height, const std::function<void(int, int, int)>& fn) {
#ifdef __EMSCRIPTEN__
    // The web build is single threaded (no SharedArrayBuffer requirements for hosts).
    fn(0, 0, height);
    return;
#endif
    unsigned hw = std::min(static_cast<unsigned>(std::max(1, g_maxRenderThreads)), std::max(1u, std::thread::hardware_concurrency()));
    int bands = static_cast<int>(std::min<unsigned>(std::min(hw, 16u), static_cast<unsigned>(std::max(1, height / 32))));
    if (bands <= 1) {
        fn(0, 0, height);
        return;
    }
    std::vector<std::thread> threads;
    for (int b = 1; b < bands; ++b) threads.emplace_back(fn, b, height * b / bands, height * (b + 1) / bands);
    fn(0, 0, height / bands);
    for (std::thread& t : threads) t.join();
}

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

// Transforms mesh vertices into world space once per item.
struct Transformed {
    std::vector<Vec3> wpos;
    std::vector<Vec3> nrm;
    std::vector<Vec4> tan;
};

Transformed TransformItem(const RenderItem& item) {
    Transformed t;
    const Mesh& m = *item.mesh;
    t.wpos.resize(m.positions.size());
    t.nrm.resize(m.positions.size());
    t.tan.resize(m.positions.size(), Vec4(1, 0, 0, 1));
    for (size_t i = 0; i < m.positions.size(); ++i) {
        t.wpos[i] = item.world.TransformPoint(m.positions[i]);
        t.nrm[i] = i < m.normals.size() ? Normalize(item.normalMatrix.TransformDir(m.normals[i])) : Vec3(0, 1, 0);
        if (i < m.tangents.size()) {
            Vec3 tw = Normalize(item.world.TransformDir(Vec3(m.tangents[i].x, m.tangents[i].y, m.tangents[i].z)));
            t.tan[i] = Vec4(tw.x, tw.y, tw.z, m.tangents[i].w);
        }
    }
    return t;
}

}  // namespace

void SetMaxRenderThreads(int threads) { g_maxRenderThreads = threads; }

RenderStats SoftwareRenderer::Render(const Scene& scene, const RenderView& view, RenderTarget& target) {
    auto start = std::chrono::steady_clock::now();
    RenderStats stats;
    std::fill(target.color.begin(), target.color.end(), Pack(view.clearColor));
    std::fill(target.depth.begin(), target.depth.end(), 1.0f);
    std::fill(target.ids.begin(), target.ids.end(), kNullEntity);

    std::vector<RenderItem> items = GatherRenderItems(scene, assets_);
    RenderLights gathered = GatherRenderLights(scene);
    Lighting lighting;
    lighting.ambient = gathered.ambient;
    lighting.dirs = gathered.dirs;
    lighting.points = gathered.points;
    lighting.eye = view.eye;
    const bool shadows = gathered.shadows;
    const float shadowStrength = gathered.shadowStrength;

    std::vector<Transformed> transformed;
    transformed.reserve(items.size());
    for (const RenderItem& it : items) transformed.push_back(TransformItem(it));
    const std::vector<DrawCall> draws = BuildDrawList(items, view.eye);

    // ----- Shadow map (first directional light, fitted to the shadow casters + receivers)
    if (shadows && !items.empty()) {
        Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
        bool anyCaster = false;
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].unlit) continue;
            anyCaster = anyCaster || items[i].castShadows;
            for (const Vec3& p : transformed[i].wpos) {
                lo = Vec3(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
                hi = Vec3(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
            }
        }
        if (anyCaster && lo.x <= hi.x) {
            ShadowMap& sm = lighting.shadow;
            sm.enabled = true;
            sm.strength = shadowStrength;
            ShadowFit fit = FitShadow(lo, hi, lighting.dirs[0].dir, sm.size);
            sm.viewProj = fit.viewProj;
            sm.texelWorld = fit.texelWorld;
            sm.depth.assign(static_cast<size_t>(sm.size) * static_cast<size_t>(sm.size), 1.0f);
            // Light-space vertices once, then rasterize depth in parallel bands.
            std::vector<std::vector<Vec4>> lightClip(items.size());
            for (size_t i = 0; i < items.size(); ++i) {
                if (!items[i].castShadows || items[i].unlit) continue;
                lightClip[i].reserve(transformed[i].wpos.size());
                for (const Vec3& p : transformed[i].wpos) lightClip[i].push_back(sm.viewProj * Vec4(p, 1.0f));
            }
            ParallelBands(sm.size, [&](int, int y0, int y1) {
                uint32_t dummyColor = 0;
                EntityId dummyId = kNullEntity;
                Rasterizer shadowRaster(sm.size, sm.size, sm.depth.data(), &dummyColor, &dummyId, y0, y1);
                RasterMaterial none;
                for (const DrawCall& dc : draws) {
                    if (dc.blend || lightClip[dc.item].empty()) continue;  // transparent surfaces cast no shadow
                    const Mesh& m = *items[dc.item].mesh;
                    const Submesh& sub = m.submeshes[dc.submesh];
                    for (uint32_t k = sub.firstIndex; k + 2 < sub.firstIndex + sub.indexCount; k += 3) {
                        Vtx v[3];
                        for (int j = 0; j < 3; ++j) v[j].clip = lightClip[dc.item][m.indices[k + static_cast<uint32_t>(j)]];
                        // Back faces into the shadow map: avoids self-shadowing acne on lit faces
                        // (double-sided surfaces put both sides in).
                        shadowRaster.Draw(v, none, nullptr, dc.material.doubleSided ? Cull::None : Cull::Front);
                    }
                }
            });
        }
    }

    // ----- Main pass
    const Mat4 viewProj = view.proj * view.view;
    std::vector<std::vector<Vec4>> clip(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        clip[i].reserve(transformed[i].wpos.size());
        for (const Vec3& p : transformed[i].wpos) clip[i].push_back(viewProj * Vec4(p, 1.0f));
    }
    int bandTriangles = 0;
    ParallelBands(target.height, [&](int band, int y0, int y1) {
      Rasterizer raster(target.width, target.height, target.depth.data(), target.color.data(), target.ids.data(), y0, y1);
      int drawn = 0;
      for (const DrawCall& dc : draws) {
        const size_t i = dc.item;
        const RenderItem& it = items[i];
        const Mesh& m = *it.mesh;
        const Transformed& tr = transformed[i];
        const Submesh& sub = m.submeshes[dc.submesh];
        RasterMaterial mat;
        mat.m = &dc.material;
        mat.flat = it.flat;
        mat.blend = dc.blend;
        mat.id = it.id;
        mat.uvOffset[0] = it.uvOffset[0];
        mat.uvOffset[1] = it.uvOffset[1];
        mat.uvScale[0] = it.uvScale[0];
        mat.uvScale[1] = it.uvScale[1];
        const Cull cull = dc.material.doubleSided ? Cull::None : Cull::Back;
        for (uint32_t k = sub.firstIndex; k + 2 < sub.firstIndex + sub.indexCount; k += 3) {
            Vtx v[3];
            for (int j = 0; j < 3; ++j) {
                uint32_t idx = m.indices[k + static_cast<uint32_t>(j)];
                v[j].wpos = tr.wpos[idx];
                v[j].nrm = tr.nrm[idx];
                v[j].tan = tr.tan[idx];
                v[j].clip = clip[i][idx];
                if (idx * 2 + 1 < m.uvs.size()) {
                    v[j].u = m.uvs[idx * 2];
                    v[j].v = m.uvs[idx * 2 + 1];
                }
            }
            drawn += raster.Draw(v, mat, &lighting, cull);
        }
      }
      if (band == 0) bandTriangles = drawn;
    });
    stats.triangles = bandTriangles;
    stats.drawnEntities = static_cast<int>(items.size());

    // ----- Overlays
    Rasterizer raster(target.width, target.height, target.depth.data(), target.color.data(), target.ids.data());
    auto line = [&](const Vec3& a, const Vec3& b, const Color& c, float alpha) {
        raster.Line(viewProj * Vec4(a, 1.0f), viewProj * Vec4(b, 1.0f), c, alpha);
    };
    if (view.drawGrid) {
        const Color gridColor(0.55f, 0.58f, 0.62f);
        for (int i = -20; i <= 20; ++i) {
            float f = static_cast<float>(i);
            if (i == 0) continue;
            line(Vec3(f, 0, -20), Vec3(f, 0, 20), gridColor, 0.25f);
            line(Vec3(-20, 0, f), Vec3(20, 0, f), gridColor, 0.25f);
        }
        line(Vec3(-20, 0, 0), Vec3(20, 0, 0), Color(0.9f, 0.25f, 0.25f), 0.8f);  // X axis
        line(Vec3(0, 0, -20), Vec3(0, 0, 20), Color(0.25f, 0.45f, 0.95f), 0.8f);  // Z axis
    }
    for (const DebugLine& l : view.lines) line(l.a, l.b, l.color, 1.0f);

    if (view.highlight != kNullEntity) DrawOutline(target, view.highlight);
    if (view.drawUI) DrawUI(scene, target, assets_);

    stats.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return stats;
}

bool MakeSceneView(const Scene& scene, float aspect, RenderView& out) {
    for (const auto& kv : scene.Pool<Camera>()) {
        if (!kv.second.active) continue;
        const Camera& cam = kv.second;
        Mat4 world = scene.WorldMatrix(kv.first);
        Vec3 eye = world.TransformPoint(Vec3(0, 0, 0));
        Vec3 fwd = Normalize(world.TransformDir(Vec3(0, 0, -1)));
        Vec3 up = Normalize(world.TransformDir(Vec3(0, 1, 0)));
        out.view = Mat4::LookAt(eye, eye + fwd, up);
        out.proj = cam.projection == "orthographic" ? Mat4::Orthographic(std::max(0.01f, cam.orthoSize), aspect, cam.nearPlane, cam.farPlane)
                                                    : Mat4::Perspective(Radians(cam.fov), aspect, cam.nearPlane, cam.farPlane);
        out.eye = eye;
        out.clearColor = cam.clearColor;
        out.cameraEntity = kv.first;
        return true;
    }
    out = MakeLookAtView(Vec3(6, 5, 8), Vec3(0, 0, 0), 60.0f, aspect);
    out.drawUI = true;  // still the game view, just without a camera entity
    return false;
}

RenderView MakeLookAtView(const Vec3& eye, const Vec3& target, float fovDeg, float aspect) {
    RenderView v;
    v.view = Mat4::LookAt(eye, target, Vec3(0, 1, 0));
    v.proj = Mat4::Perspective(Radians(fovDeg), aspect, 0.05f, 1000.0f);
    v.eye = eye;
    v.drawUI = false;  // free cameras (editor scene view) show the world only
    return v;
}

}  // namespace oe
