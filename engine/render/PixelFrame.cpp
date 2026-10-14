#include "render/Renderer.h"
#include <algorithm>
namespace oe {
void PresentPixelFrame(const RenderTarget& source, RenderTarget& target) {
    const int scale = std::max(1, std::min(target.width / source.width, target.height / source.height));
    const int left = (target.width - source.width * scale) / 2, top = (target.height - source.height * scale) / 2;
    std::fill(target.color.begin(), target.color.end(), 0xff000000u);
    std::fill(target.ids.begin(), target.ids.end(), kNullEntity);
    std::fill(target.depth.begin(), target.depth.end(), 1.0f);
    for (int y = 0; y < target.height; ++y)
        for (int x = 0; x < target.width; ++x) {
            if (x < left || y < top || x >= left + source.width * scale || y >= top + source.height * scale) continue;
            const size_t src = static_cast<size_t>((y - top) / scale) * static_cast<size_t>(source.width) + static_cast<size_t>((x - left) / scale);
            const size_t dst = static_cast<size_t>(y) * static_cast<size_t>(target.width) + static_cast<size_t>(x);
            target.color[dst] = source.color[src];
            target.ids[dst] = source.ids[src];
            target.depth[dst] = source.depth[src];
        }
}
}  // namespace oe
