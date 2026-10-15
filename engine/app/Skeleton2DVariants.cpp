#include "app/Skeleton2DVariants.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "api/Commands.h"
#include "app/Engine.h"
#include "app/Skeleton2DBuild.h"
#include "app/Skeleton2DRaster.h"
#include "assets/Assets.h"
#include "core/FileSystem.h"
#include "core/Image.h"
#include "render/ImageDraw.h"
#include "scene/Skeleton2D.h"

namespace oe {
namespace {

using namespace raster2d;

constexpr Rgba8 kMagenta{255, 0, 255, 255};

uint8_t* At(Image& image, int x, int y) { return &image.rgba[(static_cast<size_t>(y) * static_cast<size_t>(image.width) + static_cast<size_t>(x)) * 4]; }
const uint8_t* At(const Image& image, int x, int y) { return &image.rgba[(static_cast<size_t>(y) * static_cast<size_t>(image.width) + static_cast<size_t>(x)) * 4]; }

Image Blank(int w, int h) {
    Image image;
    image.width = w;
    image.height = h;
    image.rgba.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    return image;
}

// A rectangle of a texture on a canvas `grow` pixels larger on every side (transparent there).
Image Canvas(const Texture& t, int rx, int ry, int rw, int rh, int grow) {
    Image image = Blank(rw + 2 * grow, rh + 2 * grow);
    for (int y = 0; y < rh; ++y)
        for (int x = 0; x < rw; ++x) {
            const int tx = rx + x, ty = ry + y;
            if (tx < 0 || ty < 0 || tx >= t.width || ty >= t.height) continue;
            const uint32_t c = t.texels[static_cast<size_t>(ty) * static_cast<size_t>(t.width) + static_cast<size_t>(tx)];
            uint8_t* p = At(image, x + grow, y + grow);
            p[0] = static_cast<uint8_t>(c & 255), p[1] = static_cast<uint8_t>((c >> 8) & 255), p[2] = static_cast<uint8_t>((c >> 16) & 255), p[3] = static_cast<uint8_t>(c >> 24);
        }
    return image;
}

Image FromTexture(const Texture& t) { return Canvas(t, 0, 0, t.width, t.height, 0); }

// Distance in pixels (3-4 chamfer, divided by 3) from every pixel to the nearest one where `seed` is set.
std::vector<float> Distance(const std::vector<uint8_t>& seed, int w, int h) {
    const float far = 1e9f;
    std::vector<float> d(seed.size());
    for (size_t i = 0; i < seed.size(); ++i) d[i] = seed[i] ? 0 : far;
    auto relax = [&](int x, int y, int nx, int ny, float cost) {
        if (nx < 0 || ny < 0 || nx >= w || ny >= h) return;
        float& here = d[static_cast<size_t>(y * w + x)];
        here = std::min(here, d[static_cast<size_t>(ny * w + nx)] + cost);
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) relax(x, y, x - 1, y, 3), relax(x, y, x, y - 1, 3), relax(x, y, x - 1, y - 1, 4), relax(x, y, x + 1, y - 1, 4);
    for (int y = h - 1; y >= 0; --y)
        for (int x = w - 1; x >= 0; --x) relax(x, y, x + 1, y, 3), relax(x, y, x, y + 1, 3), relax(x, y, x + 1, y + 1, 4), relax(x, y, x - 1, y + 1, 4);
    for (float& v : d) v /= 3;
    return d;
}

std::string Stem(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = name.rfind('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

std::string Folder(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

void WriteImage(Engine& engine, const std::string& path, const Image& image) {
    Json args = Json::MakeObject();
    args["out"] = path;
    WriteToolImage(engine, args, path, image);
    engine.Assets().Forget(path);
}

void WriteText(Engine& engine, const std::string& path, const std::string& text) {
    const std::string full = engine.ResolvePath(path);
    CreateDirectories(ParentPath(full));
    if (!WriteTextFile(full, text)) throw ApiError("write_failed", "cannot write " + path);
}

std::string Percent(double fraction) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1f%%", fraction * 100);
    return buffer;
}

std::shared_ptr<const Texture> Load(Engine& engine, const std::string& path, const char* what) {
    engine.Assets().Forget(path);
    std::string error;
    auto texture = engine.Assets().GetTexture(path, &error);
    if (!texture) throw ApiError("not_found", std::string(what) + " does not load: " + error, "Check the path in the request (project-relative).");
    return texture;
}

}  // namespace

Json Skeleton2DVariantRequests(Engine& engine, const Json& args) {
    const std::string path = args["path"].asString();
    if (AssetManager::KindOf(path) != "skeleton2d") throw ApiError("invalid_path", "path must be a *.skel2d.json");
    std::string error;
    const auto asset = engine.Assets().GetSkeleton2D(path, &error);
    if (!asset) throw ApiError("invalid_skeleton", error, "skeleton2d.check {path} shows what is wrong.");
    const Skeleton2DDef& def = *asset;
    const std::string kind = args["kind"].asString("fill");
    if (kind != "fill" && kind != "attachment") throw ApiError("invalid_args", "kind must be fill or attachment");
    const std::string skin = args["skin"].asString("");
    const int grow = std::clamp(args["grow"].asInt(0), 0, 512);
    const bool redraw = kind == "fill" && args["redraw"].asBool();
    const std::string outDir = args["outDir"].asString(".oe/variants/" + Stem(Stem(path)));

    Skeleton2DPose setupPose = Skeleton2DSetupPose(def);
    Skeleton2DWorld(def, setupPose);
    const Shown setup = Show(def, skin, setupPose, engine.Assets());
    const View view = FitView({&setup}, 3000, std::max(0.01f, def.pixelsPerUnit));
    const Frame frame = Draw(view, setup);
    // The whole figure, for the generator to see what the part belongs to.
    Image context = Blank(view.width, view.height);
    Paint(context, 0, 0, view, frame);
    const std::string contextPath = outDir + "/context.png";
    WriteImage(engine, contextPath, context);

    std::vector<std::string> only;
    if (args.has("slots"))
        for (const Json& s : args["slots"].items()) only.push_back(s.asString());
    if (kind == "attachment") {
        if (!args.has("slot") || !args.has("name") || !args.has("description"))
            throw ApiError("invalid_args", "kind attachment needs slot, name and description", "Example: {kind: \"attachment\", slot: \"hand_l\", name: \"fist\", description: \"a closed fist\"}");
        only = {args["slot"].asString()};
        if (def.SlotIndex(only[0]) < 0) throw ApiError("not_found", "no slot '" + only[0] + "'");
    }

    Json requests = Json::MakeArray();
    for (size_t i = 0; i < setup.pieces.size(); ++i) {
        const Skeleton2DPiece& piece = setup.pieces[i];
        const Skeleton2DAttachment& a = *piece.attachment;
        const Skeleton2DSlot& slot = def.slots[static_cast<size_t>(piece.slot)];
        if (!only.empty() && std::find(only.begin(), only.end(), slot.name) == only.end()) continue;
        if (!piece.texture) continue;
        const Texture& texture = *piece.texture;
        const int rx = a.hasRect ? static_cast<int>(a.rect[0]) : 0, ry = a.hasRect ? static_cast<int>(a.rect[1]) : 0;
        const int rw = a.hasRect ? static_cast<int>(a.rect[2]) : texture.width, rh = a.hasRect ? static_cast<int>(a.rect[3]) : texture.height;
        const Image source = Canvas(texture, rx, ry, rw, rh, grow);
        const int w = source.width, h = source.height;
        std::vector<uint8_t> edit(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
        std::map<int, int> coverers;
        if (kind == "attachment") {
            std::fill(edit.begin(), edit.end(), 1);
        } else {
            // Canvas pixel -> world, the same placement Skeleton2DPieceGeometry uses.
            const float ppu = std::max(0.01f, def.pixelsPerUnit);
            const float sx = (a.width > 0 ? a.width : static_cast<float>(rw) / ppu) / static_cast<float>(rw);
            const float sy = (a.height > 0 ? a.height : static_cast<float>(rh) / ppu) / static_cast<float>(rh);
            const Mat4 place = setupPose.world[static_cast<size_t>(slot.bone)] * Skeleton2DLocalMatrix(a.x, a.y, a.rotation, a.scaleX, a.scaleY);
            std::vector<uint8_t> opaque(edit.size(), 0);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) opaque[static_cast<size_t>(y * w + x)] = At(source, x, y)[3] > 127;
            const std::vector<float> distance = Distance(opaque, w, h);
            const float reach = args.has("reach") ? args["reach"].asFloat() : 0.15f * static_cast<float>(std::max(rw, rh));
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    if (distance[static_cast<size_t>(y * w + x)] > reach) continue;
                    const float px = static_cast<float>(x - grow) + 0.5f, py = static_cast<float>(y - grow) + 0.5f;
                    const Vec3 local((px - a.pivot[0] * static_cast<float>(rw)) * sx, ((static_cast<float>(rh) - py) - a.pivot[1] * static_cast<float>(rh)) * sy, 0);
                    const Vec3 world = place.TransformPoint(local);
                    const int fx = static_cast<int>((world.x - view.left) * view.scale), fy = static_cast<int>((view.top - world.y) * view.scale);
                    if (fx < 0 || fy < 0 || fx >= view.width || fy >= view.height) continue;
                    const int top = frame.piece[static_cast<size_t>(fy * view.width + fx)];
                    if (top > static_cast<int>(i)) {
                        edit[static_cast<size_t>(y * w + x)] = 1;
                        ++coverers[setup.pieces[static_cast<size_t>(top)].slot];
                    }
                }
            // A little wider, so the repaint blends into what stays.
            const std::vector<float> near = Distance(edit, w, h);
            for (size_t k = 0; k < edit.size(); ++k) edit[k] = near[k] <= 2.0f;
            // redraw: the whole part is drawn again on its own (a part mostly hidden, where a partial repaint cannot line up).
            if (redraw) std::fill(edit.begin(), edit.end(), 1);
        }
        int editCount = 0, opaqueCount = 0;
        for (size_t k = 0; k < edit.size(); ++k) editCount += edit[k], opaqueCount += source.rgba[k * 4 + 3] > 127;
        if (kind == "fill" && editCount < std::max(16, opaqueCount / 500)) continue;

        std::string coverer;
        int most = 0;
        for (const auto& c : coverers) if (c.second > most) most = c.second, coverer = def.slots[static_cast<size_t>(c.first)].name;
        // Every part that hid a good share of it, for the prompt: the "arm_r", "arm_l" and "head" parts.
        // Redrawing a whole part: parts hanging below it (a foot under a leg) are the same limb, not things in front to leave
        // out (a partial fill names every part that hid the area).
        auto below = [&](int slotIndex) {
            for (int b = def.slots[static_cast<size_t>(slotIndex)].bone; b >= 0; b = def.bones[static_cast<size_t>(b)].parent)
                if (b == slot.bone) return true;
            return false;
        };
        std::vector<std::string> fronts;
        for (const auto& c : coverers)
            if (c.second * 10 >= most && !(redraw && below(c.first))) fronts.push_back("\"" + def.slots[static_cast<size_t>(c.first)].name + "\"");
        std::string behind = fronts.empty() ? std::string("another part") : "the " + fronts[0];
        for (size_t k = 1; k < fronts.size(); ++k) behind += (k + 1 == fronts.size() ? " and " : ", ") + fronts[k];
        if (!fronts.empty()) behind += fronts.size() > 1 ? " parts" : " part";
        Image mask = Blank(w, h), marked = source;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                uint8_t* m = At(mask, x, y);
                m[0] = m[1] = m[2] = 255;
                m[3] = edit[static_cast<size_t>(y * w + x)] ? 0 : 255;
                if (edit[static_cast<size_t>(y * w + x)] && kind == "fill" && !redraw) {
                    uint8_t* p = At(marked, x, y);
                    p[0] = kMagenta.r, p[1] = kMagenta.g, p[2] = kMagenta.b, p[3] = 255;
                }
            }
        // Generators draw at a few shapes (square, 2:3, 3:2): the canvas is padded to the nearest one, cropped again on import.
        const float shapes[] = {1.0f, 2.0f / 3.0f, 3.0f / 2.0f};
        float shape = shapes[0];
        for (float s : shapes)
            if (std::fabs(std::log(static_cast<float>(w) / static_cast<float>(h) / s)) < std::fabs(std::log(static_cast<float>(w) / static_cast<float>(h) / shape))) shape = s;
        const int pw = static_cast<float>(w) / static_cast<float>(h) > shape ? w : static_cast<int>(std::lround(static_cast<float>(h) * shape));
        const int ph = static_cast<float>(w) / static_cast<float>(h) > shape ? static_cast<int>(std::lround(static_cast<float>(w) / shape)) : h;
        const int padX = (pw - w) / 2, padY = (ph - h) / 2;
        auto pad = [&](const Image& image, bool opaqueWhite) {
            Image padded = Blank(pw, ph);
            if (opaqueWhite) std::fill(padded.rgba.begin(), padded.rgba.end(), static_cast<uint8_t>(255));
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) std::copy(At(image, x, y), At(image, x, y) + 4, At(padded, x + padX, y + padY));
            return padded;
        };
        const std::string name = kind == "fill" ? (redraw ? "redraw" : "fill") : args["name"].asString();
        const std::string folder = outDir + "/" + slot.name + "-" + name;
        WriteImage(engine, folder + "/source.png", pad(source, false));
        WriteImage(engine, folder + "/mask.png", pad(mask, true));
        WriteImage(engine, folder + "/edit.png", pad(marked, false));
        const std::string size = std::to_string(pw) + " x " + std::to_string(ph);
        std::string prompt;
        if (redraw) {
            prompt = "Image 1 is one cut-out part, \"" + slot.name + "\", of a 2D game character drawn for skeletal animation; image 2 is the whole "
                     "character. Much of it was hidden behind " + behind + " in the original drawing, so image 1 shows pieces of those parts "
                     "and cut edges. Redraw image 1 as \"" + slot.name + "\" alone, complete, as it would look if nothing were in front of it, "
                     "in exactly the same position, pose and scale on the canvas (its visible pixels mark where it is): the same art style, "
                     "colours, shading and outline. None of " + behind + " may appear. Output one image of exactly " + size +
                     " pixels, transparent background, no text, no frame.";
        } else         if (kind == "fill") {
            prompt = "Image 1 is one cut-out part, \"" + slot.name + "\", of a 2D game character drawn for skeletal animation; image 2 is the whole "
                     "character. The magenta area of image 1 was hidden behind " + behind + " in the original drawing. Repaint only the magenta "
                     "area so that \"" + slot.name + "\" continues there naturally, as if " + behind + " were not in front of it: the same art "
                     "style, colours, shading and outline as the rest of \"" + slot.name + "\". Where \"" + slot.name + "\" would not reach, leave "
                     "the pixels fully transparent. Keep every pixel outside the magenta area exactly as it is. Output one image of exactly " + size +
                     " pixels, transparent background, no magenta left, no text, no frame.";
        } else {
            prompt = "Image 1 is one cut-out part, \"" + slot.name + "\", of a 2D game character drawn for skeletal animation; image 2 is the whole "
                     "character. Draw a new version of this part: " + args["description"].asString() + ". Keep the same size and placement on "
                     "the canvas, so it can replace the original at the same joint, and the same art style, colours, shading and outline. "
                     "Output one image of exactly " + size + " pixels, transparent background, no text, no frame.";
        }
        WriteText(engine, folder + "/prompt.txt", prompt + "\n");
        Json request = Json::MakeObject();
        request["skeleton"] = path;
        request["kind"] = kind;
        request["slot"] = slot.name;
        request["attachment"] = a.name;
        if (kind == "attachment") request["name"] = name;
        if (!skin.empty()) request["skin"] = skin;
        request["image"] = a.image;
        request["rect"] = Json(Json::Array{rx, ry, rw, rh});
        request["grow"] = grow;
        if (redraw) request["redraw"] = true;
        request["crop"] = Json(Json::Array{padX, padY, w, h});  // the part's canvas inside the padded pictures
        request["source"] = folder + "/source.png";
        request["mask"] = folder + "/mask.png";
        request["edit"] = folder + "/edit.png";
        request["context"] = contextPath;
        request["prompt"] = prompt;
        request["result"] = folder + "/result.png";
        if (!coverer.empty()) request["hiddenUnder"] = coverer;
        request["editPixels"] = editCount;
        WriteText(engine, folder + "/request.json", request.dump(2) + "\n");
        Json summary = Json::MakeObject();
        summary["slot"] = slot.name;
        summary["request"] = folder + "/request.json";
        summary["images"] = Json(Json::Array{kind == "fill" ? folder + "/edit.png" : folder + "/source.png", contextPath});
        summary["result"] = folder + "/result.png";
        summary["editPixels"] = editCount;
        if (!coverer.empty()) summary["hiddenUnder"] = coverer;
        requests.push(summary);
    }
    Json out = Json::MakeObject();
    out["requests"] = requests;
    out["consent"] = "Sending these pictures to an image generator shares them with that service. Ask the user before you do.";
    out["next"] = requests.size() == 0 ? std::string("Nothing to repaint: no part has picture hidden under another one.")
                                       : "For each request: give its images (in that order) and prompt.txt to an image generator, save its answer as "
                                         "the request's result, then skeleton2d.variant_import {request}.";
    return out;
}

