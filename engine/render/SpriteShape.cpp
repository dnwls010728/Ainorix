#include "render/SpriteShape.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "scene/Components.h"

namespace oe {

namespace {

constexpr int kMaxShapePieces = 4096;  // tiled sprites: repeats drawn at most

struct ShapeVertex {
    float x, y, u, v;
};
using Polygon = std::vector<ShapeVertex>;

// Keeps the part of a convex polygon where f(x, y) >= 0 (f linear), interpolating the uvs.
template <typename F>
Polygon Clip(const Polygon& in, F f) {
    Polygon out;
    for (size_t i = 0; i < in.size(); ++i) {
        const ShapeVertex& a = in[i];
        const ShapeVertex& b = in[(i + 1) % in.size()];
        const float fa = f(a.x, a.y), fb = f(b.x, b.y);
        if (fa >= 0) out.push_back(a);
        if ((fa >= 0) != (fb >= 0)) {
            const float t = fa / (fa - fb);
            out.push_back({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.u + (b.u - a.u) * t, a.v + (b.v - a.v) * t});
        }
    }
    return out;
}

Polygon Rect(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
    // Counter-clockwise from the bottom-left; y up, v down (like the built-in quad).
    return {{x0, y0, u0, v1}, {x1, y0, u1, v1}, {x1, y1, u1, v0}, {x0, y1, u0, v0}};
}

std::vector<Polygon> Pieces(const SpriteShape& s) {
    std::vector<Polygon> pieces;
    const float ppu = std::max(0.001f, s.pixelsPerUnit);
    const float w = std::max(1e-6f, s.width), h = std::max(1e-6f, s.height);
    if (s.drawMode == "tiled") {
        // Repeats of the frame at its pixel size, from the top-left corner.
        const float tw = s.frameW / ppu / w, th = s.frameH / ppu / h;
        if (!(tw > 1e-6f) || !(th > 1e-6f)) return pieces;
        const int nx = static_cast<int>(std::ceil(1 / tw - 1e-4f)), ny = static_cast<int>(std::ceil(1 / th - 1e-4f));
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx && static_cast<int>(pieces.size()) < kMaxShapePieces; ++i) {
                const float x0 = -0.5f + static_cast<float>(i) * tw, x1 = std::min(0.5f, x0 + tw);
                const float y1 = 0.5f - static_cast<float>(j) * th, y0 = std::max(-0.5f, y1 - th);
                pieces.push_back(Rect(x0, y0, x1, y1, 0, 0, (x1 - x0) / tw, (y1 - y0) / th));
            }
        }
    } else if (s.drawMode == "sliced" && s.slice > 0) {
        // Nine slices: the border keeps its pixel size (shrunk when the sprite is smaller than two borders).
        const float bu = std::min(s.slice, s.frameW * 0.5f), bv = std::min(s.slice, s.frameH * 0.5f);
        const float bx = std::min(bu / ppu / w, 0.5f), by = std::min(bv / ppu / h, 0.5f);
        const float xs[4] = {-0.5f, -0.5f + bx, 0.5f - bx, 0.5f}, us[4] = {0, bu / s.frameW, 1 - bu / s.frameW, 1};
        const float ys[4] = {0.5f, 0.5f - by, -0.5f + by, -0.5f}, vs[4] = {0, bv / s.frameH, 1 - bv / s.frameH, 1};
        for (int j = 0; j < 3; ++j) {
            for (int i = 0; i < 3; ++i) {
                if (xs[i + 1] - xs[i] <= 1e-6f || ys[j] - ys[j + 1] <= 1e-6f) continue;
                pieces.push_back(Rect(xs[i], ys[j + 1], xs[i + 1], ys[j], us[i], vs[j], us[i + 1], vs[j + 1]));
            }
        }
    } else {
        pieces.push_back(Rect(-0.5f, -0.5f, 0.5f, 0.5f, 0, 0, 1, 1));
    }
    return pieces;
}

// Twice the signed area; clipping leaves slivers of zero area where an edge meets the cut.
float Area2(const Polygon& p) {
    float sum = 0;
    for (size_t i = 0; i < p.size(); ++i) sum += p[i].x * p[(i + 1) % p.size()].y - p[(i + 1) % p.size()].x * p[i].y;
    return sum;
}

// The visible part of each piece: left to right, bottom to top, or clockwise from the top.
std::vector<Polygon> Filled(const std::vector<Polygon>& pieces, float fill, const std::string& mode) {
    if (fill >= 1) return pieces;
    std::vector<Polygon> out;
    if (fill <= 0) return out;
    if (mode == "radial") {
        // Wedges of at most half a turn are convex: two half-planes each.
        const float turn = fill * 2 * kPi;
        const float wedges[2][2] = {{0, std::min(turn, kPi)}, {kPi, turn}};
        for (const auto& wedge : wedges) {
            if (wedge[1] <= wedge[0]) continue;
            const float sx0 = std::sin(wedge[0]), cy0 = std::cos(wedge[0]), sx1 = std::sin(wedge[1]), cy1 = std::cos(wedge[1]);
            for (const Polygon& piece : pieces) {
                Polygon p = Clip(piece, [&](float x, float y) { return -(sx0 * y - cy0 * x); });
                if (p.size() >= 3) p = Clip(p, [&](float x, float y) { return sx1 * y - cy1 * x; });
                if (p.size() >= 3 && Area2(p) > 1e-9f) out.push_back(std::move(p));
            }
        }
        return out;
    }
    const bool vertical = mode == "vertical";
    const float edge = -0.5f + fill;
    for (const Polygon& piece : pieces) {
        Polygon p = Clip(piece, [&](float x, float y) { return edge - (vertical ? y : x); });
        if (p.size() >= 3 && Area2(p) > 1e-9f) out.push_back(std::move(p));
    }
    return out;
}

