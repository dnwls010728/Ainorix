#include "render/RenderScene.h"
#include "render/Light2D.h"
#include "render/SpriteShape.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

#include "assets/Assets.h"
#include "assets/SpriteAtlas.h"
#include "scene/SpriteClip.h"
#include "scene/Components.h"
#include "scene/Pose.h"
#include "scene/Systems.h"
#include "scene/Vfx.h"
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

// The move along the camera's right/up axes that puts `point` on a whole art pixel (pixel-perfect cameras).
Vec3 PixelSnap(const Vec3& point, const Vec3& right, const Vec3& up, float step) {
    const float x = Dot(point, right), y = Dot(point, up);
    return right * (std::floor(x / step + 0.5f) * step - x) + up * (std::floor(y / step + 0.5f) * step - y);
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
            const Json& line = tm.map[row];
            const Json cell = line.isArray() && static_cast<size_t>(col) < line.size() ? line[col] : Json();
            for (const auto& uv : {std::pair<float, float>{0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}}) {
                float u = uv.first, v = uv.second;
                if (cell["flipX"].asBool()) u = 1 - u;
                if (cell["flipY"].asBool()) v = 1 - v;
                const int turns = ((cell["rotation"].asInt() / 90) % 4 + 4) % 4;
                for (int turn = 0; turn < turns; ++turn) {
                    const float old = u;
                    u = v;
                    v = 1 - old;
                }
                mesh->uvs.push_back(u0 + u * du);
                mesh->uvs.push_back(v0 + v * dv);
            }
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

// The cells of a tilemap mesh as one mesh each (Y-sorted layers), built once per tilemap mesh.
struct TileCells {
    std::shared_ptr<const Mesh> source;  // keeps the address used as the key alive
    std::vector<std::shared_ptr<const Mesh>> cells;
};
std::mutex g_tileCellMutex;
std::map<const Mesh*, TileCells> g_tileCells;

std::vector<std::shared_ptr<const Mesh>> TileCellMeshes(const std::shared_ptr<const Mesh>& source) {
    std::lock_guard<std::mutex> lock(g_tileCellMutex);
    auto found = g_tileCells.find(source.get());
    if (found != g_tileCells.end()) return found->second.cells;
    if (g_tileCells.size() >= 64) g_tileCells.clear();
    TileCells& entry = g_tileCells[source.get()];
    entry.source = source;
    for (size_t vertex = 0; vertex + 3 < source->positions.size(); vertex += 4) {
        auto mesh = std::make_shared<Mesh>();
        mesh->positions.assign(source->positions.begin() + vertex, source->positions.begin() + vertex + 4);
        mesh->normals.assign(4, Vec3(0, 0, 1));
        mesh->uvs.assign(source->uvs.begin() + vertex * 2, source->uvs.begin() + (vertex + 4) * 2);
        mesh->indices = {0, 1, 2, 0, 2, 3};
        Submesh sub;
        sub.indexCount = 6;
        mesh->submeshes = {sub};
        mesh->ComputeBounds();
        mesh->ComputeTangents();
        entry.cells.push_back(mesh);
    }
    return entry.cells;
}

// Trail ribbons sample one small texture: the color and opacity fade from the entity (u = 0) to
// the oldest point (u = 1), multiplied into the trail's image when it has one. Rebuilt only when
// those settings change. The entry keeps the image alive so its address stays a valid key.
struct TrailFade {
    std::shared_ptr<const Texture> image;
    std::shared_ptr<const Texture> texture;
};
std::mutex g_trailFadeMutex;
std::map<std::string, TrailFade> g_trailFades;

std::shared_ptr<const Texture> TrailTexture(const Trail& trail, const std::shared_ptr<const Texture>& image) {
    std::string key;
    for (float value : {trail.color.r, trail.color.g, trail.color.b, trail.endColor.r, trail.endColor.g, trail.endColor.b, trail.opacity, trail.endOpacity})
        key += std::to_string(value) + ",";
    key += std::to_string(reinterpret_cast<uintptr_t>(image.get())) + "," + std::to_string(image ? image->version : 0);
    std::lock_guard<std::mutex> lock(g_trailFadeMutex);
    auto found = g_trailFades.find(key);
    if (found != g_trailFades.end()) return found->second.texture;
    auto texture = std::make_shared<Texture>();
    const bool textured = image && image->width > 0 && image->height > 0;
    texture->width = textured ? std::min(256, image->width) : 64;
    texture->height = textured ? std::min(64, image->height) : 1;
    texture->texels.resize(static_cast<size_t>(texture->width) * static_cast<size_t>(texture->height));
    auto byte = [](float v) { return static_cast<uint32_t>(Clamp(std::isfinite(v) ? v : 0.0f, 0.0f, 1.0f) * 255.0f + 0.5f); };
    for (int y = 0; y < texture->height; ++y) {
        for (int x = 0; x < texture->width; ++x) {
            const float t = texture->width > 1 ? static_cast<float>(x) / static_cast<float>(texture->width - 1) : 0.0f;
            Color color = trail.color * (1 - t) + trail.endColor * t;
            float alpha = trail.opacity * (1 - t) + trail.endOpacity * t;
            if (textured) {
                float imageAlpha = 1.0f;
                const Color texel = image->Sample((static_cast<float>(x) + 0.5f) / static_cast<float>(texture->width),
                                                  (static_cast<float>(y) + 0.5f) / static_cast<float>(texture->height), false, &imageAlpha);
                color = color * texel;
                alpha *= imageAlpha;
            }
            texture->texels[static_cast<size_t>(y) * static_cast<size_t>(texture->width) + static_cast<size_t>(x)] =
                byte(color.r) | (byte(color.g) << 8) | (byte(color.b) << 16) | (byte(alpha) << 24);
        }
    }
    if (g_trailFades.size() > 64) g_trailFades.clear();
    g_trailFades[key] = {image, texture};
    return texture;
}

// MeshRenderer.blendShapes: the model with the weighted shapes added, kept per entity until its weights
// change (a renderer keeps its buffers for a mesh as long as the mesh lives).
std::shared_ptr<const Mesh> MorphedMesh(EntityId id, const std::shared_ptr<const Mesh>& source, const Json& values) {
    std::vector<std::pair<int, float>> weights;
    for (size_t i = 0; i < source->blendShapes.size(); ++i) {
        const Json& value = values[source->blendShapes[i].name];
        const float weight = value.isNumber() ? value.asFloat() : 0.0f;
        if (weight != 0 && std::isfinite(weight)) weights.emplace_back(static_cast<int>(i), weight);
    }
    if (weights.empty()) return source;
    struct Entry {
        std::weak_ptr<const Mesh> source;
        std::vector<std::pair<int, float>> weights;
        std::shared_ptr<const Mesh> mesh;
    };
    static std::mutex mutex;
    static std::map<std::pair<const Mesh*, EntityId>, Entry> cache;
    std::lock_guard<std::mutex> lock(mutex);
    for (auto it = cache.begin(); it != cache.end();) it = it->second.source.expired() ? cache.erase(it) : std::next(it);
    if (cache.size() > 256) cache.clear();
    Entry& entry = cache[{source.get(), id}];
    if (!entry.mesh || entry.weights != weights) {
        entry.source = source;
        entry.weights = weights;
        entry.mesh = source->Morphed(weights);
    }
    return entry.mesh;
}

}  // namespace

