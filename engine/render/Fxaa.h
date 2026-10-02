#pragma once
#include <algorithm>
#include <cmath>

#include "core/Math.h"

namespace oe {

// Compact directional FXAA on display RGB. Sample accepts pixel-center coordinates
// and bilinearly samples with clamped edges. Must match composite_fs.
template<class Sample> Color FxaaPixel(float x, float y, const Sample& sample) {
    auto luma = [](Color c) { return c.r * 0.299f + c.g * 0.587f + c.b * 0.114f; };
    const Color center = sample(x, y);
    const float m = luma(center), nw = luma(sample(x - 1, y - 1)), ne = luma(sample(x + 1, y - 1));
    const float sw = luma(sample(x - 1, y + 1)), se = luma(sample(x + 1, y + 1));
    const float lo = std::min({m, nw, ne, sw, se}), hi = std::max({m, nw, ne, sw, se});
    if (hi - lo < std::max(1.0f / 32, hi * 0.125f)) return center;
    float dx = -((nw + ne) - (sw + se)), dy = (nw + sw) - (ne + se);
    const float reduce = std::max((nw + ne + sw + se) * (0.25f * 0.125f), 1.0f / 128);
    const float scale = 1 / (std::min(std::fabs(dx), std::fabs(dy)) + reduce);
    dx = Clamp(dx * scale, -8, 8);
    dy = Clamp(dy * scale, -8, 8);
    const Color a = (sample(x - dx / 6, y - dy / 6) + sample(x + dx / 6, y + dy / 6)) * 0.5f;
    const Color b = a * 0.5f + (sample(x - dx * 0.5f, y - dy * 0.5f) + sample(x + dx * 0.5f, y + dy * 0.5f)) * 0.25f;
    const float lb = luma(b);
    return lb < lo || lb > hi ? a : b;
}

}  // namespace oe
