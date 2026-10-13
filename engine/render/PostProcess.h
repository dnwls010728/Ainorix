#pragma once
#include <algorithm>
#include <cmath>

#include "scene/Components.h"

namespace oe {

constexpr int kMaxDofRadius = 16;

// Render callers may construct settings directly, bypassing reflection's range clamping.
inline PostProcess NormalizePostProcess(PostProcess settings) {
    settings.ssr = std::isfinite(settings.ssr) ? Clamp(settings.ssr, 0, 1) : 0;
    settings.ssrDistance = std::isfinite(settings.ssrDistance) ? Clamp(settings.ssrDistance, 0.1f, 1000) : 30;
    settings.ssrThickness = std::isfinite(settings.ssrThickness) ? Clamp(settings.ssrThickness, 0.001f, 10) : 0.2f;
    settings.ssao = std::isfinite(settings.ssao) ? Clamp(settings.ssao, 0, 1) : 0;
    settings.ssaoRadius = std::isfinite(settings.ssaoRadius) ? Clamp(settings.ssaoRadius, 0.01f, 10) : 1;
    settings.ssaoBias = std::isfinite(settings.ssaoBias) ? Clamp(settings.ssaoBias, 0, 1) : 0.02f;
    settings.exposure = std::isfinite(settings.exposure) ? Clamp(settings.exposure, 0, 32) : 1;
    if (settings.toneMapping != "reinhard" && settings.toneMapping != "aces") settings.toneMapping = "none";
    settings.bloom = std::isfinite(settings.bloom) ? Clamp(settings.bloom, 0, 4) : 0;
    settings.bloomThreshold = std::isfinite(settings.bloomThreshold) ? Clamp(settings.bloomThreshold, 0, 32) : 1;
    settings.bloomRadius = std::clamp(settings.bloomRadius, 1, 32);
    settings.vignette = std::isfinite(settings.vignette) ? Clamp(settings.vignette, 0, 1) : 0;
    settings.vignetteRadius = std::isfinite(settings.vignetteRadius) ? Clamp(settings.vignetteRadius, 0, 1.5f) : 0.75f;
    settings.vignetteSoftness = std::isfinite(settings.vignetteSoftness) ? Clamp(settings.vignetteSoftness, 0.01f, 2) : 0.5f;
    settings.dofRadius = std::clamp(settings.dofRadius, 0, kMaxDofRadius);
    settings.dofFocus = std::isfinite(settings.dofFocus) ? Clamp(settings.dofFocus, 0, 100000) : 10;
    settings.dofRange = std::isfinite(settings.dofRange) ? Clamp(settings.dofRange, 0, 100000) : 2;
    settings.dofFalloff = std::isfinite(settings.dofFalloff) ? Clamp(settings.dofFalloff, 0.01f, 100000) : 8;
    settings.fog = std::isfinite(settings.fog) ? Clamp(settings.fog, 0, 1) : 0;
    settings.fogStart = std::isfinite(settings.fogStart) ? Clamp(settings.fogStart, 0, 100000) : 20;
    settings.fogEnd = std::isfinite(settings.fogEnd) ? Clamp(settings.fogEnd, settings.fogStart + 0.01f, 100000.01f) : settings.fogStart + 60;
    settings.saturation = std::isfinite(settings.saturation) ? Clamp(settings.saturation, 0, 4) : 1;
    settings.contrast = std::isfinite(settings.contrast) ? Clamp(settings.contrast, 0, 4) : 1;
    return settings;
}

// HDR is allocated only for effects that need unquantized scene lighting.
inline bool UsesHdr(const PostProcess& settings) {
    return settings.ssr > 0 || settings.exposure != 1 || settings.toneMapping != "none" || settings.bloom > 0;
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
    } else if (settings.toneMapping == "aces") {
        auto aces = [](float x) { return x * (2.51f * x + 0.03f) / (x * (2.43f * x + 0.59f) + 0.14f); };
        c = Color(aces(c.r), aces(c.g), aces(c.b));
    }
    return Color(Clamp(c.r, 0, 1), Clamp(c.g, 0, 1), Clamp(c.b, 0, 1));
}

// Color grading runs only when one of its settings differs from neutral, so default frames are unchanged.
inline bool UsesGrading(const PostProcess& s) {
    const Color& a = s.shadowTint;
    const Color& b = s.highlightTint;
    return s.saturation != 1 || s.contrast != 1 || a.r != 1 || a.g != 1 || a.b != 1 || b.r != 1 || b.g != 1 || b.b != 1;
}

// Must match Grade in composite_fs: split toning by luminance, saturation, contrast around 0.5, on display color.
inline Color Grade(Color c, const PostProcess& s) {
    auto luma = [](const Color& x) { return x.r * 0.2126f + x.g * 0.7152f + x.b * 0.0722f; };
    float t = Clamp(luma(c) / 0.9f, 0, 1);
    t = t * t * (3 - 2 * t);
    c = c * (s.shadowTint * (1 - t) + s.highlightTint * t);
    const float l = luma(c);
    c = Color(l + (c.r - l) * s.saturation, l + (c.g - l) * s.saturation, l + (c.b - l) * s.saturation);
    c = Color((c.r - 0.5f) * s.contrast + 0.5f, (c.g - 0.5f) * s.contrast + 0.5f, (c.b - 0.5f) * s.contrast + 0.5f);
    return Color(Clamp(c.r, 0, 1), Clamp(c.g, 0, 1), Clamp(c.b, 0, 1));
}

// Fraction of fogColor at `distance` meters from the eye (0 when fog is off). Must match mesh_fs.
inline float FogAmount(const PostProcess& s, float distance) {
    if (s.fog <= 0) return 0;
    return s.fog * Clamp((distance - s.fogStart) / (s.fogEnd - s.fogStart), 0, 1);
}

// Must match composite_fs: normalized pixel-center UV, cubic smoothstep, no aspect correction.
inline float VignetteFactor(const PostProcess& settings, float u, float v) {
    float x = u * 2 - 1, y = v * 2 - 1;
    float t = Clamp((std::sqrt(x * x + y * y) - settings.vignetteRadius) / settings.vignetteSoftness, 0, 1);
    return 1 - settings.vignette * t * t * (3 - 2 * t);
}

// Depth of field. Must match dof_fs. `projA`/`projB` are the projection's z row (elements (2,2) and
// (2,3)); `depth01` is the depth buffer value (NDC z * 0.5 + 0.5, 1 = nothing drawn).
inline float DofViewDepth(float depth01, float projA, float projB, bool orthographic) {
    const float ndc = depth01 * 2 - 1;
    return orthographic ? (projB - ndc) / projA : projB / (ndc + projA);
}

// Blur radius in pixels (circle of confusion) of a surface at view depth `depth`.
inline float DofCoc(const PostProcess& settings, float depth) {
    return static_cast<float>(settings.dofRadius) *
           Clamp((std::fabs(depth - settings.dofFocus) - settings.dofRange) / settings.dofFalloff, 0, 1);
}

// Weight of a sample `distance` pixels from the pixel being filtered. A sample spreads over its own
// blur radius; one behind the filtered pixel spreads no further than that pixel's radius, so a blurred
// background does not bleed over a sharp surface in front of it.
inline float DofWeight(float centerCoc, float centerDepth, float sampleCoc, float sampleDepth, int distance) {
    const float coc = sampleDepth > centerDepth ? std::min(sampleCoc, centerCoc) : sampleCoc;
    return Clamp(coc - static_cast<float>(distance) + 1, 0, 1);
}

}  // namespace oe