Material RenderItem::SubmeshMaterial(const Submesh& sub) const {
    Material m;
    const size_t slot = sub.material >= 0 ? static_cast<size_t>(sub.material) : slotMaterials.size();
    const bool slotted = slot < slotMaterials.size() && slotMaterials[slot];
    if (slotted) m = *slotMaterials[slot];
    else if (materialOverride) m = *materialOverride;
    else if (sub.material >= 0 && sub.material < static_cast<int>(mesh->materials.size())) m = mesh->materials[static_cast<size_t>(sub.material)];
    m.baseColor = m.baseColor * tint;
    if (hasShaderUniforms && m.shader && !slotted) m.shaderUniforms = shaderUniforms;  // MeshRenderer.shaderUniforms belong to `material`
    if (textureOverride) m.baseTexture = textureOverride;
    if (emission > 0.0f) {
        m.emissive = m.baseColor * emission;
        m.emissiveIntensity = 1.0f;
        m.emissiveTexture = m.baseTexture;
    }
    m.opacity *= Clamp(opacity, 0.0f, 1.0f);
    if (unlit) m.unlit = true;
    if (pointSample) m.pixelArt = true;
    if (doubleSided) m.doubleSided = true;
    if (alphaCutoff > 0.0f) {
        m.alphaMode = AlphaMode::Mask;
        m.alphaCutoff = alphaCutoff;
    }
    if (blend || additive || m.opacity < 1.0f) m.alphaMode = AlphaMode::Blend;
    if (additive) m.additive = true;
    if (!m.baseTexture && !m.shader && m.alphaMode == AlphaMode::Mask) m.alphaMode = AlphaMode::Opaque;
    if (light2DOn) {
        m.unlit = true;
        m.light2DOn = true;
        m.light2D = light2D;
        m.ambient2D = ambient2D;
        m.normalTexture = normal2D;
        m.flip2D[0] = flip2D[0];
        m.flip2D[1] = flip2D[1];
    }
    if (mask2DOn) {
        m.mask2DOn = true;
        m.mask2DFound = mask2DFound;
        m.mask2DX = mask2DX;
        m.mask2DY = mask2DY;
        m.mask2DUv = mask2DUv;
        m.mask2DTexture = mask2DTexture;
        m.mask2DCutoff = mask2DCutoff;
    }
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

std::vector<DrawCall> BuildDrawList(const std::vector<RenderItem>& items, const Vec3& eye, const RenderLights* lights, bool playing) {
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
            if (dc.material.shader) {
                const ShaderGraph& graph = *dc.material.shader;
                if (graph.cameraUniform >= 0) dc.material.shaderUniforms[static_cast<size_t>(graph.cameraUniform)] = Vec4(eye, 1);
                if (graph.playingUniform >= 0) dc.material.shaderUniforms[static_cast<size_t>(graph.playingUniform)] = playing ? Vec4(1, 1, 1, 1) : Vec4(0, 0, 0, 0);
                if (graph.lightUniform >= 0 && lights && !lights->dirs.empty())
                    dc.material.shaderUniforms[static_cast<size_t>(graph.lightUniform)] = Vec4(-Normalize(lights->dirs[0].dir), 0);
            }
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
        const RenderItem& ia = items[blended[a].item];
        const RenderItem& ib = items[blended[b].item];
        if (ia.sortLayer != ib.sortLayer) return ia.sortLayer < ib.sortLayer;
        // Y-sorted items: higher on the screen is drawn first. Unsorted items of the layer count as
        // infinitely high (behind all sorted ones), which keeps the comparison a strict weak order.
        const float ya = ia.ySort ? ia.sortY : INFINITY, yb = ib.ySort ? ib.sortY : INFINITY;
        if (ya != yb) return ya > yb;
        if (ia.sortOrder != ib.sortOrder) return ia.sortOrder < ib.sortOrder;
        if (distance[a] != distance[b]) return distance[a] > distance[b];
        return items[blended[a].item].id < items[blended[b].item].id;
    });
    for (size_t i : order) opaque.push_back(std::move(blended[i]));
    return opaque;
}

