// skeleton2d.* commands: author and inspect 2D skeleton assets (*.skel2d.json) and the Skeleton2D
// component. File writes are outside scene undo, like the other asset commands. docs/SKELETON2D.md.
#include "api/Commands.h"

#include <cmath>
#include <set>

#include "app/Engine.h"
#include "assets/Assets.h"
#include "core/FileSystem.h"
#include "scene/Components.h"
#include "scene/Skeleton2D.h"

namespace oe {
namespace {

constexpr const char* kFormatHint = "Fix the field the message names; the format is in docs/SKELETON2D.md (\"Asset format\"). skeleton2d.check reports every problem.";

void CheckPath(const std::string& path) {
    if (AssetManager::KindOf(path) != "skeleton2d") throw ApiError("invalid_path", "skeleton asset must end with .skel2d.json", "Name it like characters/hero.skel2d.json.");
}

Json ReadSkeleton(Engine& engine, const std::string& path) {
    CheckPath(path);
    std::string text, error;
    if (!ReadTextFile(engine.ResolvePath(path), text))
        throw ApiError("not_found", "cannot read " + path, "Create it with skeleton2d.create {path, values}.");
    Json data = Json::parse(text, &error);
    if (!error.empty()) throw ApiError("invalid_skeleton", "invalid JSON: " + error, kFormatHint);
    return data;
}

std::shared_ptr<const Skeleton2DDef> Validate(const Json& data) {
    std::string error;
    auto def = ParseSkeleton2D(data, &error);
    if (!def) throw ApiError("invalid_skeleton", error, kFormatHint);
    return def;
}

EntityId Target(Engine& e, const Json& args) {
    const Json& v = args["id"];
    const EntityId id = v.isString() ? e.GetScene().FindByName(v.asString()) : static_cast<EntityId>(v.asNumber());
    if (!e.GetScene().Record(id)) throw ApiError("not_found", "entity does not exist", "Pass an entity id or unique name (scene.summary lists them).");
    return id;
}

// What an agent should look at besides hard errors: images that do not load, rectangles and mesh
// vertices outside their image, bones nothing hangs on, attachments no slot or clip can show.
Json Warnings(Engine& engine, const Skeleton2DDef& def) {
    Json warnings = Json::MakeArray();
    auto warn = [&](const std::string& where, const std::string& what, const std::string& hint) {
        Json w = Json::MakeObject();
        w["where"] = where;
        w["message"] = what;
        w["hint"] = hint;
        warnings.push(w);
    };
    std::set<int> usedBones;
    for (const Skeleton2DSlot& slot : def.slots) usedBones.insert(slot.bone);
    std::set<std::pair<int, std::string>> shown;
    for (size_t i = 0; i < def.slots.size(); ++i) shown.insert({static_cast<int>(i), def.slots[i].attachment});
    for (const Skeleton2DClip& clip : def.clips) {
        for (const Skeleton2DSlotTimeline& t : clip.slots)
            for (const Skeleton2DKey& k : t.attachment) shown.insert({t.slot, k.text});
        for (const Skeleton2DBoneTimeline& t : clip.bones) usedBones.insert(t.bone);
        if (clip.duration <= 0) warn("clips." + clip.name, "has no duration (no keys and no duration field)", "Add keys or a duration in seconds.");
    }
    for (const auto& skin : def.skins)
        for (const auto& slot : skin.second)
            for (const auto& entry : slot.second) {
                const Skeleton2DAttachment& a = entry.second;
                const std::string where = "skins." + skin.first + "." + def.slots[static_cast<size_t>(slot.first)].name + "." + entry.first;
                std::string error;
                auto texture = engine.Assets().GetTexture(a.image, &error);
                if (!texture) {
                    warn(where + ".image", "image does not load: " + error, "Import or fix the image path (project-relative).");
                    continue;
                }
                const float w = static_cast<float>(texture->width), h = static_cast<float>(texture->height);
                if (a.hasRect && (a.rect[0] + a.rect[2] > w || a.rect[1] + a.rect[3] > h))
                    warn(where + ".rect", "goes outside the " + std::to_string(texture->width) + "x" + std::to_string(texture->height) + " image",
                         "Keep x + width and y + height inside the image.");
                const float rw = a.hasRect ? a.rect[2] : w, rh = a.hasRect ? a.rect[3] : h;
                for (size_t v = 0; v * 2 + 1 < a.vertices.size(); ++v)
                    if (a.vertices[v * 2] < -0.5f || a.vertices[v * 2] > rw + 0.5f || a.vertices[v * 2 + 1] < -0.5f || a.vertices[v * 2 + 1] > rh + 0.5f) {
                        warn(where + ".vertices", "vertex " + std::to_string(v) + " lies outside the image rectangle", "Vertices are rectangle pixels, x right and y down.");
                        break;
                    }
                if (a.IsMesh() && a.weights.empty() && def.bones.size() > 1)
                    warn(where + ".weights", "mesh without weights moves rigidly with its slot's bone", "Add weights to bend it across joints, or drop the vertices to make it a plain image.");
                for (const auto& list : a.weights)
                    for (const auto& influence : list) usedBones.insert(influence.first);
                if (!shown.count({slot.first, entry.first}))
                    warn(where, "no slot shows it in the setup pose and no clip switches to it", "Set the slot's attachment or key it in a clip, or remove it.");
            }
    for (size_t b = 1; b < def.bones.size(); ++b) {
        bool parentOfUsed = false;
        for (size_t c = 0; c < def.bones.size(); ++c) parentOfUsed = parentOfUsed || (def.bones[c].parent == static_cast<int>(b) && usedBones.count(static_cast<int>(c)));
        if (!usedBones.count(static_cast<int>(b)) && !parentOfUsed)
            warn("bones[" + std::to_string(b) + "]", "bone '" + def.bones[b].name + "' moves nothing (no slot, weight, clip or child uses it)",
                 "Attach a slot to it, weight vertices to it, or remove it.");
    }
    return warnings;
}

Json Stats(const Skeleton2DDef& def) {
    Json stats = Json::MakeObject();
    size_t attachments = 0, meshes = 0, vertices = 0;
    for (const auto& skin : def.skins)
        for (const auto& slot : skin.second)
            for (const auto& entry : slot.second) {
                ++attachments;
                if (entry.second.IsMesh()) ++meshes;
                vertices += entry.second.IsMesh() ? entry.second.vertices.size() / 2 : 4;
            }
    stats["bones"] = static_cast<int>(def.bones.size());
    stats["slots"] = static_cast<int>(def.slots.size());
    stats["attachments"] = static_cast<int>(attachments);
    stats["meshes"] = static_cast<int>(meshes);
    stats["vertices"] = static_cast<int>(vertices);
    Json skins = Json::MakeArray();
    for (const auto& skin : def.skins) skins.push(skin.first);
    stats["skins"] = skins;
    Json clips = Json::MakeObject();
    for (const Skeleton2DClip& clip : def.clips) {
        Json c = Json::MakeObject();
        c["duration"] = clip.duration;
        c["loop"] = clip.loop;
        clips[clip.name] = c;
    }
    stats["clips"] = clips;
    return stats;
}

Json BoneJson(const Mat4& world, const std::string& name) {
    Json b = Json::MakeObject();
    b["name"] = name;
    const Vec3 p = world.TransformPoint(Vec3());
    const Vec3 x = world.TransformDir(Vec3(1, 0, 0));
    Json position = Json::MakeArray();
    position.push(p.x);
    position.push(p.y);
    position.push(p.z);
    b["position"] = position;
    b["rotation"] = Degrees(std::atan2(x.y, x.x));
    return b;
}

}  // namespace

void RegisterSkeleton2DCommands(CommandRegistry& r) {
    r.Add({"skeleton2d.get", "Read a 2D skeleton asset with its counts (bones, slots, attachments, clips).",
           Params().Req("path", "string", "*.skel2d.json."), false, [](Engine& engine, const Json& args) {
               const std::string path = args["path"].asString();
               const Json data = ReadSkeleton(engine, path);
               Json out = Json::MakeObject();
               out["path"] = path;
               out["values"] = data;
               out["stats"] = Stats(*Validate(data));
               return out;
           }});
    for (bool create : {true, false})
        r.Add({create ? "skeleton2d.create" : "skeleton2d.set",
               create ? "Create a 2D skeleton asset (bones, slots, skins, clips); validated before writing; no scene undo."
                      : "Patch a 2D skeleton asset: top-level fields, or one clip with `clip`; arrays and objects given replace; validated before writing.",
               create ? static_cast<Json>(Params().Req("path", "string", "*.skel2d.json.").Req("values", "object", "The whole asset.")
                                              .Opt("overwrite", "boolean", "Replace an existing file."))
                      : static_cast<Json>(Params().Req("path", "string", "*.skel2d.json.").Req("values", "object", "Fields to replace.")
                                              .Opt("clip", "string", "Patch this clip's fields instead (created when new).")
                                              .Opt("remove", "boolean", "With clip: delete that clip.")),
               false, [create](Engine& engine, const Json& args) {
                   const std::string path = args["path"].asString();
                   CheckPath(path);
                   const std::string full = engine.ResolvePath(path);
                   if (create && FileExists(full) && !args["overwrite"].asBool())
                       throw ApiError("already_exists", "skeleton asset already exists", "Pass overwrite:true, or patch it with skeleton2d.set.");
                   Json data = create ? args["values"] : ReadSkeleton(engine, path);
                   if (!create) {
                       if (args.has("clip")) {
                           Json& clips = data["clips"];
                           if (!clips.isObject()) clips = Json::MakeObject();
                           const std::string name = args["clip"].asString();
                           if (args["remove"].asBool()) {
                               if (!clips.erase(name)) throw ApiError("not_found", "no clip '" + name + "'", "skeleton2d.get lists the clips.");
                           } else {
                               Json& clip = clips[name];
                               if (!clip.isObject()) clip = Json::MakeObject();
                               for (const auto& item : args["values"].members()) clip[item.first] = item.second;
                           }
                       } else {
                           for (const auto& item : args["values"].members()) data[item.first] = item.second;
                       }
                   }
                   const auto def = Validate(data);
                   CreateDirectories(ParentPath(full));
                   if (!WriteTextFile(full, data.dump(2) + "\n")) throw ApiError("write_failed", "cannot write " + path);
                   engine.Assets().Forget(path);
                   Json out = Json::MakeObject();
                   out["path"] = path;
                   out["stats"] = Stats(*def);
                   out["warnings"] = Warnings(engine, *def);
                   return out;
               }});
    r.Add({"skeleton2d.check", "Validate a 2D skeleton (a file or values) without writing: the first error with its place, counts and warnings with hints.",
           Params().Opt("path", "string", "*.skel2d.json to check.").Opt("values", "object", "An asset to check instead of a file."), false,
           [](Engine& engine, const Json& args) {
               if (!args.has("path") && !args.has("values")) throw ApiError("invalid_args", "pass path or values");
               const Json data = args.has("values") ? args["values"] : ReadSkeleton(engine, args["path"].asString());
               std::string error;
               const auto def = ParseSkeleton2D(data, &error);
               Json out = Json::MakeObject();
               out["valid"] = def != nullptr;
               if (!def) {
                   out["error"] = error;
                   out["hint"] = kFormatHint;
                   return out;
               }
               out["stats"] = Stats(*def);
               out["warnings"] = Warnings(engine, *def);
               return out;
           }});
    r.Add({"skeleton2d.state", "Playback state of an entity's Skeleton2D and where its bones are now (world position and rotation).",
           Params().Req("id", "entity", "Entity id or unique name with Skeleton2D.").Opt("bones", "boolean", "Include every bone (default true)."), false,
           [](Engine& engine, const Json& args) {
               const EntityId id = Target(engine, args);
               const Skeleton2D* sk = engine.GetScene().Get<Skeleton2D>(id);
               if (!sk) throw ApiError("component_missing", "entity has no Skeleton2D", "component.add {id, type:\"Skeleton2D\", values:{asset}}.");
               std::string error;
               const auto def = sk->asset.empty() ? nullptr : engine.Assets().GetSkeleton2D(sk->asset, &error);
               Json out = Json::MakeObject();
               out["asset"] = sk->asset;
               out["clip"] = sk->clip;
               out["time"] = sk->time;
               out["playing"] = sk->playing;
               out["finished"] = sk->finished;
               if (!sk->from.empty()) {
                   out["fadingFrom"] = sk->from;
                   out["fade"] = sk->mix > 0 ? Clamp(sk->mixed / sk->mix, 0, 1) : 1.0f;
               }
               if (!def) {
                   out["error"] = sk->asset.empty() ? std::string("Skeleton2D.asset is empty") : error;
                   out["hint"] = "Set Skeleton2D.asset to a valid *.skel2d.json (skeleton2d.check shows what is wrong).";
                   return out;
               }
               const Skeleton2DClip* clip = def->FindClip(sk->clip);
               if (!sk->clip.empty() && !clip) {
                   out["warning"] = "clip '" + sk->clip + "' does not exist; the setup pose is shown";
                   Json names = Json::MakeArray();
                   for (const Skeleton2DClip& c : def->clips) names.push(c.name);
                   out["clips"] = names;
               }
               if (clip) out["duration"] = clip->duration;
               const Skeleton2DPose pose = Skeleton2DComponentPose(*def, *sk);
               Json slots = Json::MakeObject();
               for (size_t i = 0; i < def->slots.size(); ++i) slots[def->slots[i].name] = pose.attachments[i];
               out["attachments"] = slots;
               if (args["bones"].asBool(true)) {
                   const Mat4 root = engine.GetScene().WorldMatrix(id) *
                                     Mat4::Scale(Vec3(sk->flipX ? -1.0f : 1.0f, sk->flipY ? -1.0f : 1.0f, 1.0f));
                   Json bones = Json::MakeArray();
                   for (size_t b = 0; b < def->bones.size(); ++b) bones.push(BoneJson(root * pose.world[b], def->bones[b].name));
                   out["bones"] = bones;
               }
               return out;
           }});
}

}  // namespace oe
