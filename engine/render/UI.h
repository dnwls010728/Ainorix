#pragma once
#include <memory>
#include <string>
#include <vector>

#include "render/Renderer.h"
#include "scene/Scene.h"

namespace oe {

class AssetManager;
struct Texture;
struct UISlider;

// Screen-space UI (UIText / UIPanel / UIButton / UIImage / UISlider, arranged
// by UILayout, scaled by UICanvas). Laid out on a reference canvas (default
// 1280x720, scaled with the target height). Both renderers draw the same
// quads from BuildUIQuads, so software and GPU output match.
constexpr float kUIReferenceHeight = 720.0f;

struct UIRect {
    EntityId entity = kNullEntity;
    enum class Kind { Text, Panel, Button, Image, Slider } kind = Kind::Panel;
    float x = 0, y = 0, w = 0, h = 0;  // pixels, top-left origin
    int order = 0;
    EntityId parent = kNullEntity;     // UI parent entity (kNullEntity = screen)
    float opacity = 1.0f;              // including ancestors
    bool interactable = false;         // enabled button or slider
    bool blocksInput = false;          // UIPanel.blockInput: elements drawn under it cannot be clicked
    bool scroll = false;               // has a UIScroll
    bool scrollHorizontal = false;
    float scrollMax = 0.0f;            // largest UIScroll.scroll in reference pixels (0 = content fits)
    bool clipped = false;              // inside a clipping panel: only [clip] is visible
    float clip[4] = {0, 0, 0, 0};      // x0, y0, x1, y1 in pixels
};

// Pixels per reference pixel for a target size (UICanvas, default height / 720).
float UIScale(const Scene& scene, int width, int height);

// Visible UI elements in draw order. `assets` resolves font and image files
// (null = built-in font only, images drawn as solid color).
std::vector<UIRect> LayoutUI(const Scene& scene, int width, int height, AssetManager* assets = nullptr);

// A quad of UI in whole pixels [x0, x1) x [y0, y1) (already clipped). The
// color is multiplied by an optional texture (texel coordinates s/t) and cut
// to a rounded rectangle with an optional border (shape*, unclipped).
struct UIQuad {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    Color color;
    float alpha = 1.0f;
    std::shared_ptr<const Texture> texture;  // null = solid color
    float s0 = 0, t0 = 0, s1 = 0, t1 = 0;
    bool nearest = true;
    bool shaped = false;  // rounded corners or border
    float shape[4] = {0, 0, 0, 0};  // x0, y0, x1, y1
    float radius = 0.0f;
    float border = 0.0f;
    Color borderColor;
    float borderAlpha = 0.0f;
    EntityId entity = kNullEntity;
};

// All visible UI as quads in draw order, for a target of the given size.
std::vector<UIQuad> BuildUIQuads(const Scene& scene, int width, int height, AssetManager* assets = nullptr);

// Color and alpha of a quad at a pixel center (px, py); what the GPU shader computes.
void ShadeUIQuad(const UIQuad& q, float px, float py, Color* color, float* alpha);

// Draws all visible UI on top of the frame (also writes the entity-id buffer
// where a quad is at least half opaque).
void DrawUI(const Scene& scene, RenderTarget& target, AssetManager* assets = nullptr);

// Scroll bar thumb of a scroll view along its axis, in pixels from the start of the view: where it
// starts, how long it is and how far it can travel. `view` = visible length, `extent` = content length,
// `offset` = current scroll, all in pixels; `scale` = pixels per reference pixel.
struct UIThumb {
    float at = 0, length = 0, travel = 0, margin = 0, thickness = 0;
};
UIThumb ScrollThumb(float view, float extent, float offset, float scale);

// Advances UI animation state by one real frame: UIMotion timers (restarting when an element becomes
// visible) and the eased UIButton hover/press scale. Called once per simulated frame, also while paused.
void UpdateUIMotion(Scene& scene, float dt);

// Topmost interactable element (enabled button or slider) under a pixel, or null.
const UIRect* HitTestUI(const std::vector<UIRect>& rects, float px, float py);
// Same from a scene: the entity, or kNullEntity.
EntityId HitTestButton(const Scene& scene, float px, float py, int width, int height, AssetManager* assets = nullptr);
// Slider value under a pixel (clamped, snapped to step).
float SliderValueAt(const UISlider& slider, const UIRect& rect, float px, float py);

// Pixel size of a string in a font at a font size in pixels (no wrapping).
void MeasureText(const std::string& text, float size, float* w, float* h, const std::string& font = "pixel", AssetManager* assets = nullptr);

}  // namespace oe
