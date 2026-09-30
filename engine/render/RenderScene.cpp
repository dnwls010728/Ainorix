#include "render/RenderScene.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

#include "assets/Assets.h"
#include "scene/Components.h"

namespace oe {

namespace {

// Texture rectangle of frame `index` in a columns x rows sheet, shrunk by a
// hair so nearest sampling never picks up the neighbouring frame.
void FrameRect(int index, int columns, int rows, const Texture* tex, float& u0, float& v0, float& du, float& dv) {
    columns = std::max(1, columns);
    rows = std::max(1, rows);
    int count = columns * rows;
    index = ((index % count) + count) % count;
    du = 1.0f / static_cast<float>(columns);
    dv = 1.0f / static_cast<float>(rows);
    u0 = static_cast<float>(index % columns) * du;
    v0 = static_cast<float>(index / columns) * dv;
    if (tex && tex->width > 0 && tex->height > 0) {
        float eu = 0.01f / static_cast<float>(tex->width), ev = 0.01f / static_cast<float>(tex->height);
        u0 += eu;
        v0 += ev;
        du -= 2 * eu;
        dv -= 2 * ev;
    }
}

// Tilemap meshes are rebuilt only when the map, legend or tileset change.
std::mutex g_tileMeshMutex;
std::map<std::string, std::shared_ptr<const Mesh>> g_tileMeshes;

std::shared_ptr<const Mesh> TilemapMesh(const Tilemap& tm, const Texture* tex) {
    std::string key = tm.map.dump() + "|" + tm.legend.dump() + "|" + std::to_string(tm.columns) + "x" + std::to_string(tm.rows) + "|" +
                      std::to_string(tm.tileSize) + "|" + std::to_string(tex ? tex->width : 0) + "x" + std::to_string(tex ? tex->height : 0);
    std::lock_guard<std::mutex> lock(g_tileMeshMutex);
    auto found = g_tileMeshes.find(key);
    if (found != g_tileMeshes.end()) return found->second;
    auto mesh = std::make_shared<Mesh>();
    const float ts = std::max(0.001f, tm.tileSize);
    for (int row = 0; tm.map.isArray() && row < static_cast<int>(tm.map.size()); ++row) {
        const std::string line = tm.map[row].asString("");
        for (int col = 0; col < static_cast<int>(line.size()); ++col) {
            const Json* frame = tm.legend.isObject() ? tm.legend.find(std::string(1, line[static_cast<size_t>(col)])) : nullptr;
            if (!frame || !frame->isNumber()) continue;
            float u0, v0, du, dv;
            FrameRect(frame->asInt(0), tm.columns, tm.rows, tex, u0, v0, du, dv);
            float x0 = static_cast<float>(col) * ts, x1 = x0 + ts;
            float y1 = -static_cast<float>(row) * ts, y0 = y1 - ts;
            uint32_t base = static_cast<uint32_t>(mesh->positions.size());
            mesh->positions.insert(mesh->positions.end(), {Vec3(x0, y0, 0), Vec3(x1, y0, 0), Vec3(x1, y1, 0), Vec3(x0, y1, 0)});
            mesh->normals.insert(mesh->normals.end(), 4, Vec3(0, 0, 1));
            mesh->uvs.insert(mesh->uvs.end(), {u0, v0 + dv, u0 + du, v0 + dv, u0 + du, v0, u0, v0});
            mesh->indices.insert(mesh->indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        }
    }
    Submesh s;
    s.indexCount = static_cast<uint32_t>(mesh->indices.size());
    mesh->submeshes = {s};
    mesh->ComputeBounds();
    mesh->ComputeTangents();
    if (g_tileMeshes.size() > 64) g_tileMeshes.clear();
    g_tileMeshes[key] = mesh;
    return mesh;
}

}  // namespace

Material RenderItem::SubmeshMaterial(const Submesh& sub) const {
    Material m;
    if (materialOverride) m = *materialOverride;
    else if (sub.material >= 0 && sub.material < static_cast<int>(mesh->materials.size())) m = mesh->materials[static_cast<size_t>(sub.material)];
    m.baseColor = m.baseColor * tint;
    if (textureOverride) m.baseTexture = textureOverride;
    m.opacity *= Clamp(opacity, 0.0f, 1.0f);
    if (unlit) m.unlit = true;
    if (pointSample) m.pixelArt = true;
    if (alphaCutoff > 0.0f) {
        m.alphaMode = AlphaMode::Mask;
        m.alphaCutoff = alphaCutoff;
    }
    if (blend || m.opacity < 1.0f) m.alphaMode = AlphaMode::Blend;
    if (!m.baseTexture && m.alphaMode == AlphaMode::Mask) m.alphaMode = AlphaMode::Opaque;
    return m;
}

std::vector<DrawCall> BuildDrawList(const std::vector<RenderItem>& items, const Vec3& eye) {
    std::vector<DrawCall> opaque, blended;
    std::vector<float> distance;
    for (size_t i = 0; i < items.size(); ++i) {
        const RenderItem& it = items[i];
        Vec3 center = it.world.TransformPoint((it.mesh->boundsMin + it.mesh->boundsMax) * 0.5f);
        float d = Length(center - eye);
        for (size_t s = 0; s < it.mesh->submeshes.size(); ++s) {
            if (it.mesh->submeshes[s].indexCount == 0) continue;
            DrawCall dc;
            dc.item = i;
            dc.submesh = s;
            dc.material = it.SubmeshMaterial(it.mesh->submeshes[s]);
            dc.blend = dc.material.Blended();
            if (dc.material.opacity <= 0.0f && dc.blend) continue;
            if (dc.blend) {
                blended.push_back(std::move(dc));
                distance.push_back(d);
            } else {
                opaque.push_back(std::move(dc));
            }
        }
    }
    std::vector<size_t> order(blended.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (distance[a] != distance[b]) return distance[a] > distance[b];
        return items[blended[a].item].id < items[blended[b].item].id;
    });
    for (size_t i : order) opaque.push_back(std::move(blended[i]));
    return opaque;
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
        it.opacity = mr.opacity;
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
        if (!mr.material.empty() && assets && !it.error) {
            it.materialOverride = assets->GetMaterial(mr.material);
            if (!it.materialOverride) {  // broken material file: magenta, like a missing mesh
                it.tint = Color(1, 0, 1);
                it.unlit = true;
                it.error = true;
            }
        }
        if (!mr.texture.empty() && assets && !it.error) it.textureOverride = assets->GetTexture(mr.texture);
        items.push_back(std::move(it));
    }

