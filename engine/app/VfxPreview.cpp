#include "app/Vfx.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <set>

#include "app/Engine.h"
#include "core/FileSystem.h"
#include "core/Image.h"
#include "core/Log.h"
#include "render/GpuRenderer.h"
#include "render/RenderScene.h"
#include "scene/Components.h"
#include "scene/Systems.h"

namespace oe {
namespace {
constexpr int kMaxFrames = 3600;
constexpr uint64_t kMaxParticleSteps = 20000000;

double Number(const Json& args, const char* key, double fallback, double minimum, double maximum) {
    const double value = args[key].asNumber(fallback);
    if ((args.has(key) && !args[key].isNumber()) || !std::isfinite(value) || value < minimum || value > maximum)
        throw ApiError("invalid_range", std::string(key) + " is outside its valid range", "Use a finite number between " +
                       std::to_string(minimum) + " and " + std::to_string(maximum) + ".");
    return value;
}

Vec3 Triple(const Json& value, const char* label) {
    if (!value.isArray() || value.size() != 3) throw ApiError("invalid_vfx", std::string(label) + " must be [x,y,z]", "Use three finite numbers.");
    for (const Json& coordinate : value.items()) if (!coordinate.isNumber() || !std::isfinite(coordinate.asFloat()) || std::abs(coordinate.asFloat()) > 1e6f)
        throw ApiError("invalid_range", std::string(label) + " coordinates must be finite and bounded", "Use coordinates within +/-1000000.");
    return Vec3(value[0].asFloat(), value[1].asFloat(), value[2].asFloat());
}

struct Bounds {
    Vec3 lo{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    Vec3 hi{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
    bool empty = true;
    void Add(const Vec3& point) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z) ||
            std::abs(point.x) > 1e6f || std::abs(point.y) > 1e6f || std::abs(point.z) > 1e6f)
            throw ApiError("invalid_range", "preview geometry exceeds finite +/-1000000 coordinates",
                           "Reduce speed, gravity, sizes or transform scale/position, or preview a shorter duration.");
        empty = false;
        lo.x = std::min(lo.x, point.x); lo.y = std::min(lo.y, point.y); lo.z = std::min(lo.z, point.z);
        hi.x = std::max(hi.x, point.x); hi.y = std::max(hi.y, point.y); hi.z = std::max(hi.z, point.z);
    }
    void Add(const Bounds& other) { if (!other.empty) { Add(other.lo); Add(other.hi); } }
    Json ToJson() const {
        if (empty) return Json();
        Json out = Json::MakeObject(); out["min"] = Json(Json::Array{lo.x, lo.y, lo.z}); out["max"] = Json(Json::Array{hi.x, hi.y, hi.z}); return out;
    }
};

struct IsolatedEffect {
    Scene scene;
    EntityId root;
    Vec3 origin;
    InputState input;
    uint64_t capacity = 0;
    std::vector<double> motionTimes;
    std::vector<Vec3> motionPositions;

