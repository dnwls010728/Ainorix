#pragma once
#include <cmath>

#include "scene/Components.h"

namespace oe {

// Render callers may construct settings directly, bypassing reflection's range clamping.
inline PostProcess NormalizePostProcess(PostProcess settings) {
    settings.vignette = std::isfinite(settings.vignette) ? Clamp(settings.vignette, 0, 1) : 0;
    settings.vignetteRadius = std::isfinite(settings.vignetteRadius) ? Clamp(settings.vignetteRadius, 0, 1.5f) : 0.75f;
    settings.vignetteSoftness = std::isfinite(settings.vignetteSoftness) ? Clamp(settings.vignetteSoftness, 0.01f, 2) : 0.5f;
    return settings;
}

// Must match composite_fs: normalized pixel-center UV, cubic smoothstep, no aspect correction.
inline float VignetteFactor(const PostProcess& settings, float u, float v) {
    float x = u * 2 - 1, y = v * 2 - 1;
    float t = Clamp((std::sqrt(x * x + y * y) - settings.vignetteRadius) / settings.vignetteSoftness, 0, 1);
    return 1 - settings.vignette * t * t * (3 - 2 * t);
}

}  // namespace oe
