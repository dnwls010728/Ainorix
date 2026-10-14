// Sprite asset commands: create a .sprite.json from an image (cut into frames), read and patch it, cut it
// again keeping each frame's pivot and collision, fit collision outlines to the pixels, and turn an
// entity's own sprite settings into an asset. File writes are not scene undo (like animation.set);
// sprite.convert also changes the scene and is undone there.
#include <algorithm>
#include <cmath>

#include "api/Commands.h"
#include "app/Engine.h"
#include "assets/Assets.h"
#include "assets/SpriteAtlas.h"
#include "core/FileSystem.h"
#include "scene/Components.h"
#include "scene/SpriteAsset.h"

namespace oe {
namespace {

void CheckPath(const std::string& path) {
    if (!IsSpriteAssetPath(path)) throw ApiError("invalid_path", "sprite asset path must end with .sprite.json", "e.g. sprites/hero.sprite.json");
}

// "sprites/hero.sprite.json" -> "hero_": frame names say which image they came from.
std::string DefaultPrefix(const std::string& path) {
    std::string name = path.substr(path.find_last_of("/\\") == std::string::npos ? 0 : path.find_last_of("/\\") + 1);
    name = name.substr(0, name.size() - 12);
    return name.empty() ? std::string("frame_") : name + "_";
}

Json ReadRaw(Engine& e, const std::string& path) {
    CheckPath(path);
    std::string text;
    if (!ReadTextFile(e.ResolvePath(path), text)) throw ApiError("not_found", "cannot read " + path, "Make it with sprite.create.");
    std::string error;
    Json raw = Json::parse(text, &error);
    if (!error.empty() || !raw.isObject()) throw ApiError("invalid_sprite", path + ": " + (error.empty() ? "not a JSON object" : error));
    return raw;
}

std::shared_ptr<const Texture> LoadImage(Engine& e, const std::string& texture) {
    std::string error;
    e.ResolvePath(texture);  // checks the path stays in the project
    auto image = e.Assets().GetTexture(texture, &error);
    if (!image) throw ApiError("invalid_texture", error.empty() ? "cannot read image " + texture : error);
    return image;
}

// Validated (normalized) form of a raw definition; frames must lie inside the image.
Json Normalize(Engine& e, const Json& raw) {
    Json normalized;
    std::string error;
    if (!NormalizeSpriteAsset(raw, normalized, &error)) throw ApiError("invalid_sprite", error);
    const auto image = LoadImage(e, normalized["texture"].asString());
    for (const Json& f : normalized["frames"].items())
        if (f["rect"][0].asInt() + f["rect"][2].asInt() > image->width || f["rect"][1].asInt() + f["rect"][3].asInt() > image->height)
            throw ApiError("invalid_sprite", "frame " + f["name"].asString() + " lies outside the " + std::to_string(image->width) + "x" +
                                                 std::to_string(image->height) + " image");
    if (!normalized["normalTexture"].asString().empty()) LoadImage(e, normalized["normalTexture"].asString());
    return normalized;
}

Json Write(Engine& e, const std::string& path, const Json& raw) {
    const Json normalized = Normalize(e, raw);
    const std::string full = e.ResolvePath(path);
    CreateDirectories(ParentPath(full));
    if (!WriteTextFile(full, raw.dump(2) + "\n")) throw ApiError("write_failed", "cannot write " + path);
    e.Assets().Forget(path);
    return normalized;
}

// Index into raw["frames"] of a frame argument: a number or a frame name.
size_t FrameIndex(const Json& raw, const Json& frame) {
    const Json& frames = raw["frames"];
    if (frame.isNumber()) {
        const double n = frame.asNumber();
        if (std::floor(n) != n || n < 0 || n >= static_cast<double>(frames.size()))
            throw ApiError("invalid_argument", "frame index outside 0.." + std::to_string(static_cast<int>(frames.size()) - 1));
        return static_cast<size_t>(n);
    }
    for (size_t i = 0; i < frames.size(); ++i)
        if (frames[i]["name"].asString() == frame.asString()) return i;
    throw ApiError("not_found", "no frame named '" + frame.asString() + "'", "sprite.get lists the frames.");
}

Json FrameArg(const char* doc) {
    Json p = Json::MakeObject();
    p["type"] = Json::Array{"integer", "string"};
    p["description"] = doc;
    return p;
}

// Merge `values` into `target`: null removes a field, anything else replaces it (arrays too).
void Merge(Json& target, const Json& values) {
    if (!values.isObject()) throw ApiError("invalid_argument", "values must be an object");
    for (const auto& m : values.members()) {
        if (m.second.isNull()) target.erase(m.first);
        else target[m.first] = m.second;
    }
}

Json Result(const std::string& path, const Json& raw, const Json& normalized) {
    Json out = Json::MakeObject();
    out["path"] = path;
    out["values"] = raw;
    out["frames"] = static_cast<uint64_t>(normalized["frames"].size());
    out["resolved"] = normalized;
    return out;
}

}  // namespace

void RegisterSpriteAssetCommands(CommandRegistry& r) {
    r.Add({"sprite.create",
           "Make a sprite asset (*.sprite.json) from an image: cut into frames by `slicing` (default: the whole image is one frame), with "
           "pixels per unit, filtering and the default pivot every Sprite naming it (Sprite.asset) shares. Or write `values` as given. "
           "File write, no scene undo.",
           Params()
               .Req("path", "string", "Output .sprite.json.")
               .Opt("texture", "string", "Image to cut.")
               .Opt("slicing", "object",
                    "{mode: cellSize (cellWidth, cellHeight) | cellCount (columns, rows) | auto (gap: parts closer than this many pixels "
                    "are one sprite), offset: [x, y] margin, spacing: [x, y] gutters, trim: cut cells to their opaque pixels}.")
               .Opt("pixelsPerUnit", "number", "Image pixels per world unit (default 16).")
               .Opt("pixelArt", "boolean", "Sharp nearest-neighbour pixels (default true).")
               .Opt("pivot", "array", "[x, y] 0..1 point of each frame at the entity position (default [0.5, 0.5]; [0.5, 0] = feet).")
               .Opt("prefix", "string", "Frame name prefix (default: the file name and _).")
               .Opt("values", "object", "Complete definition instead of texture/slicing.")
               .Opt("overwrite", "boolean", "Replace an existing file.")
               .Opt("dryRun", "boolean", "Return what would be written without writing it."),
           false, [](Engine& e, const Json& a) {
               const std::string path = a["path"].asString();
               CheckPath(path);
               const bool dryRun = a["dryRun"].asBool();
               if (!dryRun && FileExists(e.ResolvePath(path)) && !a["overwrite"].asBool())
                   throw ApiError("already_exists", "sprite asset exists", "Pass overwrite:true to replace it, or sprite.set to change it.");
               Json raw;
               if (a.has("values")) {
                   raw = a["values"];
               } else {
                   const std::string texture = a["texture"].asString();
                   if (texture.empty()) throw ApiError("invalid_argument", "texture or values is required");
                   const auto image = LoadImage(e, texture);
                   Json slicing = a.has("slicing") ? a["slicing"] : Json::MakeObject();
                   if (!slicing.isObject()) throw ApiError("invalid_argument", "slicing must be an object");
                   if (!slicing.has("mode")) slicing["mode"] = "cellCount";
                   raw = Json::MakeObject();
                   raw["texture"] = texture;
                   raw["pixelsPerUnit"] = a["pixelsPerUnit"].asNumber(16);
                   raw["pixelArt"] = a["pixelArt"].asBool(true);
                   raw["pivot"] = a.has("pivot") ? a["pivot"] : Json(Json::Array{0.5, 0.5});
                   raw["slicing"] = slicing;
                   raw["frames"] = SliceSpriteImage(*image, slicing, a.has("prefix") ? a["prefix"].asString() : DefaultPrefix(path));
               }
               if (dryRun) {
                   Json out = Result(path, raw, Normalize(e, raw));
                   out["dryRun"] = true;
                   return out;
               }
               return Result(path, raw, Write(e, path, raw));
           }});

    r.Add({"sprite.get", "Read a sprite asset: `values` as written, `resolved` with defaults filled in (every frame's pivot), the image size.",
           Params().Req("path", "string", ".sprite.json."), false, [](Engine& e, const Json& a) {
               const std::string path = a["path"].asString();
               const Json raw = ReadRaw(e, path);
               const Json normalized = Normalize(e, raw);
               Json out = Result(path, raw, normalized);
               const auto image = LoadImage(e, normalized["texture"].asString());
               out["image"] = Json::Array{image->width, image->height};
               return out;
           }});

    r.Add({"sprite.set",
           "Change a sprite asset: merge `values` into it (pixelsPerUnit, pixelArt, pivot, normalTexture, texture, frames), or with "
           "`frame` into that frame (name, pivot, collision in frame pixels). A null value removes the field (a frame's pivot or "
           "collision goes back to the default). Validated before writing; no scene undo.",
           Params()
               .Req("path", "string", ".sprite.json.")
               .Req("values", "object", "Fields to merge; null removes one.")
               .OptWith("frame", FrameArg("Frame index or name to change instead of the asset.")),
           false, [](Engine& e, const Json& a) {
               const std::string path = a["path"].asString();
               Json raw = ReadRaw(e, path);
               if (a.has("frame")) Merge(raw["frames"][FrameIndex(raw, a["frame"])], a["values"]);
               else Merge(raw, a["values"]);
               return Result(path, raw, Write(e, path, raw));
           }});

    r.Add({"sprite.slice",
           "Cut a sprite asset's image into frames again with new `slicing` settings. Frames that keep their name (the same cell) keep "
           "their pivot and collision. `dryRun` previews the frames. No scene undo.",
           Params()
               .Req("path", "string", ".sprite.json.")
               .Req("slicing", "object", "As sprite.create.")
               .Opt("prefix", "string", "Frame name prefix (default: the one the frames have, else the file name and _).")
               .Opt("dryRun", "boolean", "Return the frames without writing."),
           false, [](Engine& e, const Json& a) {
               const std::string path = a["path"].asString();
               Json raw = ReadRaw(e, path);
               const auto image = LoadImage(e, raw["texture"].asString());
               std::string prefix = a["prefix"].asString();
               if (!a.has("prefix")) {
                   // The prefix of the existing names ("hero_12" -> "hero_"), so the same cells keep their names.
                   const std::string first = raw["frames"].isArray() && raw["frames"].size() > 0 ? raw["frames"][0]["name"].asString() : std::string();
                   const size_t digits = first.find_last_not_of("0123456789");
                   prefix = !first.empty() && digits != first.size() - 1 ? first.substr(0, digits + 1) : DefaultPrefix(path);
               }
               const Json& slicing = a["slicing"];
               Json frames = SliceSpriteImage(*image, slicing, prefix);
               Json kept = Json::MakeArray();
               for (Json& f : frames.items())
                   for (const Json& old : raw["frames"].items())
                       if (old["name"].asString() == f["name"].asString()) {
                           bool any = false;
                           for (const char* field : {"pivot", "collision"})
                               if (old.has(field)) f[field] = old[field], any = true;
                           if (any) kept.push(f["name"]);
                       }
               raw["slicing"] = slicing;
               raw["frames"] = frames;
               const Json normalized = a["dryRun"].asBool() ? Normalize(e, raw) : Write(e, path, raw);
               Json out = Result(path, raw, normalized);
               out["kept"] = kept;
               if (a["dryRun"].asBool()) out["dryRun"] = true;
               return out;
           }});

    r.Add({"sprite.fit_collision",
           "Trace collision outlines around the opaque pixels of sprite asset frames (the largest part of each frame) and store them in "
           "the asset; Collider2D shape sprite then collides with them. No frame arguments = every frame. No scene undo.",
           Params()
               .Req("path", "string", ".sprite.json.")
               .OptWith("frame", FrameArg("One frame (index or name)."))
               .Opt("frames", "array", "Several frames.")
               .Opt("alphaCutoff", "number", "Pixels with at least this alpha are solid (default 0.5).")
               .Opt("detail", "number", "How far in pixels the outline may stray from the pixel edges to need fewer corners (default 1; 0 = every step).")
               .Opt("convex", "boolean", "The convex hull instead of the traced outline (smoother sliding, fewer pieces).")
               .Opt("dryRun", "boolean", "Return the outlines without writing."),
           false, [](Engine& e, const Json& a) {
               const std::string path = a["path"].asString();
               Json raw = ReadRaw(e, path);
               const Json normalized = Normalize(e, raw);
               const auto image = LoadImage(e, normalized["texture"].asString());
               std::vector<size_t> indices;
               if (a.has("frame")) indices.push_back(FrameIndex(raw, a["frame"]));
               for (const Json& f : a["frames"].items()) indices.push_back(FrameIndex(raw, f));
               if (!a.has("frame") && !a.has("frames"))
                   for (size_t i = 0; i < raw["frames"].size(); ++i) indices.push_back(i);
               const double cutoff = a["alphaCutoff"].asNumber(0.5), detail = a["detail"].asNumber(1);
               if (!std::isfinite(cutoff) || cutoff < 0 || cutoff > 1) throw ApiError("invalid_argument", "alphaCutoff must be 0..1");
               if (!std::isfinite(detail) || detail < 0 || detail > 64) throw ApiError("invalid_argument", "detail must be 0..64 pixels");
               Json outlines = Json::MakeObject(), skipped = Json::MakeArray();
               for (size_t i : indices) {
                   const Json& f = normalized["frames"][i];
                   const std::string name = f["name"].asString();
                   if (f["rotated"].asBool()) {
                       skipped.push(name);  // packed sideways by TexturePacker: draw its outline by hand
                       continue;
                   }
                   Json points = TraceSpriteOutline(image->texels.data(), image->width, image->height, f["rect"][0].asInt(), f["rect"][1].asInt(),
                                                    f["rect"][2].asInt(), f["rect"][3].asInt(), static_cast<float>(cutoff), static_cast<float>(detail),
                                                    a["convex"].asBool());
                   if (points.size() < 3) {
                       skipped.push(name);  // nothing opaque
                       continue;
                   }
                   for (Json& p : points.items())  // rectangle pixels -> frame canvas pixels
                       p = Json::Array{p[0].asInt() + f["trim"][0].asInt(), p[1].asInt() + f["trim"][1].asInt()};
                   raw["frames"][i]["collision"] = points;
                   outlines[name] = points;
               }
               if (!a["dryRun"].asBool() && outlines.size() > 0) Write(e, path, raw);
               Json out = Json::MakeObject();
               out["path"] = path;
               out["outlines"] = outlines;
               if (skipped.size() > 0) out["skipped"] = skipped;
               return out;
           }});

    r.Add({"sprite.convert",
           "Move a sprite's image settings (Sprite.asset as an image or atlas with settings, e.g. \"hero.png?columns=6&pivot=0.5,0\") into a "
           "new sprite asset and point Sprite.asset at it. A polygon Collider2D becomes shape sprite with its outlines (framePoints, "
           "else points) stored per frame. `all` also points every other Sprite drawing the same image the same way at the asset. "
           "Scene undo (the file stays).",
           Params()
               .Req("id", "entity", "Sprite entity.")
               .Req("path", "string", "New .sprite.json.")
               .Opt("all", "boolean", "Also convert the other sprites with the same image settings.")
               .Opt("overwrite", "boolean", "Replace an existing file."),
           true, [](Engine& e, const Json& a) {
               Scene& scene = e.GetScene();
               const Json& v = a["id"];
               const EntityId id = v.isString() ? scene.FindByName(v.asString()) : static_cast<EntityId>(v.asNumber());
               if (!scene.Record(id)) throw ApiError("not_found", "entity does not exist");
               Sprite* sp = scene.Get<Sprite>(id);
               if (!sp) throw ApiError("component_missing", "entity needs Sprite");
               if (IsSpriteAssetPath(sp->asset)) throw ApiError("invalid_sprite", "the sprite already uses " + sp->asset);
               if (const SpriteAnimation* anim = scene.Get<SpriteAnimation>(id); anim && !anim->asset.empty())
                   throw ApiError("invalid_sprite", "the sprite's image comes from " + anim->asset,
                                  "Point that .anim.json at a sprite asset instead (atlas: {path: \"x.sprite.json\"}).");
               SpriteSource source;
               std::string error;
               if (!ParseSpriteSource(sp->asset, source, &error)) throw ApiError("invalid_sprite", error);
               if (source.path.empty()) throw ApiError("invalid_sprite", "the sprite has no image", "Set Sprite.asset to an image first.");
               const std::string path = a["path"].asString();
               CheckPath(path);
               if (FileExists(e.ResolvePath(path)) && !a["overwrite"].asBool())
                   throw ApiError("already_exists", "sprite asset exists", "Pass overwrite:true, or set Sprite.asset to use it.");
               const std::string before = sp->asset;
               Json raw = Json::MakeObject();
               raw["texture"] = "";  // first in the file
               raw["pixelsPerUnit"] = source.ppu;
               raw["pixelArt"] = source.pixelArt;
               raw["pivot"] = Json::Array{std::clamp(source.pivotX, 0.0, 1.0), std::clamp(source.pivotY, 0.0, 1.0)};
               if (!source.normal.empty()) raw["normalTexture"] = source.normal;
               std::vector<std::string> keys;  // Collider2D.framePoints key of each frame
               if (AssetManager::KindOf(source.path) == "atlas") {
                   const auto atlas = e.Assets().GetAtlas(source.path, &error);
                   if (!atlas) throw ApiError("invalid_atlas", error);
                   raw["texture"] = (*atlas)["texture"];
                   raw["frames"] = Json::MakeArray();
                   for (const Json& f : (*atlas)["frames"].items()) {
                       Json frame = f;
                       frame.erase("collision");
                       raw["frames"].push(frame);
                       keys.push_back(f["name"].asString());
                   }
               } else {
                   const auto image = LoadImage(e, source.path);
                   Json slicing = Json::MakeObject();
                   slicing["mode"] = "cellCount";
                   slicing["columns"] = source.columns;
                   slicing["rows"] = source.rows;
                   raw["texture"] = source.path;
                   raw["slicing"] = slicing;
                   raw["frames"] = SliceSpriteImage(*image, slicing, DefaultPrefix(path));
                   for (size_t i = 0; i < raw["frames"].size(); ++i) keys.push_back(std::to_string(i));
               }
               // The polygon collider's outlines, from local units into each frame's pixels.
               Collider2D* collider = scene.Get<Collider2D>(id);
               const bool polygon = collider && collider->shape == "polygon" && collider->points.size() >= 3;
               if (polygon) {
                   const Json normalized = Normalize(e, raw);
                   for (size_t i = 0; i < keys.size(); ++i) {
                       const Json& local = collider->framePoints.has(keys[i]) ? collider->framePoints[keys[i]] : collider->points;
                       const SpritePixelSpace space = SpriteFramePixelSpace(normalized, normalized["frames"][i], sp->width, sp->height, false, false);
                       Json pixels = Json::MakeArray();
                       for (const Json& p : local.items())
                           if (p.isArray() && p.size() >= 2)
                               pixels.push(Json::Array{std::round(space.ToPixelX(p[0].asFloat()) * 100) / 100, std::round(space.ToPixelY(p[1].asFloat()) * 100) / 100});
                       if (pixels.size() >= 3) raw["frames"][i]["collision"] = pixels;
                   }
               }
               const Json normalized = Write(e, path, raw);
               Json changed = Json::MakeArray();
               for (auto& kv : scene.Pool<Sprite>()) {
                   // `all`: the sprites drawing the same image with the same settings (anything else would change them).
                   if (kv.first != id && !(a["all"].asBool() && kv.second.asset == before)) continue;
                   if (const SpriteAnimation* anim = scene.Get<SpriteAnimation>(kv.first); anim && !anim->asset.empty()) continue;
                   kv.second.asset = path;
                   changed.push(static_cast<uint64_t>(kv.first));
               }
               if (polygon) {
                   collider->shape = "sprite";
                   collider->framePoints = Json::MakeObject();
               }
               Json out = Result(path, raw, normalized);
               out["entities"] = changed;
               out["collider"] = polygon;
               return out;
           }});
}

}  // namespace oe
