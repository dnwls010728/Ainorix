#include "app/Engine.h"
#include <algorithm>
#include <cmath>
#include "assets/Assets.h"
#include "scene/SpriteClip.h"
#include "assets/AnimationAsset.h"
#include "render/Mesh.h"
#include "scene/Pose.h"
#include "scene/PropertyTracks.h"

namespace oe {
namespace {

struct PreviewClip {
    std::string name, displayName;
    double duration = 0, end = 0, fps = 0, loopStart = 0, loopEnd = 0;
    bool loop = true;
    const Json* frames = nullptr;
    std::vector<AnimationMarker> events;
    SpriteClipTiming timing;
    Json tracks;  // the clip's property tracks
};

std::vector<PreviewClip> PreviewClips(Engine& engine, EntityId id) {
    const Scene& scene = engine.GetScene();
    std::vector<PreviewClip> out;
    if (const SpriteAnimation* sprite = scene.Get<SpriteAnimation>(id)) {
        if (!scene.Get<Sprite>(id)) throw ApiError("missing_component", "entity has no Sprite", "Add Sprite before previewing its clips.");
        for (const auto& item : SpriteAnimationClips(*sprite, engine.Assets()).members()) {
            const Json& clip = item.second;
            const Json& frames = clip["frames"];
            if (!clip.isObject() || !frames.isArray() || frames.size() == 0) continue;
            SpriteClipTiming timing; std::string timingError;
            if (!ParseSpriteClipTiming(clip, timing, &timingError)) throw ApiError("invalid_animation", timingError);
            const double fps = timing.fps;
            PreviewClip entry;
            entry.name = item.first; entry.displayName = clip["displayName"].asString(item.first); entry.fps = fps; entry.frames = &frames;
            entry.timing = timing; entry.duration = timing.duration;
            entry.end = timing.end;
            entry.loop = SpriteAnimationLoop(*sprite, clip); entry.loopEnd = entry.duration;
            entry.tracks = clip["tracks"];
            std::string error;
            if (!SpriteClipMarkers(clip["events"], timing, entry.events, &error))
                throw ApiError("invalid_animation_events", error, "Fix markers with animation.set_events before previewing.");
            out.push_back(std::move(entry));
        }
    } else {
        const MeshRenderer* renderer = scene.Get<MeshRenderer>(id);
        if (!renderer) throw ApiError("missing_component", "entity has no SpriteAnimation or model", "Select an entity with SpriteAnimation or MeshRenderer model clips.");
        std::string error;
        auto mesh = engine.Assets().GetMesh(renderer->mesh, &error);
        if (!mesh) throw ApiError("asset_error", error, "Check MeshRenderer.mesh and its event sidecar.");
        for (const AnimationClip& clip : mesh->clips) {
            PreviewClip entry; entry.name = clip.name; entry.duration = entry.end = clip.duration; entry.events = clip.events;
            entry.displayName = clip.displayName.empty() ? clip.name : clip.displayName;
            entry.loopStart = clip.loopStart; entry.loopEnd = clip.loopEnd < 0 ? clip.duration : clip.loopEnd;
            entry.tracks = clip.tracks;
            out.push_back(std::move(entry));
        }
    }
    if (out.empty()) throw ApiError("animation_not_found", "entity has no previewable clips", "Define sprite clips or import a model with animation.");
    return out;
}

}  // namespace

Json Engine::AnimationPreviewCall(const std::string& operation, const Json& args) {
    if (operation == "close") animationPreview_.reset();
    if (animationPreview_ && (InPlaySession() || !scene_.Exists(animationPreview_->id) || scene_.Epoch() != animationPreview_->epoch))
        animationPreview_.reset();
    if (operation == "preview") {
        if (InPlaySession()) throw ApiError("animation_preview_playing", "animation preview requires a stopped scene", "Stop the game before previewing animations.");
        EntityId id = animationPreview_ ? animationPreview_->id : kNullEntity;
        if (args.has("id")) {
            if (args["id"].isString()) id = scene_.FindByName(args["id"].asString());
            else {
                const double value = args["id"].asNumber();
                if (!std::isfinite(value) || value < 1 || value > 4294967295.0 || std::floor(value) != value)
                    throw ApiError("invalid_argument", "preview id must be an entity id or name", "Use a unique entity name or a positive uint32 id.");
                id = static_cast<EntityId>(value);
            }
        }
        if (!scene_.Exists(id)) throw ApiError("not_found", "preview entity not found", "Pass id of an animated entity to animation.preview.");
        auto clips = PreviewClips(*this, id);
        AnimationPreview next = animationPreview_ ? *animationPreview_ : AnimationPreview{};
        const bool changedEntity = !animationPreview_ || next.id != id;
        if (changedEntity) {
            next = AnimationPreview{}; next.id = id; next.epoch = scene_.Epoch();
            if (const SpriteAnimation* sprite = scene_.Get<SpriteAnimation>(id)) next.clip = sprite->clip;
            else if (const Animator* animator = scene_.Get<Animator>(id)) next.clip = animator->clip;
            if (next.clip.empty()) next.clip = clips.front().name;
            if (std::none_of(clips.begin(), clips.end(), [&](const PreviewClip& clip) { return clip.name == next.clip; }))
                next.clip = clips.front().name;
        }
        if (args.has("clip")) next.clip = args["clip"].asString();
        auto chosen = std::find_if(clips.begin(), clips.end(), [&](const PreviewClip& clip) { return clip.name == next.clip; });
        if (chosen == clips.end()) throw ApiError("animation_not_found", "unknown preview clip '" + next.clip + "'", "Use animation.preview_state to list clips, or pass a valid clip.");
        if (changedEntity || (animationPreview_ && next.clip != animationPreview_->clip)) { next.time = chosen->loopStart; next.loop = chosen->loop; }
        if (args.has("loop")) next.loop = args["loop"].asBool();
        if (!next.loop && !args.has("time") && (changedEntity || (animationPreview_ && next.clip != animationPreview_->clip))) next.time = 0;
        if (args.has("ik")) next.ik = args["ik"].asBool();
        if (args.has("playing")) next.playing = args["playing"].asBool();
        if (args.has("speed")) next.speed = args["speed"].asFloat();
        if (args.has("time")) next.time = args["time"].asNumber();
        if (!std::isfinite(next.time) || !std::isfinite(next.speed)) throw ApiError("invalid_argument", "preview time/speed must be finite", "Use finite time and speed values.");
        const double duration = next.loop ? chosen->duration : chosen->end;
        next.time = std::max(0.0, std::min(next.time, duration));
        if (args.has("frames")) {
            const double value = args["frames"].asNumber();
            if (!std::isfinite(value) || value < 0 || value > 3600 || std::floor(value) != value)
                throw ApiError("invalid_argument", "preview frames must be an integer in 0..3600", "Use frames:1 for one fixed step (1/60 s), independent of playing.");
            const double period = chosen->loopEnd - chosen->loopStart;
            if (next.loop) next.time = std::max(chosen->loopStart, std::min(next.time, chosen->loopEnd));
            next.time += value * kFixedDt * next.speed;
            if (next.loop && period > 0) { next.time = std::fmod(next.time - chosen->loopStart, period); if (next.time < 0) next.time += period; next.time += chosen->loopStart; }
            else {
                if (duration == 0 || next.time >= duration || next.time <= 0) next.playing = false;
                next.time = std::max(0.0, std::min(next.time, duration));
            }
        }
        animationPreview_ = std::make_unique<AnimationPreview>(std::move(next));
    }
    Json out = Json::MakeObject(); out["active"] = animationPreview_ != nullptr;
    if (!animationPreview_) return out;
    AnimationPreview& preview = *animationPreview_;
    std::vector<PreviewClip> clips;
    try { clips = PreviewClips(*this, preview.id); }
    catch (const ApiError& error) {
        if (operation == "preview") throw;
        animationPreview_.reset(); out["active"] = false; out["error"] = error.what(); return out;
    }
    auto chosen = std::find_if(clips.begin(), clips.end(), [&](const PreviewClip& clip) { return clip.name == preview.clip; });
    if (chosen == clips.end()) { animationPreview_.reset(); out["active"] = false; return out; }
    preview.time = std::max(0.0, std::min(preview.time, preview.loop ? chosen->duration : chosen->end));
    out["displayName"] = chosen->displayName;
    if (const SpriteAnimation* sprite = scene_.Get<SpriteAnimation>(preview.id)) out["asset"] = sprite->asset;
    out["loopRange"] = Json::MakeArray(); out["loopRange"].push(chosen->loopStart); out["loopRange"].push(chosen->loopEnd);
    out["id"] = preview.id; out["clip"] = preview.clip; out["time"] = preview.time;
    out["playing"] = preview.playing; out["speed"] = preview.speed; out["loop"] = preview.loop;
    out["ik"] = preview.ik; out["ikGoals"] = 0;
    if (!chosen->frames) {
        int count = 0;
        for (const auto& goal : scene_.Pool<BoneIK>()) if (BoneIkAppliesTo(scene_, preview.id, goal.first)) ++count;
        out["ikGoals"] = count;
    }
    out["duration"] = preview.loop ? chosen->duration : chosen->end; out["fps"] = chosen->fps;
    if (chosen->frames) {
        out["frameCount"] = static_cast<uint64_t>(chosen->frames->size()); out["variableTiming"] = chosen->timing.variable;
        out["frameTimes"] = Json::MakeArray(); out["frameDurations"] = Json::MakeArray();
        for (double time : chosen->timing.starts) out["frameTimes"].push(time);
        for (double duration : chosen->timing.durations) out["frameDurations"].push(duration);
    }
    out["type"] = chosen->frames ? "SpriteAnimation" : "Animator";
    out["clips"] = Json::MakeArray();
    for (const PreviewClip& clip : clips) {
        Json item = Json::MakeObject(); item["name"] = clip.name; item["displayName"] = clip.displayName; item["duration"] = clip.duration;
        out["clips"].push(item);
    }
    out["tracks"] = chosen->tracks.isArray() ? chosen->tracks : Json::MakeArray();
    out["events"] = Json::MakeArray();
    for (const AnimationMarker& marker : chosen->events) {
        Json item = Json::MakeObject(); item["time"] = marker.time; item["name"] = marker.name;
        if (chosen->frames) item["frame"] = static_cast<int>(chosen->timing.Index(marker.time));
        out["events"].push(item);
    }
    return out;
}

Scene& Engine::AnimationRenderScene() {
    const Json state = AnimationPreviewCall("state", Json());
    if (!state["active"].asBool()) return scene_;
    AnimationPreview& preview = *animationPreview_;
    preview.scene = scene_;  // refresh edits/undo/hot reload without touching the authored scene
    if (SpriteAnimation* sprite = preview.scene.Get<SpriteAnimation>(preview.id)) {
        sprite->clip = sprite->current = preview.clip; sprite->time = static_cast<float>(preview.time); sprite->playing = false;
        const Json& clip = SpriteAnimationClips(*sprite, *assets_)[preview.clip];
        SpriteClipTiming timing; ParseSpriteClipTiming(clip, timing, nullptr);
        const size_t index = timing.Index(preview.time);
        preview.scene.Get<Sprite>(preview.id)->frame = clip["frames"][index];
        if (clip.has("tracks")) ApplyPropertyTracks(preview.scene, preview.id, clip["tracks"], preview.time);
    } else {
        Animator& animator = preview.scene.Add<Animator>(preview.id);
        animator.clip = preview.clip; animator.time = static_cast<float>(preview.time); animator.playing = false;
        animator.from.clear(); animator.fade = 0; animator.shown = preview.clip;
        if (!preview.ik) {
            for (auto& goal : preview.scene.Pool<BoneIK>())
                if (BoneIkAppliesTo(preview.scene, preview.id, goal.first)) goal.second.weight = 0;
        }
        const MeshRenderer* renderer = preview.scene.Get<MeshRenderer>(preview.id);
        std::shared_ptr<const Mesh> mesh = renderer ? assets_->GetMesh(renderer->mesh) : nullptr;
        if (const AnimationClip* clip = mesh ? FindAnimationClip(*mesh, preview.clip) : nullptr)
            ApplyPropertyTracks(preview.scene, preview.id, clip->tracks, preview.time);
    }
    UpdateBoneAttachments(preview.scene, *assets_);
    return preview.scene;
}

}  // namespace oe
