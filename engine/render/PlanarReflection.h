#pragma once
#include "render/Renderer.h"
#include "render/RenderScene.h"

namespace oe {

// One front-facing visible mirror, nearest plane origin then entity id. Invalid/inactive surfaces are skipped.
struct RenderPlanarReflection {
    EntityId id = kNullEntity;
    Vec3 position, normal;           // World-space plane origin and unit normal.
    float strength = 0;
    int resolution = 256;           // Square capture, clamped 64..1024.
    RenderView view;                // Mirrored camera with an oblique near plane; no post effects/overlays.
};
RenderPlanarReflection SelectPlanarReflection(const Scene& scene, const std::vector<RenderItem>& items, const RenderView& view);

// The projection a sky background derives its view rays from. A planar reflection's projection has
// its depth row bent onto the mirror plane, so its far points can lie behind the eye and the usual
// far-minus-near ray points the wrong way; any ordinary depth row gives the same rays.
inline Mat4 SkyRayProjection(const RenderView& view) {
    Mat4 proj = view.proj;
    if (!view.reflectionCapture) return proj;
    const bool ortho = proj.at(3, 3) == 1.0f;
    proj.at(2, 0) = 0; proj.at(2, 1) = 0;
    proj.at(2, 2) = ortho ? -0.02f : -1.002f;
    proj.at(2, 3) = ortho ? -1.002f : -0.2002f;
    return proj;
}

}  // namespace oe
