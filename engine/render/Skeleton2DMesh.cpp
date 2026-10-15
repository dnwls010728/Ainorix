#include "render/Skeleton2DMesh.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>

#include "assets/Assets.h"

namespace oe {
namespace {

std::mutex g_skeletonMeshMutex;
// Each entry keeps its skeleton alive, so a reloaded asset never reuses the address in a key.
struct CachedMesh {
    std::shared_ptr<const Skeleton2DDef> def;
    std::shared_ptr<const Mesh> mesh;
};
std::map<std::string, CachedMesh> g_skeletonMeshes;
constexpr size_t kMaxCachedSkeletonMeshes = 512;

struct Shown {
    int slot = 0;
    const Skeleton2DAttachment* attachment = nullptr;
    std::shared_ptr<const Texture> texture;
    Vec4 color;
};

std::shared_ptr<const Mesh> BuildMesh(const Skeleton2DDef& def, const std::vector<Shown>& shown) {
    auto mesh = std::make_shared<Mesh>();
    Skeleton2DPose setup = Skeleton2DSetupPose(def);
    Skeleton2DWorld(def, setup);
    for (size_t b = 0; b < def.bones.size(); ++b) {
        ModelJoint joint;
        joint.inverseBind = setup.world[b].Inverse();
        mesh->joints.push_back(joint);
    }
    const float ppu = std::max(0.01f, def.pixelsPerUnit);
    for (size_t d = 0; d < shown.size(); ++d) {
        const Shown& s = shown[d];
        const Skeleton2DAttachment& a = *s.attachment;
        const Skeleton2DSlot& slot = def.slots[static_cast<size_t>(s.slot)];
        const float texW = s.texture ? static_cast<float>(s.texture->width) : 64.0f;
        const float texH = s.texture ? static_cast<float>(s.texture->height) : 64.0f;
        const float rx = a.hasRect ? a.rect[0] : 0, ry = a.hasRect ? a.rect[1] : 0;
        const float rw = a.hasRect ? a.rect[2] : texW, rh = a.hasRect ? a.rect[3] : texH;
        const float sx = (a.width > 0 ? a.width : rw / ppu) / rw, sy = (a.height > 0 ? a.height : rh / ppu) / rh;
        // Rectangle pixels (y down) -> attachment space (pivot at 0, +Y up) -> the slot bone's setup space.
        const Mat4 place = setup.world[static_cast<size_t>(slot.bone)] * Skeleton2DLocalMatrix(a.x, a.y, a.rotation, a.scaleX, a.scaleY);
        std::vector<float> pixels = a.vertices;
        std::vector<uint32_t> triangles = a.triangles;
        if (!a.IsMesh()) {
            pixels = {0, rh, rw, rh, rw, 0, 0, 0};  // bottom-left, bottom-right, top-right, top-left
            triangles = {0, 1, 2, 0, 2, 3};
        }
        const uint32_t base = static_cast<uint32_t>(mesh->positions.size());
        const float z = static_cast<float>(d) * kSkeleton2DSlotStep;
        for (size_t v = 0; v < pixels.size() / 2; ++v) {
            const float px = pixels[v * 2], py = pixels[v * 2 + 1];
            const Vec3 local((px - a.pivot[0] * rw) * sx, ((rh - py) - a.pivot[1] * rh) * sy, 0);
            Vec3 p = place.TransformPoint(local);
            p.z = z;
            mesh->positions.push_back(p);
            mesh->normals.push_back(Vec3(0, 0, 1));
            mesh->uvs.push_back((rx + px) / texW);
            mesh->uvs.push_back((ry + py) / texH);
            mesh->colors.push_back(s.color);
            SkinVertex skin;
            if (v < a.weights.size()) {
                for (size_t k = 0; k < a.weights[v].size() && k < 4; ++k) {
                    skin.joints[k] = static_cast<uint16_t>(a.weights[v][k].first);
                    skin.weights[k] = a.weights[v][k].second;
                }
            } else {
                skin.joints[0] = static_cast<uint16_t>(slot.bone);
                skin.weights[0] = 1;
            }
            mesh->skin.push_back(skin);
        }
        // One submesh per run of slots with the same image and blend.
        const bool additive = slot.additive;
        const bool sameAsLast = d > 0 && !mesh->submeshes.empty() && shown[d - 1].texture == s.texture &&
                                def.slots[static_cast<size_t>(shown[d - 1].slot)].additive == additive;
        if (!sameAsLast) {
            Material m;
            m.name = slot.name;
            m.baseTexture = s.texture;
            if (!s.texture) m.baseColor = Color(1, 0, 1);  // missing image: magenta, like missing meshes
            m.pixelArt = def.pixelArt;
            m.doubleSided = true;
            m.roughness = 1;
            if (additive) {
                m.alphaMode = AlphaMode::Blend;
                m.additive = true;
            }
            mesh->materials.push_back(m);
            Submesh sub;
            sub.firstIndex = static_cast<uint32_t>(mesh->indices.size());
            sub.material = static_cast<int>(mesh->materials.size() - 1);
            mesh->submeshes.push_back(sub);
        }
        for (uint32_t index : triangles) mesh->indices.push_back(base + index);
        mesh->submeshes.back().indexCount = static_cast<uint32_t>(mesh->indices.size()) - mesh->submeshes.back().firstIndex;
    }
    mesh->ComputeBounds();
    if (mesh->joints.size() > Mesh::kDrawJoints) mesh->SplitSkinPalettes();
    return mesh;
}

}  // namespace

Skeleton2DDrawable BuildSkeleton2DDrawable(const std::shared_ptr<const Skeleton2DDef>& asset, const std::string& skin, const Skeleton2DPose& pose,
                                           AssetManager* assets) {
    const Skeleton2DDef& def = *asset;
    Skeleton2DDrawable out;
    std::vector<Shown> shown;
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%p|", static_cast<const void*>(&def));
    std::string key = buffer + skin;
    for (int slot : pose.drawOrder) {
        const size_t s = static_cast<size_t>(slot);
        if (s >= pose.attachments.size() || pose.attachments[s].empty()) continue;
        Shown item;
        item.slot = slot;
        item.attachment = def.FindAttachment(skin, slot, pose.attachments[s]);
        if (!item.attachment) continue;  // this skin has no such attachment: the slot shows nothing
        if (assets) item.texture = assets->GetTexture(item.attachment->image);
        if (!item.texture) out.missing.push_back(item.attachment->image);
        const Vec4 c = pose.colors[s];
        item.color = c;
        std::snprintf(buffer, sizeof(buffer), "|%d:%p:%p:%.4f,%.4f,%.4f,%.4f", slot, static_cast<const void*>(item.attachment),
                      static_cast<const void*>(item.texture.get()), static_cast<double>(c.x), static_cast<double>(c.y),
                      static_cast<double>(c.z), static_cast<double>(c.w));
        key += buffer;
        shown.push_back(std::move(item));
    }
    for (size_t b = 0; b < def.bones.size() && b < pose.world.size(); ++b) out.palette.push_back(pose.world[b]);
    if (shown.empty()) return out;
    {
        std::lock_guard<std::mutex> lock(g_skeletonMeshMutex);
        auto found = g_skeletonMeshes.find(key);
        if (found != g_skeletonMeshes.end()) out.mesh = found->second.mesh;
    }
    if (!out.mesh) {
        out.mesh = BuildMesh(def, shown);
        std::lock_guard<std::mutex> lock(g_skeletonMeshMutex);
        if (g_skeletonMeshes.size() >= kMaxCachedSkeletonMeshes) g_skeletonMeshes.clear();
        g_skeletonMeshes[key] = CachedMesh{asset, out.mesh};
    }
    for (size_t b = 0; b < out.palette.size(); ++b) out.palette[b] = out.palette[b] * out.mesh->joints[b].inverseBind;
    return out;
}

}  // namespace oe
