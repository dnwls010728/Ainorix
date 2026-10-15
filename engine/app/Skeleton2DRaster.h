#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "assets/Assets.h"
#include "core/Image.h"
#include "render/ImageDraw.h"
#include "render/Skeleton2DMesh.h"
#include "scene/Skeleton2D.h"

// A small rasterizer for the skeleton review tools (app/Skeleton2DReview.cpp, app/Skeleton2DVariants.cpp): draws the
// attachments of a pose back to front and tells, for every pixel, which piece shows it and from which texel. No GPU,
// deterministic, nearest-texel; it is for measuring, not for the game's picture.
namespace oe::raster2d {

// A pose's pieces with their vertices moved by the pose.
struct Shown {
    std::vector<Skeleton2DPiece> pieces;
    std::vector<std::vector<Vec3>> posed;
};

inline Shown Show(const Skeleton2DDef& def, const std::string& skin, const Skeleton2DPose& pose, AssetManager& assets) {
    Shown s;
    s.pieces = Skeleton2DPieces(def, skin, pose, &assets);
    const std::vector<Mat4> palette = Skeleton2DPalette(def, pose);
    for (const Skeleton2DPiece& piece : s.pieces) {
        std::vector<Vec3> moved;
        for (size_t v = 0; v < piece.positions.size(); ++v) moved.push_back(Skeleton2DSkinPoint(palette, piece.skin[v], piece.positions[v]));
        s.posed.push_back(std::move(moved));
    }
    return s;
}

// What part of the world a frame shows: `left`/`top` world corner, `scale` pixels per world unit.
struct View {
    float left = 0, top = 0, scale = 1;
    int width = 1, height = 1;
};

inline View FitView(const std::vector<const Shown*>& shown, int maxPixels, float maxScale) {
    float lx = 1e30f, ly = 1e30f, hx = -1e30f, hy = -1e30f;
    for (const Shown* s : shown)
        for (const auto& list : s->posed)
            for (const Vec3& p : list) lx = std::min(lx, p.x), ly = std::min(ly, p.y), hx = std::max(hx, p.x), hy = std::max(hy, p.y);
    if (lx > hx) lx = ly = -1, hx = hy = 1;
    const float margin = 0.04f * std::max(hx - lx, hy - ly);
    lx -= margin, ly -= margin, hx += margin, hy += margin;
    View v;
    v.scale = std::min(maxScale, static_cast<float>(maxPixels) / std::max(1e-3f, std::max(hx - lx, hy - ly)));
    v.left = lx;
    v.top = hy;
    v.width = std::max(1, static_cast<int>(std::ceil((hx - lx) * v.scale)));
    v.height = std::max(1, static_cast<int>(std::ceil((hy - ly) * v.scale)));
    return v;
}

// Calls fn(piece, pixel, texel index, texel colour) for every pixel of every piece where its picture is opaque,
// back to front (later calls cover earlier ones).
template <class Fn>
void Rasterize(const View& view, const Shown& shown, Fn&& fn) {
    for (size_t i = 0; i < shown.pieces.size(); ++i) {
        const Skeleton2DPiece& piece = shown.pieces[i];
        const Texture* texture = piece.texture.get();
        if (!texture || texture->width <= 0) continue;
        const std::vector<Vec3>& p = shown.posed[i];
        for (size_t t = 0; t + 2 < piece.triangles.size(); t += 3) {
            const uint32_t ia = piece.triangles[t], ib = piece.triangles[t + 1], ic = piece.triangles[t + 2];
            float sx[3], sy[3];
            const uint32_t idx[3] = {ia, ib, ic};
            for (int k = 0; k < 3; ++k) {
                sx[k] = (p[idx[k]].x - view.left) * view.scale;
                sy[k] = (view.top - p[idx[k]].y) * view.scale;
            }
            const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0]);
            if (std::fabs(area) < 1e-6f) continue;
            const int x0 = std::max(0, static_cast<int>(std::floor(std::min({sx[0], sx[1], sx[2]}))));
            const int x1 = std::min(view.width - 1, static_cast<int>(std::ceil(std::max({sx[0], sx[1], sx[2]}))));
            const int y0 = std::max(0, static_cast<int>(std::floor(std::min({sy[0], sy[1], sy[2]}))));
            const int y1 = std::min(view.height - 1, static_cast<int>(std::ceil(std::max({sy[0], sy[1], sy[2]}))));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
                    const float w0 = ((sx[1] - px) * (sy[2] - py) - (sy[1] - py) * (sx[2] - px)) / area;
                    const float w1 = ((sx[2] - px) * (sy[0] - py) - (sy[2] - py) * (sx[0] - px)) / area;
                    const float w2 = 1 - w0 - w1;
                    if (w0 < -1e-5f || w1 < -1e-5f || w2 < -1e-5f) continue;
                    const float u = w0 * piece.uvs[ia * 2] + w1 * piece.uvs[ib * 2] + w2 * piece.uvs[ic * 2];
                    const float v = w0 * piece.uvs[ia * 2 + 1] + w1 * piece.uvs[ib * 2 + 1] + w2 * piece.uvs[ic * 2 + 1];
                    const int tx = std::clamp(static_cast<int>(u * static_cast<float>(texture->width)), 0, texture->width - 1);
                    const int ty = std::clamp(static_cast<int>(v * static_cast<float>(texture->height)), 0, texture->height - 1);
                    const int texel = ty * texture->width + tx;
                    const uint32_t color = texture->texels[static_cast<size_t>(texel)];
                    if ((color >> 24) <= 127) continue;
                    fn(static_cast<int>(i), y * view.width + x, texel, color);
                }
        }
    }
}

// The top piece and its colour at every pixel.
struct Frame {
    std::vector<int> piece;
    std::vector<int> texel;
    std::vector<uint32_t> color;
};

inline Frame Draw(const View& view, const Shown& shown) {
    Frame f;
    const size_t n = static_cast<size_t>(view.width) * static_cast<size_t>(view.height);
    f.piece.assign(n, -1);
    f.texel.assign(n, 0);
    f.color.assign(n, 0);
    Rasterize(view, shown, [&](int piece, int pixel, int texel, uint32_t color) {
        f.piece[static_cast<size_t>(pixel)] = piece;
        f.texel[static_cast<size_t>(pixel)] = texel;
        f.color[static_cast<size_t>(pixel)] = color;
    });
    return f;
}

// Paints a frame into an image region: the figure over a dark checkerboard.
inline void Paint(Image& image, int ox, int oy, const View& view, const Frame& frame) {
    for (int y = 0; y < view.height; ++y)
        for (int x = 0; x < view.width; ++x) {
            const uint32_t c = frame.color[static_cast<size_t>(y * view.width + x)];
            const uint8_t back = ((x / 8 + y / 8) % 2) ? 46 : 56;
            if (frame.piece[static_cast<size_t>(y * view.width + x)] < 0) FillRect(image, ox + x, oy + y, 1, 1, {back, back, static_cast<uint8_t>(back + 8), 255});
            else FillRect(image, ox + x, oy + y, 1, 1, {static_cast<uint8_t>(c & 255), static_cast<uint8_t>((c >> 8) & 255), static_cast<uint8_t>((c >> 16) & 255), 255});
        }
}

}  // namespace oe::raster2d
