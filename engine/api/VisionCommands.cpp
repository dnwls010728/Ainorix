// Pictures for an agent's eyes: image.grid (a zoomed coordinate grid over part of an image, with the
// agent's guesses marked) and vision.calibrate (marks at known pixels; the agent's readings are scored
// before it relies on them to rig). docs/SKELETON2D.md, "Reading coordinates".
#include "api/Commands.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "app/Engine.h"
#include "assets/Assets.h"
#include "core/FileSystem.h"
#include "render/ImageDraw.h"

namespace oe {
namespace {

constexpr Rgba8 kMinorLine{0, 230, 255, 70}, kMajorLine{255, 210, 0, 170}, kLabel{255, 255, 255, 255};
constexpr Rgba8 kMark{255, 40, 200, 255};

// Deterministic stream for calibration layouts (xorshift32; seed 0 is moved off the fixed point).
struct Stream {
    uint32_t s;
    explicit Stream(uint32_t seed) : s(seed ? seed : 0x9E3779B9u) {}
    uint32_t Next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    int Range(int lo, int hi) { return lo + static_cast<int>(Next() % static_cast<uint32_t>(hi - lo + 1)); }
};

// The checkerboard behind transparent pixels, then the texel over it.
Rgba8 OverChecker(uint32_t texel, int x, int y) {
    const uint8_t back = ((x / 8 + y / 8) % 2) ? 70 : 100;
    const unsigned a = texel >> 24;
    auto mix = [&](unsigned v) { return static_cast<uint8_t>((v * a + back * (255 - a) + 127) / 255); };
    return {mix(texel & 255), mix((texel >> 8) & 255), mix((texel >> 16) & 255), 255};
}

int AutoStep(int extent) {
    for (int step : {1, 2, 5, 10, 20, 25, 50, 100, 200, 500, 1000})
        if (extent / step <= 32) return step;
    return 2000;
}

struct Mark {
    float x = 0, y = 0;
    std::string label;
};

std::vector<Mark> Marks(const Json& list) {
    std::vector<Mark> marks;
    if (list.isNull()) return marks;
    if (!list.isArray() || list.size() > 64) throw ApiError("invalid_args", "marks must be up to 64 [x, y, label?] entries");
    for (const Json& m : list.items()) {
        if (!m.isArray() || m.size() < 2 || !m[0].isNumber() || !m[1].isNumber())
            throw ApiError("invalid_args", "each mark is [x, y] or [x, y, \"label\"] in image pixels", "Example: \"marks\": [[120, 48, \"elbow\"]]");
        marks.push_back({m[0].asFloat(), m[1].asFloat(), m.size() > 2 ? m[2].asString("") : std::string()});
    }
    return marks;
}

void DrawMark(Image& image, float x, float y, const std::string& label, Rgba8 color, int textScale) {
    DrawLine(image, x - 7, y, x - 2, y, color, 1.5f);
    DrawLine(image, x + 2, y, x + 7, y, color, 1.5f);
    DrawLine(image, x, y - 7, x, y - 2, color, 1.5f);
    DrawLine(image, x, y + 2, x, y + 7, color, 1.5f);
    DrawCircle(image, x, y, 1.2f, color, true);
    if (!label.empty()) DrawText(image, static_cast<int>(x) + 9, static_cast<int>(y) - 9 - TextHeight(textScale) / 2, label, color, textScale);
}

std::string Labels(int count) {
    std::string labels;
    for (int i = 0; i < count; ++i) labels += static_cast<char>('A' + i);
    return labels;
}

}  // namespace

std::string WriteToolImage(Engine& engine, const Json& args, const std::string& fallback, const Image& image) {
    const std::string out = args["out"].asString(fallback);
    std::string extension = out.size() >= 4 ? out.substr(out.size() - 4) : std::string();
    for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (out.size() < 5 || extension != ".png") throw ApiError("invalid_path", "out must end with .png");
    const std::string full = engine.ResolvePath(out);
    CreateDirectories(ParentPath(full));
    if (!WritePng(full, image, true)) throw ApiError("write_failed", "cannot write " + out);
    return out;
}

void RegisterVisionCommands(CommandRegistry& r) {
    r.Add({"image.grid",
           "Draw a zoomed coordinate grid over (part of) an image so an agent can read pixel positions exactly; marks show where it "
           "thinks points are. Writes a PNG to read.",
           Params()
               .Req("path", "string", "Project image (png/jpg/...).")
               .OptWith("rect", Json::parse(R"({"type":"array","description":"[x, y, w, h] in image pixels (default: the whole image)."})"))
               .Opt("step", "integer", "Grid spacing in image pixels (default: about 30 lines across). Every fifth line is yellow and labeled.")
               .Opt("scale", "integer", "Zoom: output pixels per image pixel, 1..32 (default: up to about 1024 pixels wide).")
               .OptWith("marks", Json::parse(R"({"type":"array","description":"Points to draw: [[x, y, \"label\"], ...] in image pixels (check a guess)."})"))
               .Opt("out", "string", "PNG path in the project (default .oe/vision/grid.png)."),
           false, [](Engine& engine, const Json& args) {
               const std::string path = args["path"].asString();
               std::string error;
               const auto texture = engine.Assets().GetTexture(path, &error);
               if (!texture) throw ApiError("not_found", "image does not load: " + error, "Pass a project-relative image path (asset.list {kind:\"texture\"}).");
               int rx = 0, ry = 0, rw = texture->width, rh = texture->height;
               if (args.has("rect")) {
                   const Json& r = args["rect"];
                   if (!r.isArray() || r.size() != 4) throw ApiError("invalid_args", "rect must be [x, y, w, h]");
                   rx = std::max(0, r[0].asInt());
                   ry = std::max(0, r[1].asInt());
                   rw = std::min(texture->width - rx, r[2].asInt());
                   rh = std::min(texture->height - ry, r[3].asInt());
                   if (rw < 1 || rh < 1)
                       throw ApiError("invalid_args", "rect lies outside the " + std::to_string(texture->width) + "x" + std::to_string(texture->height) + " image");
               }
               const int step = std::max(1, args["step"].asInt(AutoStep(std::max(rw, rh))));
               const int scale = std::clamp(args["scale"].asInt(std::max(1, 1024 / std::max(rw, rh))), 1, 32);
               const int textScale = rw * scale >= 600 || rh * scale >= 600 ? 2 : 1;
               const int digits = static_cast<int>(std::to_string(std::max(rx + rw, ry + rh)).size());
               const int left = digits * 6 * textScale + 10, top = TextHeight(textScale) + 10;
               Image image;
               image.width = left + rw * scale + 6;
               image.height = top + rh * scale + 6;
               if (static_cast<int64_t>(image.width) * image.height > 4096 * 4096) throw ApiError("invalid_args", "grid image too large", "Use a smaller rect or scale.");
               image.rgba.assign(static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 4, 0);
               FillRect(image, 0, 0, image.width, image.height, {34, 36, 44, 255});
               for (int y = 0; y < rh * scale; ++y)
                   for (int x = 0; x < rw * scale; ++x) {
                       const uint32_t texel = texture->texels[static_cast<size_t>(ry + y / scale) * static_cast<size_t>(texture->width) + static_cast<size_t>(rx + x / scale)];
                       BlendPixel(image, left + x, top + y, OverChecker(texel, x, y));
                   }
               // Lines sit on pixel edges: the line labeled 40 is the left edge of pixel column 40.
               const bool labelAll = step * scale >= digits * 6 * textScale + 6;
               for (int axis = 0; axis < 2; ++axis) {
                   const int origin = axis == 0 ? rx : ry, extent = axis == 0 ? rw : rh;
                   for (int g = (origin + step - 1) / step * step; g <= origin + extent; g += step) {
                       const bool major = g % (step * 5) == 0;
                       const float at = static_cast<float>((axis == 0 ? left : top) + (g - origin) * scale) - 0.5f;
                       if (axis == 0) DrawLine(image, at, static_cast<float>(top), at, static_cast<float>(top + rh * scale), major ? kMajorLine : kMinorLine);
                       else DrawLine(image, static_cast<float>(left), at, static_cast<float>(left + rw * scale), at, major ? kMajorLine : kMinorLine);
                       if (!major && !labelAll) continue;
                       const std::string text = std::to_string(g);
                       if (axis == 0) DrawText(image, static_cast<int>(at) - TextWidth(text, textScale) / 2, 4, text, major ? kMajorLine : kLabel, textScale, false);
                       else DrawText(image, left - 5 - TextWidth(text, textScale), static_cast<int>(at) - TextHeight(textScale) / 2, text, major ? kMajorLine : kLabel, textScale, false);
                   }
               }
               const std::vector<Mark> marks = Marks(args["marks"]);
               for (const Mark& m : marks)
                   DrawMark(image, static_cast<float>(left) + (m.x - static_cast<float>(rx)) * static_cast<float>(scale),
                            static_cast<float>(top) + (m.y - static_cast<float>(ry)) * static_cast<float>(scale), m.label, kMark, textScale);
               Json out = Json::MakeObject();
               out["out"] = WriteToolImage(engine, args, ".oe/vision/grid.png", image);
               out["width"] = image.width;
               out["height"] = image.height;
               out["imageSize"] = Json(Json::Array{texture->width, texture->height});
               out["rect"] = Json(Json::Array{rx, ry, rw, rh});
               out["step"] = step;
               out["scale"] = scale;
               out["origin"] = Json(Json::Array{left, top});
               out["read"] = "Labels are image pixel coordinates (x right, y down) of the line on the left/top edge of that pixel. Output pixel (u, v) "
                             "is image pixel (rect.x + (u - origin.x) / scale, rect.y + (v - origin.y) / scale).";
               return out;
           }});
    r.Add({"vision.calibrate",
           "Check that you read image coordinates accurately before rigging: without answers, draws labeled dots at secret positions; "
           "with answers {A:[x,y],...}, scores them (pixel error, pass/fail).",
           Params()
               .Opt("seed", "integer", "Layout seed (default 1); the same seed draws the same dots.")
               .Opt("count", "integer", "Number of dots, 3..12 (default 6).")
               .Opt("size", "integer", "Image size in pixels, 128..1024 (default 384).")
               .Opt("answers", "object", "Your readings: {\"A\": [x, y], ...}, the centre of each dot in image pixels (x right, y down).")
               .Opt("tolerance", "number", "Largest error that passes, in pixels (default 3).")
               .Opt("out", "string", "PNG path in the project (default .oe/vision/calibrate.png)."),
           false, [](Engine& engine, const Json& args) {
               const uint32_t seed = static_cast<uint32_t>(std::max(0, args["seed"].asInt(1)));
               const int count = std::clamp(args["count"].asInt(6), 3, 12);
               const int size = std::clamp(args["size"].asInt(384), 128, 1024);
               const std::string labels = Labels(count);
               // Dots at least 40 px apart and 20 px from the edges; distractor blobs make it look like a picture.
               Stream rng(seed * 2654435761u + static_cast<uint32_t>(count) * 97u + static_cast<uint32_t>(size));
               std::vector<std::pair<int, int>> dots;
               for (int i = 0; i < count; ++i) {
                   for (int attempt = 0; attempt < 400; ++attempt) {
                       const int x = rng.Range(20, size - 21), y = rng.Range(20, size - 21);
                       bool apart = true;
                       for (const auto& d : dots) apart = apart && (d.first - x) * (d.first - x) + (d.second - y) * (d.second - y) >= 1600;
                       if (apart || attempt == 399) {
                           dots.emplace_back(x, y);
                           break;
                       }
                   }
               }
               Json out = Json::MakeObject();
               out["seed"] = static_cast<int>(seed);
               out["count"] = count;
               out["size"] = size;
               if (!args.has("answers")) {
                   Image image;
                   image.width = image.height = size;
                   image.rgba.assign(static_cast<size_t>(size) * static_cast<size_t>(size) * 4, 0);
                   for (int y = 0; y < size; ++y)
                       for (int x = 0; x < size; ++x)
                           BlendPixel(image, x, y, {static_cast<uint8_t>(200 + 40 * x / size), static_cast<uint8_t>(190 + 30 * y / size), 170, 255});
                   for (int i = 0; i < 10; ++i)
                       DrawCircle(image, static_cast<float>(rng.Range(0, size)), static_cast<float>(rng.Range(0, size)), static_cast<float>(rng.Range(10, size / 6)),
                                  {static_cast<uint8_t>(rng.Range(60, 160)), static_cast<uint8_t>(rng.Range(60, 160)), static_cast<uint8_t>(rng.Range(60, 160)), 70}, true);
                   static const Rgba8 colors[] = {{220, 30, 30, 255}, {20, 120, 230, 255}, {20, 160, 60, 255}, {150, 40, 200, 255}, {230, 120, 0, 255}, {0, 150, 150, 255}};
                   for (int i = 0; i < count; ++i) {
                       const float cx = static_cast<float>(dots[static_cast<size_t>(i)].first) + 0.5f, cy = static_cast<float>(dots[static_cast<size_t>(i)].second) + 0.5f;
                       const Rgba8 c = colors[i % 6];
                       DrawCircle(image, cx, cy, 3.0f, c, true);
                       DrawText(image, static_cast<int>(cx) + 6, static_cast<int>(cy) - 12, std::string(1, labels[static_cast<size_t>(i)]), c, 1);
                   }
                   out["out"] = WriteToolImage(engine, args, ".oe/vision/calibrate.png", image);
                   Json names = Json::MakeArray();
                   for (char c : labels) names.push(std::string(1, c));
                   out["labels"] = names;
                   out["next"] = "Read the centre of each dot (use image.grid on this image, zoomed around each dot), then call vision.calibrate "
                                 "with the same seed, count and size and answers {\"A\": [x, y], ...}.";
                   return out;
               }
               const Json& answers = args["answers"];
               const double tolerance = args["tolerance"].asNumber(3);
               double total = 0, worst = 0;
               int missing = 0;
               Json errors = Json::MakeObject();
               for (int i = 0; i < count; ++i) {
                   const std::string label(1, labels[static_cast<size_t>(i)]);
                   const Json* a = answers.find(label);
                   if (!a || !a->isArray() || a->size() != 2 || !(*a)[0].isNumber() || !(*a)[1].isNumber()) {
                       ++missing;
                       errors[label] = nullptr;
                       continue;
                   }
                   const double dx = (*a)[0].asNumber() - (dots[static_cast<size_t>(i)].first + 0.5);
                   const double dy = (*a)[1].asNumber() - (dots[static_cast<size_t>(i)].second + 0.5);
                   const double d = std::sqrt(dx * dx + dy * dy);
                   errors[label] = std::round(d * 10) / 10;
                   total += d;
                   worst = std::max(worst, d);
               }
               const bool passed = missing == 0 && worst <= tolerance;
               out["errors"] = errors;
               out["mean"] = count > missing ? std::round(total / (count - missing) * 10) / 10 : 0.0;
               out["max"] = std::round(worst * 10) / 10;
               out["missing"] = missing;
               out["passed"] = passed;
               if (!passed)
                   out["hint"] = missing ? std::string("Answer every label.")
                                         : "Your readings are off by up to " + std::to_string(static_cast<int>(std::ceil(worst))) +
                                               " px. Zoom in with image.grid {rect around the dot, step: 5}, and check a guess with marks before answering. "
                                               "If you still cannot read within tolerance, tell the user that automatic rigging may be inaccurate.";
               return out;
           }});
}

}  // namespace oe