std::vector<RenderItem> GatherRenderItems(const Scene& scene, AssetManager* assets, const Mat4& cameraView, float time, float pixelStep) {
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
        if (mr.materials.isArray() && assets && !it.error) {
            for (size_t slot = 0; slot < mr.materials.size() && !it.error; ++slot) {
                const std::string path = mr.materials[slot].asString("");
                if (path.empty()) continue;
                it.slotMaterials.resize(mr.materials.size());
                it.slotMaterials[slot] = assets->GetMaterial(path);
                if (!it.slotMaterials[slot]) {  // broken material file: magenta, like a missing mesh
                    it.tint = Color(1, 0, 1);
                    it.unlit = true;
                    it.error = true;
                    it.slotMaterials.clear();
                }
            }
        }
        if (!mr.texture.empty() && assets && !it.error) it.textureOverride = assets->GetTexture(mr.texture);
        if (mr.shaderUniforms.isObject() && mr.shaderUniforms.size() != 0 && !it.error) {
            // Per-entity uniform values over the material's; an unknown name or bad value is as loud as a broken material.
            const ShaderGraph* graph = it.materialOverride ? it.materialOverride->shader.get() : nullptr;
            if (graph) it.shaderUniforms = it.materialOverride->shaderUniforms;
            if (graph && OverrideShaderUniforms(*graph, mr.shaderUniforms, it.shaderUniforms, nullptr)) {
                it.hasShaderUniforms = true;
            } else {
                it.tint = Color(1, 0, 1);
                it.unlit = true;
                it.error = true;
                it.materialOverride.reset();
            }
        }
        if (!it.mesh->joints.empty()) {
            // parentPose: a layer of the parent's model moves with it instead of posing on its own.
            EntityId posed = kv.first;
            const EntityRecord* self = mr.parentPose ? scene.Record(kv.first) : nullptr;
            const MeshRenderer* above = self ? scene.Get<MeshRenderer>(self->parent) : nullptr;
            if (above && above->mesh == mr.mesh) posed = self->parent;
            it.joints = JointPalette(*it.mesh, EvaluateEntityPose(scene, posed, *it.mesh));
        }
        if (!it.error && !it.mesh->blendShapes.empty()) {
            // A parentPose layer without weights of its own wears the parent's face.
            const Json* shapes = &mr.blendShapes;
            EntityId owner = kv.first;
            if (mr.parentPose && shapes->size() == 0) {
                const EntityRecord* self = scene.Record(kv.first);
                const MeshRenderer* above = self ? scene.Get<MeshRenderer>(self->parent) : nullptr;
                // The same copy as the parent draws: made and uploaded once for both.
                if (above && above->mesh == mr.mesh) shapes = &above->blendShapes, owner = self->parent;
            }
            if (shapes->isObject() && shapes->size() != 0) it.mesh = MorphedMesh(owner, it.mesh, *shapes);
        }
        items.push_back(std::move(it));
    }

    // Particle billboards are built from the camera's axes.
    const Mat4 cameraWorld = cameraView.Inverse();
    const Vec3 right = Normalize(cameraWorld.TransformDir(Vec3(1, 0, 0)));
    const Vec3 up = Normalize(cameraWorld.TransformDir(Vec3(0, 1, 0)));
    const Vec3 normal = Normalize(Cross(right, up));

    // 2D: sprites are unit quads scaled to the frame size; tilemaps one mesh each.
    const Mesh* quad = GetBuiltinMesh("quad");
    const std::shared_ptr<const Light2DMap> light2DMap = GetLight2DMap(scene, assets);
    for (const auto& kv : scene.Pool<Sprite>()) {
        Sprite sp = kv.second;
        Json atlasRect;   // the frame's rectangle in the image (atlas frames, rectangles listed in an .anim.json)
        Json namedFrame;  // an atlas frame: original size, trim and pivot
        bool brokenAnimation = false;
        // What it draws: Sprite.asset, or the image of its shared animation with the Sprite.asset settings.
        std::string source = sp.asset;
        if (assets) if (const SpriteAnimation* animation = scene.Get<SpriteAnimation>(kv.first)) if (!animation->asset.empty()) {
            auto definition = assets->GetAnimation(animation->asset);
            if (!definition) brokenAnimation = true;
            else {
                const SpriteAssets lookup = assets->SpriteLookup();
                source = SpriteAssetOf(scene, kv.first, &lookup);
                const Json& clip = (*definition)["clips"][animation->clip];
                if (clip["frames"].size() > 0 && std::isfinite(animation->time)) {
                    SpriteClipTiming timing;
                    if (ParseSpriteClipTiming(clip, timing, nullptr))
                        sp.frame = clip["frames"][timing.Index(animation->time)];
                    else
                        brokenAnimation = true;
                }
                const Json& image = (*definition)["atlas"];
                if (image.isObject() && !image.has("path")) {  // rectangles listed in the definition
                    SpriteSource settings;
                    ParseSpriteSource(sp.asset, settings);
                    settings.path = image["texture"].asString();
                    settings.columns = settings.rows = 1;
                    source = FormatSpriteSource(settings);
                    const int index = sp.frame;
                    if (index >= 0 && static_cast<size_t>(index) < image["frames"].size())
                        atlasRect = image["frames"][index];
                    else brokenAnimation = true;
                }
            }
        }
        // The image and what every copy shares: frames, pixels per unit, filtering, pivots.
        std::string texture, normalTexture;
        int columns = 1, rows = 1;
        float pixelsPerUnit = 16, pivotX = 0.5f, pivotY = 0.5f;
        bool pixelArt = true;
        if (assets && !source.empty()) {
            const auto def = assets->GetSprite(source);
            if (!def) {
                brokenAnimation = true;
            } else {
                texture = (*def)["texture"].asString();
                normalTexture = (*def)["normalTexture"].asString();
                pixelsPerUnit = (*def)["pixelsPerUnit"].asFloat(16);
                pixelArt = (*def)["pixelArt"].asBool(true);
                pivotX = (*def)["pivot"][0].asFloat(0.5f);
                pivotY = (*def)["pivot"][1].asFloat(0.5f);
                if ((*def)["grid"].isArray()) {  // a grid cuts it exactly as sheets always were (fractional cells, wrapping index)
                    columns = (*def)["grid"][0].asInt(1);
                    rows = (*def)["grid"][1].asInt(1);
                } else if (atlasRect.isNull() && (*def)["frames"].size() > 0) {
                    const Json* frame = FindAtlasFrame(*def, sp.frame.ToJson());
                    if (!frame)
                        brokenAnimation = true;
                    else {
                        namedFrame = *frame;
                        atlasRect = (*frame)["rect"];
                    }
                }
            }
        }
        if (!sp.visible) continue;
        RenderItem it;
        it.id = kv.first;
        it.tint = brokenAnimation ? Color(1, 0, 1) : sp.color;
        it.error = brokenAnimation;
        it.unlit = !sp.lit;
        it.castShadows = sp.castShadows;
        it.doubleSided = sp.castShadows;  // the light may see either face
        it.pointSample = pixelArt;
        it.alphaCutoff = std::max(0.0f, sp.alphaCutoff);
        it.blend = sp.alphaCutoff <= 0.0f;  // no cutoff: soft edges, alpha blended
        it.opacity = sp.opacity;
        it.emission = sp.lit ? std::max(0.0f, sp.emission) : 0.0f;
        it.sortLayer = sp.layer;
        it.sortOrder = sp.order;
        it.ySort = sp.ySort;
        it.sortY = cameraView.TransformPoint(scene.WorldMatrix(kv.first).TransformPoint(Vec3())).y + sp.ySortOffset;
        if (sp.ySort) it.blend = true;
        it.additive = sp.blend == "add";
        it.mesh = std::shared_ptr<const Mesh>(std::shared_ptr<const Mesh>(), quad);
        if (!texture.empty() && assets) it.textureOverride = assets->GetTexture(texture);
        if (!sp.material.empty() && assets) {
            it.materialOverride = assets->GetMaterial(sp.material);
            if (!it.materialOverride) {  // broken material file: magenta, like a missing mesh
                it.tint = Color(1, 0, 1);
                it.unlit = true;
                it.error = true;
            }
        }
        const Texture* tex = it.textureOverride.get();
        if (!texture.empty() && !tex) {
            it.tint = Color(1, 0, 1);  // missing image: magenta, like missing meshes
            it.error = true;
            it.alphaCutoff = 0;
            it.blend = false;
        }
        float u0, v0, du, dv;
        FrameRect(sp.frame, columns, rows, tex, u0, v0, du, dv);
        if (atlasRect.isArray() && tex) {
            const float x = atlasRect[0].asFloat(), y = atlasRect[1].asFloat();
            const float width = atlasRect[2].asFloat(), height = atlasRect[3].asFloat();
            if (x + width > tex->width || y + height > tex->height) { it.tint = Color(1, 0, 1); it.error = true; }
            // Filtered sprites stop half a texel inside so they do not blend in the neighbouring frame;
            // point-sampled ones keep the frame's scale (texel centres at pixel centres), only a hair inside.
            const float inset = pixelArt ? 0.01f : 0.5f;
            u0 = (x + inset) / static_cast<float>(tex->width); v0 = (y + inset) / static_cast<float>(tex->height);
            du = (width - 2 * inset) / static_cast<float>(tex->width); dv = (height - 2 * inset) / static_cast<float>(tex->height);
        }
        it.uvOffset[0] = sp.flipX ? u0 + du : u0;
        it.uvScale[0] = sp.flipX ? -du : du;
        it.uvOffset[1] = sp.flipY ? v0 + dv : v0;
        it.uvScale[1] = sp.flipY ? -dv : dv;
        float ppu = std::max(0.001f, pixelsPerUnit);
        float frameW = tex ? static_cast<float>(tex->width) / static_cast<float>(std::max(1, columns)) : ppu;
        float frameH = tex ? static_cast<float>(tex->height) / static_cast<float>(std::max(1, rows)) : ppu;
        if (atlasRect.isArray()) { frameW = atlasRect[2].asFloat(); frameH = atlasRect[3].asFloat(); }
        float w = sp.width > 0 ? sp.width : frameW / ppu;
        float h = sp.height > 0 ? sp.height : frameH / ppu;
        float centerX = (0.5f - pivotX) * w, centerY = (0.5f - pivotY) * h;
        if (namedFrame.isObject()) {
            const float originalW = namedFrame["size"][0].asFloat(), originalH = namedFrame["size"][1].asFloat();
            const bool rotated = namedFrame["rotated"].asBool();
            const float trimmedW = rotated ? frameH : frameW, trimmedH = rotated ? frameW : frameH;
            const float sx = sp.width > 0 ? sp.width / originalW : 1 / ppu;
            const float sy = sp.height > 0 ? sp.height / originalH : 1 / ppu;
            centerX = (namedFrame["trim"][0].asFloat() + trimmedW / 2 - namedFrame["pivot"][0].asFloat() * originalW) * sx;
            centerY = (originalH * (1 - namedFrame["pivot"][1].asFloat()) - namedFrame["trim"][1].asFloat() - trimmedH / 2) * sy;
            if (sp.flipX) centerX = -centerX;
            if (sp.flipY) centerY = -centerY;
            w = trimmedW * sx;
            h = trimmedH * sy;
            if (rotated) {  // the mesh's uvs undo the rotation and the flips (SpriteShapeMesh)
                it.uvOffset[0] = u0;
                it.uvOffset[1] = v0;
                it.uvScale[0] = du;
                it.uvScale[1] = dv;
            }
        }
        // Draw modes and fill are geometry; a plain sprite keeps the shared quad.
        SpriteShape shape;
        shape.drawMode = sp.drawMode;
        shape.slice = sp.slice;
        shape.fill = std::isfinite(sp.fill) ? Clamp(sp.fill, 0, 1) : 1.0f;
        shape.fillMode = sp.fillMode;
        shape.frameW = std::max(1.0f, namedFrame.isObject() && namedFrame["rotated"].asBool() ? frameH : frameW);
        shape.frameH = std::max(1.0f, namedFrame.isObject() && namedFrame["rotated"].asBool() ? frameW : frameH);
        shape.width = w;
        shape.height = h;
        shape.pixelsPerUnit = ppu;
        shape.rotated = namedFrame.isObject() && namedFrame["rotated"].asBool();
        shape.flipX = sp.flipX;
        shape.flipY = sp.flipY;
        if (auto mesh = SpriteShapeMesh(shape)) {
            if (mesh->indices.empty()) continue;  // fill 0: nothing to draw
            it.mesh = mesh;
        }
        if (sp.light2D) {
            it.light2DOn = true;
            it.light2D = light2DMap;
            it.ambient2D = sp.ambient2D;
            if (!normalTexture.empty() && assets) it.normal2D = assets->GetTexture(normalTexture);
            it.flip2D[0] = sp.flipX ? -1.0f : 1.0f;
            it.flip2D[1] = sp.flipY ? -1.0f : 1.0f;
        }
        // Pivot on the entity position; `layer` and `order` nudge toward the camera (+Z) so cut-out
        // sprites, which write depth, agree with the sort order of blended ones.
        const float nudge = static_cast<float>(sp.layer) * 0.01f + static_cast<float>(sp.order) * 0.001f;
        Mat4 local = Mat4::Translation(Vec3(centerX, centerY, nudge)) * Mat4::Scale(Vec3(w, h, 1.0f));
        it.world = scene.WorldMatrix(kv.first) * local;
        // Pixel-perfect: the picture's corner on a whole art pixel (snapping the pivot would leave an
        // odd-sized frame half a pixel off, sampling texel edges: doubled or missing rows).
        if (pixelStep > 0) it.world = Mat4::Translation(PixelSnap(it.world.TransformPoint(Vec3(-0.5f, -0.5f, 0)), right, up, pixelStep)) * it.world;
        it.normalMatrix = it.world.Inverse().Transposed();
        items.push_back(std::move(it));
    }
    for (const auto& kv : scene.Pool<Tilemap>()) {
        const Tilemap& source = kv.second;
        if (!source.visible) continue;
        std::vector<Tilemap> layers;
        Tilemap base = source;
        base.layers = Json::MakeArray();
        layers.push_back(base);
        for (const Json& layer : source.layers.items()) {
            Tilemap part = base;
            part.map = layer["map"];
            part.layer = layer["layer"].asInt(source.layer);
            part.order = layer["order"].asInt(source.order);
            part.ySort = layer["ySort"].asBool(source.ySort);
            part.ySortOffset = layer["ySortOffset"].asFloat(source.ySortOffset);
            part.visible = layer["visible"].asBool(true);
            layers.push_back(part);
        }
        for (const Tilemap& tm : layers) {
            if (!tm.visible) continue;
            RenderItem it;
            it.id = kv.first;
            it.tint = tm.color;
            it.unlit = !tm.lit;
            it.castShadows = false;
            it.pointSample = tm.pixelArt;
            it.alphaCutoff = 0.5f;
            it.sortLayer = tm.layer;
            it.sortOrder = tm.order;
            it.ySort = tm.ySort;
            it.blend = tm.ySort;
            if (tm.light2D) {
                it.light2DOn = true;
                it.light2D = light2DMap;
                it.ambient2D = tm.ambient2D;
            }
            TilesetLookup lookup = assets ? assets->Tilesets() : TilesetLookup();
            TileRules rules = BuildTileRules(tm, &lookup);
            for (auto& rule : rules.tiles)
                if (!rule.second.animation.empty()) {
                    const auto& animation = rule.second.animation;
                    const size_t index = static_cast<size_t>(std::floor(std::max(0.0f, time) * rule.second.fps)) % animation.size();
                    rule.second.frame = animation[index];
                    rule.second.variants.clear();
                    rule.second.autotile = AutoTile::None;
                    rules.key += "|" + std::to_string(rule.first) + ":" + std::to_string(index);
                }
            if (!rules.image.empty() && assets) it.textureOverride = assets->GetTexture(rules.image);
            it.mesh = TilemapMesh(tm, rules, it.textureOverride.get());
            if (it.mesh->indices.empty()) continue;
            it.world = scene.WorldMatrix(kv.first) *
                       Mat4::Translation(Vec3(0, 0, static_cast<float>(tm.layer) * 0.01f + static_cast<float>(tm.order) * 0.001f));
            if (pixelStep > 0) it.world = Mat4::Translation(PixelSnap(it.world.TransformPoint(Vec3()), right, up, pixelStep)) * it.world;
            it.normalMatrix = it.world.Inverse().Transposed();
            if (!tm.ySort) {
                items.push_back(std::move(it));
                continue;
            }
            // A sorted layer draws each cell on its own, sorted by its bottom edge.
            for (const auto& cellMesh : TileCellMeshes(it.mesh)) {
                RenderItem cell = it;
                cell.mesh = cellMesh;
                const Vec3 pivot = (cellMesh->positions[0] + cellMesh->positions[1]) * 0.5f;
                cell.sortY = cameraView.TransformPoint(cell.world.TransformPoint(pivot)).y + tm.ySortOffset;
                items.push_back(std::move(cell));
            }
        }
    }
    ApplySpriteMasks(scene, items);
    // Darkness2D: one camera-facing quad at the near plane, textured with a light map built here
    // so both renderers show the same thing. Each light multiplies the darkness that is left.
    for (const auto& kv : scene.Pool<Darkness2D>()) {
        const Darkness2D& dark = kv.second;
        const Camera* cam = scene.Get<Camera>(kv.first);
        if (!dark.enabled || !cam || !cam->active || cam->projection != "orthographic" || dark.opacity <= 0.0f) continue;
        const float viewH = std::max(0.01f, cam->orthoSize) * 2.0f * 1.1f;  // margin: the map repeats at its edges
        const float viewW = viewH * 3.0f;                                     // covers any aspect up to about 3:1
        const int rows = std::max(16, std::min(512, dark.resolution));
        const int cols = rows * 3;
        std::vector<float> left(static_cast<size_t>(rows) * static_cast<size_t>(cols), 1.0f);
        const Mat4 camWorld = scene.WorldMatrix(kv.first);
        const Mat4 toCamera = camWorld.Inverse();
        const float texel = viewH / static_cast<float>(rows);
        bool shadows = false;
        for (const auto& lv : scene.Pool<Light2D>()) shadows = shadows || (lv.second.enabled && lv.second.shadows && lv.second.mode == "darkness");
        const Occluders2D occluders = shadows ? GatherOccluders2D(scene, assets) : Occluders2D();
        size_t lightCount = 0;
        for (const auto& lv : scene.Pool<Light2D>()) {
            const Light2D& light = lv.second;
            if (!light.enabled || !(light.radius > 0.0f) || !(light.strength > 0.0f)) continue;
            if (light.mode != "darkness") continue;
            if (light.shadows && lightCount++ >= 8) continue;
            const Vec3 p = toCamera.TransformPoint(scene.WorldMatrix(lv.first).TransformPoint(Vec3()));
            if (!std::isfinite(p.x) || !std::isfinite(p.y)) continue;
            const float inner = Clamp(light.inner, 0.0f, 0.99f), strength = Clamp(light.strength, 0.0f, 1.0f);
            const float softness = Clamp(light.softness, 0.0f, 10.0f);
            const Vec3 source = scene.WorldMatrix(lv.first).TransformPoint(Vec3());
            const std::vector<int> near = light.shadows ? OccluderEdgesNear(occluders, source, light.radius + softness) : std::vector<int>();
            // texel (c, r) is centred at x = (c + 0.5) * texel - viewW / 2, y = viewH / 2 - (r + 0.5) * texel
            const int c0 = std::max(0, static_cast<int>(std::floor((p.x - light.radius + viewW * 0.5f) / texel)));
            const int c1 = std::min(cols - 1, static_cast<int>(std::ceil((p.x + light.radius + viewW * 0.5f) / texel)));
            const int r0 = std::max(0, static_cast<int>(std::floor((viewH * 0.5f - p.y - light.radius) / texel)));
            const int r1 = std::min(rows - 1, static_cast<int>(std::ceil((viewH * 0.5f - p.y + light.radius) / texel)));
            for (int r = r0; r <= r1; ++r) {
                const float dy = viewH * 0.5f - (static_cast<float>(r) + 0.5f) * texel - p.y;
                for (int c = c0; c <= c1; ++c) {
                    const float dx = (static_cast<float>(c) + 0.5f) * texel - viewW * 0.5f - p.x;
                    const float t = std::sqrt(dx * dx + dy * dy) / light.radius;
                    if (t >= 1.0f) continue;
                    float visible = 1.0f;
                    if (light.shadows) {
                        visible = Visibility2D(occluders, source, softness, camWorld.TransformPoint(Vec3(dx + p.x, dy + p.y, p.z)), &near);
                        if (visible <= 0.0f) continue;
                    }
                    const float x = t <= inner ? 0.0f : (t - inner) / (1.0f - inner);
                    left[static_cast<size_t>(r) * static_cast<size_t>(cols) + static_cast<size_t>(c)] *= 1.0f - strength * visible * (1.0f - x * x * x);
                }
            }
        }
        auto map = std::make_shared<Texture>();
        map->width = cols;
        map->height = rows;
        map->texels.resize(left.size());
        for (size_t i = 0; i < left.size(); ++i)
            map->texels[i] = (static_cast<uint32_t>(Clamp(left[i], 0.0f, 1.0f) * 255.0f + 0.5f) << 24) | 0x00FFFFFFu;
        RenderItem it;
        it.id = kv.first;
        it.pickable = false;
        it.mesh = std::shared_ptr<const Mesh>(std::shared_ptr<const Mesh>(), quad);
        it.textureOverride = map;
        it.tint = dark.color;
        it.opacity = Clamp(dark.opacity, 0.0f, 1.0f);
        it.blend = true;
        it.unlit = true;
        it.castShadows = false;
        it.sortLayer = dark.layer;
        it.world = camWorld * Mat4::Translation(Vec3(0, 0, -(cam->nearPlane + (cam->farPlane - cam->nearPlane) * 0.0005f))) *
                   Mat4::Scale(Vec3(viewW, viewH, 1.0f));
        it.normalMatrix = it.world.Inverse().Transposed();
        items.push_back(std::move(it));
    }
    // Each particle shares the static quad. Stable birth order also resolves equal-distance blends.
    const float axes[3][3] = {{right.x, right.y, right.z}, {up.x, up.y, up.z}, {normal.x, normal.y, normal.z}};
    auto gatherEmitter = [&](EntityId owner, const ParticleEmitter& emitter, const Mat4& emitterWorld) {
        if (emitter.particles.empty()) return;
        std::shared_ptr<const Texture> texture;
        if (!emitter.texture.empty() && assets) texture = assets->GetTexture(emitter.texture);
        const int columns = std::max(1, std::min(4096, emitter.columns)), rows = std::max(1, std::min(4096, emitter.rows));
        float u0, v0, du, dv;
        FrameRect(emitter.frame, columns, rows, texture.get(), u0, v0, du, dv);
        // Lifetime curves are live settings like the texture; a malformed one falls back to start/end.
        ParticleCurve sizeCurve, colorCurve, opacityCurve;
        ParseParticleCurve(emitter.sizeOverLife, false, sizeCurve, nullptr);
        ParseParticleCurve(emitter.colorOverLife, true, colorCurve, nullptr);
        ParseParticleCurve(emitter.opacityOverLife, false, opacityCurve, nullptr);
        const int flipbook = std::max(1, std::min(1000000, emitter.frames));
        const bool alignVelocity = emitter.align == "velocity";
        const size_t first = items.size();
        const auto material = !emitter.material.empty() && assets ? assets->GetMaterial(emitter.material) : nullptr;
        auto sourceMesh = emitter.mesh.empty() ? std::shared_ptr<const Mesh>(std::shared_ptr<const Mesh>(), quad) :
            (assets ? assets->GetMesh(emitter.mesh) : std::shared_ptr<const Mesh>(std::shared_ptr<const Mesh>(), GetBuiltinMesh(emitter.mesh)));
        if (sourceMesh && !emitter.mesh.empty() && (!sourceMesh->joints.empty() || sourceMesh->positions.size() > 65536 ||
            sourceMesh->indices.size() > 196608 || sourceMesh->positions.size()*emitter.particles.size() > 1000000)) sourceMesh.reset();
        for (const Particle& particle : emitter.particles) {
            if (!std::isfinite(particle.age) || !std::isfinite(particle.lifetime) || particle.lifetime <= 0) continue;
            float t = Clamp(particle.age / particle.lifetime, 0, 1);
            float size = sizeCurve.Empty() ? particle.startSize * (1 - t) + particle.endSize * t : sizeCurve.Sample(t).x * particle.sizeScale;
            float opacity = opacityCurve.Empty() ? particle.startOpacity * (1 - t) + particle.endOpacity * t : opacityCurve.Sample(t).x;
            if (particle.hasExpressionSize) size=particle.expressionSize;
            if (particle.hasExpressionOpacity) opacity=particle.expressionOpacity;
            if (!std::isfinite(size) || size <= 0 || !std::isfinite(opacity) || opacity <= 0) continue;
            Vec3 center = particle.worldSpace ? particle.position : emitterWorld.TransformPoint(particle.position);
            if (!std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(center.z)) continue;
            RenderItem it;
            it.id = owner;
            it.mesh = std::shared_ptr<const Mesh>(std::shared_ptr<const Mesh>(), quad);
            it.textureOverride = texture;
            it.materialOverride = material;
            if (colorCurve.Empty()) {
                it.tint = particle.startColor * (1 - t) + particle.endColor * t;
            } else {
                const Vec3 c = colorCurve.Sample(t);
                it.tint = Color(c.x, c.y, c.z);
            }
            if (particle.hasExpressionColor) it.tint=Color(particle.expressionColor.x,particle.expressionColor.y,particle.expressionColor.z);
            it.opacity = Clamp(opacity, 0, 1);
            it.sortLayer = emitter.layer;
            it.additive = emitter.blend == "add";
            it.blend = true;
            it.unlit = !material || material->unlit;
            it.castShadows = false;
            it.doubleSided = !emitter.mesh.empty();
            it.pointSample = emitter.pixelArt;
            if (!emitter.texture.empty() && !texture) {
                it.tint = Color(1, 0, 1);
                it.error = true;
            }
            if (flipbook > 1) {
                // frameRate 0 plays the strip once over the lifetime; otherwise it loops at that rate.
                int played = emitter.frameRate > 0 ? static_cast<int>(std::floor(particle.age * Clamp(emitter.frameRate, 0, 240)))
                                                   : (emitter.randomFrame ? 0 : std::min(flipbook - 1, static_cast<int>(t * static_cast<float>(flipbook))));
                const int index = emitter.frame + (particle.frameOffset + std::max(0, played)) % flipbook;
                FrameRect(index, columns, rows, texture.get(), u0, v0, du, dv);
            }
            it.uvOffset[0] = u0;
            it.uvOffset[1] = v0;
            it.uvScale[0] = du;
            it.uvScale[1] = dv;
            // Billboard axes: the camera's, turned toward the travel direction and/or by the particle's rotation.
            Vec3 across = right, along = up;
            float length = size;
            if (alignVelocity) {
                const Vec3 velocity = particle.worldSpace ? particle.velocity : emitterWorld.TransformDir(particle.velocity);
                const Vec3 onScreen = velocity - normal * Dot(velocity, normal);
                const float speed = Length(onScreen);
                if (std::isfinite(speed) && speed > 1e-6f) {
                    along = onScreen / speed;
                    across = Cross(along, normal);
                    if (emitter.stretch > 0) length += Clamp(emitter.stretch, 0, 100) * speed;
                }
            }
            if (particle.rotation != 0 && std::isfinite(particle.rotation)) {
                const float angle = Radians(particle.rotation), c = std::cos(angle), s = std::sin(angle);
                const Vec3 turnedAcross = across * c + along * s;
                along = along * c - across * s;
                across = turnedAcross;
            }
            const float turned[2][3] = {{across.x, across.y, across.z}, {along.x, along.y, along.z}};
            // Size is in world meters, independent of emitter scale; only local centers follow it.
            for (int axis = 0; axis < 3; ++axis) {
                it.world.at(axis, 0) = turned[0][axis] * size;
                it.world.at(axis, 1) = turned[1][axis] * length;
                it.world.at(axis, 2) = axes[2][axis];
            }
            it.world.at(0, 3) = center.x;
            it.world.at(1, 3) = center.y;
            it.world.at(2, 3) = center.z;
            if (!(particle.rotation3D == Vec3()) || !(particle.axisScale == Vec3(1, 1, 1)) || !(emitter.pivot == Vec3())) {
                // Rotate in unscaled billboard axes, then apply size/stretch once on the geometry.
                for (int axis = 0; axis < 3; ++axis) {
                    it.world.at(axis, 0) = turned[0][axis];
                    it.world.at(axis, 1) = turned[1][axis];
                }
                it.world = it.world * Mat4::RotationEuler(particle.rotation3D) *
                    Mat4::Scale(Vec3(size * particle.axisScale.x, length * particle.axisScale.y, size * particle.axisScale.z)) * Mat4::Translation(emitter.pivot * -1);
            }
            if (!emitter.mesh.empty() && sourceMesh) {
                // Static mesh particles retain emitter orientation; lifetime size scales all axes.
                Mat4 basis = particle.worldSpace ? particle.meshBasis : emitterWorld;
                for (int axis = 0; axis < 3; ++axis) {
                    Vec3 column(basis.at(0,axis), basis.at(1,axis), basis.at(2,axis));
                    column = Normalize(column);
                    basis.at(0,axis)=column.x; basis.at(1,axis)=column.y; basis.at(2,axis)=column.z;
                }
                basis.at(0,3)=center.x; basis.at(1,3)=center.y; basis.at(2,3)=center.z;
                it.world = basis * Mat4::RotationEuler(Vec3(0,0,particle.rotation)) * Mat4::Scale(emitter.meshScale * size);
                if (!(particle.rotation3D == Vec3()) || !(particle.axisScale == Vec3(1, 1, 1)) || !(emitter.pivot == Vec3()))
                    it.world = basis * Mat4::RotationEuler(Vec3(0,0,particle.rotation)) * Mat4::RotationEuler(particle.rotation3D) *
                        Mat4::Scale(Vec3(emitter.meshScale.x * size * particle.axisScale.x, emitter.meshScale.y * size * particle.axisScale.y,
                            emitter.meshScale.z * size * particle.axisScale.z)) * Mat4::Translation(emitter.pivot * -1);
                it.mesh = sourceMesh;
            }
            if ((!emitter.material.empty() && !material) || (!emitter.mesh.empty() && !sourceMesh)) {
                it.tint = Color(1,0,1); it.error = true; it.unlit = true;
            }
            it.normalMatrix = it.world.Inverse().Transposed();
            const Vec3 velocity = particle.worldSpace ? particle.velocity : emitterWorld.TransformDir(particle.velocity);
            it.particleInput = Vec4(t, particle.random, Length(velocity), 0);
            it.particleCustom = particle.custom;
            if (emitter.batching) {
                auto mesh = std::make_shared<Mesh>(*it.mesh);
                const Vec4 data = it.particleInput;
                it.particleInput = Vec4();
                it.particleCustom = Vec4();
                mesh->colors.assign(mesh->positions.size(), Vec4(it.tint.r,it.tint.g,it.tint.b,it.opacity));
                mesh->particleData.assign(mesh->positions.size(), data);
                mesh->particleCustom.assign(mesh->positions.size(), particle.custom);
                for (size_t vertex=0; vertex<mesh->positions.size(); ++vertex) {
                    mesh->positions[vertex] = it.world.TransformPoint(mesh->positions[vertex]);
                    if (vertex < mesh->normals.size()) mesh->normals[vertex] = Normalize(it.normalMatrix.TransformDir(mesh->normals[vertex]));
                    if (vertex*2+1 < mesh->uvs.size()) {
                        mesh->uvs[vertex*2] = mesh->uvs[vertex*2]*du + u0;
                        mesh->uvs[vertex*2+1] = mesh->uvs[vertex*2+1]*dv + v0;
                    }
                }
                mesh->skin.clear(); mesh->joints.clear(); mesh->clips.clear();
                mesh->ComputeBounds(); mesh->ComputeTangents();
                it.mesh = mesh; it.world = Mat4{}; it.normalMatrix = Mat4{};
                it.tint = Color(1,1,1); it.opacity = 1;
                it.uvOffset[0]=it.uvOffset[1]=0; it.uvScale[0]=it.uvScale[1]=1;
            }
            items.push_back(std::move(it));
        }
        if (emitter.batching && items.size() > first + 1 && (material || emitter.mesh.empty() ||
            (sourceMesh && sourceMesh->submeshes.size() == 1))) {
            // Opt-in group sorting: retain back-to-front order inside the emitter.
            std::stable_sort(items.begin()+static_cast<std::ptrdiff_t>(first), items.end(), [&](const RenderItem& a, const RenderItem& b) {
                const Vec3 eye = cameraWorld.TransformPoint(Vec3());
                return Length((a.mesh->boundsMin+a.mesh->boundsMax)*0.5f-eye) >
                       Length((b.mesh->boundsMin+b.mesh->boundsMax)*0.5f-eye);
            });
            auto mesh = std::make_shared<Mesh>();
            for (size_t index=first; index<items.size(); ++index) {
                const Mesh& part = *items[index].mesh;
                const uint32_t offset = static_cast<uint32_t>(mesh->positions.size());
                mesh->positions.insert(mesh->positions.end(),part.positions.begin(),part.positions.end());
                mesh->normals.insert(mesh->normals.end(),part.normals.begin(),part.normals.end());
                mesh->uvs.insert(mesh->uvs.end(),part.uvs.begin(),part.uvs.end());
                mesh->colors.insert(mesh->colors.end(),part.colors.begin(),part.colors.end());
                mesh->particleData.insert(mesh->particleData.end(),part.particleData.begin(),part.particleData.end());
                mesh->particleCustom.insert(mesh->particleCustom.end(),part.particleCustom.begin(),part.particleCustom.end());
                for (uint32_t vertex : part.indices) mesh->indices.push_back(vertex+offset);
            }
            mesh->materials = items[first].mesh->materials;
            Submesh sub; sub.indexCount=static_cast<uint32_t>(mesh->indices.size());
            sub.material=items[first].mesh->submeshes.front().material;
            mesh->submeshes={sub}; mesh->ComputeBounds(); mesh->ComputeTangents();
            items[first].mesh=mesh; items.resize(first+1);
        }
    };
    for (const auto& kv : scene.Pool<VFXSystem>()) if (kv.second.definition)
        for (size_t i = 0; i < kv.second.emitters.size(); ++i)
            gatherEmitter(kv.first, kv.second.emitters[i], scene.WorldMatrix(kv.first) * kv.second.definition->emitters[i].local);
    // Trails: one ribbon mesh per entity in world coordinates, rebuilt every frame from its path and
    // turned toward the camera. The fade along its length comes from the texture (TrailTexture).
    auto gatherTrail = [&](EntityId owner, const Trail& trail, const Vec3& head, const Vec3& tip, bool emitting) {
        std::vector<TrailPoint> path = trail.points;
        if (emitting) {  // the ribbon always reaches the entity, also between path points
            if (std::isfinite(head.x) && std::isfinite(head.y) && std::isfinite(head.z) &&
                (path.empty() || Length(head - path.back().position) > 1e-5f ||
                 (trail.mode == "ribbon" && Length(tip-path.back().tip) > 1e-5f))) path.push_back({head, 0.0f, tip});
        }
        if (path.size() < 2 || !std::isfinite(trail.width) || !std::isfinite(trail.endWidth)) return;
        std::shared_ptr<const Texture> image;
        if (!trail.texture.empty() && assets) image = assets->GetTexture(trail.texture);
        const auto material = !trail.material.empty() && assets ? assets->GetMaterial(trail.material) : nullptr;
        if (trail.texture.empty() && material) image = material->baseTexture;
        const bool missing = (!trail.texture.empty() && !image) || (!trail.material.empty() && !material);
        std::shared_ptr<const Texture> fade = TrailTexture(trail, missing ? nullptr : image);
        const float lifetime = std::isfinite(trail.lifetime) ? Clamp(trail.lifetime, 0.01f, 60) : 0.01f;
        const float texelU = 1.0f / static_cast<float>(fade->width), texelV = 1.0f / static_cast<float>(fade->height);
        auto mesh = std::make_shared<Mesh>();
        Vec3 side = right;
        for (size_t i = 0; i < path.size(); ++i) {
            const Vec3 tangent = path[std::min(i + 1, path.size() - 1)].position - path[i > 0 ? i - 1 : 0].position;
            const Vec3 across = Cross(tangent, normal);
            const float span = Length(across);
            if (std::isfinite(span) && span > 1e-6f) side = across / span;  // otherwise keep the previous side (path toward the camera)
            const float u = Clamp(path[i].age / lifetime, 0, 1);
            const float half = std::max(0.0f, trail.width * (1 - u) + trail.endWidth * u) * 0.5f;
            // Texel centers of the first/last column and row: bilinear sampling never wraps head into tail.
            const float tu = texelU * 0.5f + u * (1.0f - texelU);
            mesh->positions.push_back(trail.mode == "ribbon" ? path[i].position : path[i].position - side * half);
            mesh->positions.push_back(trail.mode == "ribbon" ? path[i].tip : path[i].position + side * half);
            if (material) mesh->particleData.insert(mesh->particleData.end(), 2, Vec4(u,0,0,0));
            mesh->normals.insert(mesh->normals.end(), 2, normal);
            mesh->uvs.insert(mesh->uvs.end(), {tu, 1.0f - texelV * 0.5f, tu, texelV * 0.5f});
            if (i == 0) continue;
            const uint32_t base = static_cast<uint32_t>(i - 1) * 2;
            mesh->indices.insert(mesh->indices.end(), {base, base + 1, base + 3, base, base + 3, base + 2});
        }
        Submesh sub;
        sub.indexCount = static_cast<uint32_t>(mesh->indices.size());
        mesh->submeshes = {sub};
        mesh->ComputeBounds();
        mesh->ComputeTangents();
        RenderItem it;
        it.id = owner;
        it.mesh = mesh;
        it.textureOverride = fade;
        it.materialOverride = material;
        it.tint = missing ? Color(1, 0, 1) : Color(1, 1, 1);
        it.error = missing;
        it.blend = true;
        it.additive = trail.blend == "add";
        it.unlit = true;
        it.castShadows = false;
        it.doubleSided = true;
        it.unlit = !material || material->unlit;
        it.sortLayer = trail.layer;
        items.push_back(std::move(it));
    };
    auto trailItem = [&](EntityId id, const Trail& trail, const Mat4& world, bool playing) {
        Vec3 base, tip;
        const bool valid = TrailAnchors(scene, trail, world, base, tip);
        gatherTrail(id, trail, base, tip, valid && playing && TrailEmitting(trail));
    };
    for (const auto& kv : scene.Pool<Trail>()) trailItem(kv.first, kv.second, scene.WorldMatrix(kv.first), true);
    for (const auto& kv : scene.Pool<VFXSystem>()) if (kv.second.definition)
        for (size_t i = 0; i < kv.second.trails.size(); ++i)
            trailItem(kv.first, kv.second.trails[i], scene.WorldMatrix(kv.first) * kv.second.definition->trails[i].local, VfxPlaying(kv.second));
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

