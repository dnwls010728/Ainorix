#pragma once
// Sprite assets (*.sprite.json): one image cut into frames, with what every sprite drawing that image
// shares: pixels per unit, filtering, the pivot of each frame and the collision outline of each frame.
// Sprite.asset names one; Collider2D shape "sprite" collides with the outline of the frame on show.
// Loading/validation lives in assets/SpriteAtlas.cpp (NormalizeSpriteAsset); this file is what the
// scene, physics and renderers need without knowing the asset manager.
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "core/Json.h"
#include "scene/Reflect.h"

namespace oe {

class Scene;
struct Collider2D;

// Normalized sprite assets and validated .anim.json definitions by project path (null when missing or
// broken). AssetManager::SpriteLookup() gives one; tests can supply their own.
struct SpriteAssets {
    std::function<std::shared_ptr<const Json>(const std::string&)> sprite;
    std::function<std::shared_ptr<const Json>(const std::string&)> animation;
};

bool IsSpriteAssetPath(const std::string& path);

// Sprite.asset also takes an image or an atlas directly, with the settings a sprite asset file would hold written
// after it: "coin.png" (the whole image, defaults), "hero.png?columns=6&rows=1&ppu=16&pivot=0.5,0&pixelArt=0&normal=n.png"
// (a grid of frames), "hero.atlas.json?ppu=32" (atlas frames with their own pivots). "?ppu=32" alone gives settings to a
// sprite whose image comes from a shared .anim.json. Older files' texture/atlas/columns/rows/pixelsPerUnit/pivotX/pivotY/
// pixelArt/normalTexture fields load as this form (Sprite::UpgradeFrom), drawn exactly as before.
struct SpriteSource {
    std::string path;  // image, atlas or "" (no image)
    int columns = 1, rows = 1;
    double ppu = 16, pivotX = 0.5, pivotY = 0.5;
    bool pixelArt = true;
    std::string normal;
};
// False for a .sprite.json (a file, not this form) or malformed settings.
bool ParseSpriteSource(const std::string& asset, SpriteSource& out, std::string* error = nullptr);
std::string FormatSpriteSource(const SpriteSource& source);  // settings at their default are left out

// The sprite asset an entity draws from: Sprite.asset, else a .sprite.json named by Sprite.atlas or by the
// atlas path of its SpriteAnimation's shared .anim.json. Empty when it draws from a plain image.
std::string SpriteAssetOf(const Scene& scene, EntityId id, const SpriteAssets* assets);

// The asset frame for a Sprite.frame value (index or name); null when absent.
const Json* SpriteAssetFrame(const Json& asset, const SpriteFrame& frame);

// Frame-canvas pixels (top-left origin, Y down) -> the entity's local units as the sprite draws that frame
// (pivot, pixels per unit or Sprite.width/height, flips). `frame` is a normalized asset frame.
struct SpritePixelSpace {
    float originX = 0, originY = 0;  // pivot in canvas pixels
    float scaleX = 1, scaleY = 1;    // units per pixel, negative when flipped
    float ToLocalX(float px) const { return (px - originX) * scaleX; }
    float ToLocalY(float py) const { return (originY - py) * scaleY; }
    float ToPixelX(float x) const { return x / scaleX + originX; }
    float ToPixelY(float y) const { return originY - y / scaleY; }
};
SpritePixelSpace SpriteFramePixelSpace(const Json& asset, const Json& frame, float width, float height, bool flipX, bool flipY);

// The outline Collider2D shape "sprite" uses now: the collision of the frame the entity's Sprite shows (or
// the frame's opaque rectangle when it has none), in local units. `key` names what it was made from (asset,
// frame, size, flips) so physics remakes shapes only when it changes. False with a reason when the entity
// has no sprite asset or frame.
bool SpriteColliderOutline(const Scene& scene, EntityId id, const SpriteAssets* assets, Json& points, std::string* key, std::string* error = nullptr);

// The outline of a Collider2D in local units: shape "sprite" from the sprite asset, polygon/edge their points
// (or the framePoints outline of the frame on show). Empty array when there is none. `key` as above ("" for
// outlines that do not follow the sprite).
Json Collider2DOutline(const Scene& scene, EntityId id, const Collider2D& collider, const SpriteAssets* assets, std::string* key = nullptr);

// Traces the outline of the opaque pixels (alpha >= cutoff) of a texture rectangle, as frame pixels relative
// to the rectangle's top-left corner. The largest separate part is taken; `detail` is how far (in pixels) the
// simplified outline may stray from the pixel edges (0 = every step). `convex` returns its convex hull
// instead. Fewer than 3 points when nothing is opaque.
Json TraceSpriteOutline(const uint32_t* texels, int textureWidth, int textureHeight, int x, int y, int width, int height, float cutoff,
                        float detail, bool convex);

}  // namespace oe
