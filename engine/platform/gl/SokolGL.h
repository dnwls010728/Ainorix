#pragma once
#include <cstdint>

#include "sokol_gfx.h"

namespace oe {

// Reads a single-sampled RGBA8 image back (top row first). GL backends only.
bool GLReadPixels(sg_image image, int width, int height, uint32_t* out);

}  // namespace oe
