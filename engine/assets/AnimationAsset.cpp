#include "assets/AnimationAsset.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include "api/Commands.h"
#include "app/Engine.h"
#include "assets/Assets.h"
#include "scene/SpriteClip.h"
#include "core/FileSystem.h"
#include "scene/Components.h"

namespace oe {
namespace {
bool SpritePath(std::string path) {
    for (char& c : path) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return path.size() >= 10 && path.substr(path.size() - 10) == ".anim.json";
}
void Fields(const Json& object, std::initializer_list<const char*> names) {
    if (!object.isObject()) throw std::runtime_error("expected an object");
    for (const auto& field : object.members()) {
        bool found = false;
        for (const char* name : names) found = found || field.first == name;
        if (!found) throw std::runtime_error("unknown animation field: " + field.first);
    }
}
int Integer(const Json& value, int low, int high) {
    const double n = value.asNumber(-1);
    if (!value.isNumber() || !std::isfinite(n) || n < low || n > high || std::floor(n) != n)
        throw std::runtime_error("animation integer outside " + std::to_string(low) + ".." + std::to_string(high));
    return static_cast<int>(n);
}
void Name(const Json& value) {
    if (!value.isString() || value.asString().empty() || value.asString().size() > 128)
        throw std::runtime_error("displayName must contain 1..128 bytes");
}
Json ReadDefinition(Engine& engine, const std::string& path) {
    std::string text;
    if (!ReadTextFile(engine.ResolvePath(path), text)) throw ApiError("not_found", "cannot read " + path);
    try { return Json::parse(text); }
    catch (const std::exception& ex) { throw ApiError("invalid_animation", ex.what(), "Fix the animation JSON."); }
}
void ValidateDefinition(Engine& engine, const std::string& path, const Json& data) {
    std::string error;
    if (AssetManager::KindOf(path) != "animation") throw ApiError("invalid_path", "animation asset must end with .anim.json or .clips.json");
    if (SpritePath(path)) {
        if (!ValidateSpriteAnimationAsset(data, &error)) throw ApiError("invalid_animation", error);
        engine.ResolvePath((data.has("sheet") ? data["sheet"] : data["atlas"])["texture"].asString());
    } else {
        const std::string model = path.substr(0, path.size() - 11);
        if (AssetManager::KindOf(model) != "model") throw ApiError("invalid_path", "clips.json must be beside a .glb/.gltf/.fbx model");
        Mesh check;
        for (const Json& file : data["clips"].items()) {
            // Sources stay relative to the model, including ../ paths inside the project.
            const std::string full = engine.ResolvePath(JoinPath(ParentPath(model), file.asString()));
            Mesh source;
            if (!LoadModelFile(full, source, &error)) throw ApiError("invalid_animation", error, "Fix the external clip source before attaching it.");
        }
        if (!LoadModelFile(engine.ResolvePath(model), check, &error, &data)) throw ApiError("invalid_animation", error);
    }
}
}  // namespace

bool ValidateSpriteAnimationAsset(const Json& data, std::string* error) {
    try {
        Fields(data, {"sheet", "atlas", "clips"});
        if (data.has("sheet") == data.has("atlas")) throw std::runtime_error("use exactly one of sheet or atlas");
        const Json& image = data.has("sheet") ? data["sheet"] : data["atlas"];
        Fields(image, data.has("sheet") ? std::initializer_list<const char*>{"texture", "columns", "rows"} : std::initializer_list<const char*>{"texture", "frames"});
        if (!image["texture"].isString() || image["texture"].asString().empty()) throw std::runtime_error("texture needs a project-relative image path");
        int count = 0;
        if (data.has("sheet")) count = Integer(image["columns"], 1, 4096) * Integer(image["rows"], 1, 4096);
        else {
            if (!image["frames"].isArray() || image["frames"].size() == 0 || image["frames"].size() > 65536) throw std::runtime_error("atlas frames needs 1..65536 [x,y,width,height] rectangles");
            for (const Json& rect : image["frames"].items()) {
                if (!rect.isArray() || rect.size() != 4) throw std::runtime_error("atlas rectangle needs [x,y,width,height]");
                Integer(rect[0], 0, 65536); Integer(rect[1], 0, 65536);
                Integer(rect[2], 1, 65536); Integer(rect[3], 1, 65536);
            }
            count = static_cast<int>(image["frames"].size());
        }
        if (!data["clips"].isObject() || data["clips"].size() == 0 || data["clips"].size() > 256) throw std::runtime_error("clips needs 1..256 named clips");
        for (const auto& item : data["clips"].members()) {
            if (item.first.empty() || item.first.size() > 128) throw std::runtime_error("clip name needs 1..128 bytes");
            const Json& clip = item.second;
            Fields(clip, {"frames", "fps", "loop", "events", "displayName", "durations"});
            if (!clip["frames"].isArray() || clip["frames"].size() == 0 || clip["frames"].size() > 65536) throw std::runtime_error("clip frames needs 1..65536 indices");
            for (const Json& index : clip["frames"].items()) Integer(index, 0, count - 1);
            const double fps = clip["fps"].asNumber(8);
            if ((clip.has("fps") && !clip["fps"].isNumber()) || !std::isfinite(fps) || fps < 0.001 || fps > 10000) throw std::runtime_error("fps must be in [0.001,10000]");
            if (clip.has("loop") && !clip["loop"].isBool()) throw std::runtime_error("loop must be boolean");
            if (clip.has("displayName")) Name(clip["displayName"]);
            std::vector<AnimationMarker> markers; std::string message;
            SpriteClipTiming timing;
            if (!ParseSpriteClipTiming(clip, timing, &message) || !SpriteClipMarkers(clip["events"], timing, markers, &message)) throw std::runtime_error(message);
        }
        return true;
    } catch (const std::exception& ex) { if (error) *error = ex.what(); return false; }
}

bool ApplyAnimationMetadata(const Json& data, Mesh& mesh, std::string* error) {
    try {
        Fields(data, {"clips", "metadata"});
        if (data.has("clips")) {
            if (!data["clips"].isArray() || data["clips"].size() > 256) throw std::runtime_error("clips must be an array of at most 256 source paths");
            for (const Json& source : data["clips"].items()) if (!source.isString() || source.asString().empty()) throw std::runtime_error("clip sources must be nonempty paths");
        }
        if (data.has("metadata") && !data["metadata"].isObject()) throw std::runtime_error("metadata must be a clip-name object");
        for (AnimationClip& clip : mesh.clips) clip.metadataSidecar = data.has("metadata");
        for (const auto& item : data["metadata"].members()) {
            auto found = std::find_if(mesh.clips.begin(), mesh.clips.end(), [&](const AnimationClip& clip) { return clip.name == item.first; });
            if (found == mesh.clips.end()) throw std::runtime_error("metadata: unknown clip " + item.first);
            Fields(item.second, {"displayName", "loopRange", "events"});
            if (item.second.has("displayName")) { Name(item.second["displayName"]); found->displayName = item.second["displayName"].asString(); }
            if (item.second.has("loopRange")) {
                const Json& range = item.second["loopRange"];
                if (!range.isArray() || range.size() != 2 || !range[0].isNumber() || !range[1].isNumber()) throw std::runtime_error("loopRange needs [start,end] seconds");
                const double start = range[0].asNumber(), end = range[1].asNumber();
                if (!std::isfinite(start) || !std::isfinite(end) || start < 0 || end <= start || end > found->duration) throw std::runtime_error("loopRange must satisfy 0 <= start < end <= clip duration");
                found->loopStart = static_cast<float>(start); found->loopEnd = static_cast<float>(end);
                if (found->loopEnd <= found->loopStart) throw std::runtime_error("loopRange is too small for float playback time");
            }
            if (item.second.has("events")) {
                std::string message;
                if (!ParseAnimationMarkers(item.second["events"], found->duration, 0, 0, found->events, &message)) throw std::runtime_error(message);
                found->sidecarEvents = true;
            }
        }
        return true;
    } catch (const std::exception& ex) { if (error) *error = ex.what(); return false; }
}

const Json& SpriteAnimationClips(const SpriteAnimation& animation, AssetManager& assets) {
    if (animation.asset.empty()) return animation.clips;
    std::string error;
    auto definition = assets.GetAnimation(animation.asset, &error);
    if (!definition) throw ApiError("invalid_animation", error, "Fix SpriteAnimation.asset with animation.get/set or use inline clips.");
    return (*definition)["clips"];
}
bool SpriteAnimationLoop(const SpriteAnimation& animation, const Json& clip) {
    return animation.asset.empty() || animation.loopOverride < 0 ? clip["loop"].asBool(true) : animation.loopOverride != 0;
}

void RegisterAnimationAssetCommands(CommandRegistry& registry) {
    RegisterSpriteClipCommands(registry);
    registry.Add({"animation.get", "Read a reusable sprite animation or model clips sidecar.", Params().Req("path", "string", ".anim.json or <model>.clips.json."), false,
        [](Engine& engine, const Json& args) {
            const std::string path = args["path"].asString();
            const Json data = ReadDefinition(engine, path); ValidateDefinition(engine, path, data);
            Json out = Json::MakeObject(); out["path"] = path; out["values"] = data; return out;
        }});
    for (bool create : {true, false}) registry.Add({create ? "animation.create" : "animation.set",
        create ? "Create a shared animation asset; file writes are not scene undo." : "Patch an animation asset root or named clip; validates before writing; no scene undo.",
        create ? static_cast<Json>(Params().Req("path", "string", ".anim.json or <model>.clips.json.").Req("values", "object", "Complete animation definition.").Opt("overwrite", "boolean", "Replace an existing file.")) :
                 static_cast<Json>(Params().Req("path", "string", "Shared animation asset path.").Req("values", "object", "Fields to merge; arrays replace.").Opt("clip", "string", "Sprite clip or model metadata entry to patch.")), false,
        [create](Engine& engine, const Json& args) {
            const std::string path = args["path"].asString(), full = engine.ResolvePath(path);
            if (create && FileExists(full) && !args["overwrite"].asBool()) throw ApiError("already_exists", "animation asset already exists", "Pass overwrite:true to replace it.");
            Json data = create ? args["values"] : ReadDefinition(engine, path);
            if (!create) {
                Json* target = &data;
                if (args.has("clip")) target = &data[SpritePath(path) ? "clips" : "metadata"][args["clip"].asString()];
                for (const auto& item : args["values"].members()) (*target)[item.first] = item.second;
            }
            ValidateDefinition(engine, path, data);
            CreateDirectories(ParentPath(full));
            if (!WriteTextFile(full, data.dump(2) + "\n")) throw ApiError("write_failed", "cannot write " + path);
            engine.Assets().Forget(path);
            if (!SpritePath(path)) engine.Assets().Forget(path.substr(0, path.size() - 11));
            Json out = Json::MakeObject(); out["path"] = path; out["values"] = data; return out;
        }});
}
}  // namespace oe
