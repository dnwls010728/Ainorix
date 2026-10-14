#include "api/Commands.h"
#include <algorithm>
#include <cmath>
#include "app/Engine.h"
#include "assets/Assets.h"
#include "core/FileSystem.h"
#include "render/RenderScene.h"
#include "scene/Components.h"
#include "scene/TileGrid.h"
namespace oe {
namespace {
EntityId Target(Engine& e, const Json& args) {
    const Json& v = args["id"];
    const EntityId id = v.isString() ? e.GetScene().FindByName(v.asString()) : static_cast<EntityId>(v.asNumber());
    if (!e.GetScene().Record(id)) throw ApiError("not_found", "entity does not exist");
    return id;
}
Tilemap& Map(Engine& e, const Json& args) {
    auto tm = e.GetScene().Get<Tilemap>(Target(e, args));
    if (!tm) throw ApiError("component_missing", "entity needs Tilemap");
    return *tm;
}
int Layer(const Tilemap& tm, const Json& args, bool insert = false) {
    const double n = args["layer"].asNumber(-1);
    if (!std::isfinite(n) || std::floor(n) != n || n < -1 || n >= static_cast<double>(tm.layers.size() + (insert ? 1 : 0)))
        throw ApiError("invalid_layer", "layer must be -1 (legacy map) or an existing zero-based layer index");
    return static_cast<int>(n);
}
int Coordinate(const Json& v) {
    if (!v.isNumber() || !std::isfinite(v.asNumber()) || std::floor(v.asNumber()) != v.asNumber() || v.asNumber() < 0 || v.asNumber() > 4095)
        throw ApiError("invalid_cell", "cell coordinates must be integers in 0..4095");
    return v.asInt();
}
void CheckMap(const Json& map) {
    if (!map.isArray() || map.size() > 4096) throw ApiError("invalid_tilemap", "map needs up to 4096 rows");
    size_t cells = 0;
    for (const Json& row : map.items()) {
        const size_t n = row.isString() ? row.asString().size() : row.size();
        cells += n;
        if ((!row.isString() && !row.isArray()) || n > 4096 || cells > 1048576) throw ApiError("invalid_tilemap", "map exceeds row/cell limits");
        if (row.isArray())
            for (const Json& cell : row.items()) {
                const Json& tile = cell.isObject() ? cell["tile"] : cell;
                if (!tile.isString() || tile.asString().size() != 1) throw ApiError("invalid_tilemap", "each cell needs one tile character");
                if (cell.isObject())
                    for (const auto& f : cell.members()) {
                        if (f.first == "tile") continue;
                        if (f.first == "flipX" || f.first == "flipY") {
                            if (!f.second.isBool()) throw ApiError("invalid_tilemap", "cell flips must be boolean");
                        } else if (f.first == "rotation") {
                            const double r = f.second.asNumber(-1);
                            if (!f.second.isNumber() || (r != 0 && r != 90 && r != 180 && r != 270))
                                throw ApiError("invalid_tilemap", "rotation must be 0,90,180,270");
                        } else
                            throw ApiError("invalid_tilemap", "unknown cell field: " + f.first);
                    }
            }
    }
}

// Convex hull (counter-clockwise) of integer points, Andrew's monotone chain.
std::vector<std::pair<int, int>> Hull(std::vector<std::pair<int, int>> points) {
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());
    if (points.size() < 3) return points;
    auto cross = [](const std::pair<int, int>& o, const std::pair<int, int>& a, const std::pair<int, int>& b) {
        return static_cast<long long>(a.first - o.first) * (b.second - o.second) - static_cast<long long>(a.second - o.second) * (b.first - o.first);
    };
    std::vector<std::pair<int, int>> hull(points.size() * 2);
    size_t k = 0;
    for (size_t i = 0; i < points.size(); ++i) {
        while (k >= 2 && cross(hull[k - 2], hull[k - 1], points[i]) <= 0) --k;
        hull[k++] = points[i];
    }
    for (size_t i = points.size() - 1, lower = k + 1; i-- > 0;) {
        while (k >= lower && cross(hull[k - 2], hull[k - 1], points[i]) <= 0) --k;
        hull[k++] = points[i];
    }
    hull.resize(k - 1);
    return hull;
}

// Drops the corner that cuts off the least area until at most `count` remain (Box2D polygons
// take 8 vertices). The outline shrinks a little at each drop.
std::vector<Vec3> Simplify(std::vector<Vec3> p, size_t count) {
    while (p.size() > count) {
        size_t best = 0;
        float bestArea = INFINITY;
        for (size_t i = 0; i < p.size(); ++i) {
            const Vec3& a = p[(i + p.size() - 1) % p.size()];
            const Vec3& b = p[i];
            const Vec3& c = p[(i + 1) % p.size()];
            const float area = std::fabs((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x));
            if (area < bestArea) {
                bestArea = area;
                best = i;
            }
        }
        p.erase(p.begin() + static_cast<std::ptrdiff_t>(best));
    }
    return p;
}

// Schema of a frame argument: a sheet index or an atlas frame name.
Json FrameParam(const char* doc) {
    Json p = Json::MakeObject();
    p["type"] = Json::Array{"integer", "string"};
    p["description"] = doc;
    return p;
}

// Collider2D.framePoints key of a frame argument: an index (number or digits) or an atlas frame name.
std::string FrameKey(const Json& frame) {
    if (frame.isNumber()) {
        const double n = frame.asNumber();
        if (!std::isfinite(n) || n < 0 || std::floor(n) != n || n > 65535) throw ApiError("invalid_argument", "frame index must be an integer in 0..65535");
        return std::to_string(static_cast<int>(n));
    }
    const std::string name = frame.asString();
    if (!frame.isString() || name.empty() || name.size() > 128) throw ApiError("invalid_argument", "frame must be an index or an atlas frame name (1..128 bytes)");
    return name;
}

// Convex outline (counter-clockwise, at most maxPoints corners) of the opaque pixels of the frame the
// sprite shows now, as [x, y] points in the entity's local units.
Json OutlineFrame(Engine& e, EntityId id, float cutoff, size_t maxPoints) {
    const RenderItem* item = nullptr;
    const std::vector<RenderItem> items = GatherRenderItems(e.GetScene(), &e.Assets());
    for (const RenderItem& it : items)
        if (it.id == id) item = &it;
    if (!item || !item->textureOverride || item->textureOverride->width <= 0)
        throw ApiError("invalid_sprite", "the sprite has no drawn image to outline", "Give it a texture or an atlas frame first.");
    const Texture& tex = *item->textureOverride;
    // The frame's pixel rectangle from the item's uv rect (flips have a negative scale).
    const float uA = item->uvOffset[0], uB = item->uvOffset[0] + item->uvScale[0];
    const float vA = item->uvOffset[1], vB = item->uvOffset[1] + item->uvScale[1];
    const int x0 = static_cast<int>(std::floor(std::min(uA, uB) * static_cast<float>(tex.width) + 0.05f));
    const int x1 = static_cast<int>(std::ceil(std::max(uA, uB) * static_cast<float>(tex.width) - 0.05f));
    const int y0 = static_cast<int>(std::floor(std::min(vA, vB) * static_cast<float>(tex.height) + 0.05f));
    const int y1 = static_cast<int>(std::ceil(std::max(vA, vB) * static_cast<float>(tex.height) - 0.05f));
    std::vector<std::pair<int, int>> corners;
    for (int y = std::max(0, y0); y < std::min(tex.height, y1); ++y)
        for (int x = std::max(0, x0); x < std::min(tex.width, x1); ++x) {
            const float alpha = static_cast<float>(tex.texels[static_cast<size_t>(y) * static_cast<size_t>(tex.width) + static_cast<size_t>(x)] >> 24) / 255.0f;
            if (alpha < cutoff) continue;
            corners.insert(corners.end(), {{x, y}, {x + 1, y}, {x, y + 1}, {x + 1, y + 1}});
        }
    const auto hull = Hull(corners);
    if (hull.size() < 3) throw ApiError("invalid_sprite", "the frame has no opaque area to outline", "Lower alphaCutoff.");
    // Frame pixels -> the item's unit quad (flips included) -> world -> the entity's local space.
    const Mat4 toLocal = e.GetScene().WorldMatrix(id).Inverse() * item->world;
    const float fw = static_cast<float>(std::max(1, x1 - x0)), fh = static_cast<float>(std::max(1, y1 - y0));
    std::vector<Vec3> points;
    for (const auto& corner : hull) {
        const float u = (static_cast<float>(corner.first - x0)) / fw, v = (static_cast<float>(corner.second - y0)) / fh;
        const float qx = item->uvScale[0] < 0 ? 0.5f - u : u - 0.5f;
        const float qy = item->uvScale[1] < 0 ? v - 0.5f : 0.5f - v;
        points.push_back(toLocal.TransformPoint(Vec3(qx, qy, 0)));
    }
    // Flips mirror the outline: keep it counter-clockwise.
    float area = 0;
    for (size_t i = 0; i < points.size(); ++i)
        area += points[i].x * points[(i + 1) % points.size()].y - points[(i + 1) % points.size()].x * points[i].y;
    if (area < 0) std::reverse(points.begin(), points.end());
    points = Simplify(points, maxPoints);
    Json out = Json::MakeArray();
    // Rounded to 1/1000 unit: stable, readable scene files.
    for (const Vec3& p : points) out.push(Json::Array{std::round(static_cast<double>(p.x) * 1000) / 1000, std::round(static_cast<double>(p.y) * 1000) / 1000});
    return out;
}

}  // namespace
void RegisterTwoDimensionalCommands(CommandRegistry& r) {
    r.Add({"tilemap.layer_get", "Read tilemap layers and the legacy map.", Params().Req("id", "entity", "Tilemap."), false,
           [](Engine& e, const Json& a) {
               const Tilemap& tm = Map(e, a);
               Json out = Json::MakeObject();
               out["map"] = tm.map;
               out["layers"] = tm.layers;
               return out;
           }});
    r.Add({"tilemap.layer_set", "Append, change, move or remove an entry of Tilemap.layers (or replace the base map, layer -1); scene undo.",
           Params()
               .Req("id", "entity", "Tilemap.")
               .Req("layer", "integer", "-1 = the base map; 0.. = Tilemap.layers; layers.size appends.")
               .Opt("values", "object", "name, map, layer, order, ySort, ySortOffset, visible, collision; patches the existing fields.")
               .Opt("remove", "boolean", "Remove this entry of Tilemap.layers.")
               .Opt("moveTo", "integer", "Move this entry of Tilemap.layers to that index (draw order inside a sorting layer).")
               .Opt("merge", "string", "Undo group key."),
           true, [](Engine& e, const Json& a) {
               Tilemap& tm = Map(e, a);
               const int layer = Layer(tm, a, true);
               if (a["remove"].asBool()) {
                   if (layer < 0 || static_cast<size_t>(layer) >= tm.layers.size())
                       throw ApiError("invalid_layer", "cannot remove the base map or a layer that does not exist");
                   tm.layers.items().erase(tm.layers.items().begin() + layer);
                   return Json(true);
               }
               if (a.has("moveTo")) {
                   const double to = a["moveTo"].asNumber(-1);
                   if (layer < 0 || static_cast<size_t>(layer) >= tm.layers.size() || std::floor(to) != to || to < 0 ||
                       to >= static_cast<double>(tm.layers.size()))
                       throw ApiError("invalid_layer", "moveTo needs an existing layer and a target index in 0..layers.size-1");
                   Json moved = tm.layers[layer];
                   auto& list = tm.layers.items();
                   list.erase(list.begin() + layer);
                   list.insert(list.begin() + static_cast<int>(to), moved);
                   return Json(static_cast<int>(to));
               }
               const Json& v = a["values"];
               if (!v.isObject()) throw ApiError("invalid_tilemap", "values are required");
               Json value = layer < 0 ? Json::MakeObject() : (static_cast<size_t>(layer) < tm.layers.size() ? tm.layers[layer] : Json::MakeObject());
               for (const auto& f : v.members()) {
                   if (f.first != "name" && f.first != "map" && f.first != "layer" && f.first != "order" && f.first != "ySort" &&
                       f.first != "ySortOffset" && f.first != "visible" && f.first != "collision")
                       throw ApiError("invalid_tilemap", "unknown layer field: " + f.first);
                   value[f.first] = f.second;
               }
               if (!value.has("map")) value["map"] = Json::MakeArray();
               CheckMap(value["map"]);
               Json validation = Json::MakeObject();
               validation["layers"] = Json::Array{value};
               std::string error;
               if (!ValidateTilemapData(validation, &error)) throw ApiError("invalid_tilemap", error);
               if (layer < 0) {
                   tm.map = value["map"];
                   return tm.map;
               }
               if (tm.layers.size() >= 64 && static_cast<size_t>(layer) == tm.layers.size()) throw ApiError("invalid_tilemap", "at most 64 layers");
               if (static_cast<size_t>(layer) == tm.layers.size())
                   tm.layers.push(value);
               else
                   tm.layers[layer] = value;
               return value;
           }});
    for (bool write : {false, true})
        r.Add({write ? "tilemap.cell_set" : "tilemap.cell_get",
               write ? "Set a transformed cell with scene undo." : "Read a cell, its transform and tile-rule user data.",
               write ? static_cast<Json>(Params()
                                             .Req("id", "entity", "Tilemap.")
                                             .Req("col", "integer", "Column.")
                                             .Req("row", "integer", "Row.")
                                             .Opt("layer", "integer", "-1 legacy, default.")
                                             .Req("values", "object", "tile,flipX,flipY,rotation."))
                     : static_cast<Json>(Params()
                                             .Req("id", "entity", "Tilemap.")
                                             .Req("col", "integer", "Column.")
                                             .Req("row", "integer", "Row.")
                                             .Opt("layer", "integer", "-1 legacy, default.")),
               write, [write](Engine& e, const Json& a) {
                   Tilemap& tm = Map(e, a);
                   const int layer = Layer(tm, a), col = Coordinate(a["col"]), row = Coordinate(a["row"]);
                   Tilemap part = tm;
                   part.layers = Json::MakeArray();
                   if (layer >= 0) part.map = tm.layers[layer]["map"];
                   if (write) {
                       Json line = static_cast<size_t>(row) < part.map.size() ? part.map[row] : Json::MakeArray();
                       if (line.isString()) {
                           Json array = Json::MakeArray();
                           for (char c : line.asString()) array.push(std::string(1, c));
                           line = array;
                       }
                       while (line.size() <= static_cast<size_t>(col)) line.push(" ");
                       line[col] = a["values"];
                       while (part.map.size() <= static_cast<size_t>(row)) part.map.push("");
                       part.map[row] = line;
                       CheckMap(part.map);
                       if (layer < 0)
                           tm.map = part.map;
                       else
                           tm.layers[layer]["map"] = part.map;
                   }
                   Json out = Json::MakeObject();
                   const char c = TileAt(part, col, row);
                   out["tile"] = c ? std::string(1, c) : "";
                   const Json& map = part.map;
                   if (static_cast<size_t>(row) < map.size() && map[row].isArray() && static_cast<size_t>(col) < map[row].size())
                       out["values"] = map[row][col];
                   TilesetLookup lookup = e.Assets().Tilesets();
                   const TileRules rules = BuildTileRules(part, &lookup);
                   const TileDef* rule = RuleAt(part, rules, col, row);
                   out["data"] = rule ? rule->data : Json::MakeObject();
                   return out;
               }});
    r.Add({"tilemap.rule_set", "Validate and replace a tile rule in the selected map's legend (scene undo).",
           Params()
               .Req("id", "entity", "Tilemap.")
               .Req("char", "string", "One ASCII character.")
               .Req("values", "object", "Tile rule including frame, collision, animation, data.")
               .Opt("merge", "string", "Undo group."),
           true, [](Engine& e, const Json& a) {
               Tilemap& tm = Map(e, a);
               const std::string key = a["char"].asString();
               if (key.size() != 1) throw ApiError("invalid_tile", "char needs one character");
               TileDef rule;
               std::string error;
               if (!ParseTileRule(a["values"], rule, &error)) throw ApiError("invalid_tile", error);
               tm.legend[key] = a["values"];
               return a["values"];
           }});
    r.Add({"sprite.collision",
           "Set the sprite's Collider2D polygon in local world units (scene undo): its points, or with `frame` the outline used while "
           "that frame shows (Collider2D.framePoints).",
           Params()
               .Req("id", "entity", "Sprite.")
               .Opt("points", "array", "3..64 [x,y] points (required unless clear).")
               .OptWith("frame", FrameParam("Frame index or atlas frame name: write framePoints[frame] instead of points."))
               .Opt("clear", "boolean", "With frame: remove that frame's outline, so the frame uses points again.")
               .Opt("merge", "string", "Undo group for point dragging."),
           true, [](Engine& e, const Json& a) {
               const EntityId id = Target(e, a);
               if (!e.GetScene().Get<Sprite>(id)) throw ApiError("component_missing", "entity needs Sprite");
               const bool perFrame = a.has("frame");
               const std::string key = perFrame ? FrameKey(a["frame"]) : std::string();
               Collider2D* existing = e.GetScene().Get<Collider2D>(id);
               if (a["clear"].asBool()) {
                   if (!perFrame) throw ApiError("invalid_argument", "clear needs a frame", "Pass frame: the frame whose outline to remove.");
                   Json values = Json::MakeObject();
                   values["framePoints"] = existing ? existing->framePoints : Json::MakeObject();
                   values["framePoints"].erase(key);
                   if (existing) existing->framePoints = values["framePoints"];
                   return values;
               }
               const Json& points = a["points"];
               if (!points.isArray() || points.size() < 3 || points.size() > 64) throw ApiError("invalid_polygon", "polygon needs 3..64 points");
               for (const Json& p : points.items())
                   if (!p.isArray() || p.size() != 2 || !p[0].isNumber() || !p[1].isNumber() || !std::isfinite(p[0].asNumber()) ||
                       !std::isfinite(p[1].asNumber()))
                       throw ApiError("invalid_polygon", "points must be finite [x,y] pairs");
               Json values = Json::MakeObject();
               values["shape"] = "polygon";
               if (perFrame) {
                   if (existing && existing->framePoints.size() >= 1024 && !existing->framePoints.has(key))
                       throw ApiError("invalid_polygon", "at most 1024 frame outlines");
                   values["framePoints"] = existing ? existing->framePoints : Json::MakeObject();
                   values["framePoints"][key] = points;
                   // A new polygon collider gets the frame's outline as its default too, so other frames collide.
                   if (!existing || existing->shape != "polygon" || existing->points.size() < 3) values["points"] = points;
               } else {
                   values["points"] = points;
               }
               std::string error;
               if (!ApplyComponentJson(*TypeRegistry::Find("Collider2D"), &e.GetScene().Add<Collider2D>(id), values, &error))
                   throw ApiError("invalid_polygon", error);
               return values;
           }});
    r.Add({"sprite.outline",
           "Fit a convex Collider2D polygon around the opaque pixels of the sprite's current frame (scene undo). Returns the points. "
           "`frame` / `frames` / `allFrames` fit those frames instead and store each one in Collider2D.framePoints.",
           Params()
               .Req("id", "entity", "Sprite entity.")
               .Opt("alphaCutoff", "number", "Pixels with at least this alpha count as solid (default 0.5).")
               .Opt("maxPoints", "integer", "Corners kept, 3..8 (default 8: the most a Box2D polygon takes).")
               .OptWith("frame", FrameParam("One frame (index or atlas frame name) to fit into framePoints."))
               .Opt("frames", "array", "Several frames to fit into framePoints.")
               .Opt("allFrames", "boolean", "Fit every frame of the sheet or atlas (at most 1024) into framePoints.")
               .Opt("merge", "string", "Undo group key."),
           true, [](Engine& e, const Json& a) {
               const EntityId id = Target(e, a);
               Sprite* sprite = e.GetScene().Get<Sprite>(id);
               if (!sprite) throw ApiError("component_missing", "entity needs Sprite");
               const double maxPoints = a["maxPoints"].asNumber(8);
               if (std::floor(maxPoints) != maxPoints || maxPoints < 3 || maxPoints > 8) throw ApiError("invalid_argument", "maxPoints must be 3..8");
               const float cutoff = static_cast<float>(a["alphaCutoff"].asNumber(0.5));
               std::vector<Json> frames;
               if (a.has("frame")) frames.push_back(a["frame"]);
               for (const Json& f : a["frames"].items()) frames.push_back(f);
               if (a["allFrames"].asBool()) {
                   if (!sprite->atlas.empty()) {
                       auto atlas = e.Assets().GetAtlas(sprite->atlas);
                       if (!atlas) throw ApiError("invalid_sprite", "cannot read the sprite's atlas " + sprite->atlas);
                       for (const Json& f : (*atlas)["frames"].items()) frames.push_back(f["name"]);
                   } else {
                       for (int i = 0; i < std::max(1, sprite->columns) * std::max(1, sprite->rows); ++i) frames.push_back(Json(i));
                   }
               }
               if (frames.size() > 1024) throw ApiError("invalid_argument", "at most 1024 frames per call");
               if (frames.empty()) {
                   Json values = Json::MakeObject();
                   values["shape"] = "polygon";
                   values["points"] = OutlineFrame(e, id, cutoff, static_cast<size_t>(maxPoints));
                   std::string error;
                   if (!ApplyComponentJson(*TypeRegistry::Find("Collider2D"), &e.GetScene().Add<Collider2D>(id), values, &error))
                       throw ApiError("invalid_polygon", error);
                   return values;
               }
               // Each frame is shown in turn to fit it, then the sprite's own frame comes back.
               const SpriteFrame shown = sprite->frame;
               Json framePoints = e.GetScene().Get<Collider2D>(id) ? e.GetScene().Get<Collider2D>(id)->framePoints : Json::MakeObject();
               if (!framePoints.isObject()) framePoints = Json::MakeObject();
               Json skipped = Json::MakeArray();
               std::vector<std::string> keys;
               for (const Json& frame : frames) keys.push_back(FrameKey(frame));  // all checked before the sprite changes
               for (const std::string& key : keys) {
                   const bool index = key.find_first_not_of("0123456789") == std::string::npos;
                   e.GetScene().Get<Sprite>(id)->frame = index ? Json(std::stoi(key)) : Json(key);
                   try {
                       framePoints[key] = OutlineFrame(e, id, cutoff, static_cast<size_t>(maxPoints));
                   } catch (const ApiError&) {
                       skipped.push(key);  // an empty frame has nothing to outline
                   }
               }
               e.GetScene().Get<Sprite>(id)->frame = shown;
               if (framePoints.size() > 1024) throw ApiError("invalid_polygon", "at most 1024 frame outlines");
               Json values = Json::MakeObject();
               values["shape"] = "polygon";
               values["framePoints"] = framePoints;
               const Collider2D* existing = e.GetScene().Get<Collider2D>(id);
               if (!existing || existing->shape != "polygon" || existing->points.size() < 3)
                   for (const auto& m : framePoints.members()) {
                       values["points"] = m.second;  // the first fitted outline: frames without one still collide
                       break;
                   }
               std::string error;
               if (!ApplyComponentJson(*TypeRegistry::Find("Collider2D"), &e.GetScene().Add<Collider2D>(id), values, &error))
                   throw ApiError("invalid_polygon", error);
               if (skipped.size() > 0) values["skipped"] = skipped;
               return values;
           }});
}
}  // namespace oe