    IsolatedEffect(Engine& engine, EntityId id, const Json& args, int frames, int passes) : root(id) {
        CheckVfx(engine, id);
        const Scene& source = engine.GetScene();
        std::set<EntityId> selected{id}; std::vector<EntityId> pending{id};
        while (!pending.empty()) {
            const EntityId current = pending.back(); pending.pop_back();
            for (EntityId child : source.Children(current)) if (selected.insert(child).second) pending.push_back(child);
            if (selected.size() > 256) throw ApiError("invalid_range", "effect subtree exceeds 256 entities", "Preview a smaller subtree.");
        }
        std::set<EntityId> needed = selected;
        for (EntityId ancestor = source.Record(id)->parent; ancestor; ancestor = source.Record(ancestor)->parent) {
            needed.insert(ancestor);
            if (needed.size() > 512) throw ApiError("invalid_range", "effect hierarchy is too deep", "Use a shorter parent chain.");
        }
        // Only transforms and selected effect settings enter the preview. Scripts, physics,
        // unrelated geometry and transient particles/points cannot affect it.
        for (EntityId current : needed) {
            const EntityRecord& record = *source.Record(current);
            scene.Create(record.name, record.parent, current);
            if (const Transform* transform = source.Get<Transform>(current)) scene.Add<Transform>(current) = *transform;
            if (!selected.count(current)) continue;
            for (const char* name : {"ParticleEmitter", "Trail"}) {
                const ComponentType& type = *TypeRegistry::Find(name);
                const void* original = source.GetComponent(current, type);
                if (!original) continue;
                std::string error;
                if (!ApplyComponentJson(type, scene.AddComponent(current, type), ComponentToJson(type, original), &error))
                    throw ApiError("invalid_vfx", error);
            }
            if (const ParticleEmitter* emitter = scene.Get<ParticleEmitter>(current)) capacity += static_cast<uint64_t>(emitter->maxParticles);
            // Trail gather builds a mesh/texture; count it in the bounded work estimate too.
            if (const Trail* trail = scene.Get<Trail>(current)) capacity += static_cast<uint64_t>(trail->maxPoints);
        }
        // Entity ids need not follow hierarchy order. Restore links only after all records exist.
        for (EntityId current : needed) scene.Record(current)->parent = source.Record(current)->parent;
        for (EntityId current : selected) {
            const Mat4 world = scene.WorldMatrix(current);
            for (float number : world.m) if (!std::isfinite(number) || std::abs(number) > 1e6f)
                throw ApiError("invalid_range", "effect transform is non-finite or too large", "Use bounded finite transforms.");
        }
        if (capacity > 40000 || capacity * static_cast<uint64_t>(std::max(1, frames)) * static_cast<uint64_t>(passes) > kMaxParticleSteps)
            throw ApiError("invalid_range", "VFX request exceeds the particle-step budget", "Reduce duration/times or maxParticles/maxPoints; limit is 20000000 particle-steps and 40000 capacity.");
        if (!scene.Get<Transform>(root)) scene.Add<Transform>(root);
        origin = scene.Get<Transform>(root)->position;
        if (args.has("motion")) {
            if (!args["motion"].isArray() || args["motion"].size() < 2 || args["motion"].size() > 32)
                throw ApiError("invalid_vfx", "motion needs 2..32 keys", "Use [[seconds,[x,y,z]],...] offsets in the root parent's coordinates.");
            for (const Json& key : args["motion"].items()) {
                if (!key.isArray() || key.size() != 2 || !key[0].isNumber() || !std::isfinite(key[0].asNumber()) ||
                    key[0].asNumber() < 0 || key[0].asNumber() > 60 || (!motionTimes.empty() && key[0].asNumber() <= motionTimes.back()))
                    throw ApiError("invalid_vfx", "motion times must increase within 0..60 seconds", "Use [[0,[0,0,0]],[1,[2,0,0]]].");
                motionTimes.push_back(key[0].asNumber()); motionPositions.push_back(Triple(key[1], "motion"));
            }
            Move(0);
        }
    }
    void Move(double t) {
        if (motionTimes.empty()) return;
        Vec3 offset = motionPositions.front();
        if (t >= motionTimes.back()) offset = motionPositions.back();
        else for (size_t i = 1; i < motionTimes.size(); ++i) if (t <= motionTimes[i]) {
            const float factor = static_cast<float>(std::max(0.0, (t - motionTimes[i - 1]) / (motionTimes[i] - motionTimes[i - 1])));
            offset = motionPositions[i - 1] * (1 - factor) + motionPositions[i] * factor; break;
        }
        scene.Get<Transform>(root)->position = origin + offset;
    }
    void Step(int frame) {
        Move(static_cast<double>(frame) * Engine::kFixedDt);
        UpdateSystems(scene, input, static_cast<float>(Engine::kFixedDt));
        UpdateLateSystems(scene, static_cast<float>(Engine::kFixedDt));
    }
    bool Finished() const {
        for (const auto& pair : scene.Pool<ParticleEmitter>()) if (!ParticlesFinished(pair.second)) return false;
        for (const auto& pair : scene.Pool<Trail>()) if (pair.second.emitting || !pair.second.points.empty()) return false;
        return true;
    }
    size_t Count() const { size_t count = 0; for (const auto& pair : scene.Pool<ParticleEmitter>()) count += pair.second.particles.size(); return count; }
    size_t PointCount() const { size_t count = 0; for (const auto& pair : scene.Pool<Trail>()) count += pair.second.points.size(); return count; }
    Bounds Box(const Mat4& view = Mat4{}) const {
        Bounds bounds;
        for (const RenderItem& item : GatherRenderItems(scene, nullptr, view)) for (int corner = 0; corner < 8; ++corner)
            bounds.Add(item.world.TransformPoint(Vec3((corner & 1) ? item.boundsMax.x : item.boundsMin.x,
                (corner & 2) ? item.boundsMax.y : item.boundsMin.y, (corner & 4) ? item.boundsMax.z : item.boundsMin.z)));
        return bounds;
    }
};

RenderView View(const Json& args, const Bounds& bounds) {
    const Vec3 center = bounds.empty ? Vec3() : (bounds.lo + bounds.hi) * 0.5f;
    const float radius = bounds.empty ? 1 : std::max(0.2f, Length(bounds.hi - bounds.lo) * 0.5f);
    RenderView view = MakeLookAtView(center + Normalize(Vec3(0.3f, 0.25f, 1)) * (radius * 3.5f), center, 45, 1);
    view.proj = Mat4::Perspective(Radians(45), 1, std::max(0.001f, radius * 0.01f), std::max(10.0f, radius * 10));
    if (args.has("camera")) {
        const Json& camera = args["camera"];
        if (!camera.isObject() || !camera.has("eye") || !camera.has("target"))
            throw ApiError("invalid_vfx", "camera requires eye and target", "Use camera:{eye:[0,2,8],target:[0,1,0],fov:45}.");
        for (const auto& entry : camera.members()) if (entry.first != "eye" && entry.first != "target" && entry.first != "fov" &&
            entry.first != "projection" && entry.first != "orthoSize") throw ApiError("unknown_field", "unknown camera field '" + entry.first + "'", "Use eye,target,fov,projection,orthoSize.");
        const Vec3 eye = Triple(camera["eye"], "camera.eye"), target = Triple(camera["target"], "camera.target");
        if (Length(eye - target) < 0.001f) throw ApiError("invalid_vfx", "camera eye and target coincide", "Move eye away from target.");
        view = MakeLookAtView(eye, target, static_cast<float>(Number(camera, "fov", 45, 1, 170)), 1);
        const std::string projection = camera["projection"].asString("perspective");
        if (projection == "orthographic") view.proj = Mat4::Orthographic(static_cast<float>(Number(camera, "orthoSize", 5, 0.001, 10000)), 1, 0.001f, 1000000);
        else if (projection != "perspective") throw ApiError("invalid_vfx", "unknown camera projection", "Use perspective or orthographic.");
    }
    view.drawUI = false; view.clearColor = Color(0.02f, 0.025f, 0.04f);
    return view;
}

IRenderer& Renderer(Engine& engine, const Json& args) {
    const std::string name = args["renderer"].asString("software");
    if (name == "software") return engine.Renderer();
    if (name != "gpu") throw ApiError("invalid_vfx", "renderer must be software or gpu", "Omit renderer for deterministic software output.");
    std::string error;
    if (!engine.EnableGpu(nullptr, &error)) throw ApiError("gpu_unavailable", error, "Use renderer:software.");
    return *engine.Gpu();
}

std::vector<int> Frames(const Json& args) {
    const Json times = args.has("times") ? args["times"] : Json(Json::Array{0.1, 0.3, 0.6, 1.0});
    if (!times.isArray() || times.size() == 0 || times.size() > 16)
        throw ApiError("invalid_range", "times needs 1..16 entries", "Use increasing seconds within 0..60.");
    std::vector<int> frames;
    for (const Json& time : times.items()) {
        if (!time.isNumber() || !std::isfinite(time.asNumber()) || time.asNumber() < 0 || time.asNumber() > 60)
            throw ApiError("invalid_range", "times must be within 0..60 seconds", "Use finite nonnegative seconds.");
        const int frame = static_cast<int>(std::round(time.asNumber() / Engine::kFixedDt));
        if (!frames.empty() && frame <= frames.back()) throw ApiError("invalid_range", "times must map to increasing fixed frames", "Separate times by at least 1/60 second.");
        frames.push_back(frame);
    }
    return frames;
}
}  // namespace