std::mutex g_shapeMutex;
std::map<std::string, std::shared_ptr<const Mesh>> g_shapes;

}  // namespace

std::shared_ptr<const Mesh> SpriteShapeMesh(const SpriteShape& s) {
    const bool plain = (s.drawMode == "simple" || (s.drawMode == "sliced" && s.slice <= 0)) && s.fill >= 1;
    if (plain && !s.rotated) return nullptr;
    char numbers[256];
    std::snprintf(numbers, sizeof(numbers), "|%.6g|%.6g|%.6g|%.6g|%.6g|%.6g|%.6g|%d%d%d", s.slice, s.fill, s.frameW, s.frameH, s.width, s.height,
                  s.pixelsPerUnit, s.rotated ? 1 : 0, s.flipX ? 1 : 0, s.flipY ? 1 : 0);
    const std::string key = s.drawMode + "|" + s.fillMode + numbers;
    std::lock_guard<std::mutex> lock(g_shapeMutex);
    auto found = g_shapes.find(key);
    if (found != g_shapes.end()) return found->second;

    auto mesh = std::make_shared<Mesh>();
    for (const Polygon& polygon : Filled(Pieces(s), s.fill, s.fillMode)) {
        const uint32_t base = static_cast<uint32_t>(mesh->positions.size());
        for (const ShapeVertex& v : polygon) {
            mesh->positions.push_back(Vec3(v.x, v.y, 0));
            mesh->normals.push_back(Vec3(0, 0, 1));
            float u = v.u, t = v.v;
            if (s.rotated) {  // TexturePacker stores the frame turned clockwise; undo it before the flips
                u = s.flipY ? v.v : 1 - v.v;
                t = s.flipX ? 1 - v.u : v.u;
            }
            mesh->uvs.push_back(u);
            mesh->uvs.push_back(t);
        }
        for (uint32_t i = 1; i + 1 < polygon.size(); ++i) mesh->indices.insert(mesh->indices.end(), {base, base + i, base + i + 1});
    }
    Submesh sub;
    sub.indexCount = static_cast<uint32_t>(mesh->indices.size());
    mesh->submeshes = {sub};
    mesh->ComputeBounds();
    mesh->ComputeTangents();
    if (g_shapes.size() >= 256) g_shapes.clear();
    g_shapes[key] = mesh;
    return mesh;
}

void ApplySpriteMasks(const Scene& scene, std::vector<RenderItem>& items) {
    for (RenderItem& item : items) {
        const Sprite* sp = scene.Get<Sprite>(item.id);
        if (!sp || sp->mask == kNullEntity || !scene.Get<Sprite>(sp->mask)) continue;
        item.mask2DOn = true;  // a mask that is not drawn (invisible, broken) hides the sprite
        for (const RenderItem& mask : items) {
            if (mask.id != sp->mask || !scene.Get<Sprite>(mask.id)) continue;
            // World -> the mask quad's local x/y (-0.5..0.5), then its frame uvs.
            const Mat4 inverse = mask.world.Inverse();
            item.mask2DX = Vec4(inverse.at(0, 0), inverse.at(0, 1), inverse.at(0, 2), inverse.at(0, 3));
            item.mask2DY = Vec4(inverse.at(1, 0), inverse.at(1, 1), inverse.at(1, 2), inverse.at(1, 3));
            item.mask2DUv = Vec4(mask.uvOffset[0], mask.uvOffset[1], mask.uvScale[0], mask.uvScale[1]);
            item.mask2DTexture = mask.textureOverride;
            item.mask2DCutoff = mask.alphaCutoff > 0 ? mask.alphaCutoff : 0.5f;
            item.mask2DFound = true;
            break;
        }
    }
    items.erase(std::remove_if(items.begin(), items.end(),
                               [&](const RenderItem& item) {
                                   const Sprite* sp = scene.Get<Sprite>(item.id);
                                   return sp && sp->maskOnly;
                               }),
                items.end());
}


float SpriteMaskAlpha(const Material& m, const Vec3& wpos) {
    if (!m.mask2DFound) return 0;
    const Vec4 p(wpos, 1);
    const float lx = m.mask2DX.x * p.x + m.mask2DX.y * p.y + m.mask2DX.z * p.z + m.mask2DX.w;
    const float ly = m.mask2DY.x * p.x + m.mask2DY.y * p.y + m.mask2DY.z * p.z + m.mask2DY.w;
    if (std::fabs(lx) > 0.5f || std::fabs(ly) > 0.5f) return 0;
    const Texture* t = m.mask2DTexture.get();
    if (!t || t->width <= 0 || t->height <= 0) return 1;
    const float u = (lx + 0.5f) * m.mask2DUv.z + m.mask2DUv.x, v = (0.5f - ly) * m.mask2DUv.w + m.mask2DUv.y;
    const int x = std::clamp(static_cast<int>(std::floor((u - std::floor(u)) * static_cast<float>(t->width))), 0, t->width - 1);
    const int y = std::clamp(static_cast<int>(std::floor((v - std::floor(v)) * static_cast<float>(t->height))), 0, t->height - 1);
    const float alpha = static_cast<float>(t->texels[static_cast<size_t>(y) * static_cast<size_t>(t->width) + static_cast<size_t>(x)] >> 24) / 255.0f;
    return alpha >= m.mask2DCutoff ? 1.0f : 0.0f;
}

}  // namespace oe
