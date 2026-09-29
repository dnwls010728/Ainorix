#pragma once
#include <vector>

#include "render/Renderer.h"
#include "scene/Scene.h"

namespace oe {

// Screen-space UI (UIText / UIPanel / UIButton components) laid out on a
// 1280x720 reference canvas that is scaled to the target height.
constexpr float kUIReferenceHeight = 720.0f;

struct UIRect {
    EntityId entity = kNullEntity;
    enum class Kind { Text, Panel, Button } kind = Kind::Panel;
    float x = 0, y = 0, w = 0, h = 0;  // pixels, top-left origin
    int order = 0;
};

// Visible UI elements in draw order (order, then entity id).
std::vector<UIRect> LayoutUI(const Scene& scene, int width, int height);

// Draws all visible UI on top of the frame (also writes the entity-id buffer).
void DrawUI(const Scene& scene, RenderTarget& target);

// Topmost visible button under a pixel, or kNullEntity.
EntityId HitTestButton(const Scene& scene, float px, float py, int width, int height);

// Pixel size of a string rendered with the built-in font at `lineHeight`.
void MeasureText(const std::string& text, float lineHeight, float* w, float* h);

}  // namespace oe