    // 2D: sprites are unit quads scaled to the frame size; tilemaps one mesh each.
    const Mesh* quad = GetBuiltinMesh("quad");
    for (const auto& kv : scene.Pool<Sprite>()) {
        const Sprite& sp = kv.second;
        if (!sp.visible) continue;
        RenderItem it;
        it.id = kv.first;
        it.tint = sp.color;
        it.unlit = !sp.lit;
        it.castShadows = false;
        it.pointSample = sp.pixelArt;
        it.alphaCutoff = std::max(0.0f, sp.alphaCutoff);
        it.blend = sp.alphaCutoff <= 0.0f;  // no cutoff: soft edges, alpha blended
        it.opacity = sp.opacity;
        it.mesh = std::shared_ptr<const Mesh>(std::shared_ptr<const Mesh>(), quad);
        if (!sp.texture.empty() && assets) it.textureOverride = assets->GetTexture(sp.texture);
        const Texture* tex = it.textureOverride.get();
        if (!sp.texture.empty() && !tex) {
            it.tint = Color(1, 0, 1);  // missing image: magenta, like missing meshes
            it.error = true;
            it.alphaCutoff = 0;
            it.blend = false;
        }
        float u0, v0, du, dv;
        FrameRect(sp.frame, sp.columns, sp.rows, tex, u0, v0, du, dv);
        it.uvOffset[0] = sp.flipX ? u0 + du : u0;
        it.uvScale[0] = sp.flipX ? -du : du;
        it.uvOffset[1] = sp.flipY ? v0 + dv : v0;
        it.uvScale[1] = sp.flipY ? -dv : dv;
        float ppu = std::max(0.001f, sp.pixelsPerUnit);
        float frameW = tex ? static_cast<float>(tex->width) / static_cast<float>(std::max(1, sp.columns)) : ppu;
        float frameH = tex ? static_cast<float>(tex->height) / static_cast<float>(std::max(1, sp.rows)) : ppu;
        float w = sp.width > 0 ? sp.width : frameW / ppu;
        float h = sp.height > 0 ? sp.height : frameH / ppu;
        // Pivot on the entity position; `order` nudges toward the camera (+Z).
        Mat4 local = Mat4::Translation(Vec3((0.5f - sp.pivotX) * w, (0.5f - sp.pivotY) * h, static_cast<float>(sp.order) * 0.001f)) *
                     Mat4::Scale(Vec3(w, h, 1.0f));
        it.world = scene.WorldMatrix(kv.first) * local;
        it.normalMatrix = it.world.Inverse().Transposed();
        items.push_back(std::move(it));
    }
    for (const auto& kv : scene.Pool<Tilemap>()) {
        const Tilemap& tm = kv.second;
        if (!tm.visible) continue;
        RenderItem it;
        it.id = kv.first;
        it.tint = tm.color;
        it.unlit = !tm.lit;
        it.castShadows = false;
        it.pointSample = tm.pixelArt;
        it.alphaCutoff = 0.5f;
        if (!tm.tileset.empty() && assets) it.textureOverride = assets->GetTexture(tm.tileset);
        it.mesh = TilemapMesh(tm, it.textureOverride.get());
        if (it.mesh->indices.empty()) continue;
        it.world = scene.WorldMatrix(kv.first);
        it.normalMatrix = it.world.Inverse().Transposed();
        items.push_back(std::move(it));
    }
    std::stable_sort(items.begin(), items.end(), [](const RenderItem& a, const RenderItem& b) { return a.id < b.id; });
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
