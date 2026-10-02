#pragma once
#include <algorithm>
#include <cmath>

#include "scene/Components.h"

namespace oe {

// Render callers may construct settings directly, bypassing reflection's range clamping.
inline PostProcess NormalizePostProcess(PostProcess settings) {
    settings.exposure = std::isfinite(settings.exposure) ? Clamp(settings.exposure, 0, 32) : 1;
    if (settings.toneMapping != "reinhard") settings.toneMapping = "none";
    settings.bloom = std::isfinite(settings.bloom) ? Clamp(settings.bloom, 0, 4) : 0;
    settings.bloomThreshold = std::isfinite(settings.bloomThreshold) ? Clamp(settings.bloomThreshold, 0, 32) : 1;
    settings.bloomRadius = std::clamp(settings.bloomRadius, 1, 32);
    settings.vignette = std::isfinite(settings.vignette) ? Clamp(settings.vignette, 0, 1) : 0;
    settings.vignetteRadius = std::isfinite(settings.vignetteRadius) ? Clamp(settings.vignetteRadius, 0, 1.5f) : 0.75f;
    settings.vignetteSoftness = std::isfinite(settings.vignetteSoftness) ? Clamp(settings.vignetteSoftness, 0.01f, 2) : 0.5f;
    return settings;
}

// HDR is allocated only for effects that need unquantized scene lighting.
inline bool UsesHdr(const PostProcess& settings) {
    return settings.exposure != 1 || settings.toneMapping != "none" || settings.bloom > 0;
}

// Extract radiance above the brightest-channel threshold without shifting hue.
inline Color BloomHighlight(Color c, float threshold) {
    const float peak = std::max({c.r, c.g, c.b});
    return peak > threshold ? c * ((peak - threshold) / peak) : Color(0, 0, 0);
}

// Float16's largest finite value is the shared HDR bound on CPU and GPU.
inline Color ClampHdr(Color c) {
    return Color(Clamp(c.r, 0, 65504), Clamp(c.g, 0, 65504), Clamp(c.b, 0, 65504));
}

inline Color ToneMap(Color c, const PostProcess& settings) {
    c = ClampHdr(c) * settings.exposure;
    if (settings.toneMapping == "reinhard") {
        c = Color(c.r / (1 + c.r), c.g / (1 + c.g), c.b / (1 + c.b));
    }
    return Color(Clamp(c.r, 0, 1), Clamp(c.g, 0, 1), Clamp(c.b, 0, 1));
}

// Must match composite_fs: normalized pixel-center UV, cubic smoothstep, no aspect correction.
inline float VignetteFactor(const PostProcess& settings, float u, float v) {
    float x = u * 2 - 1, y = v * 2 - 1;
    float t = Clamp((std::sqrt(x * x + y * y) - settings.vignetteRadius) / settings.vignetteSoftness, 0, 1);
    return 1 - settings.vignette * t * t * (3 - 2 * t);
}

}  // namespace oe