Json Skeleton2DVariantImport(Engine& engine, const Json& args) {
    const std::string requestPath = args["request"].asString();
    const Json request = engine.ReadProjectJson(requestPath);
    const std::string kind = request["kind"].asString("fill");
    const std::string resultPath = args["result"].asString(request["result"].asString());
    const Image source = FromTexture(*Load(engine, request["source"].asString(), "the request's source"));
    const Image mask = FromTexture(*Load(engine, request["mask"].asString(), "the request's mask"));
    Image result = FromTexture(*Load(engine, resultPath, "the result"));
    const int w = source.width, h = source.height;

    // Same aspect; another size is scaled to the source's (generators work at their own sizes).
    const double aspectSource = static_cast<double>(w) / h, aspectResult = static_cast<double>(result.width) / result.height;
    if (std::fabs(aspectResult / aspectSource - 1) > 0.03)
        throw ApiError("variant_rejected", "the result is " + std::to_string(result.width) + "x" + std::to_string(result.height) + ", not the shape of the " +
                                               std::to_string(w) + "x" + std::to_string(h) + " source",
                       "Ask again for exactly " + std::to_string(w) + " x " + std::to_string(h) + " pixels.");
    if (result.width != w || result.height != h) {
        Image scaled = Blank(w, h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const float fx = (static_cast<float>(x) + 0.5f) * static_cast<float>(result.width) / static_cast<float>(w) - 0.5f;
                const float fy = (static_cast<float>(y) + 0.5f) * static_cast<float>(result.height) / static_cast<float>(h) - 0.5f;
                const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, result.width - 1), y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, result.height - 1);
                const int x1 = std::min(x0 + 1, result.width - 1), y1 = std::min(y0 + 1, result.height - 1);
                const float tx = Clamp(fx - static_cast<float>(x0), 0, 1), ty = Clamp(fy - static_cast<float>(y0), 0, 1);
                for (int c = 0; c < 4; ++c) {
                    const float top = At(result, x0, y0)[c] * (1 - tx) + At(result, x1, y0)[c] * tx;
                    const float bottom = At(result, x0, y1)[c] * (1 - tx) + At(result, x1, y1)[c] * tx;
                    At(scaled, x, y)[c] = static_cast<uint8_t>(std::lround(top * (1 - ty) + bottom * ty));
                }
            }
        result = scaled;
    }
    // Transparency: the result's own, or keyed out from the colour of its corners when it came back opaque.
    int seeThrough = 0;
    for (size_t k = 3; k < result.rgba.size(); k += 4) seeThrough += result.rgba[k] < 250;
    const bool keyed = seeThrough < w * h / 100;
    float key[3] = {0, 0, 0};
    if (keyed) {
        int n = 0;
        for (int cy : {0, h - 6})
            for (int cx : {0, w - 6})
                for (int y = std::max(0, cy); y < std::min(h, cy + 6); ++y)
                    for (int x = std::max(0, cx); x < std::min(w, cx + 6); ++x, ++n)
                        for (int c = 0; c < 3; ++c) key[c] += At(result, x, y)[c];
        for (float& k : key) k /= static_cast<float>(std::max(1, n));
    }
    auto alphaOf = [&](const uint8_t* p) {
        if (!keyed) return static_cast<float>(p[3]);
        const float d = std::sqrt((p[0] - key[0]) * (p[0] - key[0]) + (p[1] - key[1]) * (p[1] - key[1]) + (p[2] - key[2]) * (p[2] - key[2]));
        return Clamp((d - 24) / 40, 0, 1) * 255;
    };

    // Where the picture had to stay: the shift and tint that match it best, and how far it still is.
    const bool fill = kind == "fill";
    const int maxShift = fill ? std::clamp(args["maxShift"].asInt(8), 0, 32) : 0;
    auto keep = [&](int x, int y) { return At(mask, x, y)[3] > 127 && At(source, x, y)[3] > 127; };
    // Generators redraw a picture a little moved and often a little larger or smaller: a source pixel is looked up at
    // the result's centre + (pixel - centre) * scale + shift. Scales first (coarse sampling), then shifts at the best scale.
    const float centerX = static_cast<float>(w) * 0.5f, centerY = static_cast<float>(h) * 0.5f;
    float bestScale = 1;
    int bestX = 0, bestY = 0;
    auto mapped = [&](int x, int y, float scale, int dx, int dy, int& rx, int& ry) {
        rx = static_cast<int>(std::lround(centerX + (static_cast<float>(x) - centerX) * scale)) + dx;
        ry = static_cast<int>(std::lround(centerY + (static_cast<float>(y) - centerY) * scale)) + dy;
        return rx >= 0 && ry >= 0 && rx < w && ry < h;
    };
    auto score = [&](float scale, int dx, int dy, int stride) {
        double sum = 0;
        int n = 0;
        for (int y = 0; y < h; y += stride)
            for (int x = 0; x < w; x += stride) {
                int rx, ry;
                if (!keep(x, y) || !mapped(x, y, scale, dx, dy, rx, ry)) continue;
                const uint8_t *s = At(source, x, y), *r = At(result, rx, ry);
                sum += std::abs(s[0] - r[0]) + std::abs(s[1] - r[1]) + std::abs(s[2] - r[2]);
                ++n;
            }
        // A tiny cost per pixel of shift and per step of scale: in flat areas every fit is as good, and then none is right.
        return n > 0 ? sum / n + 0.05 * (std::abs(dx) + std::abs(dy)) + 20.0 * std::fabs(scale - 1) : 1e30;
    };
    double best = 1e30;
    if (fill)
        for (int step = -4; step <= 4; ++step) {
            const float scale = 1 + 0.02f * static_cast<float>(step);
            for (int dy = -maxShift; dy <= maxShift; dy += 2)
                for (int dx = -maxShift; dx <= maxShift; dx += 2) {
                    const double s = score(scale, dx, dy, 4);
                    if (s < best) best = s, bestScale = scale, bestX = dx, bestY = dy;
                }
        }
    // Every shift near the best one, at the best scale, more finely.
    const int coarseX = bestX, coarseY = bestY;
    best = 1e30;
    for (int dy = coarseY - 2; dy <= coarseY + 2; ++dy)
        for (int dx = coarseX - 2; dx <= coarseX + 2; ++dx) {
            if (std::abs(dx) > maxShift || std::abs(dy) > maxShift) continue;
            const double s = score(bestScale, dx, dy, 2);
            if (s < best) best = s, bestX = dx, bestY = dy;
        }
    auto shifted = [&](int x, int y) {
        int rx, ry;
        mapped(x, y, bestScale, bestX, bestY, rx, ry);
        return At(result, std::clamp(rx, 0, w - 1), std::clamp(ry, 0, h - 1));
    };
    double sums[2][3] = {{0, 0, 0}, {0, 0, 0}};
    int kept = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (keep(x, y)) {
                ++kept;
                for (int c = 0; c < 3; ++c) sums[0][c] += At(source, x, y)[c], sums[1][c] += shifted(x, y)[c];
            }
    float gain[3] = {1, 1, 1};
    // A channel too dark to measure (a red part has no blue) keeps its gain of 1.
    for (int c = 0; c < 3; ++c)
        gain[c] = sums[0][c] > 16.0 * kept && sums[1][c] > 16.0 * kept ? Clamp(static_cast<float>(sums[0][c] / sums[1][c]), 0.75f, 1.33f) : 1;
    double residual = 0;
    if (fill && kept > 0) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                if (keep(x, y))
                    for (int c = 0; c < 3; ++c) residual += std::fabs(At(source, x, y)[c] - std::min(255.0f, shifted(x, y)[c] * gain[c]));
        residual /= static_cast<double>(kept) * 3 * 255;
        const double tolerance = args["tolerance"].asNumber(0.08);
        if (residual > tolerance)
            throw ApiError("variant_rejected",
                           "the result changed the picture outside the area it was asked to repaint (difference " + Percent(residual) +
                               " after the best shift and tint; limit " + Percent(tolerance) + ")",
                           "Ask again, stressing: change only the magenta area, keep every other pixel and the canvas size. Or pass a larger tolerance if the "
                           "change is acceptable.");
    }

    // The new picture: the source with the masked area taken from the result.
    Image out = source;
    int edited = 0, magenta = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (At(mask, x, y)[3] > 127) continue;
            const uint8_t* r = shifted(x, y);
            uint8_t* o = At(out, x, y);
            for (int c = 0; c < 3; ++c) o[c] = static_cast<uint8_t>(std::min(255.0f, static_cast<float>(r[c]) * gain[c]));
            o[3] = static_cast<uint8_t>(std::lround(alphaOf(r)));
            ++edited;
            if (o[3] > 127 && r[0] > 230 && r[1] < 40 && r[2] > 230) ++magenta;
        }
    if (fill && magenta > edited / 20)
        throw ApiError("variant_rejected", "the result still shows magenta in " + std::to_string(magenta) + " of the " + std::to_string(edited) + " repainted pixels",
                       "Ask again: the magenta area must be painted over completely.");

    // Back to the part's canvas (requests pad it to a shape generators draw).
    if (request.has("crop")) {
        const int cx = request["crop"][0].asInt(), cy = request["crop"][1].asInt(), cw = request["crop"][2].asInt(), ch = request["crop"][3].asInt();
        if (cx < 0 || cy < 0 || cw < 1 || ch < 1 || cx + cw > w || cy + ch > h) throw ApiError("invalid_args", "the request's crop lies outside its pictures");
        Image cropped = Blank(cw, ch);
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x) std::copy(At(out, x + cx, y + cy), At(out, x + cx, y + cy) + 4, At(cropped, x, y));
        out = cropped;
    }
    // Point the skeleton at it.
    const std::string skeletonPath = request["skeleton"].asString();
    Json data = engine.ReadProjectJson(skeletonPath);
    const std::string slotName = request["slot"].asString(), attachmentName = request["attachment"].asString();
    // The request's skin (default) holds it, else whichever skin does.
    Json* attachment = nullptr;
    std::string skinName;
    const std::string wanted = request["skin"].asString("default");
    if (data["skins"].isObject())
        for (auto& skin : data["skins"].members()) {
            if (!skin.second.isObject() || (attachment && skinName == wanted)) continue;
            for (auto& slot : skin.second.members()) {
                if (slot.first != slotName || !slot.second.isObject()) continue;
                for (auto& entry : slot.second.members())
                    if (entry.first == attachmentName && (!attachment || skin.first == wanted)) attachment = &entry.second, skinName = skin.first;
            }
        }
    if (!attachment) throw ApiError("not_found", "the skeleton no longer has attachment '" + attachmentName + "' in slot '" + slotName + "'");
    Json updated = *attachment;
    const std::string folder = Folder(request["image"].asString());
    // A picture filled before is replaced under its own name (no -filled-filled).
    std::string stem = Stem(request["image"].asString());
    if (stem.size() > 7 && stem.compare(stem.size() - 7, 7, "-filled") == 0) stem.resize(stem.size() - 7);
    const std::string imagePath = fill ? folder + stem + "-filled.png" : folder + slotName + "-" + request["name"].asString() + ".png";
    WriteImage(engine, imagePath, out);
    updated["image"] = imagePath;
    if (fill) updated["filled"] = true;
    updated.erase("rect");  // the new picture is the rectangle alone
    const int grow = request["grow"].asInt(0);
    const float canvasW = static_cast<float>(out.width), canvasH = static_cast<float>(out.height);
    const float rw = request["rect"][2].asFloat(canvasW), rh = request["rect"][3].asFloat(canvasH);
    if (grow > 0) {
        // The pivot keeps its pixel, so the picture stays where it was; a given world size grows with the canvas.
        const float px = updated["pivot"][0].asFloat(0.5f) * rw, py = (1 - updated["pivot"][1].asFloat(0.5f)) * rh;
        updated["pivot"] = Json(Json::Array{(px + static_cast<float>(grow)) / canvasW, 1 - (py + static_cast<float>(grow)) / canvasH});
        if (updated.has("width")) updated["width"] = updated["width"].asFloat() * canvasW / rw;
        if (updated.has("height")) updated["height"] = updated["height"].asFloat() * canvasH / rh;
    }
    int vertexCount = 0;
    if (updated.has("vertices")) {
        // A new grid over the new picture (the old cell size); each vertex takes the weights of the old vertices nearest to it.
        const Json oldVertices = updated["vertices"], oldWeights = updated["weights"];
        // The old cell: the most common step between neighbouring vertex columns (edge columns are narrower).
        std::vector<int> columns;
        for (size_t v = 0; v < oldVertices.size(); v += 2) columns.push_back(oldVertices[static_cast<int>(v)].asInt());
        std::sort(columns.begin(), columns.end());
        columns.erase(std::unique(columns.begin(), columns.end()), columns.end());
        std::map<int, int> steps;
        for (size_t c = 1; c < columns.size(); ++c) ++steps[columns[c] - columns[c - 1]];
        int cell = 0, seen = 0;
        for (const auto& s : steps)
            if (s.second > seen || (s.second == seen && s.first > cell)) cell = s.first, seen = s.second;
        std::vector<int> vertices;
        std::vector<uint32_t> triangles;
        Skeleton2DGridMesh(out, cell, 8, static_cast<int>(Skeleton2DDef::kMaxMeshVertices), vertices, triangles);
        Json vertexJson = Json::MakeArray(), triangleJson = Json::MakeArray(), weightJson = Json::MakeArray();
        for (int v : vertices) vertexJson.push(v);
        for (uint32_t t : triangles) triangleJson.push(static_cast<int>(t));
        for (size_t v = 0; v + 1 < vertices.size() && oldWeights.isArray(); v += 2) {
            std::vector<std::pair<float, size_t>> nearest;
            for (size_t u = 0; u + 1 < oldVertices.size(); u += 2) {
                const float dx = static_cast<float>(vertices[v]) - (oldVertices[static_cast<int>(u)].asFloat() + static_cast<float>(grow));
                const float dy = static_cast<float>(vertices[v + 1]) - (oldVertices[static_cast<int>(u + 1)].asFloat() + static_cast<float>(grow));
                nearest.emplace_back(dx * dx + dy * dy, u / 2);
            }
            std::sort(nearest.begin(), nearest.end());
            nearest.resize(std::min<size_t>(3, nearest.size()));
            std::map<std::string, float> blend;
            float total = 0;
            for (const auto& n : nearest) {
                const float share = 1 / (std::sqrt(n.first) + 1);
                for (const Json& pair : oldWeights[static_cast<int>(n.second)].items()) blend[pair[0].asString()] += pair[1].asFloat() * share;
                total += share;
            }
            std::vector<std::pair<float, std::string>> ranked;
            for (const auto& b : blend) ranked.emplace_back(b.second / total, b.first);
            std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first || (a.first == b.first && a.second < b.second); });
            ranked.resize(std::min<size_t>(4, ranked.size()));
            float sum = 0;
            for (const auto& r : ranked) sum += r.first;
            Json list = Json::MakeArray();
            for (const auto& r : ranked) {
                Json pair = Json::MakeArray();
                pair.push(r.second);
                pair.push(std::round(r.first / sum * 1000) / 1000);
                list.push(pair);
            }
            weightJson.push(list);
        }
        updated["vertices"] = vertexJson;
        updated["triangles"] = triangleJson;
        if (oldWeights.isArray()) updated["weights"] = weightJson;
        vertexCount = static_cast<int>(vertices.size() / 2);
    }
    const std::string newName = fill ? attachmentName : request["name"].asString();
    data["skins"][skinName][slotName][newName] = updated;
    std::string parseError;
    if (!ParseSkeleton2D(data, &parseError)) throw ApiError("invalid_skeleton", parseError);
    WriteText(engine, skeletonPath, data.dump(2) + "\n");
    engine.Assets().Forget(skeletonPath);

    Json response = Json::MakeObject();
    response["image"] = imagePath;
    response["skeleton"] = skeletonPath;
    response["slot"] = slotName;
    response["attachment"] = newName;
    response["shift"] = Json(Json::Array{bestX, bestY});
    response["scale"] = std::round(bestScale * 100) / 100;
    response["tint"] = Json(Json::Array{std::round(gain[0] * 100) / 100, std::round(gain[1] * 100) / 100, std::round(gain[2] * 100) / 100});
    if (fill) response["difference"] = std::round(residual * 1000) / 1000;
    response["keyedBackground"] = keyed;
    response["editedPixels"] = edited;
    if (vertexCount) response["vertices"] = vertexCount;
    response["next"] = fill ? "Check it moving: skeleton2d.analyze {path} should no longer report this slot as revealed."
                            : "Show it: key the slot's attachment in a clip (slots: {" + slotName + ": {attachment: [[t, \"" + newName + "\"]]}}).";
    return response;
}

}  // namespace oe
