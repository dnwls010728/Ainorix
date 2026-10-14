#include "assets/SpriteAtlas.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>
#include "api/Commands.h"
#include "app/Engine.h"
#include "assets/Assets.h"
#include "core/FileSystem.h"
#include "render/Mesh.h"
namespace oe {
namespace {
int Pixel(const Json& v, int min = 0) {
    const double n = v.asNumber(-1);
    if (!v.isNumber() || !std::isfinite(n) || n < min || n > 65536 || std::floor(n) != n)
        throw std::runtime_error("atlas pixel value must be an integer in range");
    return static_cast<int>(n);
}
Json Rect(const Json& v) {
    if (!v.isObject()) throw std::runtime_error("TexturePacker frame needs a rectangle object");
    return Json::Array{v["x"], v["y"], v["w"], v["h"]};
}

// A frame found by slicing: its pixels, and when trimmed the canvas it came from.
struct SliceRect {
    int x = 0, y = 0, w = 0, h = 0;
    int canvasW = 0, canvasH = 0, trimX = 0, trimY = 0;  // canvasW 0 = untrimmed
    int index = -1;                                      // grid cell number (names stay stable when empty cells are skipped)
};

bool Opaque(const Texture& image, int x, int y) {
    return (image.texels[static_cast<size_t>(y) * static_cast<size_t>(image.width) + static_cast<size_t>(x)] >> 24) != 0;
}

// Cells of cw x ch pixels from (ox, oy), `sx`/`sy` pixels apart (sheets drawn with a margin and gutters).
// Without a margin or gutters the cells must tile the image exactly; with them, cells that do not fit are left out.
std::vector<SliceRect> GridCells(const Texture& image, int cw, int ch, bool trim, int ox = 0, int oy = 0, int sx = 0, int sy = 0) {
    const bool spaced = ox || oy || sx || sy;
    if (!spaced && (image.width % cw || image.height % ch))
        throw ApiError("invalid_atlas", "the cell size must divide the image size",
                       "Pick a cell size that divides " + std::to_string(image.width) + "x" + std::to_string(image.height) + ", or set offset/spacing.");
    const int cols = image.width >= ox + cw ? (image.width - ox + sx) / (cw + sx) : 0;
    const int rows = image.height >= oy + ch ? (image.height - oy + sy) / (ch + sy) : 0;
    if (cols * rows > 65536) throw ApiError("invalid_atlas", "too many frames (65536 at most)");
    std::vector<SliceRect> out;
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < cols; ++col) {
            SliceRect s{ox + col * (cw + sx), oy + row * (ch + sy), cw, ch};
            s.index = row * cols + col;
            if (trim) {
                int x0 = cw, y0 = ch, x1 = -1, y1 = -1;
                for (int y = 0; y < ch; ++y)
                    for (int x = 0; x < cw; ++x)
                        if (Opaque(image, s.x + x, s.y + y)) {
                            x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
                        }
                if (x1 < 0) continue;  // empty cell
                s = {s.x + x0, s.y + y0, x1 - x0 + 1, y1 - y0 + 1, cw, ch, x0, y0, s.index};
            }
            out.push_back(s);
        }
    }
    return out;
}

