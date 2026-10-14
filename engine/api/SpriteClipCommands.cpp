#include "api/Commands.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include "app/Engine.h"
#include "assets/Assets.h"
#include "assets/SpriteAtlas.h"
#include "assets/AnimationAsset.h"
#include "scene/Components.h"
#include "scene/SpriteClip.h"
#include "scene/PropertyTracks.h"

namespace oe {
namespace {
EntityId Target(Engine& engine, const Json& args) {
    const double value = args["id"].asNumber();
    const EntityId id = args["id"].isString() ? engine.GetScene().FindByName(args["id"].asString()) :
        (value >= 1 && value <= 4294967295.0 ? static_cast<EntityId>(value) : kNullEntity);
    if (!engine.GetScene().Get<SpriteAnimation>(id) || !engine.GetScene().Get<Sprite>(id))
        throw ApiError("missing_component", "sprite clip editing needs Sprite and SpriteAnimation", "Select an animated sprite.");
    return id;
}
struct SpriteSource { Json image; const Json* rects = nullptr; };
SpriteSource ImageDefinition(Engine& engine, EntityId id) {
    const SpriteAnimation& animation = *engine.GetScene().Get<SpriteAnimation>(id);
    if (!animation.asset.empty()) {
        auto asset = engine.Assets().GetAnimation(animation.asset);
        if (!asset) throw ApiError("invalid_animation", "cannot read shared sprite definition", "Repair it with animation.set.");
        const Json& image = (*asset)[asset->has("atlas") ? "atlas" : "sheet"];
        if (image.has("path")) {
            auto atlas = engine.Assets().GetAtlas(image["path"].asString());
            if (!atlas) throw ApiError("invalid_atlas", "cannot read sprite atlas");
            SpriteSource source; source.image["texture"] = (*atlas)["texture"]; source.image["atlas"] = true; source.rects = &(*atlas)["frames"]; return source;
        }
        SpriteSource source; source.image["texture"] = image["texture"]; source.image["columns"] = image["columns"]; source.image["rows"] = image["rows"];
        source.image["atlas"] = asset->has("atlas"); if (asset->has("atlas")) source.rects = &image["frames"]; return source;
    }
    const Sprite& sprite = *engine.GetScene().Get<Sprite>(id);
    if (!sprite.atlas.empty()) {
        auto atlas = engine.Assets().GetAtlas(sprite.atlas);
        if (!atlas) throw ApiError("invalid_atlas", "cannot read sprite atlas");
        SpriteSource source; source.image["texture"] = (*atlas)["texture"]; source.image["atlas"] = true; source.rects = &(*atlas)["frames"]; return source;
    }
    Json image = Json::MakeObject(); image["texture"] = sprite.texture;
    image["columns"] = std::max(1, std::min(4096, sprite.columns)); image["rows"] = std::max(1, std::min(4096, sprite.rows));
    return {image, nullptr};
}
int Index(const Json& args, const char* field, size_t size, bool insertion = false) {
    const double value = args[field].asNumber(-1);
    if (!std::isfinite(value) || value < 0 || std::floor(value) != value || value >= static_cast<double>(size + (insertion ? 1 : 0)))
        throw ApiError("invalid_argument", std::string(field) + " is outside the clip sequence");
    return static_cast<int>(value);
}
void EditSequence(Json& clip, const Json& args) {
    const std::string operation = args["operation"].asString();
    if (operation == "uniform") { clip.erase("durations"); return; }
    SpriteClipTiming timing; std::string error;
    if (!ParseSpriteClipTiming(clip, timing, &error)) throw ApiError("invalid_animation", error);
    if (operation == "duration") {
        const int index = Index(args, "index", timing.starts.size());
        Json durations = Json::MakeArray(); for (double duration : timing.durations) durations.push(duration);
        durations[index] = args["duration"]; clip["durations"] = durations; return;
    }
    std::vector<int> order(timing.starts.size()); std::iota(order.begin(), order.end(), 0);
    int destination = 0;
    if (operation == "append" || operation == "insert") {
        destination = operation == "append" ? static_cast<int>(order.size()) : Index(args, "index", order.size(), true);
        order.insert(order.begin() + destination, -1);
    } else if (operation == "move") {
        const int index = Index(args, "index", order.size()); destination = Index(args, "to", order.size());
        const int old = order[static_cast<size_t>(index)]; order.erase(order.begin() + index); order.insert(order.begin() + destination, old);
    } else if (operation == "remove") {
        if (order.size() == 1) throw ApiError("invalid_argument", "a clip must retain one frame");
        order.erase(order.begin() + Index(args, "index", order.size()));
    } else throw ApiError("invalid_argument", "unknown sequence operation", "Use append, insert, move, remove, duration or uniform.");
    Json frames = Json::MakeArray(), durations = Json::MakeArray();
    std::vector<int> remap(timing.starts.size(), -1);
    for (size_t i = 0; i < order.size(); ++i) {
        const int old = order[i];
        frames.push(old < 0 ? args["frame"] : clip["frames"][old]);
        durations.push(old < 0 ? args["duration"].asNumber(1 / timing.fps) : timing.durations[static_cast<size_t>(old)]);
        if (old >= 0) remap[static_cast<size_t>(old)] = static_cast<int>(i);
    }
    Json events = Json::MakeArray();
    for (Json event : clip["events"].items()) {
        const int old = event["frame"].asInt(-1);
        if (old < 0 || static_cast<size_t>(old) >= remap.size()) throw ApiError("invalid_animation_events", "invalid source event frame");
        if (remap[static_cast<size_t>(old)] >= 0) { event["frame"] = remap[static_cast<size_t>(old)]; events.push(event); }
    }
    clip["frames"] = frames;
    if (timing.variable || args.has("duration")) clip["durations"] = durations;
    if (clip.has("events")) clip["events"] = events;
}
}  // namespace

void RegisterSpriteClipCommands(CommandRegistry& registry) {
    registry.Add({"animation.get_clip", "Read sprite clip fields and a bounded sheet/atlas frame page for visual authoring.",
        Params().Req("id", "entity", "Sprite entity.").Opt("clip", "string", "Clip key; default authored clip.")
            .Opt("offset", "integer", "First source frame (default 0).").Opt("limit", "integer", "Page size 1..128 (default 64).").Opt("frame", "string", "Resolve a named atlas frame instead of offset.").Opt("sourceOnly", "boolean", "Omit clip values for lightweight frame queries."), false,
        [](Engine& engine, const Json& args) {
            const EntityId id = Target(engine, args); const SpriteAnimation& animation = *engine.GetScene().Get<SpriteAnimation>(id);
            const std::string name = args["clip"].asString(animation.clip); const SpriteSource source = ImageDefinition(engine, id); const Json& image = source.image;
            const auto texture = image["texture"].asString().empty() ? nullptr : engine.Assets().GetTexture(image["texture"].asString());
            const int count = source.rects ? static_cast<int>(source.rects->size()) : image["columns"].asInt() * image["rows"].asInt();
            const double requested = args["limit"].asNumber(64);
            if (requested < 1 || requested > 128) throw ApiError("invalid_argument", "page limit must be in 1..128");
            const int limit = static_cast<int>(requested);int offset = args.has("offset") ? Index(args, "offset", static_cast<size_t>(count)) : 0;
            if(args.has("frame")) {
                bool found=false;if(source.rects)for(size_t i=0;i<source.rects->size();++i)if((*source.rects)[i]["name"]==args["frame"]){offset=static_cast<int>(i);found=true;break;}
                if(!found)throw ApiError("invalid_frame","atlas frame name does not exist");
            }
            if (offset >= count) throw ApiError("invalid_argument", "invalid source frame page");
            Json out = Json::MakeObject(); out["id"] = id; out["clip"] = name; out["asset"] = animation.asset;
            if (!args["sourceOnly"].asBool()) out["values"] = SpriteAnimationClips(animation, engine.Assets())[name]; out["image"] = image;
            out["textureWidth"] = texture ? texture->width : 0; out["textureHeight"] = texture ? texture->height : 0;
            out["frameCount"] = count; out["offset"] = offset; out["frames"] = Json::MakeArray();
            for (int index = offset; index < std::min(count, offset + limit); ++index) {
                Json frame = Json::MakeObject(); frame["index"] = index;
                if (source.rects) {
                    const Json& sourceFrame = (*source.rects)[index];
                    frame["rect"] = sourceFrame.isObject() ? sourceFrame["rect"] : sourceFrame;
                    if (sourceFrame.isObject()) frame["name"] = sourceFrame["name"];
                }
                else {
                    const double width = texture ? static_cast<double>(texture->width) / image["columns"].asInt() : 1;
                    const double height = texture ? static_cast<double>(texture->height) / image["rows"].asInt() : 1;
                    frame["rect"] = Json(Json::Array{(index % image["columns"].asInt()) * width, (index / image["columns"].asInt()) * height, width, height});
                }
                out["frames"].push(frame);
            }
            return out;
        }});
    registry.Add({"animation.set_clip", "Create/patch a sprite clip or edit its sequence; frame operations preserve event anchors. Inline undo; shared asset writes have no scene undo.",
        Params().Req("id", "entity", "Sprite entity.").Req("clip", "string", "Stable clip key.")
            .Opt("values", "object", "Clip fields to patch: frames, durations, fps, loop, displayName, events, tracks (keyframed component fields, see animation.set_tracks).")
            .Opt("operation", "string", "create|append|insert|move|remove|duration|uniform (remove explicit durations).")
            .Opt("index", "integer", "Sequence index.").Opt("to", "integer", "Final sequence index for move.")
            .Opt("frame", "integer", "Source frame for append/insert.").Opt("duration", "number", "Frame seconds.")
            .Opt("merge", "string", "Inline undo group key."), true,
        [](Engine& engine, const Json& args) {
            const EntityId id = Target(engine, args); const SpriteAnimation& animation = *engine.GetScene().Get<SpriteAnimation>(id);
            const std::string name = args["clip"].asString();
            if (name.empty() || name.size() > 128) throw ApiError("invalid_argument", "clip key needs 1..128 bytes");
            if (!args.has("values") && !args.has("operation")) throw ApiError("invalid_argument", "provide values or operation");
            Json clip = SpriteAnimationClips(animation, engine.Assets())[name];
            if (args["operation"].asString() == "create" && clip.isObject()) throw ApiError("already_exists", "sprite clip already exists", "Choose a different clip key, or patch it with values.");
            for (const auto& item : args["values"].members()) clip[item.first] = item.second;
            if (args.has("operation") && args["operation"].asString() != "create") EditSequence(clip, args);
            for (const auto& item : clip.members())
                if (item.first != "frames" && item.first != "durations" && item.first != "fps" && item.first != "loop" && item.first != "displayName" && item.first != "events" && item.first != "tracks")
                    throw ApiError("invalid_animation", "unknown sprite clip field: " + item.first);
            SpriteClipTiming timing; std::vector<AnimationMarker> markers; std::string error;
            if (!ParseSpriteClipTiming(clip, timing, &error) || !SpriteClipMarkers(clip["events"], timing, markers, &error)) throw ApiError("invalid_animation", error);
            if (clip.has("tracks") && !ValidatePropertyTracks(clip["tracks"], &error)) throw ApiError("invalid_animation", error);
            for (const Json& frame : clip["frames"].items()) {
                if (frame.isString() && !frame.asString().empty() && frame.asString().size() <= 128) continue;
                if (!frame.isNumber() || frame.asNumber() < 0 || frame.asNumber() > 16777215 || std::floor(frame.asNumber()) != frame.asNumber()) throw ApiError("invalid_animation", "frames need nonnegative integer indices or atlas names");
            }
            if (clip.has("displayName") && (!clip["displayName"].isString() || clip["displayName"].asString().empty() || clip["displayName"].asString().size() > 128)) throw ApiError("invalid_animation", "displayName needs 1..128 bytes");
            if (clip.has("loop") && !clip["loop"].isBool()) throw ApiError("invalid_animation", "loop must be boolean");
            if (clip.has("fps") && (!clip["fps"].isNumber() || !std::isfinite(clip["fps"].asNumber()) || clip["fps"].asNumber() < 0.001 || clip["fps"].asNumber() > 10000)) throw ApiError("invalid_animation", "fps must be in 0.001..10000");
            if (!animation.asset.empty()) {
                Json call = Json::MakeObject(); call["path"] = animation.asset; call["clip"] = name; call["values"] = clip;
                // Uniform conversion removes a field; root replacement preserves every other shared clip.
                if (args["operation"].asString() == "uniform") {
                    auto data = engine.Assets().GetAnimation(animation.asset); call.erase("clip"); call["values"]["clips"] = (*data)["clips"]; call["values"]["clips"][name] = clip;
                    for (const auto& item : clip.members()) call["values"].erase(item.first);
                }
                Json result = engine.Call("animation.set", call);
                if (!result["ok"].asBool()) throw ApiError(result["error"]["code"].asString(), result["error"]["message"].asString());
            } else engine.GetScene().Get<SpriteAnimation>(id)->clips[name] = clip;
            Json out = Json::MakeObject(); out["id"] = id; out["clip"] = name; out["values"] = clip; out["asset"] = animation.asset; return out;
        }});
}
}  // namespace oe