void ForEachVertexOffset(const RenderItem& item, const DrawCall& draw, float time,
                         const std::function<void(uint32_t, const Vec3&, const Vec3&)>& fn) {
    const Material& material = draw.material;
    if (!material.shader || material.shader->offset < 0) return;
    const Mesh& m = *item.mesh;
    const Submesh& sub = m.submeshes[draw.submesh];
    std::vector<uint8_t> done(m.positions.size(), 0);
    ShaderInputs input;
    input.baseColor = Vec4(material.baseColor.r, material.baseColor.g, material.baseColor.b, material.opacity);
    input.time = time;
    for (uint32_t k = sub.firstIndex; k < sub.firstIndex + sub.indexCount; ++k) {
        const uint32_t vertex = m.indices[k];
        if (done[vertex]) continue;
        done[vertex] = 1;
        // The same world position, normal and uv as the renderers' vertex transform.
        const Mat4 skin = item.VertexSkinMatrix(vertex);
        const Mat4 skinNormal = item.joints.empty() ? Mat4{} : skin.Inverse().Transposed();
        const Vec3 wpos = item.world.TransformPoint(skin.TransformPoint(m.positions[vertex]));
        const Vec3 normal = vertex < m.normals.size()
            ? Normalize(item.normalMatrix.TransformDir(skinNormal.TransformDir(m.normals[vertex]))) : Vec3(0, 1, 0);
        float u = 0, v = 0;
        if (vertex * 2 + 1 < m.uvs.size()) {
            u = m.uvs[vertex * 2];
            v = m.uvs[vertex * 2 + 1];
        }
        input.uv = Vec4((u * item.uvScale[0] + item.uvOffset[0]) * material.tiling[0] + material.offset[0],
                        (v * item.uvScale[1] + item.uvOffset[1]) * material.tiling[1] + material.offset[1], 0, 0);
        input.particle = item.particleInput + (vertex < m.particleData.size() ? m.particleData[vertex] : Vec4());
        input.custom = item.particleCustom + (vertex < m.particleCustom.size() ? m.particleCustom[vertex] : Vec4());
        input.baseColor = Vec4(material.baseColor.r, material.baseColor.g, material.baseColor.b, material.opacity);
        if (vertex < m.colors.size()) {
            const Vec4 c = m.colors[vertex];
            input.baseColor = Vec4(input.baseColor.x*c.x, input.baseColor.y*c.y, input.baseColor.z*c.z, input.baseColor.w*c.w);
        }
        input.position = Vec4(wpos, 1);
        input.normal = Vec4(normal, 0);
        fn(vertex, wpos, ShaderVertexOffset(*material.shader, input, material.shaderUniforms));
    }
}