Json PreviewVfx(Engine& engine, EntityId id, const Json& args) {
    const std::vector<int> frames = Frames(args);
    const int size = static_cast<int>(Number(args, "size", 256, 32, 512));
    // Two bounded passes give every tile the same camera without retaining full particle snapshots.
    IsolatedEffect first(engine, id, args, frames.back(), 2);
    Bounds bounds; int frame = 0;
    for (int sample : frames) { while (frame < sample) first.Step(++frame); bounds.Add(first.Box()); }
    const RenderView fixedView = View(args, bounds);
    IRenderer& renderer = Renderer(engine, args);
    IsolatedEffect effect(engine, id, args, frames.back(), 2);
    const int columns = std::min(4, static_cast<int>(frames.size()));
    const int rows = (static_cast<int>(frames.size()) + columns - 1) / columns;
    RenderTarget sheet; sheet.Resize(columns * size, rows * size);
    std::fill(sheet.color.begin(), sheet.color.end(), 0xff0a0605u);
    Json out = Json::MakeObject(); out["times"] = Json::MakeArray(); out["frames"] = Json::MakeArray(); frame = 0;
    for (size_t index = 0; index < frames.size(); ++index) {
        while (frame < frames[index]) effect.Step(++frame);
        RenderTarget tile; tile.Resize(size, size);
        RenderView view = fixedView; view.shaderTime = static_cast<float>(static_cast<double>(frame) * Engine::kFixedDt);
        renderer.Render(effect.scene, view, tile);
        for (int y = 0; y < size; ++y) {
            const int x0 = static_cast<int>(index) % columns * size, y0 = static_cast<int>(index) / columns * size;
            std::copy_n(tile.color.begin() + y * size, size, sheet.color.begin() + (y0 + y) * sheet.width + x0);
        }
        const double t = static_cast<double>(frame) * Engine::kFixedDt; out["times"].push(t);
        Json info = Json::MakeObject(); info["t"] = t; info["count"] = static_cast<uint64_t>(effect.Count());
        info["pointCount"] = static_cast<uint64_t>(effect.PointCount()); info["bbox"] = effect.Box(view.view).ToJson();
        info["hash"] = Format("%016llx", static_cast<unsigned long long>(tile.Hash())); out["frames"].push(info);
    }
    out["width"] = sheet.width; out["height"] = sheet.height; out["columns"] = columns; out["tileSize"] = size;
    out["renderer"] = renderer.Name(); out["hash"] = Format("%016llx", static_cast<unsigned long long>(sheet.Hash()));
    const Image image = sheet.ToImage();
    if (args.has("out")) {
        const std::string path = engine.ResolvePath(args["out"].asString());
        CreateDirectories(ParentPath(path));
        if (!WritePng(path, image)) throw ApiError("write_failed", "cannot write " + path, "Choose a writable project-relative output path.");
        out["path"] = path;
    }
    if (args["inline"].asBool(!args.has("out"))) out["png_base64"] = Base64Encode(EncodePng(image));
    return out;
}

