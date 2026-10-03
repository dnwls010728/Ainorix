#include "app/AssetPreview.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "app/Engine.h"
#include "assets/Assets.h"
#include "render/RenderScene.h"
#include "scene/Components.h"
#include "scene/Prefab.h"

namespace oe {

bool RenderAssetPreview(Engine& engine, const std::string& path, int size, RenderTarget& image, std::string* error) {
    const auto fail = [&](const std::string& message) { if (error) *error = message; return false; };
    engine.ResolvePath(path);
    if (size < 16 || size > 256) return fail("preview size must be 16..256 pixels");
    Scene preview;
    const std::string kind = AssetManager::KindOf(path);
    if (kind == "texture") {
        const auto texture = engine.Assets().GetTexture(path, error);
        if (!texture) return false;
        image.Resize(size, size);
        const float scale = static_cast<float>(size) / static_cast<float>(std::max(texture->width, texture->height));
        const int width = std::max(1, static_cast<int>(static_cast<float>(texture->width) * scale));
        const int height = std::max(1, static_cast<int>(static_cast<float>(texture->height) * scale));
        for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
            const int sx = x - (size - width) / 2, sy = y - (size - height) / 2;
            uint32_t color = ((x / 8 + y / 8) % 2) ? 0xFF303030 : 0xFF484848;
            if (sx >= 0 && sy >= 0 && sx < width && sy < height) {
                const size_t sampleY = static_cast<size_t>(sy) * static_cast<size_t>(texture->height) / static_cast<size_t>(height);
                const size_t sampleX = static_cast<size_t>(sx) * static_cast<size_t>(texture->width) / static_cast<size_t>(width);
                const uint32_t texel = texture->texels[sampleY * static_cast<size_t>(texture->width) + sampleX];
                const uint32_t alpha = texel >> 24;
                uint32_t blended = 0xFF000000;
                for (int shift : {0, 8, 16}) blended |= ((((texel >> shift) & 255) * alpha +
                                                        ((color >> shift) & 255) * (255 - alpha) + 127) / 255) << shift;
                color = blended;
            }
            image.color[static_cast<size_t>(y * size + x)] = color;
        }
        return true;
    }
    if (kind == "model" || kind == "material") {
        if (kind == "model" && !engine.Assets().GetMesh(path, error)) return false;
        if (kind == "material" && !engine.Assets().GetMaterial(path, error)) return false;
        EntityId root = preview.Create("Preview");
        preview.Add<Transform>(root);
        MeshRenderer& renderer = preview.Add<MeshRenderer>(root);
        renderer.mesh = kind == "model" ? path : "sphere";
        if (kind == "material") renderer.material = path;
    } else if (kind == "prefab") {
        if (!InstantiatePrefab(preview, engine.ReadProjectJson(path), path, 0, error)) return false;
    } else if (kind == "scene") {
        if (!preview.FromJson(engine.ReadProjectJson(path), error)) return false;
    } else return fail("preview supports texture, model, material, prefab and scene assets");
    const auto items = GatherRenderItems(preview, &engine.Assets());
    if (items.empty()) return fail("asset has no renderable geometry");
    const float limit = std::numeric_limits<float>::max();
    Vec3 minimum(limit, limit, limit), maximum(-limit, -limit, -limit);
    for (const RenderItem& item : items) {
        for (int corner = 0; corner < 8; ++corner) {
            const Vec3 local((corner & 1) ? item.boundsMax.x : item.boundsMin.x,
                             (corner & 2) ? item.boundsMax.y : item.boundsMin.y,
                             (corner & 4) ? item.boundsMax.z : item.boundsMin.z);
            const Vec3 position = item.world.TransformPoint(local);
            minimum.x = std::min(minimum.x, position.x); minimum.y = std::min(minimum.y, position.y); minimum.z = std::min(minimum.z, position.z);
            maximum.x = std::max(maximum.x, position.x); maximum.y = std::max(maximum.y, position.y); maximum.z = std::max(maximum.z, position.z);
        }
    }
    const Vec3 center = (minimum + maximum) * 0.5f;
    const float radius = std::max(0.1f, Length(maximum - minimum) * 0.5f);
    RenderView view = MakeLookAtView(center + Normalize(Vec3(1, 0.7f, 1.6f)) * (radius * 3.2f), center, 45, 1);
    view.proj = Mat4::Perspective(Radians(45), 1, radius * 0.01f, radius * 8);
    view.drawUI = false;
    view.clearColor = Color(0.12f, 0.14f, 0.18f);
    image.Resize(size, size);
    engine.Renderer().Render(preview, view, image);
    return true;
}
}  // namespace oe
