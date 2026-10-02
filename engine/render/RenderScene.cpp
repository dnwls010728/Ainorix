#include "render/RenderScene.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

#include "assets/Assets.h"
#include "scene/Components.h"
#include "scene/Systems.h"
#include "scene/TileGrid.h"

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

std::shared_ptr<const Mesh> TilemapMesh(const Tilemap& tm, const TileRules& rules, const Texture* tex) {
    std::string key = tm.map.dump() + "|" + rules.key + "|" + std::to_string(tm.tileSize) + "|" + std::to_string(tex ? tex->width : 0) + "x" +
                      std::to_string(tex ? tex->height : 0);
    std::lock_guard<std::mutex> lock(g_tileMeshMutex);
    auto found = g_tileMeshes.find(key);
    if (found != g_tileMeshes.end()) return found->second;
    auto mesh = std::make_shared<Mesh>();
    const float ts = std::max(0.001f, tm.tileSize);
    TileFrames frames = ResolveFrames(tm, rules);
    for (int row = 0; row < frames.height; ++row) {
        for (int col = 0; col < frames.width; ++col) {
            int frame = frames.frames[static_cast<size_t>(row) * static_cast<size_t>(frames.width) + static_cast<size_t>(col)];
            if (frame < 0) continue;
            float u0, v0, du, dv;
            FrameRect(frame, rules.columns, rules.rows, tex, u0, v0, du, dv);
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
    if (!m.baseTexture && !m.shader && m.alphaMode == AlphaMode::Mask) m.alphaMode = AlphaMode::Opaque;
    return m;
}

Mat4 RenderItem::VertexSkinMatrix(size_t vertex) const {
    if (joints.empty() || vertex >= mesh->skin.size()) return Mat4{};
    const SkinVertex& skin = mesh->skin[vertex];
    float total = 0;
    Mat4 matrix;
    for (float& value : matrix.m) value = 0;
    for (size_t k = 0; k < 4; ++k) {
        float weight = skin.weights[k];
        if (weight == 0) continue;
        const Mat4& joint = joints[skin.joints[k]];
        for (size_t j = 0; j < 16; ++j) matrix.m[j] += joint.m[j] * weight;
        total += weight;
    }
    return total > 0 ? matrix : Mat4{};
}

std::vector<DrawCall> BuildDrawList(const std::vector<RenderItem>& items, const Vec3& eye) {
    std::vector<DrawCall> opaque, blended;
    std::vector<float> distance;
    for (size_t i = 0; i < items.size(); ++i) {
        const RenderItem& it = items[i];
        Vec3 center = it.world.TransformPoint((it.boundsMin + it.boundsMax) * 0.5f);
        float d = Length(center - eye);
        for (size_t s = 0; s < it.mesh->submeshes.size(); ++s) {
            if (it.mesh->submeshes[s].indexCount == 0) continue;
            DrawCall dc;
            dc.item = i;
            dc.submesh = s;
            dc.material = it.SubmeshMaterial(it.mesh->submeshes[s]);
            dc.blend = dc.material.Blended();
            if (dc.material.opacity <= 0.0f && dc.blend && !dc.material.shader) continue;
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

std::vector<RenderItem> GatherRenderItems(const Scene& scene, AssetManager* assets, const Mat4& cameraView) {
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
        if (!it.mesh->joints.empty()) {
            const Animator* animator = scene.Get<Animator>(kv.first);
            const AnimationClip* clip = animator ? FindAnimationClip(*it.mesh, animator->clip) : nullptr;
            it.joints = EvaluateAnimationPose(*it.mesh, clip, animator ? animator->time : 0);
        }
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
        TilesetLookup lookup = assets ? assets->Tilesets() : TilesetLookup();
        TileRules rules = BuildTileRules(tm, &lookup);
        if (!rules.image.empty() && assets) it.textureOverride = assets->GetTexture(rules.image);
        it.mesh = TilemapMesh(tm, rules, it.textureOverride.get());
        if (it.mesh->indices.empty()) continue;
        it.world = scene.WorldMatrix(kv.first);
        it.normalMatrix = it.world.Inverse().Transposed();
        items.push_back(std::move(it));
    }
    // Each particle shares the static quad. Stable birth order also resolves equal-distance blends.
    const Mat4 cameraWorld = cameraView.Inverse();
    const Vec3 right = Normalize(cameraWorld.TransformDir(Vec3(1, 0, 0)));
    const Vec3 up = Normalize(cameraWorld.TransformDir(Vec3(0, 1, 0)));
    const Vec3 normal = Normalize(Cross(right, up));
    const float axes[3][3] = {{right.x, right.y, right.z}, {up.x, up.y, up.z}, {normal.x, normal.y, normal.z}};
    for (const auto& kv : scene.Pool<ParticleEmitter>()) {
        const ParticleEmitter& emitter = kv.second;
        if (emitter.particles.empty()) continue;
        const Mat4 emitterWorld = scene.WorldMatrix(kv.first);
        std::shared_ptr<const Texture> texture;
        if (!emitter.texture.empty() && assets) texture = assets->GetTexture(emitter.texture);
        float u0, v0, du, dv;
        FrameRect(emitter.frame, std::max(1, std::min(4096, emitter.columns)),
                  std::max(1, std::min(4096, emitter.rows)), texture.get(), u0, v0, du, dv);
        for (const Particle& particle : emitter.particles) {
            if (!std::isfinite(particle.age) || !std::isfinite(particle.lifetime) || particle.lifetime <= 0) continue;
            float t = Clamp(particle.age / particle.lifetime, 0, 1);
            float size = particle.startSize * (1 - t) + particle.endSize * t;
            float opacity = particle.startOpacity * (1 - t) + particle.endOpacity * t;
            if (!std::isfinite(size) || size <= 0 || !std::isfinite(opacity) || opacity <= 0) continue;
            Vec3 center = particle.worldSpace ? particle.position : emitterWorld.TransformPoint(particle.position);
            if (!std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(center.z)) continue;
            RenderItem it;
            it.id = kv.first;
            it.mesh = std::shared_ptr<const Mesh>(std::shared_ptr<const Mesh>(), quad);
            it.textureOverride = texture;
            it.tint = particle.startColor * (1 - t) + particle.endColor * t;
            it.opacity = Clamp(opacity, 0, 1);
            it.blend = true;
            it.unlit = true;
            it.castShadows = false;
            it.pointSample = true;
            if (!emitter.texture.empty() && !texture) {
                it.tint = Color(1, 0, 1);
                it.error = true;
            }
            it.uvOffset[0] = u0;
            it.uvOffset[1] = v0;
            it.uvScale[0] = du;
            it.uvScale[1] = dv;
            // Size is in world meters, independent of emitter scale; only local centers follow it.
            for (int axis = 0; axis < 3; ++axis) {
                it.world.at(axis, 0) = axes[0][axis] * size;
                it.world.at(axis, 1) = axes[1][axis] * size;
                it.world.at(axis, 2) = axes[2][axis];
            }
            it.world.at(0, 3) = center.x;
            it.world.at(1, 3) = center.y;
            it.world.at(2, 3) = center.z;
            it.normalMatrix = it.world.Inverse().Transposed();
            items.push_back(std::move(it));
        }
    }
    std::stable_sort(items.begin(), items.end(), [](const RenderItem& a, const RenderItem& b) { return a.id < b.id; });
    for (RenderItem& item : items) {
        item.boundsMin = item.mesh->boundsMin;
        item.boundsMax = item.mesh->boundsMax;
        if (item.joints.empty()) continue;
        item.boundsMin = Vec3(1e30f, 1e30f, 1e30f);
        item.boundsMax = Vec3(-1e30f, -1e30f, -1e30f);
        for (size_t i = 0; i < item.mesh->positions.size(); ++i) {
            Vec3 p = item.VertexSkinMatrix(i).TransformPoint(item.mesh->positions[i]);
            item.boundsMin = Vec3(std::min(item.boundsMin.x, p.x), std::min(item.boundsMin.y, p.y), std::min(item.boundsMin.z, p.z));
            item.boundsMax = Vec3(std::max(item.boundsMax.x, p.x), std::max(item.boundsMax.y, p.y), std::max(item.boundsMax.z, p.z));
        }
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