float OffsetMargin(const Material& material) {
    if (!material.shader || material.shader->offset < 0) return 0;
    return material.shader->maxOffset >= 0 ? material.shader->maxOffset : kDefaultOffsetMargin;
}

void OffsetRange(const Material& material, Vec3& lo, Vec3& hi) {
    if (material.shader && material.shader->offset >= 0 && material.shader->hasOffsetBounds) {
        lo = material.shader->offsetMin;
        hi = material.shader->offsetMax;
        return;
    }
    const float margin = OffsetMargin(material);
    lo = Vec3(-margin, -margin, -margin);
    hi = Vec3(margin, margin, margin);
}

void ExtendOffsetBounds(const std::vector<RenderItem>& items, const std::vector<DrawCall>& draws, Vec3& lo, Vec3& hi) {
    for (const DrawCall& draw : draws) {
        const RenderItem& it = items[draw.item];
        if (it.unlit || !draw.material.shader || draw.material.shader->offset < 0) continue;
        Vec3 a, b, offsetLo, offsetHi;
        OffsetRange(draw.material, offsetLo, offsetHi);
        ItemWorldBounds(it, offsetLo, offsetHi, a, b);
        lo = Vec3(std::min(lo.x, a.x), std::min(lo.y, a.y), std::min(lo.z, a.z));
        hi = Vec3(std::max(hi.x, b.x), std::max(hi.y, b.y), std::max(hi.z, b.z));
    }
}

