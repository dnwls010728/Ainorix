#pragma once
#include <vector>

#include "core/Math.h"
#include "render/PostProcess.h"

namespace oe {

// Per-pixel opaque shading inputs. Missing hits leave the original reflected ambient unchanged.
struct ScreenReflectionPixel {
    Vec3 normal;
    Color weight{0, 0, 0};
    Color fallback{0, 0, 0};
    float roughness = 1;
};

// Fixed 64-step trace and five bisections, mirrored by ssr_fs. No history, random jitter or recursion.
inline float TraceScreenReflection(const Vec3& origin, const Vec3& direction, const Mat4& view,
                                   const Mat4& projection, const PostProcess& post, const std::vector<float>& depth,
                                   int width, int height, int sourceX, int sourceY, int& hitX, int& hitY) {
    const bool ortho = projection.at(3, 3) == 1;
    auto gapAt = [&](float distance, float& gap, float& u, float& v) {
        const Vec3 point = origin + direction * distance;
        const Vec4 clip = projection * view * Vec4(point, 1);
        if (clip.w <= 1e-6f) return false;
        u = clip.x / clip.w * 0.5f + 0.5f; v = 0.5f - clip.y / clip.w * 0.5f;
        if (u <= 0 || v <= 0 || u >= 1 || v >= 1 || clip.z < -clip.w || clip.z > clip.w) return false;
        hitX = static_cast<int>(u * width); hitY = static_cast<int>(v * height);
        const float z = depth[static_cast<size_t>(hitY) * width + hitX];
        gap = z >= 1 || (hitX == sourceX && hitY == sourceY) ? -1e6f :
              -(view * Vec4(point, 1)).z - DofViewDepth(z, projection.at(2, 2), projection.at(2, 3), ortho);
        return true;
    };
    float previous = 0, previousGap = -1e6f;
    for (int step = 1; step <= 64; ++step) {
        const float distance = post.ssrDistance * static_cast<float>(step) / 64;
        float gap, u, v;
        if (!gapAt(distance, gap, u, v)) return 0;
        if (gap >= 0 && previousGap < 0) {
            float lo = previous, hi = distance;
            for (int refine = 0; refine < 5; ++refine) {
                const float mid = (lo + hi) * 0.5f;
                if (!gapAt(mid, gap, u, v)) return 0;
                if (gap >= 0) hi = mid; else lo = mid;
            }
            if (!gapAt(hi, gap, u, v) || gap < 0 || gap > post.ssrThickness) return 0;
            const float edge = std::min({u, v, 1 - u, 1 - v});
            return Clamp(edge / 0.05f, 0, 1) * (1 - hi / post.ssrDistance);
        }
        previous = distance; previousGap = gap;
    }
    return 0;
}

}  // namespace oe
