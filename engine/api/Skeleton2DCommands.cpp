// skeleton2d.* commands: author and inspect 2D skeleton assets (*.skel2d.json) and the Skeleton2D
// component. File writes are outside scene undo, like the other asset commands. docs/SKELETON2D.md.
#include "api/Commands.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>

#include "app/Engine.h"
#include "app/Skeleton2DBuild.h"
#include "app/Skeleton2DReview.h"
#include "assets/Assets.h"
#include "core/FileSystem.h"
#include "render/ImageDraw.h"
#include "render/Renderer.h"
#include "render/Skeleton2DMesh.h"
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
    r.Add({"skeleton2d.draft_template",
           "A starting rig draft for a kind of character: joints (with where each goes), bones and parts in draw order. With an image, "
           "joints and regions are first guesses from its opaque bounds and body proportions, to correct with image.grid.",
           Params()
               .Req("kind", "string", "humanoid-front or humanoid-side (facing right).")
               .Opt("image", "string", "The character illustration (transparent background) the draft is for.")
               .Opt("joints", "object", "Joints you placed: {name: [x, y]} in image pixels; regions are fitted to them (widths measured from the image).")
               .Opt("out", "string", "Also write the draft to this *.skel2d-draft.json."),
           false, [](Engine& engine, const Json& args) {
               Json result = Skeleton2DDraftTemplate(engine, args["kind"].asString(), args["image"].asString(""), args["joints"]);
               if (args.has("out")) {
                   const std::string out = args["out"].asString();
                   const std::string full = engine.ResolvePath(out);
                   CreateDirectories(ParentPath(full));
                   if (!WriteTextFile(full, result["draft"].dump(2) + "\n")) throw ApiError("write_failed", "cannot write " + out);
                   result["out"] = out;
               }
               return result;
           }});
    r.Add({"skeleton2d.build",
           "Build a skeleton asset from a rig draft: cut each part out of the illustration, mesh it, weight it to its bones, place it so the setup "
           "pose is the picture. Keeps the clips of an existing asset. Returns coverage (pixels no part took), warnings and a picture of the draft.",
           Params()
               .Opt("draft", "string", "*.skel2d-draft.json to build from (or values).")
               .Opt("values", "object", "The draft itself; with draft it is also written there.")
               .Req("out", "string", "*.skel2d.json to write.")
               .Opt("partsDir", "string", "Folder for the cut part images (default: <out name>-parts/).")
               .Opt("dryRun", "boolean", "Only check and draw the draft picture; write nothing else.")
               .Opt("draftImage", "string", "PNG for the draft picture (default .oe/vision/draft.png)."),
           false, [](Engine& engine, const Json& args) {
               Json draft;
               if (args.has("values")) {
                   draft = args["values"];
                   if (args.has("draft") && !args["dryRun"].asBool()) {
                       const std::string full = engine.ResolvePath(args["draft"].asString());
                       CreateDirectories(ParentPath(full));
                       if (!WriteTextFile(full, draft.dump(2) + "\n")) throw ApiError("write_failed", "cannot write " + args["draft"].asString());
                   }
               } else if (args.has("draft")) {
                   std::string text, error;
                   if (!ReadTextFile(engine.ResolvePath(args["draft"].asString()), text))
                       throw ApiError("not_found", "cannot read " + args["draft"].asString(), "Make one with skeleton2d.draft_template {kind, image, out}.");
                   draft = Json::parse(text, &error);
                   if (!error.empty()) throw ApiError("invalid_draft", "invalid JSON: " + error);
               } else {
                   throw ApiError("invalid_args", "pass draft (a file) or values (the draft)");
               }
               return BuildSkeleton2D(engine, draft, args["out"].asString(), args["partsDir"].asString(""), args["dryRun"].asBool(),
                                      args["draftImage"].asString(""));
           }});
    auto poseParams = [](Params p) {
        return p.Opt("path", "string", "*.skel2d.json (or id).")
            .Opt("id", "entity", "Entity whose Skeleton2D asset and skin to use.")
            .Opt("skin", "string", "Skin to show.")
            .Opt("clip", "string", "Clip to sample, or \"*\" for every clip. Without a clip: stress poses bending each joint by +-angle.")
            .Opt("samples", "integer", "Evenly spaced times per clip (default 8).")
            .OptWith("times", Json::parse(R"({"type":"array","description":"Exact clip times in seconds instead of samples."})"))
            .Opt("angle", "number", "Stress poses: degrees each joint is bent both ways (default 45).");
    };
    r.Add({"skeleton2d.analyze",
           "Find what goes wrong when a skeleton moves: picture that was hidden under another part shows (revealed), joints open (gap), "
           "triangles fold (flipped) or stretch, a loop does not close (seam). Problems come with hints; writes a picture of the worst poses.",
           poseParams(Params())
               .Opt("masks", "boolean", "Also write each slot's revealed texels as a mask PNG (white = revealed) for repainting.")
               .Opt("render", "boolean", "Write the picture of the setup and worst poses (default true).")
               .Opt("out", "string", "PNG for that picture (default .oe/vision/analyze.png)."),
           false, [](Engine& engine, const Json& args) { return AnalyzeSkeleton2D(engine, args); }});
    r.Add({"skeleton2d.poses",
           "A contact sheet of a skeleton's poses (a clip sampled, or stress poses), each labeled; focus zooms every pose around one joint.",
           poseParams(Params())
               .Opt("focus", "string", "Bone whose joint every cell is centred on.")
               .Opt("zoom", "number", "With focus: magnification, 1..16 (default 3).")
               .Opt("columns", "integer", "Cells per row (default up to 4).")
               .Opt("cell", "integer", "Cell size in pixels, 64..1024 (default 256).")
               .Opt("out", "string", "PNG path (default .oe/vision/poses.png)."),
           false, [](Engine& engine, const Json& args) { return Skeleton2DContactSheet(engine, args); }});
    r.Add({"skeleton2d.overlay",
           "Draw a skeleton with its rig on top: bones (yellow, named), mesh edges and vertices, optionally one bone's weights as a heat map. "
           "Returns where each bone is in the picture. Writes a PNG to read.",
           Params()
               .Opt("path", "string", "*.skel2d.json to draw (or id).")
               .Opt("id", "entity", "Entity whose Skeleton2D (asset, skin, clip, time) is drawn.")
               .Opt("clip", "string", "Clip to pose (default: the entity's, or the setup pose).")
               .Opt("time", "number", "Seconds into the clip.")
               .Opt("skin", "string", "Skin to show.")
               .Opt("weights", "string", "Bone whose weights tint the vertices: blue 0 .. red 1.")
               .Opt("mesh", "boolean", "Draw mesh edges and vertices (default true).")
               .Opt("labels", "boolean", "Name the bones (default true).")
               .Opt("size", "integer", "Longer side of the picture, 128..2048 (default 768).")
               .Opt("out", "string", "PNG path in the project (default .oe/vision/skeleton.png)."),
           false, [](Engine& engine, const Json& args) {
               Skeleton2D component;
               if (args.has("id")) {
                   const Skeleton2D* source = engine.GetScene().Get<Skeleton2D>(Target(engine, args));
                   if (!source) throw ApiError("component_missing", "entity has no Skeleton2D");
                   component = *source;
               } else if (args.has("path")) {
                   CheckPath(args["path"].asString());
                   component.asset = args["path"].asString();
               } else {
                   throw ApiError("invalid_args", "pass path or id");
               }
               if (args.has("clip")) component.clip = args["clip"].asString(), component.time = 0;
               if (args.has("time")) component.time = args["time"].asFloat();
               if (args.has("skin")) component.skin = args["skin"].asString();
               // Started from scratch: the clip at the time, no fade, not flipped, fully visible.
               component.current.clear();
               component.from.clear();
               component.flipX = component.flipY = false;
               component.opacity = 1;
               component.visible = true;
               std::string error;
               const auto def = component.asset.empty() ? nullptr : engine.Assets().GetSkeleton2D(component.asset, &error);
               if (!def) throw ApiError("invalid_skeleton", component.asset.empty() ? std::string("the entity's Skeleton2D has no asset") : error, kFormatHint);
               if (!component.clip.empty() && !def->FindClip(component.clip))
                   throw ApiError("not_found", "no clip '" + component.clip + "'", "skeleton2d.get lists the clips.");
               const int weightBone = args.has("weights") ? def->BoneIndex(args["weights"].asString()) : -1;
               if (args.has("weights") && weightBone < 0) throw ApiError("not_found", "no bone '" + args["weights"].asString() + "'");
               const Skeleton2DPose pose = Skeleton2DComponentPose(*def, component);
               const Skeleton2DDrawable drawable = BuildSkeleton2DDrawable(def, component.skin, pose, &engine.Assets());

               // Where everything is now: skinned vertices, bone origins and tips.
               std::vector<Vec3> vertices;
               if (drawable.mesh)
                   for (size_t v = 0; v < drawable.mesh->positions.size(); ++v) {
                       const SkinVertex& skin = drawable.mesh->skin[v];
                       Vec3 p;
                       for (size_t k = 0; k < 4; ++k)
                           if (skin.weights[k] > 0) p = p + drawable.palette[skin.joints[k]].TransformPoint(drawable.mesh->positions[v]) * skin.weights[k];
                       vertices.push_back(p);
                   }
               std::vector<Vec3> origins, tips;
               for (size_t b = 0; b < def->bones.size(); ++b) {
                   origins.push_back(pose.world[b].TransformPoint(Vec3()));
                   tips.push_back(pose.world[b].TransformPoint(Vec3(def->bones[b].length, 0, 0)));
               }
               float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
               for (const auto* list : {&vertices, &origins, &tips})
                   for (const Vec3& p : *list) {
                       lo[0] = std::min(lo[0], p.x), lo[1] = std::min(lo[1], p.y);
                       hi[0] = std::max(hi[0], p.x), hi[1] = std::max(hi[1], p.y);
                   }
               float spanX = std::max(0.5f, hi[0] - lo[0]) * 1.15f, spanY = std::max(0.5f, hi[1] - lo[1]) * 1.15f;
               const float cx = (lo[0] + hi[0]) * 0.5f, cy = (lo[1] + hi[1]) * 0.5f;
               const int size = std::clamp(args["size"].asInt(768), 128, 2048);
               const int width = spanX >= spanY ? size : std::max(64, static_cast<int>(static_cast<float>(size) * spanX / spanY));
               const int height = spanX >= spanY ? std::max(64, static_cast<int>(static_cast<float>(size) * spanY / spanX)) : size;
               spanX = spanY * static_cast<float>(width) / static_cast<float>(height);  // square pixels

               Scene preview;
               const EntityId entity = preview.Create("Skeleton");
               preview.Add<Transform>(entity);
               preview.Add<Skeleton2D>(entity) = component;
               RenderView view;
               view.eye = Vec3(cx, cy, 50);
               view.view = Mat4::LookAt(view.eye, Vec3(cx, cy, 0), Vec3(0, 1, 0));
               view.proj = Mat4::Orthographic(spanY * 0.5f, static_cast<float>(width) / static_cast<float>(height), 0.1f, 100);
               view.clearColor = Color(0.16f, 0.17f, 0.21f);
               view.drawUI = false;
               RenderTarget target;
               target.Resize(width, height);
               engine.Renderer().Render(preview, view, target);
               Image image = target.ToImage();
               const float pixels = static_cast<float>(height) / spanY;
               const float left = cx - spanX * 0.5f, top = cy + spanY * 0.5f;
               auto px = [&](const Vec3& p) { return std::make_pair((p.x - left) * pixels, (top - p.y) * pixels); };

               if (drawable.mesh && args["mesh"].asBool(true)) {
                   const Mesh& m = *drawable.mesh;
                   for (size_t i = 0; i + 2 < m.indices.size(); i += 3)
                       for (size_t e = 0; e < 3; ++e) {
                           const auto a = px(vertices[m.indices[i + e]]), b = px(vertices[m.indices[i + (e + 1) % 3]]);
                           DrawLine(image, a.first, a.second, b.first, b.second, {255, 255, 255, 90});
                       }
                   for (size_t v = 0; v < vertices.size(); ++v) {
                       const auto p = px(vertices[v]);
                       if (weightBone < 0) {
                           DrawCircle(image, p.first, p.second, 1.5f, {255, 255, 255, 200}, true);
                           continue;
                       }
                       float w = 0;
                       for (size_t k = 0; k < 4; ++k)
                           if (m.skin[v].joints[k] == weightBone) w += m.skin[v].weights[k];
                       DrawCircle(image, p.first, p.second, 3.5f, {static_cast<uint8_t>(255 * w), 40, static_cast<uint8_t>(255 * (1 - w)), 255}, true);
                   }
               }
               const bool labels = args["labels"].asBool(true);
               // Names go beside their joint where they do not cover an earlier name.
               std::vector<std::array<int, 4>> placed;
               auto label = [&](float x, float y, const std::string& text, Rgba8 color) {
                   const int w = TextWidth(text) + 2, h = TextHeight() + 2;
                   const int ox = static_cast<int>(x), oy = static_cast<int>(y);
                   const int tries[][2] = {{6, -10}, {6, 4}, {-w - 6, -10}, {-w - 6, 4}, {6, -20}, {-w - 6, -20}, {6, 14}, {-w - 6, 14}};
                   std::array<int, 4> at{ox + tries[0][0], oy + tries[0][1], w, h};
                   for (const auto& t : tries) {
                       const std::array<int, 4> r{ox + t[0], oy + t[1], w, h};
                       bool free = true;
                       for (const auto& p : placed) free = free && (r[0] >= p[0] + p[2] || p[0] >= r[0] + r[2] || r[1] >= p[1] + p[3] || p[1] >= r[1] + r[3]);
                       if (free) {
                           at = r;
                           break;
                       }
                   }
                   at[0] = std::clamp(at[0], 0, std::max(0, image.width - w));
                   at[1] = std::clamp(at[1], 0, std::max(0, image.height - h));
                   placed.push_back(at);
                   DrawText(image, at[0] + 1, at[1] + 1, text, color);
               };
               Json bones = Json::MakeArray();
               for (size_t b = 0; b < def->bones.size(); ++b) {
                   const auto o = px(origins[b]), t = px(tips[b]);
                   const int parent = def->bones[b].parent;
                   if (parent >= 0) {
                       const auto p = px(origins[static_cast<size_t>(parent)]);
                       DrawLine(image, p.first, p.second, o.first, o.second, {200, 200, 200, 110});
                   }
                   const Rgba8 color = static_cast<int>(b) == weightBone ? Rgba8{255, 80, 80, 255} : Rgba8{255, 215, 0, 255};
                   if (def->bones[b].length > 0) DrawLine(image, o.first, o.second, t.first, t.second, color, 3);
                   DrawCircle(image, o.first, o.second, 4, {0, 0, 0, 255}, true);
                   DrawCircle(image, o.first, o.second, 3, color, true);
                   if (labels) label(o.first, o.second, def->bones[b].name, color);
                   Json bone = Json::MakeObject();
                   bone["name"] = def->bones[b].name;
                   bone["pixel"] = Json(Json::Array{std::round(o.first), std::round(o.second)});
                   bone["world"] = Json(Json::Array{origins[b].x, origins[b].y});
                   bones.push(bone);
               }
               Json out = Json::MakeObject();
               out["out"] = WriteToolImage(engine, args, ".oe/vision/skeleton.png", image);
               out["width"] = width;
               out["height"] = height;
               out["pixelsPerUnit"] = pixels;
               out["vertices"] = static_cast<int>(vertices.size());
               out["bones"] = bones;
               if (!drawable.missing.empty()) {
                   Json missing = Json::MakeArray();
                   for (const std::string& m : drawable.missing) missing.push(m);
                   out["missingImages"] = missing;
               }
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