void ItemWorldBounds(const RenderItem& item, float margin, Vec3& lo, Vec3& hi) {
    ItemWorldBounds(item, Vec3(-margin, -margin, -margin), Vec3(margin, margin, margin), lo, hi);
}

void ItemWorldBounds(const RenderItem& item, const Vec3& offsetLo, const Vec3& offsetHi, Vec3& lo, Vec3& hi) {
    lo = Vec3(1e30f, 1e30f, 1e30f);
    hi = Vec3(-1e30f, -1e30f, -1e30f);
    const Vec3 &a = item.boundsMin, &b = item.boundsMax;
    for (int c = 0; c < 8; ++c) {
        const Vec3 p = item.world.TransformPoint(Vec3(c & 1 ? b.x : a.x, c & 2 ? b.y : a.y, c & 4 ? b.z : a.z));
        lo = Vec3(std::min(lo.x, p.x + offsetLo.x), std::min(lo.y, p.y + offsetLo.y), std::min(lo.z, p.z + offsetLo.z));
        hi = Vec3(std::max(hi.x, p.x + offsetHi.x), std::max(hi.y, p.y + offsetHi.y), std::max(hi.z, p.z + offsetHi.z));
    }
}

bool BoxInFrustum(const Mat4& viewProj, const Vec3& lo, const Vec3& hi, bool depth) {
    int outside[6] = {0, 0, 0, 0, 0, 0};
    for (int c = 0; c < 8; ++c) {
        const Vec4 p = viewProj * Vec4(c & 1 ? hi.x : lo.x, c & 2 ? hi.y : lo.y, c & 4 ? hi.z : lo.z, 1.0f);
        outside[0] += p.x < -p.w;
        outside[1] += p.x > p.w;
        outside[2] += p.y < -p.w;
        outside[3] += p.y > p.w;
        outside[4] += p.z < -p.w;
        outside[5] += p.z > p.w;
    }
    for (int plane = 0; plane < (depth ? 6 : 4); ++plane)
        if (outside[plane] == 8) return false;
    return true;
}

