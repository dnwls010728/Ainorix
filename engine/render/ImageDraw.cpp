#include "render/ImageDraw.h"

#include <algorithm>
#include <cmath>

#include "core/Math.h"
#include "render/Font.h"

namespace oe {

void BlendPixel(Image& image, int x, int y, Rgba8 c) {
    if (x < 0 || y < 0 || x >= image.width || y >= image.height || c.a == 0) return;
    uint8_t* p = &image.rgba[(static_cast<size_t>(y) * static_cast<size_t>(image.width) + static_cast<size_t>(x)) * 4];
    const unsigned a = c.a, keep = 255 - a;
    p[0] = static_cast<uint8_t>((c.r * a + p[0] * keep + 127) / 255);
    p[1] = static_cast<uint8_t>((c.g * a + p[1] * keep + 127) / 255);
    p[2] = static_cast<uint8_t>((c.b * a + p[2] * keep + 127) / 255);
    p[3] = static_cast<uint8_t>(std::min(255u, a + p[3] * keep / 255));
}

void FillRect(Image& image, int x, int y, int w, int h, Rgba8 color) {
    const int x0 = std::max(0, x), y0 = std::max(0, y);
    const int x1 = std::min(image.width, x + w), y1 = std::min(image.height, y + h);
    for (int yy = y0; yy < y1; ++yy)
        for (int xx = x0; xx < x1; ++xx) BlendPixel(image, xx, yy, color);
}

void DrawLine(Image& image, float x0, float y0, float x1, float y1, Rgba8 color, float thickness) {
    // Every pixel whose centre lies within half the thickness of the segment, each touched once.
    const float r = std::max(0.5f, thickness * 0.5f);
    const int minX = static_cast<int>(std::floor(std::min(x0, x1) - r)), maxX = static_cast<int>(std::ceil(std::max(x0, x1) + r));
    const int minY = static_cast<int>(std::floor(std::min(y0, y1) - r)), maxY = static_cast<int>(std::ceil(std::max(y0, y1) + r));
    const float dx = x1 - x0, dy = y1 - y0, length2 = dx * dx + dy * dy;
    for (int y = std::max(0, minY); y <= std::min(image.height - 1, maxY); ++y)
        for (int x = std::max(0, minX); x <= std::min(image.width - 1, maxX); ++x) {
            const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
            const float t = length2 > 0 ? Clamp(((px - x0) * dx + (py - y0) * dy) / length2, 0.0f, 1.0f) : 0;
            const float ex = x0 + dx * t - px, ey = y0 + dy * t - py;
            if (ex * ex + ey * ey <= r * r) BlendPixel(image, x, y, color);
        }
}

void DrawCircle(Image& image, float cx, float cy, float radius, Rgba8 color, bool filled) {
    const int minX = static_cast<int>(std::floor(cx - radius - 1)), maxX = static_cast<int>(std::ceil(cx + radius + 1));
    const int minY = static_cast<int>(std::floor(cy - radius - 1)), maxY = static_cast<int>(std::ceil(cy + radius + 1));
    for (int y = std::max(0, minY); y <= std::min(image.height - 1, maxY); ++y)
        for (int x = std::max(0, minX); x <= std::min(image.width - 1, maxX); ++x) {
            const float ex = static_cast<float>(x) + 0.5f - cx, ey = static_cast<float>(y) + 0.5f - cy;
            const float d = std::sqrt(ex * ex + ey * ey);
            if (filled ? d <= radius : std::fabs(d - radius) <= 0.6f) BlendPixel(image, x, y, color);
        }
}

int TextWidth(const std::string& text, int scale) {
    return text.empty() ? 0 : static_cast<int>(text.size()) * 6 * scale - scale;
}

int DrawText(Image& image, int x, int y, const std::string& text, Rgba8 color, int scale, bool outline) {
    auto glyphs = [&](int ox, int oy, Rgba8 c) {
        int cx = x + ox;
        for (char ch : text) {
            const uint8_t* columns = PixelFontGlyph(static_cast<unsigned char>(ch));
            for (int col = 0; col < 5; ++col)
                for (int row = 0; row < 7; ++row)
                    if (columns[col] & (1 << row)) FillRect(image, cx + col * scale, y + oy + row * scale, scale, scale, c);
            cx += 6 * scale;
        }
    };
    if (outline) {
        const Rgba8 dark{0, 0, 0, static_cast<uint8_t>(std::min<int>(255, color.a))};
        for (int oy = -1; oy <= 1; ++oy)
            for (int ox = -1; ox <= 1; ++ox)
                if (ox || oy) glyphs(ox, oy, dark);
    }
    glyphs(0, 0, color);
    return TextWidth(text, scale);
}

}  // namespace oe
