#include "render/PlanarReflection.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace oe {
namespace {
bool Finite(const Vec3& p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
float Dot4(const Vec4& a, const Vec4& b) { return a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w; }
}

RenderPlanarReflection SelectPlanarReflection(const Scene& scene, const std::vector<RenderItem>& items, const RenderView& view) {
    RenderPlanarReflection selected;
    if (view.reflectionCapture || view.sceneColor) return selected;
    float nearest = std::numeric_limits<float>::infinity();
    for (const RenderItem& item : items) {
        const PlanarReflection* mirror = scene.Get<PlanarReflection>(item.id);
        if (!mirror || !scene.Get<MeshRenderer>(item.id) || !mirror->enabled || !std::isfinite(mirror->strength) || mirror->strength <= 0) continue;
        Vec3 lo, hi;
        ItemWorldBounds(item, Vec3(), Vec3(), lo, hi);
        if (!BoxInFrustum(view.proj * view.view, lo, hi)) continue;
        const Vec3 position = item.world.TransformPoint(mirror->offset);
        Vec3 normal = item.normalMatrix.TransformDir(mirror->normal);
        const float normalLength = Length(normal);
        if (!Finite(position) || !Finite(normal) || !std::isfinite(normalLength) || normalLength < 1e-6f) continue;
        normal = Normalize(normal);
        const float bias = std::isfinite(mirror->clipBias) ? Clamp(mirror->clipBias, 0.001f, 0.1f) : 0.01f;
        if (Dot(normal, view.eye - position) <= bias) continue;  // One-sided mirror, no grazing/singular camera.
        const float distance = Dot(position - view.eye, position - view.eye);
        if (!std::isfinite(distance) || distance > nearest || (distance == nearest && item.id >= selected.id)) continue;
        const float d = -Dot(normal, position);
        Mat4 reflection;
        const float n[3] = {normal.x, normal.y, normal.z};
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) reflection.at(row,col) -= 2*n[row]*n[col];
            reflection.at(row,3) = -2*d*n[row];
        }
        RenderView reflected = view;
        reflected.view = view.view * reflection;
        reflected.eye = reflection.TransformPoint(view.eye);
        // Reflection reverses handedness. Flip projection X as well so ordinary front-face culling remains valid.
        for (int col = 0; col < 4; ++col) reflected.proj.at(0,col) = -reflected.proj.at(0,col);
        const Vec4 plane = reflected.view.Inverse().Transposed() * Vec4(normal, d - bias);
        const Vec4 corner = reflected.proj.Inverse() * Vec4(plane.x >= 0 ? 1.0f : -1.0f, plane.y >= 0 ? 1.0f : -1.0f, 1, 1);
        const float denominator = Dot4(plane, corner);
        if (!std::isfinite(denominator) || denominator <= 1e-5f) continue;
        const Vec4 clip = plane * (2 / denominator);
        const float c[4] = {clip.x,clip.y,clip.z,clip.w};
        for (int col = 0; col < 4; ++col) reflected.proj.at(2,col) = c[col] - reflected.proj.at(3,col);
        reflected.reflectionCapture = true;
        reflected.excludeEntity = item.id;
        reflected.sceneColor = nullptr;
        reflected.postProcess = PostProcess();
        reflected.drawUI = false; reflected.drawGrid = false;
        reflected.highlight = kNullEntity; reflected.lines.clear();
        nearest = distance;
        selected.id = item.id; selected.position = position; selected.normal = normal;
        selected.strength = Clamp(mirror->strength, 0, 1);
        selected.resolution = std::clamp(mirror->resolution, 64, 1024);
        selected.view = reflected;
    }
    return selected;
}
}  // namespace oe
