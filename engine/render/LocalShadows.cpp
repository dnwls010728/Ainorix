#include "render/RenderScene.h"

#include <algorithm>
#include <cmath>

namespace oe {

int PointShadowFace(const Vec3& d) {
    const float x = std::fabs(d.x), y = std::fabs(d.y), z = std::fabs(d.z);
    if (x >= y && x >= z) return d.x >= 0 ? 0 : 1;
    if (y >= z) return d.y >= 0 ? 2 : 3;
    return d.z >= 0 ? 4 : 5;
}

int LocalShadowSlot(const LocalShadowSet& shadows, bool point, size_t lightIndex) {
    for (size_t i = 0; i < shadows.lights.size(); ++i) {
        const auto& light = shadows.lights[i];
        if (light.point == point && light.lightIndex == lightIndex) return static_cast<int>(i);
    }
    return -1;
}

LocalShadowSet SelectLocalShadows(const RenderLights& lights, const Mat4& viewProj, const Vec3& eye) {
    struct Candidate {
        RenderLocalShadow light;
        float range = 0, distance2 = 0, outerCos = 1;
        Vec3 dir;
    };
    std::vector<Candidate> candidates;
    auto add = [&](EntityId id, bool point, size_t index, Vec3 pos, Color color, float range, bool shadows, Vec3 dir, float outerCos) {
        if (!shadows || !std::isfinite(range) || range <= 0 || !std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) return;
        if (!(color.r > 0 || color.g > 0 || color.b > 0)) return;
        const Vec3 half(range, range, range);
        if (!BoxInFrustum(viewProj, pos - half, pos + half)) return;
        const float distance2 = Dot(pos - eye, pos - eye);
        if (!std::isfinite(distance2)) return;
        candidates.push_back({{id, point, index, pos, 0, point ? 6 : 1}, range, distance2, outerCos, dir});
    };
    for (size_t i = 0; i < std::min<size_t>(16, lights.points.size()); ++i) {
        const auto& light = lights.points[i];
        add(light.id, true, i, light.pos, light.color, light.range, light.shadows, Vec3(), 1);
    }
    for (size_t i = 0; i < lights.spots.size(); ++i) {
        const auto& light = lights.spots[i];
        add(light.id, false, i, light.pos, light.color, light.range, light.shadows, light.dir, light.outerCos);
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.distance2 != b.distance2) return a.distance2 < b.distance2;
        if (a.light.id != b.light.id) return a.light.id < b.light.id;
        return a.light.point && !b.light.point;
    });
    LocalShadowSet result;
    const Vec3 faceDirs[] = {Vec3(1, 0, 0), Vec3(-1, 0, 0), Vec3(0, 1, 0), Vec3(0, -1, 0), Vec3(0, 0, 1), Vec3(0, 0, -1)};
    for (const auto& candidate : candidates) {
        if (result.lights.size() == static_cast<size_t>(kMaxLocalShadowLights)) break;
        RenderLocalShadow light = candidate.light;
        light.firstMap = static_cast<int>(result.maps.size());
        const float nearPlane = std::max(0.001f, std::min(0.1f, candidate.range * 0.01f));
        const float farPlane = std::max(nearPlane * 1.01f, candidate.range);
        const float halfAngle = light.point ? kPi * 0.25f : std::acos(Clamp(candidate.outerCos, -1.0f, 1.0f));
        const Mat4 projection = Mat4::Perspective(halfAngle * 2, 1, nearPlane, farPlane);
        const float texelScale = 2 * std::tan(halfAngle) / static_cast<float>(kLocalShadowTileSize);
        for (int face = 0; face < light.mapCount; ++face) {
            const Vec3 dir = light.point ? faceDirs[face] : candidate.dir;
            const Vec3 up = std::fabs(dir.y) > 0.95f ? Vec3(0, 0, 1) : Vec3(0, 1, 0);
            const Mat4 view = Mat4::LookAt(light.pos, light.pos + dir, up);
            result.maps.push_back({projection * view, texelScale, nearPlane, farPlane});
        }
        result.lights.push_back(light);
    }
    return result;
}

LocalShadowSample PrepareLocalShadowSample(const LocalShadowSet& shadows, size_t slot, const Vec3& position, const Vec3& normal) {
    const auto& light = shadows.lights[slot];
    const Vec3 direction = position - light.pos;
    int face = light.point ? PointShadowFace(direction) : 0;
    const auto& map = shadows.maps[static_cast<size_t>(light.firstMap + face)];
    const float depth = std::max(map.nearPlane, (map.viewProj * Vec4(position, 1)).w);
    const Vec3 displaced = position + normal * (map.texelScale * depth * 1.5f);
    if (light.point) face = PointShadowFace(displaced - light.pos);
    const float bias = map.nearPlane * map.farPlane / (map.farPlane - map.nearPlane) * map.texelScale * 0.5f / depth;
    return {light.firstMap + face, displaced, bias};
}

}  // namespace oe