RenderSpotLight MakeRenderSpotLight(const Scene& scene, EntityId id) {
    RenderSpotLight result;
    result.id = id;
    const SpotLight* light = scene.Get<SpotLight>(id);
    if (!light) return result;
    auto finite = [](float value, float fallback) { return std::isfinite(value) ? value : fallback; };
    result.pos = scene.WorldMatrix(id).TransformPoint(Vec3());
    // Apply rotations from child to root, never scale/shear (including zero or mirrored scales).
    Vec3 direction(0, 0, -1);
    for (EntityId current = id; current != kNullEntity;) {
        const EntityRecord* record = scene.Record(current);
        if (!record) break;
        if (const Transform* transform = scene.Get<Transform>(current))
            direction = Mat4::RotationEuler(transform->rotation).TransformDir(direction);
        current = record->parent;
    }
    result.dir = Normalize(direction);
    const float intensity = std::max(0.0f, finite(light->intensity, 0.0f));
    result.color = Color(std::max(0.0f, finite(light->color.r, 0.0f)), std::max(0.0f, finite(light->color.g, 0.0f)),
                         std::max(0.0f, finite(light->color.b, 0.0f))) * intensity;
    result.range = std::max(0.01f, finite(light->range, 6.0f));
    const float outer = Clamp(finite(light->outerAngle, 30.0f), 0.1f, 89.0f);
    const float inner = Clamp(finite(light->innerAngle, 20.0f), 0.0f, outer);
    result.innerCos = std::cos(Radians(inner));
    result.outerCos = std::cos(Radians(outer));
    result.shadows = light->shadows;
    return result;
}

