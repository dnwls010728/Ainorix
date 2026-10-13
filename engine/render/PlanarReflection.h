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

}  // namespace oe