Json AnalyzeVfx(Engine& engine, EntityId id, const Json& args) {
    const int end = static_cast<int>(std::round(Number(args, "duration", 5, Engine::kFixedDt, 60) / Engine::kFixedDt));
    const int interval = std::max(1, static_cast<int>(std::round(Number(args, "step", 0.25, Engine::kFixedDt, 60) / Engine::kFixedDt)));
    if (end > kMaxFrames || (end + interval - 1) / interval + 1 > 241)
        throw ApiError("invalid_range", "analysis exceeds 241 samples", "Increase step or reduce duration.");
    const bool render = args["render"].asBool(false);
    IsolatedEffect effect(engine, id, args, end, render ? 2 : 1);
    Json out = Json::MakeObject(); out["duration"] = static_cast<double>(end) * Engine::kFixedDt;
    out["step"] = static_cast<double>(interval) * Engine::kFixedDt; out["finishedAt"] = Json(); out["samples"] = Json::MakeArray();
    size_t peak = 0, pointPeak = 0; int peakFrame = 0; uint64_t budget = 0;
    for (const auto& pair : effect.scene.Pool<ParticleEmitter>()) budget += static_cast<uint64_t>(pair.second.maxParticles);
    Bounds bounds; std::vector<int> samples;
    for (int frame = 0; frame <= end; ++frame) {
        if (frame) effect.Step(frame);
        const size_t count = effect.Count();
        if (count > peak) { peak = count; peakFrame = frame; }
        pointPeak = std::max(pointPeak, effect.PointCount());
        if (effect.Finished() && out["finishedAt"].isNull()) out["finishedAt"] = static_cast<double>(frame) * Engine::kFixedDt;
        if (frame % interval != 0 && frame != end) continue;
        const Bounds box = effect.Box(); bounds.Add(box); samples.push_back(frame);
        Json sample = Json::MakeObject(); sample["t"] = static_cast<double>(frame) * Engine::kFixedDt;
        sample["count"] = static_cast<uint64_t>(count); sample["pointCount"] = static_cast<uint64_t>(effect.PointCount());
        sample["bbox"] = box.ToJson(); out["samples"].push(sample);
    }
    out["peakCount"] = static_cast<uint64_t>(peak); out["peakAt"] = static_cast<double>(peakFrame) * Engine::kFixedDt;
    out["peakPointCount"] = static_cast<uint64_t>(pointPeak); out["budget"] = budget;
    out["budgetRatio"] = budget ? static_cast<double>(peak) / static_cast<double>(budget) : 0;
    if (render) {
        const RenderView fixedView = View(args, bounds);
        IsolatedEffect rendered(engine, id, args, end, 2); int frame = 0;
        for (size_t i = 0; i < samples.size(); ++i) {
            while (frame < samples[i]) rendered.Step(++frame);
            RenderTarget tile; tile.Resize(128, 128); engine.Renderer().Render(rendered.scene, fixedView, tile);
            double luminance = 0;
            for (uint32_t color : tile.color) luminance += (0.2126 * (color & 255) + 0.7152 * ((color >> 8) & 255) + 0.0722 * ((color >> 16) & 255)) / 255;
            out["samples"][i]["meanLuminance"] = luminance / static_cast<double>(tile.color.size());
        }
    }
    const std::string text = out.dump();
    out["hash"] = Format("%016llx", static_cast<unsigned long long>(Fnv1a64(reinterpret_cast<const uint8_t*>(text.data()), text.size())));
    return out;
}
}  // namespace oe
