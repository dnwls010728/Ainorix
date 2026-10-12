// Reflection probes: taking a probe's picture of the scenery and choosing the probes a frame uses.
// See docs/ENVIRONMENT.md.
#include <algorithm>
#include <chrono>
#include <cmath>

#include "assets/Assets.h"
#include "core/Log.h"
#include "render/RenderScene.h"
#include "render/Renderer.h"
#include "scene/Components.h"

namespace oe {

bool ProbeShows(const Scene& scene, EntityId id) {
    for (int depth = 0; id != kNullEntity && depth < 64; ++depth) {
        if (scene.Get<ReflectionProbe>(id) || scene.Get<RigidBody>(id) || scene.Get<CharacterBody>(id) || scene.Get<RigidBody2D>(id) ||
            scene.Get<CharacterBody2D>(id) || scene.Get<Animator>(id) || scene.Get<VFXSystem>(id) || scene.Get<Trail>(id))
            return false;
        const EntityRecord* record = scene.Record(id);
        id = record ? record->parent : kNullEntity;
    }
    return true;
}

std::shared_ptr<const EnvironmentMap> CaptureProbe(const Scene& scene, AssetManager* assets, const RenderProbe& probe) {
    if (!assets || assets->ProbeScene() != &scene) return nullptr;
    auto& pictures = assets->ProbePictures();
    const auto found = pictures.find(probe.id);
    if (found != pictures.end()) return found->second;
    const auto start = std::chrono::steady_clock::now();

    // Six faces around the capture position, each one texel wider than a quarter turn on every side
    // so the equirectangular resample below never reads across an edge.
    const int res = probe.resolution, size = res + 2;
    const float tanHalf = static_cast<float>(size) / static_cast<float>(res);
    struct Face {
        Vec3 forward, up, right;
        std::vector<Color> color;
    };
    Face faces[6];
    const Vec3 forwards[6] = {Vec3(1, 0, 0), Vec3(-1, 0, 0), Vec3(0, 1, 0), Vec3(0, -1, 0), Vec3(0, 0, 1), Vec3(0, 0, -1)};
    RenderView background;  // without a Skybox the picture's background is the game camera's clear color
    MakeSceneView(scene, 1.0f, background);
    SoftwareRenderer renderer(assets);
    RenderTarget target;
    target.Resize(size, size);
    for (int i = 0; i < 6; ++i) {
        Face& face = faces[i];
        face.forward = forwards[i];
        const Vec3 hint = i == 2 ? Vec3(0, 0, 1) : i == 3 ? Vec3(0, 0, -1) : Vec3(0, 1, 0);
        face.right = Normalize(Cross(face.forward, hint));
        face.up = Cross(face.right, face.forward);
        RenderView view;
        view.view = Mat4::LookAt(probe.pos, probe.pos + face.forward, hint);
        view.proj = Mat4::Perspective(2.0f * std::atan(tanHalf), 1.0f, probe.nearPlane, probe.farPlane);
        view.eye = probe.pos;
        view.clearColor = background.clearColor;
        view.drawUI = false;
        view.sceneColor = &face.color;
        renderer.Render(scene, view, target);
    }

    // Equirectangular image in world axes (the inverse of EnvUV): 4 x 2 faces wide and high.
    const int width = res * 4, height = res * 2;
    std::vector<float> rgb(static_cast<size_t>(width) * static_cast<size_t>(height) * 3);
    for (int y = 0; y < height; ++y) {
        const float theta = (static_cast<float>(y) + 0.5f) / static_cast<float>(height) * kPi;
        for (int x = 0; x < width; ++x) {
            const float phi = ((static_cast<float>(x) + 0.5f) / static_cast<float>(width) - 0.5f) * 2.0f * kPi;
            const Vec3 d(std::sin(theta) * std::sin(phi), std::cos(theta), -std::sin(theta) * std::cos(phi));
            const float ax = std::fabs(d.x), ay = std::fabs(d.y), az = std::fabs(d.z);
            const int index = ax >= ay && ax >= az ? (d.x > 0 ? 0 : 1) : ay >= az ? (d.y > 0 ? 2 : 3) : (d.z > 0 ? 4 : 5);
            const Face& face = faces[index];
            const float along = Dot(d, face.forward);
            const float fx = (Dot(d, face.right) / along / tanHalf * 0.5f + 0.5f) * static_cast<float>(size) - 0.5f;
            const float fy = (0.5f - Dot(d, face.up) / along / tanHalf * 0.5f) * static_cast<float>(size) - 0.5f;
            const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, size - 1), y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, size - 1);
            const int x1 = std::min(x0 + 1, size - 1), y1 = std::min(y0 + 1, size - 1);
            const float tx = Clamp(fx - static_cast<float>(x0), 0.0f, 1.0f), ty = Clamp(fy - static_cast<float>(y0), 0.0f, 1.0f);
            auto at = [&](int px, int py) { return face.color[static_cast<size_t>(py) * static_cast<size_t>(size) + static_cast<size_t>(px)]; };
            const Color c = (at(x0, y0) * (1.0f - tx) + at(x1, y0) * tx) * (1.0f - ty) + (at(x0, y1) * (1.0f - tx) + at(x1, y1) * tx) * ty;
            float* out = &rgb[(static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 3];
            out[0] = c.r;
            out[1] = c.g;
            out[2] = c.b;
        }
    }
    std::shared_ptr<const EnvironmentMap> map = BuildEnvironmentMap(rgb.data(), width, height);
    pictures[probe.id] = map;
    OE_LOG_INFO("render", "reflection probe %u: picture taken (%d px faces, %.1f ms)", static_cast<unsigned>(probe.id), res,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    return map;
}

std::vector<RenderProbe> ActiveProbes(const Scene& scene, AssetManager* assets, const RenderLights& lights, const Mat4& viewProj, const Vec3& eye) {
    std::vector<RenderProbe> out;
    if (!assets || assets->ProbeScene() != &scene || lights.probes.empty()) return out;
    struct Pick {
        float distance;
        const RenderProbe* probe;
    };
    std::vector<Pick> picks;
    for (const RenderProbe& p : lights.probes) {
        if (p.intensity <= 0.0f) continue;
        const Vec3 fade(p.blend, p.blend, p.blend);
        const Vec3 lo = p.lo - fade, hi = p.hi + fade;
        if (!BoxInFrustum(viewProj, lo, hi)) continue;
        const Vec3 away(std::max({lo.x - eye.x, eye.x - hi.x, 0.0f}), std::max({lo.y - eye.y, eye.y - hi.y, 0.0f}),
                        std::max({lo.z - eye.z, eye.z - hi.z, 0.0f}));
        picks.push_back({Length(away), &p});
    }
    std::stable_sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) { return a.distance < b.distance; });
    if (picks.size() > static_cast<size_t>(kMaxProbes)) picks.resize(static_cast<size_t>(kMaxProbes));
    for (const Pick& pick : picks) {
        RenderProbe p = *pick.probe;
        p.map = CaptureProbe(scene, assets, p);
        if (!p.map || p.map->levels.empty()) continue;
        for (int k = 0; k < 9; ++k) p.sh[k] = p.map->sh[k] * p.intensity;
        out.push_back(std::move(p));
    }
    auto volume = [](const RenderProbe& p) { return (p.hi.x - p.lo.x) * (p.hi.y - p.lo.y) * (p.hi.z - p.lo.z); };
    std::stable_sort(out.begin(), out.end(), [&](const RenderProbe& a, const RenderProbe& b) {
        const float va = volume(a), vb = volume(b);
        return va != vb ? va < vb : a.id < b.id;
    });
    return out;
}

}  // namespace oe
