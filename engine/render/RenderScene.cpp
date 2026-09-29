#include "render/RenderScene.h"

#include <algorithm>

#include "assets/Assets.h"
#include "scene/Components.h"

namespace oe {

const Texture* RenderItem::SubmeshTexture(const Submesh& sub) const {
    if (textureOverride) return textureOverride.get();
    if (sub.texture >= 0 && sub.texture < static_cast<int>(mesh->textures.size())) return mesh->textures[static_cast<size_t>(sub.texture)].get();
    return nullptr;
}

std::vector<RenderItem> GatherRenderItems(const Scene& scene, AssetManager* assets) {
    std::vector<RenderItem> items;
    for (const auto& kv : scene.Pool<MeshRenderer>()) {
        const MeshRenderer& mr = kv.second;
        if (!mr.visible) continue;
        RenderItem it;
        it.id = kv.first;
        it.world = scene.WorldMatrix(kv.first);
        it.normalMatrix = it.world.Inverse().Transposed();
        it.tint = mr.color;
        it.unlit = mr.unlit;
        it.flat = mr.shading == "flat";
        it.castShadows = mr.castShadows;
        it.mesh = assets ? assets->GetMesh(mr.mesh) : std::shared_ptr<const Mesh>(std::shared_ptr<const Mesh>(), GetBuiltinMesh(mr.mesh));
        if (!it.mesh) {
            // Missing/broken asset: a loud magenta cube so it shows up in screenshots.
            it.mesh = std::shared_ptr<const Mesh>(std::shared_ptr<const Mesh>(), GetBuiltinMesh("cube"));
            it.tint = Color(1, 0, 1);
            it.unlit = true;
            it.error = true;
        }
        if (!mr.texture.empty() && assets && !it.error) it.textureOverride = assets->GetTexture(mr.texture);
        items.push_back(std::move(it));
    }
    return items;
}

RenderLights GatherRenderLights(const Scene& scene) {
    RenderLights lights;
    for (const auto& kv : scene.Pool<DirectionalLight>()) {
        RenderDirLight l;
        l.dir = Normalize(scene.WorldMatrix(kv.first).TransformDir(Vec3(0, 0, -1)));
        l.color = kv.second.color * kv.second.intensity;
        lights.ambient = lights.ambient + kv.second.ambient;
        if (lights.dirs.empty()) {
            lights.shadows = kv.second.shadows;
            lights.shadowStrength = kv.second.shadowStrength;
        }
        lights.dirs.push_back(l);
    }
    if (lights.dirs.empty()) {
        lights.dirs.push_back({Normalize(Vec3(-0.4f, -1.0f, -0.3f)), Color(1, 1, 1)});
        lights.ambient = Color(0.25f, 0.25f, 0.28f);
    }
    for (const auto& kv : scene.Pool<PointLight>()) {
        lights.points.push_back({scene.WorldMatrix(kv.first).TransformPoint(Vec3(0, 0, 0)), kv.second.color * kv.second.intensity, std::max(0.01f, kv.second.range)});
    }
    return lights;
}

ShadowFit FitShadow(const Vec3& lo, const Vec3& hi, const Vec3& dir, int mapSize) {
    Vec3 center = (lo + hi) * 0.5f;
    float radius = std::max(0.5f, Length(hi - lo) * 0.5f);
    Vec3 up = std::fabs(dir.y) > 0.95f ? Vec3(0, 0, 1) : Vec3(0, 1, 0);
    Mat4 lview = Mat4::LookAt(center - dir * (radius * 2.0f), center, up);
    Mat4 lproj = Mat4::Orthographic(radius, 1.0f, 0.01f, radius * 4.0f);
    ShadowFit fit;
    fit.viewProj = lproj * lview;
    fit.texelWorld = 2.0f * radius / static_cast<float>(mapSize);
    return fit;
}

}  // namespace oe
