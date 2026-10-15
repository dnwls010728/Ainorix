#pragma once
#include <cstdint>
#include <string>

#include "core/Image.h"

namespace oe {

// Small deterministic drawing on an RGBA image, for the pictures tools hand to agents (coordinate
// grids, calibration marks, rig overlays). Colors are straight-alpha RGBA; drawing blends over the
// image. Coordinates are pixels, x right and y down; shapes are clipped to the image.
struct Rgba8 {
    uint8_t r = 255, g = 255, b = 255, a = 255;
};

void BlendPixel(Image& image, int x, int y, Rgba8 color);
void FillRect(Image& image, int x, int y, int w, int h, Rgba8 color);
// A line `thickness` pixels wide between two points (pixel centres at +0.5).
void DrawLine(Image& image, float x0, float y0, float x1, float y1, Rgba8 color, float thickness = 1);
void DrawCircle(Image& image, float cx, float cy, float radius, Rgba8 color, bool filled);
// ASCII text in the built-in 5x7 pixel font, each font pixel `scale` image pixels, with an optional
// one-pixel outline so labels stay readable over any picture. Returns the width drawn.
int DrawText(Image& image, int x, int y, const std::string& text, Rgba8 color, int scale = 1, bool outline = true);
// Width and height of DrawText's text (without the outline).
int TextWidth(const std::string& text, int scale = 1);
inline int TextHeight(int scale = 1) { return 7 * scale; }

}  // namespace oe