RenderLights GatherRenderLights(const Scene& scene, AssetManager* assets) {
    RenderLights lights;
    for (const auto& kv : scene.Pool<DirectionalLight>()) {
        RenderDirLight l;
        l.dir = Normalize(scene.WorldMatrix(kv.first).TransformDir(Vec3(0, 0, -1)));
        l.color = kv.second.color * kv.second.intensity;
        lights.ambient = lights.ambient + kv.second.ambient;
        lights.groundAmbient = lights.groundAmbient + (kv.second.hemisphere ? kv.second.groundAmbient : kv.second.ambient);
        lights.hemisphere = lights.hemisphere || kv.second.hemisphere;
        if (lights.dirs.empty()) {
            lights.shadows = kv.second.shadows;
            lights.shadowStrength = kv.second.shadowStrength;
            lights.shadowDistance = std::isfinite(kv.second.shadowDistance) ? std::max(0.0f, kv.second.shadowDistance) : 0.0f;
            lights.shadowCascades = std::clamp(kv.second.shadowCascades, 1, kMaxShadowCascades);
        }
        lights.dirs.push_back(l);
    }
    if (lights.dirs.empty()) {
        lights.dirs.push_back({Normalize(Vec3(-0.4f, -1.0f, -0.3f)), Color(1, 1, 1)});
        lights.ambient = Color(0.25f, 0.25f, 0.28f);
    }
    for (const auto& kv : scene.Pool<PointLight>()) {
        lights.points.push_back({scene.WorldMatrix(kv.first).TransformPoint(Vec3(0, 0, 0)), kv.second.color * kv.second.intensity,
                                 std::max(0.01f, kv.second.range), kv.first, kv.second.shadows});
    }
    for (const auto& kv : scene.Pool<SpotLight>()) {
        if (lights.spots.size() == static_cast<size_t>(kMaxSpotLights)) break;
        lights.spots.push_back(MakeRenderSpotLight(scene, kv.first));
    }
    for (const auto& kv : scene.Pool<Skybox>()) {  // first one wins
        const Skybox& s = kv.second;
        const float i = std::max(0.0f, s.intensity);
        RenderEnvironment& e = lights.env;
        e.present = true;
        e.sky = s.sky * i; e.horizon = s.horizon * i; e.ground = s.ground * i;
        e.reflections = Clamp(s.reflections, 0.0f, 1.0f);
        e.background = s.background;
        e.sun = s.sun && !lights.dirs.empty();
        if (e.sun) {
            e.sunDir = lights.dirs[0].dir;         // the first directional light is the sun
            e.sunColor = lights.dirs[0].color;
            e.sunCos = std::cos(Radians(Clamp(s.sunSize, 0.1f, 30.0f)));
        }
        e.texture = s.texture;
        if (!s.texture.empty() && assets) e.map = assets->GetEnvironment(s.texture);  // a failed load keeps the colors
        if (e.map) {
            e.intensity = i;
            const float yaw = Radians(std::isfinite(s.rotation) ? s.rotation : 0.0f);
            e.yawCos = std::cos(yaw);
            e.yawSin = std::sin(yaw);
            for (int k = 0; k < 9; ++k) e.sh[k] = e.map->sh[k] * i;
            e.sun = false;  // the image has its own sun
        }
        break;
    }
    for (const auto& kv : scene.Pool<ReflectionProbe>()) {
        const ReflectionProbe& r = kv.second;
        auto positive = [](float v, float least) { return std::isfinite(v) ? std::max(least, v) : least; };
        RenderProbe p;
        p.id = kv.first;
        p.pos = scene.WorldMatrix(kv.first).TransformPoint(Vec3(0, 0, 0));
        const Vec3 half(positive(r.size.x, 0.01f) * 0.5f, positive(r.size.y, 0.01f) * 0.5f, positive(r.size.z, 0.01f) * 0.5f);
        const Vec3 center = p.pos + r.offset;
        p.lo = center - half;
        p.hi = center + half;
        p.blend = positive(r.blend, 1e-3f);
        p.intensity = positive(r.intensity, 0.0f);
        p.boxProjection = r.boxProjection;
        p.ambient = r.ambient;
        p.resolution = std::clamp(r.resolution, 16, 256);
        p.nearPlane = positive(r.nearPlane, 0.01f);
        p.farPlane = std::max(p.nearPlane * 2.0f, positive(r.farPlane, 1.0f));
        lights.probes.push_back(p);
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

ShadowFit FitShadowToView(const Mat4& view, const Mat4& proj, float distance, const Vec3& lo, const Vec3& hi,
                          const Vec3& dir, int mapSize) {
    // Corners of the view frustum from the near plane to `distance` (NDC z of that view depth).
    const bool ortho = proj.at(3, 3) == 1.0f;
    const float p22 = proj.at(2, 2), p23 = proj.at(2, 3);
    const float farZ = std::min(1.0f, ortho ? -p22 * distance + p23 : (-p22 * distance + p23) / distance);
    const Mat4 toWorld = (proj * view).Inverse();
    Vec3 corners[8];
    Vec3 center;
    for (int c = 0; c < 8; ++c) {
        const Vec4 p = toWorld * Vec4(c & 1 ? 1.0f : -1.0f, c & 2 ? 1.0f : -1.0f, c & 4 ? farZ : -1.0f, 1.0f);
        corners[c] = p.xyz() * (1.0f / p.w);
        center = center + corners[c] * 0.125f;
    }
    // A sphere around the slice keeps the map's size fixed while the camera turns.
    float radius = 0.5f;
    for (const Vec3& p : corners) radius = std::max(radius, Length(p - center));
    radius = std::ceil(radius);
    if (!std::isfinite(radius) || Length(hi - lo) * 0.5f <= radius) return FitShadow(lo, hi, dir, mapSize);
    // Toward the light: far enough back that every caster of the scene box is in front of the near plane.
    float back = radius * 2.0f;
    for (int c = 0; c < 8; ++c) {
        const Vec3 p(c & 1 ? hi.x : lo.x, c & 2 ? hi.y : lo.y, c & 4 ? hi.z : lo.z);
        back = std::max(back, Dot(p - center, dir * -1.0f) + 1.0f);
    }
    const Vec3 up = std::fabs(dir.y) > 0.95f ? Vec3(0, 0, 1) : Vec3(0, 1, 0);
    const Mat4 lview = Mat4::LookAt(center - dir * back, center, up);
    const Mat4 lproj = Mat4::Orthographic(radius, 1.0f, 0.01f, back + radius * 2.0f);
    ShadowFit fit;
    fit.viewProj = lproj * lview;
    // Snap to whole texels: the world origin lands on a texel corner of the map.
    const Vec4 origin = fit.viewProj * Vec4(0, 0, 0, 1);
    const float texels = static_cast<float>(mapSize) * 0.5f;
    const float dx = std::round(origin.x * texels) / texels - origin.x, dy = std::round(origin.y * texels) / texels - origin.y;
    fit.viewProj = Mat4::Translation(Vec3(dx, dy, 0)) * fit.viewProj;
    fit.texelWorld = 2.0f * radius / static_cast<float>(mapSize);
    return fit;
}

ShadowCascadeSet FitShadowCascades(const Mat4& view, const Mat4& proj, float distance, int count,
                                  const Vec3& lo, const Vec3& hi, const Vec3& dir, int legacyMapSize) {
    ShadowCascadeSet result;
    result.fits[0] = distance > 0 ? FitShadowToView(view, proj, distance, lo, hi, dir, legacyMapSize)
                                : FitShadow(lo, hi, dir, legacyMapSize);
    count = std::clamp(count, 1, kMaxShadowCascades);
    if (count == 1) return result;
    const bool ortho = proj.at(3, 3) == 1.0f;
    const float p22 = proj.at(2, 2), p23 = proj.at(2, 3);
    const float nearDepth = ortho ? (p23 + 1.0f) / p22 : p23 / (p22 - 1.0f);
    const float cameraFar = ortho ? (p23 - 1.0f) / p22 : p23 / (p22 + 1.0f);
    if (!std::isfinite(nearDepth) || !std::isfinite(cameraFar) || nearDepth <= 0 || cameraFar <= nearDepth) return result;
    const float farDepth = std::min(cameraFar, distance > 0 && std::isfinite(distance)
        ? Clamp(distance, nearDepth + 0.01f, 100000.0f) : 1000.0f);
    if (farDepth <= nearDepth) return result;
    result.count = count;
    const Mat4 toWorld = (proj * view).Inverse();
    const Vec3 up = std::fabs(dir.y) > 0.95f ? Vec3(0, 0, 1) : Vec3(0, 1, 0);
    // Rotation stays anchored to world axes, independent of camera position.
    const Mat4 lightRotation = Mat4::LookAt(Vec3(), dir, up);
    for (int i = 0; i < count; ++i) {
        const float t = static_cast<float>(i + 1) / static_cast<float>(count);
        const float uniform = Lerp(nearDepth, farDepth, t);
        const float logarithmic = nearDepth * std::pow(farDepth / nearDepth, t);
        result.splits[static_cast<size_t>(i)] = i == count - 1 ? farDepth : ortho ? uniform : (uniform + logarithmic) * 0.5f;
        const float previous = i == 0 ? nearDepth : result.splits[static_cast<size_t>(i - 1)];
        result.blendStarts[static_cast<size_t>(i)] = Lerp(previous, result.splits[static_cast<size_t>(i)], 0.9f);
    }
    for (int i = 0; i < count; ++i) {
        const float from = i == 0 ? nearDepth : result.blendStarts[static_cast<size_t>(i - 1)];
        const float to = result.splits[static_cast<size_t>(i)];
        Vec3 corners[8], center;
        for (int c = 0; c < 8; ++c) {
            const float depth = c & 4 ? to : from;
            const float z = ortho ? -p22 * depth + p23 : (-p22 * depth + p23) / depth;
            const Vec4 world = toWorld * Vec4(c & 1 ? 1.0f : -1.0f, c & 2 ? 1.0f : -1.0f, z, 1);
            corners[c] = world.xyz() / world.w;
            center += corners[c] * 0.125f;
        }
        float radius = 0.5f;
        for (const Vec3& corner : corners) radius = std::max(radius, Length(corner - center));
        // Quantized sphere extent stays fixed as the camera rotates; add a texel guard for snapping/PCF.
        radius = std::ceil(radius * 16.0f) / 16.0f;
        radius *= static_cast<float>(kCascadeMapSize) / static_cast<float>(kCascadeMapSize - 4);
        const float texel = 2.0f * radius / static_cast<float>(kCascadeMapSize);
        const Vec3 lightCenter = lightRotation.TransformPoint(center);
        const float snappedX = std::round(lightCenter.x / texel) * texel;
        const float snappedY = std::round(lightCenter.y / texel) * texel;
        float minZ = 1e30f, maxZ = -1e30f;
        // All casters toward the light remain in range, even when outside the camera's slice.
        for (int c = 0; c < 8; ++c) {
            const Vec3 p = lightRotation.TransformPoint(Vec3(c & 1 ? hi.x : lo.x, c & 2 ? hi.y : lo.y, c & 4 ? hi.z : lo.z));
            minZ = std::min(minZ, p.z); maxZ = std::max(maxZ, p.z);
            const float receiverZ = lightRotation.TransformPoint(corners[c]).z;
            minZ = std::min(minZ, receiverZ); maxZ = std::max(maxZ, receiverZ);
        }
        Mat4 projection;
        projection.at(0, 0) = 1.0f / radius; projection.at(0, 3) = -snappedX / radius;
        projection.at(1, 1) = 1.0f / radius; projection.at(1, 3) = -snappedY / radius;
        minZ -= 1.0f; maxZ += 1.0f;
        projection.at(2, 2) = -2.0f / (maxZ - minZ);
        projection.at(2, 3) = (maxZ + minZ) / (maxZ - minZ);
        result.fits[static_cast<size_t>(i)] = {projection * lightRotation, texel, texel * 0.5f / (maxZ - minZ)};
    }
    return result;
}

int ShadowCascadeIndex(const ShadowCascadeSet& cascades, float depth) {
    for (int i = 0; i < cascades.count; ++i) if (depth < cascades.splits[static_cast<size_t>(i)]) return i;
    return cascades.count;
}

float ShadowCascadeBlend(const ShadowCascadeSet& cascades, int index, float depth) {
    if (index < 0 || index >= cascades.count) return 0;
    const size_t i = static_cast<size_t>(index);
    return Clamp((depth - cascades.blendStarts[i]) / (cascades.splits[i] - cascades.blendStarts[i]), 0.0f, 1.0f);
}

}  // namespace oe
