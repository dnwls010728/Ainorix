#include "render/Decals.h"
#include <algorithm>
#include <cmath>
#include "assets/Assets.h"
#include "render/Renderer.h"
#include "render/RenderScene.h"
#include "scene/Components.h"

namespace oe {
namespace {
bool Finite(const Vec3& p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
float Bounded(float value, float lo, float hi, float fallback) { return std::isfinite(value) ? Clamp(value,lo,hi) : fallback; }
}

std::vector<RenderDecal> GatherDecals(const Scene& scene, AssetManager* assets, const RenderView& view) {
    std::vector<RenderDecal> result;
    for (const auto& pair : scene.Pool<Decal>()) {
        const Decal& source = pair.second;
        if (!source.enabled || pair.first == view.excludeEntity || !std::isfinite(source.opacity) || source.opacity <= 0 ||
            !Finite(source.size) || source.size.x <= 0 || source.size.y <= 0 || source.size.z <= 0) continue;
        const Vec3 size(std::min(source.size.x,1000.0f), std::min(source.size.y,1000.0f), std::min(source.size.z,1000.0f));
        const Mat4 world = scene.WorldMatrix(pair.first);
        const float determinant = Dot(world.TransformDir(Vec3(1,0,0)), Cross(world.TransformDir(Vec3(0,1,0)),world.TransformDir(Vec3(0,0,1))));
        if (!std::isfinite(determinant) || std::fabs(determinant) < 1e-8f) continue;
        Vec3 lo(1e30f,1e30f,1e30f), hi(-1e30f,-1e30f,-1e30f);
        for (int corner = 0; corner < 8; ++corner) {
            const Vec3 p = world.TransformPoint(Vec3(corner&1 ? size.x*0.5f : -size.x*0.5f,
                                                   corner&2 ? size.y*0.5f : -size.y*0.5f,
                                                   corner&4 ? size.z*0.5f : -size.z*0.5f));
            lo = Vec3(std::min(lo.x,p.x),std::min(lo.y,p.y),std::min(lo.z,p.z));
            hi = Vec3(std::max(hi.x,p.x),std::max(hi.y,p.y),std::max(hi.z,p.z));
        }
        if (!Finite(lo) || !Finite(hi) || !BoxInFrustum(view.proj*view.view,lo,hi)) continue;
        RenderDecal decal;
        decal.id = pair.first; decal.order = source.order;
        decal.worldToBox = Mat4::Scale(Vec3(1/size.x,1/size.y,1/size.z)) * world.Inverse();
        decal.normal = Normalize(world.Inverse().Transposed().TransformDir(Vec3(0,1,0)));
        decal.color = Color(Bounded(source.color.r,0,1,1),Bounded(source.color.g,0,1,1),Bounded(source.color.b,0,1,1));
        decal.opacity = Clamp(source.opacity,0,1);
        decal.edgeFade = Bounded(source.edgeFade,0,0.5f,0.1f);
        decal.normalThreshold = Bounded(source.normalThreshold,-1,0.99f,0.2f);
        result.push_back(std::move(decal));
    }
    std::sort(result.begin(),result.end(),[](const RenderDecal& a,const RenderDecal& b) {
        return a.order != b.order ? a.order < b.order : a.id < b.id;
    });
    if (result.size() > kMaxDecals) result.resize(kMaxDecals);
    for (RenderDecal& decal : result) {
        const Decal& source = *scene.Get<Decal>(decal.id);
        if (!source.texture.empty()) {
            if (assets) decal.texture = assets->GetTexture(source.texture);
            if (!decal.texture) decal.color = Color(1,0,1);  // Same visible missing-asset signal as meshes/materials.
        }
    }
    return result;
}

float DecalCoverage(const RenderDecal& decal, const Vec3& position, const Vec3& normal, float& u, float& v) {
    const Vec3 local = decal.worldToBox.TransformPoint(position);
    const float edge = 0.5f - std::max({std::fabs(local.x),std::fabs(local.y),std::fabs(local.z)});
    if (edge < 0) return 0;
    u = local.x + 0.5f; v = 0.5f - local.z;
    const float fade = decal.edgeFade > 0 ? Clamp(edge/decal.edgeFade,0,1) : 1;
    const float facing = Clamp((Dot(normal,decal.normal)-decal.normalThreshold)/(1-decal.normalThreshold),0,1);
    return decal.opacity * fade * facing;
}

Color ApplyDecals(const std::vector<RenderDecal>& decals, const Texture* atlas, const Vec3& position, const Vec3& normal, Color base) {
    if (!atlas) return base;
    for (size_t i = 0; i < decals.size(); ++i) {
        float u = 0, v = 0;
        float alpha = DecalCoverage(decals[i],position,normal,u,v);
        if (alpha <= 0) continue;
        u = (static_cast<float>(i%4*kDecalTileSize)+1+u*(kDecalTileSize-2))/kDecalAtlasSize;
        v = (static_cast<float>(i/4*kDecalTileSize)+1+v*(kDecalTileSize-2))/kDecalAtlasSize;
        float textureAlpha = 1;
        const Color color = atlas->Sample(u,v,false,&textureAlpha) * decals[i].color;
        alpha *= textureAlpha;
        base = base*(1-alpha) + color*alpha;
    }
    return base;
}

std::shared_ptr<const Texture> DecalAtlas::Prepare(const std::vector<RenderDecal>& decals) {
    if (decals.empty()) {
        atlas_.reset(); sources_.fill(nullptr); versions_.fill(0); count_ = 0;
        return nullptr;
    }
    bool changed = !atlas_ || count_ != decals.size();
    for (size_t i = 0; i < decals.size(); ++i)
        changed = changed || sources_[i] != decals[i].texture || versions_[i] != (decals[i].texture ? decals[i].texture->version : 0);
    if (!changed) return atlas_;
    atlas_ = std::make_shared<Texture>();
    atlas_->width = atlas_->height = kDecalAtlasSize;
    atlas_->texels.assign(static_cast<size_t>(kDecalAtlasSize)*kDecalAtlasSize,0xFFFFFFFF);
    sources_.fill(nullptr); versions_.fill(0); count_ = decals.size();
    for (size_t i = 0; i < decals.size(); ++i) {
        const auto& texture = decals[i].texture;
        sources_[i] = texture; versions_[i] = texture ? texture->version : 0;
        if (!texture || texture->texels.empty()) continue;
        const int ox = static_cast<int>(i%4)*kDecalTileSize, oy = static_cast<int>(i/4)*kDecalTileSize;
        for (int y = 0; y < kDecalTileSize; ++y) for (int x = 0; x < kDecalTileSize; ++x) {
            const float u = (static_cast<float>(std::clamp(x,1,kDecalTileSize-2))-0.5f)/(kDecalTileSize-2);
            const float v = (static_cast<float>(std::clamp(y,1,kDecalTileSize-2))-0.5f)/(kDecalTileSize-2);
            float alpha = 1;
            const Color c = texture->Sample(Clamp(u,0.5f/texture->width,1-0.5f/texture->width),
                                             Clamp(v,0.5f/texture->height,1-0.5f/texture->height),false,&alpha);
            auto byte = [](float value) { return static_cast<uint32_t>(Clamp(value,0,1)*255+0.5f); };
            atlas_->texels[static_cast<size_t>(oy+y)*kDecalAtlasSize+static_cast<size_t>(ox+x)] = byte(c.r)|(byte(c.g)<<8)|(byte(c.b)<<16)|(byte(alpha)<<24);
        }
    }
    return atlas_;
}
}  // namespace oe