// Groups of opaque pixels (parts closer than `gap` pixels join), ordered in rows from the top.
std::vector<SliceRect> FindSprites(const Texture& image, int gap) {
    const int w = image.width, h = image.height;
    std::vector<int> label(static_cast<size_t>(w) * static_cast<size_t>(h), -1);
    std::vector<SliceRect> boxes;
    std::vector<std::pair<int, int>> stack;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (!Opaque(image, x, y) || label[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] >= 0) continue;
            const int id = static_cast<int>(boxes.size());
            int x0 = x, y0 = y, x1 = x, y1 = y;
            stack.push_back({x, y});
            label[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] = id;
            while (!stack.empty()) {
                const auto p = stack.back();
                stack.pop_back();
                x0 = std::min(x0, p.first), y0 = std::min(y0, p.second), x1 = std::max(x1, p.first), y1 = std::max(y1, p.second);
                const int next[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (const auto& d : next) {
                    const int nx = p.first + d[0], ny = p.second + d[1];
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h || !Opaque(image, nx, ny)) continue;
                    int& l = label[static_cast<size_t>(ny) * static_cast<size_t>(w) + static_cast<size_t>(nx)];
                    if (l >= 0) continue;
                    l = id;
                    stack.push_back({nx, ny});
                }
            }
            boxes.push_back({x0, y0, x1 - x0 + 1, y1 - y0 + 1});
            if (boxes.size() > 65536) throw ApiError("invalid_atlas", "too many separate parts (65536 at most)");
        }
    }
    // Join boxes that overlap once grown by the gap (a sword next to its hand), until nothing changes.
    for (bool merged = true; merged;) {
        merged = false;
        for (size_t i = 0; i < boxes.size() && !merged; ++i) {
            for (size_t j = i + 1; j < boxes.size(); ++j) {
                const SliceRect& a = boxes[i];
                const SliceRect& b = boxes[j];
                if (a.x - gap > b.x + b.w - 1 || b.x - gap > a.x + a.w - 1 || a.y - gap > b.y + b.h - 1 || b.y - gap > a.y + a.h - 1) continue;
                const int x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
                const int x1 = std::max(a.x + a.w, b.x + b.w), y1 = std::max(a.y + a.h, b.y + b.h);
                boxes[i] = {x0, y0, x1 - x0, y1 - y0};
                boxes.erase(boxes.begin() + static_cast<std::ptrdiff_t>(j));
                merged = true;
                break;
            }
        }
    }
    // Rows: a box belongs to the row whose band contains its vertical center; rows top to bottom,
    // boxes in a row left to right.
    std::sort(boxes.begin(), boxes.end(), [](const SliceRect& a, const SliceRect& b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
    std::vector<std::vector<SliceRect>> rows;
    int bandEnd = -1;
    for (const SliceRect& box : boxes) {
        if (rows.empty() || box.y + box.h / 2 > bandEnd) {
            rows.emplace_back();
            bandEnd = box.y + box.h - 1;
        } else {
            bandEnd = std::max(bandEnd, box.y + box.h - 1);
        }
        rows.back().push_back(box);
    }
    std::vector<SliceRect> out;
    for (auto& row : rows) {
        std::sort(row.begin(), row.end(), [](const SliceRect& a, const SliceRect& b) { return a.x < b.x; });
        out.insert(out.end(), row.begin(), row.end());
    }
    return out;
}
}  // namespace
bool NormalizeSpriteAtlas(const Json& input, Json& output, std::string* error) {
    try {
        const bool packed = input.has("meta");
        const std::string texture = (packed ? input["meta"]["image"] : input["texture"]).asString();
        if (texture.empty() || texture.size() > 1024) throw std::runtime_error("atlas needs a texture path");
        const Json& source = input["frames"];
        if ((!source.isArray() && !source.isObject()) || source.size() == 0 || source.size() > 65536)
            throw std::runtime_error("atlas needs 1..65536 frames");
        output = Json::MakeObject();
        output["texture"] = texture;
        output["frames"] = Json::MakeArray();
        std::set<std::string> names;
        for (size_t i = 0; i < source.size(); ++i) {
            const Json& item = source.isArray() ? source[i] : source.members()[i].second;
            const std::string name = source.isObject() ? source.members()[i].first : item[packed ? "filename" : "name"].asString();
            if (name.empty() || name.size() > 128 || !names.insert(name).second)
                throw std::runtime_error("atlas frame names must be unique (1..128 bytes)");
            Json f = Json::MakeObject();
            f["name"] = name;
            const Json rect = packed ? Rect(item["frame"]) : item["rect"];
            if (!rect.isArray() || rect.size() != 4) throw std::runtime_error("atlas rect needs [x,y,width,height]");
            Pixel(rect[0]);
            Pixel(rect[1]);
            const int w = Pixel(rect[2], 1), h = Pixel(rect[3], 1);
            const bool rotated = item["rotated"].asBool(false);
            const int drawW = rotated ? h : w, drawH = rotated ? w : h;
            Json size = packed ? Json(Json::Array{item["sourceSize"]["w"], item["sourceSize"]["h"]}) : item["size"];
            if ((!packed && !item.has("size")) || (packed && !item.has("sourceSize"))) size = Json::Array{drawW, drawH};
            if (!size.isArray() || size.size() != 2) throw std::runtime_error("atlas size needs [originalWidth,originalHeight]");
            const int sw = Pixel(size[0], 1), sh = Pixel(size[1], 1);
            Json trim = packed && item.has("spriteSourceSize") ? Json(Json::Array{item["spriteSourceSize"]["x"], item["spriteSourceSize"]["y"]})
                                                               : item["trim"];
            if (!trim.isArray()) trim = Json::Array{0, 0};
            if (trim.size() != 2) throw std::runtime_error("atlas trim needs [left,top]");
            const int tx = Pixel(trim[0]), ty = Pixel(trim[1]);
            if (tx + drawW > sw || ty + drawH > sh) throw std::runtime_error("atlas trimmed rectangle exceeds original canvas");
            Json pivot =
                packed && item["pivot"].isObject() ? Json(Json::Array{item["pivot"]["x"], 1 - item["pivot"]["y"].asFloat(0.5f)}) : item["pivot"];
            if (!pivot.isArray()) pivot = Json::Array{0.5, 0.5};
            if (pivot.size() != 2) throw std::runtime_error("atlas pivot needs [x,y]");
            for (const Json& v : pivot.items())
                if (!v.isNumber() || !std::isfinite(v.asNumber()) || v.asNumber() < 0 || v.asNumber() > 1)
                    throw std::runtime_error("atlas pivot must be in 0..1");
            f["rect"] = rect;
            f["size"] = size;
            f["trim"] = trim;
            f["pivot"] = pivot;
            if (rotated) f["rotated"] = true;
            output["frames"].push(f);
        }
        return true;
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        return false;
    }
}
const Json* FindAtlasFrame(const Json& atlas, const Json& frame) {
    const Json& frames = atlas["frames"];
    if (frame.isString()) {
        // Name -> index of the last atlas looked up per thread (sprites of one atlas come in runs);
        // a hit is confirmed by the name, so a reloaded atlas at the same address cannot mislead it.
        thread_local const Json* indexed = nullptr;
        thread_local std::unordered_map<std::string, size_t> names;
        const std::string& name = frame.asString();
        if (indexed == &frames) {
            const auto found = names.find(name);
            if (found != names.end() && found->second < frames.size() && frames[found->second]["name"].asString() == name) return &frames[found->second];
        }
        indexed = &frames;
        names.clear();
        for (size_t i = 0; i < frames.size(); ++i) names.emplace(frames[i]["name"].asString(), i);
        const auto found = names.find(name);
        return found != names.end() ? &frames[found->second] : nullptr;
    } else if (frame.isNumber() && frame.asInt() >= 0 && static_cast<size_t>(frame.asInt()) < frames.size())
        return &frames[frame.asInt()];
    return nullptr;
}
void RegisterAtlasCommands(CommandRegistry& registry) {
    registry.Add({"atlas.create", "Write a named sprite atlas, normalize TexturePacker JSON, or slice a grid; no scene undo.",
                  Params()
                      .Req("path", "string", "Output .atlas.json.")
                      .Opt("values", "object", "Atlas or TexturePacker definition.")
                      .Opt("source", "string", "TexturePacker JSON to read (image resolves beside source).")
                      .Opt("texture", "string", "Grid image to slice.")
                      .Opt("cellWidth", "integer", "Slice width in pixels.")
                      .Opt("cellHeight", "integer", "Slice height in pixels.")
                      .Opt("trim", "boolean", "Grid: cut each cell to its opaque pixels, keeping the cell as the original size so frames do not jump; empty cells are skipped.")
                      .Opt("auto", "boolean", "Instead of a grid, find each sprite of `texture` by the transparent space around it (top to bottom, left to right).")
                      .Opt("gap", "integer", "auto: parts closer than this many pixels are one sprite (default 2).")
                      .Opt("pivot", "array", "[x, y] pivot of every frame, 0..1 like Sprite.pivotX/Y ((0.5, 0) = bottom center).")
                      .Opt("offset", "array", "Grid: [x, y] pixels before the first cell (sheet margin).")
                      .Opt("spacing", "array", "Grid: [x, y] pixels between cells (gutters).")
                      .Opt("prefix", "string", "Frame name prefix (default frame_).")
                      .Opt("overwrite", "boolean", "Replace existing file.")
                      .Opt("dryRun", "boolean", "Return the frames that would be written without writing the file (slicing previews)."),
                  false, [](Engine& e, const Json& args) {
                      const std::string path = args["path"].asString(), full = e.ResolvePath(path);
                      if (AssetManager::KindOf(path) != "atlas") throw ApiError("invalid_path", "atlas path must end with .atlas.json");
                      const bool dryRun = args["dryRun"].asBool();
                      if (!dryRun && FileExists(full) && !args["overwrite"].asBool())
                          throw ApiError("already_exists", "atlas exists", "Pass overwrite:true to replace it.");
                      Json input = args["values"];
                      if (args.has("source")) {
                          const std::string source = args["source"].asString();
                          std::string text;
                          if (!ReadTextFile(e.ResolvePath(source), text)) throw ApiError("not_found", "cannot read atlas source");
                          input = Json::parse(text);
                          const std::string image = input["meta"]["image"].asString();
                          input["meta"]["image"] = JoinPath(ParentPath(source), image);
                      } else if (args.has("texture")) {
                          const std::string texture = args["texture"].asString();
                          std::string error;
                          auto image = e.Assets().GetTexture(texture, &error);
                          if (!image) throw ApiError("invalid_texture", error);
                          const std::string prefix = args["prefix"].asString("frame_");
                          input = Json::MakeObject();
                          input["texture"] = texture;
                          input["frames"] = Json::MakeArray();
                          auto pair = [&](const char* name, int& x, int& y) {
                              const Json& v = args[name];
                              if (v.isNull()) return;
                              if (!v.isArray() || v.size() != 2) throw ApiError("invalid_argument", std::string(name) + " needs [x, y] pixels");
                              x = Pixel(v[0]);
                              y = Pixel(v[1]);
                          };
                          int ox = 0, oy = 0, sx = 0, sy = 0;
                          pair("offset", ox, oy);
                          pair("spacing", sx, sy);
                          const std::vector<SliceRect> rects =
                              args["auto"].asBool() ? FindSprites(*image, args.has("gap") ? Pixel(args["gap"]) : 2)
                                                    : GridCells(*image, Pixel(args["cellWidth"], 1), Pixel(args["cellHeight"], 1), args["trim"].asBool(), ox, oy, sx, sy);
                          if (rects.empty()) throw ApiError("invalid_atlas", "the image has no opaque pixels to slice");
                          for (size_t i = 0; i < rects.size(); ++i) {
                              const SliceRect& s = rects[i];
                              Json f = Json::MakeObject();
                              f["name"] = prefix + std::to_string(s.index >= 0 ? s.index : static_cast<int>(i));
                              f["rect"] = Json::Array{s.x, s.y, s.w, s.h};
                              if (s.canvasW > 0) {
                                  f["size"] = Json::Array{s.canvasW, s.canvasH};
                                  f["trim"] = Json::Array{s.trimX, s.trimY};
                              }
                              if (args.has("pivot")) f["pivot"] = args["pivot"];
                              input["frames"].push(f);
                          }
                      }
                      Json data;
                      std::string error;
                      if (!NormalizeSpriteAtlas(input, data, &error)) throw ApiError("invalid_atlas", error);
                      e.ResolvePath(data["texture"].asString());
                      auto texture = e.Assets().GetTexture(data["texture"].asString(), &error);
                      if (!texture) throw ApiError("invalid_texture", error);
                      for (const Json& f : data["frames"].items())
                          if (f["rect"][0].asInt() + f["rect"][2].asInt() > texture->width ||
                              f["rect"][1].asInt() + f["rect"][3].asInt() > texture->height)
                              throw ApiError("invalid_atlas", "frame outside texture");
                      if (dryRun) {
                          Json out = Json::MakeObject();
                          out["path"] = path;
                          out["values"] = data;
                          out["exists"] = FileExists(full);
                          out["dryRun"] = true;
                          return out;
                      }
                      CreateDirectories(ParentPath(full));
                      if (!WriteTextFile(full, data.dump(2) + "\n")) throw ApiError("write_failed", "cannot write atlas");
                      e.Assets().Forget(path);
                      Json out = Json::MakeObject();
                      out["path"] = path;
                      out["values"] = data;
                      return out;
                  }});
}
}  // namespace oe
