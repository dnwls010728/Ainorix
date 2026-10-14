#pragma once
#include <memory>
#include <vector>

#include "render/RenderScene.h"

namespace oe {

// Sprite draw modes as geometry (docs/2D.md, "Draw modes, fill and masks"): the quad of a sprite
// becomes repeated tiles, nine slices and/or the part `fill` leaves visible. Positions are in the
// unit quad's space (-0.5..0.5, scaled by the item's world matrix), uvs cover the frame (0..1, v
// down) and go through the item's uv rect like the built-in quad's. Shared by both renderers.
struct SpriteShape {
    std::string drawMode = "simple";  // simple | tiled | sliced
    float slice = 0;                  // sliced: border in source pixels
    float fill = 1;                   // 0..1 visible
    std::string fillMode = "horizontal";
    float frameW = 1, frameH = 1;     // source frame in pixels
    float width = 1, height = 1;      // drawn size in world units
    float pixelsPerUnit = 16;
    bool rotated = false, flipX = false, flipY = false;  // atlas frame stored turned clockwise
};
// Null when the plain quad does (simple, fill 1, not rotated). Cached by its parameters.
std::shared_ptr<const Mesh> SpriteShapeMesh(const SpriteShape& shape);

// Sprite.mask: each masked item is clipped (per pixel, in both renderers) to the opaque texels of
// the mask sprite's frame over its rectangle. Items of maskOnly sprites are removed afterwards.
void ApplySpriteMasks(const Scene& scene, std::vector<RenderItem>& items);
// The mask factor (0 or 1) of a masked material at a world position (mesh_fs mask2d_alpha).
float SpriteMaskAlpha(const Material& m, const Vec3& wpos);

}  // namespace oe
