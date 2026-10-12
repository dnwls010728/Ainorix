#include "render/Environment.h"

#include <algorithm>
#include <cmath>

namespace oe {

namespace {

using Level = EnvironmentMap::Level;

// Area (box) resample of one axis: every output texel averages the input span it covers.
void ResampleAxis(const std::vector<float>& src, int sw, int sh, std::vector<float>& dst, int dw, int dh, bool horizontal) {
    dst.assign(static_cast<size_t>(dw) * static_cast<size_t>(dh) * 3, 0.0f);
    const int sn = horizontal ? sw : sh, dn = horizontal ? dw : dh;
    const int lines = horizontal ? sh : sw;
    const double scale = static_cast<double>(sn) / static_cast<double>(dn);
    for (int d = 0; d < dn; ++d) {
        const double lo = d * scale, hi = (d + 1) * scale;
        const int first = static_cast<int>(std::floor(lo)), last = std::min(sn - 1, static_cast<int>(std::ceil(hi)) - 1);
        for (int line = 0; line < lines; ++line) {
            double sum[3] = {0, 0, 0}, total = 0;
            for (int s = first; s <= last; ++s) {
                const double cover = std::min(hi, static_cast<double>(s + 1)) - std::max(lo, static_cast<double>(s));
                if (cover <= 0) continue;
                const size_t at = horizontal ? (static_cast<size_t>(line) * sw + s) * 3 : (static_cast<size_t>(s) * sw + line) * 3;
                for (int c = 0; c < 3; ++c) sum[c] += src[at + c] * cover;
                total += cover;
            }
            const size_t out = horizontal ? (static_cast<size_t>(line) * dw + d) * 3 : (static_cast<size_t>(d) * dw + line) * 3;
            for (int c = 0; c < 3; ++c) dst[out + c] = total > 0 ? static_cast<float>(sum[c] / total) : 0.0f;
        }
    }
}

// The next level: half the size, blurred by a Gaussian lobe of `sigma` radians (on top of what the
// source level already has). Separable: along each row in angle (wider in texels toward the poles,
// where a row covers less of the sphere), then down the columns.
Level BlurHalf(const Level& src, float sigma) {
    Level out;
    out.width = std::max(1, src.width / 2);
    out.height = std::max(1, src.height / 2);
    const int sw = src.width, sh = src.height, dw = out.width, dh = out.height;
    const float texelU = 2.0f * kPi / static_cast<float>(sw), texelV = kPi / static_cast<float>(sh);
    const float inv2s2 = 1.0f / (2.0f * sigma * sigma);

    // Rows: dw x sh.
    std::vector<float> rows(static_cast<size_t>(dw) * static_cast<size_t>(sh) * 3);
    std::vector<float> kernel;
    for (int y = 0; y < sh; ++y) {
        const float sinTheta = std::max(1e-4f, std::sin((static_cast<float>(y) + 0.5f) * texelV));
        const float step = texelU * sinTheta;  // angle between neighbouring texels of this row
        const int radius = std::min(sw / 2, std::max(1, static_cast<int>(std::ceil(3.0f * sigma / step))));
        kernel.resize(static_cast<size_t>(radius) * 2);
        float total = 0;
        for (int o = -radius + 1; o <= radius; ++o) {  // output texel center lies between source texels 2i and 2i + 1
            const float d = (static_cast<float>(o) - 0.5f) * step;
            total += kernel[static_cast<size_t>(o + radius - 1)] = std::exp(-d * d * inv2s2);
        }
        for (float& k : kernel) k /= total;
        const float* line = &src.rgb[static_cast<size_t>(y) * sw * 3];
        for (int x = 0; x < dw; ++x) {
            float sum[3] = {0, 0, 0};
            for (int o = -radius + 1; o <= radius; ++o) {
                int sx = (2 * x + o) % sw;
                if (sx < 0) sx += sw;
                const float k = kernel[static_cast<size_t>(o + radius - 1)];
                const float* p = line + static_cast<size_t>(sx) * 3;
                sum[0] += p[0] * k; sum[1] += p[1] * k; sum[2] += p[2] * k;
            }
            float* q = &rows[(static_cast<size_t>(y) * dw + x) * 3];
            q[0] = sum[0]; q[1] = sum[1]; q[2] = sum[2];
        }
    }

    // Columns: dw x dh.
    out.rgb.resize(static_cast<size_t>(dw) * static_cast<size_t>(dh) * 3);
    const int radius = std::max(1, static_cast<int>(std::ceil(3.0f * sigma / texelV)));
    for (int y = 0; y < dh; ++y) {
        kernel.assign(static_cast<size_t>(radius) * 2, 0.0f);
        float total = 0;
        for (int o = -radius + 1; o <= radius; ++o) {
            const int sy = 2 * y + o;
            if (sy < 0 || sy >= sh) continue;  // past a pole: left out
            const float d = (static_cast<float>(o) - 0.5f) * texelV;
            total += kernel[static_cast<size_t>(o + radius - 1)] = std::exp(-d * d * inv2s2);
        }
        for (int x = 0; x < dw; ++x) {
            float sum[3] = {0, 0, 0};
            for (int o = -radius + 1; o <= radius; ++o) {
                const float k = kernel[static_cast<size_t>(o + radius - 1)];
                if (k == 0.0f) continue;
                const float* p = &rows[(static_cast<size_t>(2 * y + o) * dw + x) * 3];
                sum[0] += p[0] * k; sum[1] += p[1] * k; sum[2] += p[2] * k;
            }
            float* q = &out.rgb[(static_cast<size_t>(y) * dw + x) * 3];
            q[0] = sum[0] / total; q[1] = sum[1] / total; q[2] = sum[2] / total;
        }
    }
    return out;
}

Color Texel(const Level& l, int x, int y) {
    const float* p = &l.rgb[(static_cast<size_t>(y) * l.width + x) * 3];
    return Color(p[0], p[1], p[2]);
}

Color Bilinear(const Level& l, float u, float v) {
    const float x = u * static_cast<float>(l.width) - 0.5f, y = v * static_cast<float>(l.height) - 0.5f;
    const float fx0 = std::floor(x), fy0 = std::floor(y);
    const float fx = x - fx0, fy = y - fy0;
    int x0 = static_cast<int>(fx0) % l.width;
    if (x0 < 0) x0 += l.width;
    const int x1 = (x0 + 1) % l.width;
    const int y0 = std::min(l.height - 1, std::max(0, static_cast<int>(fy0)));
    const int y1 = std::min(l.height - 1, std::max(0, static_cast<int>(fy0) + 1));
    const Color top = Texel(l, x0, y0) * (1.0f - fx) + Texel(l, x1, y0) * fx;
    const Color bottom = Texel(l, x0, y1) * (1.0f - fx) + Texel(l, x1, y1) * fx;
    return top * (1.0f - fy) + bottom * fy;
}

}  // namespace

Color EnvironmentMap::Sample(const Vec3& dir, float lod) const {
    if (levels.empty()) return Color(0, 0, 0);
    float u, v;
    EnvUV(dir, u, v);
    lod = Clamp(lod, 0.0f, MaxLod());
    const size_t l0 = static_cast<size_t>(lod);
    const size_t l1 = std::min(levels.size() - 1, l0 + 1);
    const float f = lod - static_cast<float>(l0);
    const Color a = Bilinear(levels[l0], u, v);
    if (f <= 0.0f || l1 == l0) return a;
    return a * (1.0f - f) + Bilinear(levels[l1], u, v) * f;
}

Color EnvironmentMap::Irradiance(const Vec3& normal) const {
    const Vec3 e = EnvIrradianceSH(sh, normal);
    return Color(std::max(0.0f, e.x), std::max(0.0f, e.y), std::max(0.0f, e.z));
}

std::shared_ptr<EnvironmentMap> BuildEnvironmentMap(const float* rgb, int width, int height) {
    if (!rgb || width <= 0 || height <= 0) return nullptr;
    auto map = std::make_shared<EnvironmentMap>();

    // Level 0: the image at a power-of-two size, twice as wide as high.
    int w = 8;
    while (w * 2 <= std::min(width, kEnvMaxWidth)) w *= 2;
    const int h = w / 2;
    Level base;
    base.width = w;
    base.height = h;
    {
        std::vector<float> src(rgb, rgb + static_cast<size_t>(width) * static_cast<size_t>(height) * 3);
        for (float& value : src) value = std::isfinite(value) ? Clamp(value, 0.0f, 65504.0f) : 0.0f;
        std::vector<float> wide;
        ResampleAxis(src, width, height, wide, w, height, true);
        ResampleAxis(wide, w, height, base.rgb, w, h, false);
    }
    map->levels.push_back(std::move(base));

    // Blurred levels down to 4 x 2: level k is what a surface of roughness k / (levels - 1) reflects.
    int count = 1;
    for (int size = w; size > 4; size /= 2) ++count;
    for (int k = 1; k < count; ++k) {
        const float now = EnvLobeSigma(static_cast<float>(k) / static_cast<float>(count - 1));
        const float before = EnvLobeSigma(static_cast<float>(k - 1) / static_cast<float>(count - 1));
        const Level& src = map->levels.back();
        const float texel = 2.0f * kPi / static_cast<float>(std::max(1, src.width / 2));
        // At least half an output texel, so halving the size never aliases.
        const float sigma = std::max(std::sqrt(std::max(0.0f, now * now - before * before)), 0.5f * texel);
        map->levels.push_back(BlurHalf(src, sigma));
    }

    // Irradiance: project a small level on nine spherical harmonics and weigh the bands for a
    // cosine lobe. The polynomial of EnvIrradianceSH evaluates it.
    const Level& small = map->levels[std::min<size_t>(map->levels.size() - 1, w > 512 ? 2 : (w > 256 ? 1 : 0))];
    double sums[9][3] = {};
    const double dTheta = kPi / small.height, dPhi = 2.0 * kPi / small.width;
    for (int y = 0; y < small.height; ++y) {
        const double theta = (y + 0.5) * dTheta, sinTheta = std::sin(theta), cosTheta = std::cos(theta);
        const double solid = sinTheta * dTheta * dPhi;
        for (int x = 0; x < small.width; ++x) {
            const double phi = ((x + 0.5) / small.width - 0.5) * 2.0 * kPi;
            const double dx = sinTheta * std::sin(phi), dy = cosTheta, dz = -sinTheta * std::cos(phi);
            const double basis[9] = {0.282095, 0.488603 * dy, 0.488603 * dz, 0.488603 * dx, 1.092548 * dx * dy,
                                     1.092548 * dy * dz, 0.315392 * (3.0 * dz * dz - 1.0), 1.092548 * dx * dz,
                                     0.546274 * (dx * dx - dy * dy)};
            const float* p = &small.rgb[(static_cast<size_t>(y) * small.width + x) * 3];
            for (int i = 0; i < 9; ++i)
                for (int c = 0; c < 3; ++c) sums[i][c] += p[c] * basis[i] * solid;
        }
    }
    // Band weight (pi, 2pi/3, pi/4) times the basis constant, over pi.
    static const double kScale[9] = {0.282095, 0.488603 * 2.0 / 3.0, 0.488603 * 2.0 / 3.0, 0.488603 * 2.0 / 3.0, 1.092548 * 0.25,
                                     1.092548 * 0.25, 0.315392 * 0.25, 1.092548 * 0.25, 0.546274 * 0.25};
    for (int i = 0; i < 9; ++i)
        map->sh[i] = Vec3(static_cast<float>(sums[i][0] * kScale[i]), static_cast<float>(sums[i][1] * kScale[i]),
                          static_cast<float>(sums[i][2] * kScale[i]));
    return map;
}

}  // namespace oe
