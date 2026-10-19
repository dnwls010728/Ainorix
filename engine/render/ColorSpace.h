#pragma once
#include <algorithm>
#include <cmath>

#include "core/Math.h"

namespace oe {

// Linear lighting (project.json `"rendering": {"colorSpace": "linear"}`, docs/RENDERING.md "Color space"):
// authored colors and color textures are sRGB-encoded; they are decoded to linear light before lighting,
// and the lit image is encoded back after tone mapping. The default "gamma" lights the stored numbers as
// they are. Must match SrgbToLinear/LinearToSrgb in Shaders.glsl.
inline float SrgbToLinear(float c) {
    c = std::max(0.0f, c);
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
inline float LinearToSrgb(float c) {
    c = Clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}
inline Color SrgbToLinear(const Color& c) { return Color(SrgbToLinear(c.r), SrgbToLinear(c.g), SrgbToLinear(c.b)); }
inline Color LinearToSrgb(const Color& c) { return Color(LinearToSrgb(c.r), LinearToSrgb(c.g), LinearToSrgb(c.b)); }
// An authored color as the renderers light it: decoded with linear lighting, unchanged otherwise.
inline Color SceneColor(const Color& c, bool linear) { return linear ? SrgbToLinear(c) : c; }
inline Vec4 SceneColor(const Vec4& c, bool linear) {
    return linear ? Vec4(SrgbToLinear(c.x), SrgbToLinear(c.y), SrgbToLinear(c.z), c.w) : c;
}

}  // namespace oe
