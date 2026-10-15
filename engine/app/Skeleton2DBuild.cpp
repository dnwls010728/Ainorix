#include "app/Skeleton2DBuild.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>

#include "api/Commands.h"
#include "app/Engine.h"
#include "assets/Assets.h"
#include "core/FileSystem.h"
#include "core/Image.h"
#include "core/Math.h"
#include "render/ImageDraw.h"
#include "scene/Skeleton2D.h"

namespace oe {
namespace {

constexpr const char* kDraftHint = "Fix the draft field the message names; the draft format is in docs/SKELETON2D.md (\"Building from a picture\").";

struct P2 {
    float x = 0, y = 0;
};
P2 operator-(P2 a, P2 b) { return {a.x - b.x, a.y - b.y}; }
P2 operator+(P2 a, P2 b) { return {a.x + b.x, a.y + b.y}; }
P2 operator*(P2 a, float s) { return {a.x * s, a.y * s}; }

[[noreturn]] void Fail(const std::string& where, const std::string& what) { throw ApiError("invalid_draft", where + ": " + what, kDraftHint); }

void OnlyFields(const Json& object, const std::string& where, std::initializer_list<const char*> allowed) {
    if (!object.isObject()) Fail(where, "must be an object");
    for (const auto& member : object.members()) {
        bool known = false;
        for (const char* name : allowed) known = known || member.first == name;
        if (known) continue;
        std::string list;
        for (const char* name : allowed) list += (list.empty() ? "" : ", ") + std::string(name);
        Fail(where, "unknown field '" + member.first + "' (allowed: " + list + ")");
    }
}

float Number(const Json& v, const std::string& where, float lo = -1e6f, float hi = 1e6f) {
    if (!v.isNumber() || !std::isfinite(v.asNumber()) || v.asNumber() < lo || v.asNumber() > hi) Fail(where, "must be a number in range");
    return v.asFloat();
}

P2 Point(const Json& v, const std::string& where) {
    if (!v.isArray() || v.size() != 2) Fail(where, "must be [x, y] in image pixels");
    return {Number(v[0], where), Number(v[1], where)};
}

float Round4(float v) { return std::round(v * 10000.0f) / 10000.0f; }

float Angle(P2 from, P2 to) { return std::atan2(-(to.y - from.y), to.x - from.x); }  // world radians (+Y up)

float Wrap180(float degrees) {
    while (degrees > 180) degrees -= 360;
    while (degrees <= -180) degrees += 360;
    return degrees;
}

float SegmentDistance(P2 p, P2 a, P2 b) {
    const P2 ab = b - a, ap = p - a;
    const float len2 = ab.x * ab.x + ab.y * ab.y;
    const float t = len2 > 0 ? Clamp((ap.x * ab.x + ap.y * ab.y) / len2, 0, 1) : 0;
    const P2 d = p - (a + ab * t);
    return std::sqrt(d.x * d.x + d.y * d.y);
}

bool InPolygon(const std::vector<P2>& poly, float x, float y) {
    bool inside = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const P2 a = poly[i], b = poly[j];
        if ((a.y > y) != (b.y > y) && x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x) inside = !inside;
    }
    return inside;
}

// A band `half` pixels to each side of a chain of points (a limb), as a polygon.
std::vector<P2> Ribbon(const std::vector<P2>& chain, float half) {
    std::vector<P2> left, right;
    for (size_t i = 0; i < chain.size(); ++i) {
        const P2 a = chain[i > 0 ? i - 1 : 0], b = chain[std::min(i + 1, chain.size() - 1)];
        P2 d = b - a;
        const float len = std::max(1e-3f, std::sqrt(d.x * d.x + d.y * d.y));
        d = d * (1 / len);
        const P2 n{-d.y, d.x};
        // The ends reach a little past the joints, so neighbouring parts overlap there.
        P2 p = chain[i];
        if (i == 0) p = p - d * (half * 0.6f);
        if (i + 1 == chain.size()) p = p + d * (half * 0.6f);
        left.push_back(p + n * half);
        right.push_back(p - n * half);
    }
    std::vector<P2> poly = left;
    for (size_t i = right.size(); i-- > 0;) poly.push_back(right[i]);
    return poly;
}

struct Bone {
    std::string name;
    int parent = -1;
    P2 from, to;
    bool hasTo = false;
    float angle = 0;  // world radians
    float length = 0;  // pixels
};

struct Part {
    std::string name;
    enum Kind { Polygon, Ellipse, Rect, Picture } kind = Polygon;
    std::vector<P2> polygon;
    float shape[4] = {0, 0, 0, 0};  // ellipse cx, cy, rx, ry / rect x, y, w, h
    std::string image;              // Picture: its own image, placed with its top-left at `at`
    P2 at;
    std::vector<int> bones;
    bool mesh = false;
    int cell = 0;
    // Results.
    int x0 = 0, y0 = 0, w = 0, h = 0;  // the cut, in illustration pixels
    Image cut;
    // A repaint kept from before (skeleton2d.variant_import): `cut` is then the repainted picture (its canvas maybe
    // grown), `plain` the cut written beside it.
    bool filled = false;
    Image plain;
    std::string plainPath;
    int pixels = 0;
    std::string path;
    Json vertices = Json::MakeArray(), triangles = Json::MakeArray(), weights = Json::MakeArray();

    bool Contains(float x, float y) const {
        switch (kind) {
            case Polygon: return InPolygon(polygon, x, y);
            case Ellipse: {
                const float dx = (x - shape[0]) / std::max(1e-3f, shape[2]), dy = (y - shape[1]) / std::max(1e-3f, shape[3]);
                return dx * dx + dy * dy <= 1;
            }
            case Rect: return x >= shape[0] && y >= shape[1] && x < shape[0] + shape[2] && y < shape[1] + shape[3];
            case Picture: break;
        }
        return false;
    }
    void Bounds(float& lx, float& ly, float& hx, float& hy) const {
        lx = ly = 1e30f;
        hx = hy = -1e30f;
        if (kind == Polygon)
            for (P2 p : polygon) lx = std::min(lx, p.x), ly = std::min(ly, p.y), hx = std::max(hx, p.x), hy = std::max(hy, p.y);
        else if (kind == Ellipse)
            lx = shape[0] - shape[2], ly = shape[1] - shape[3], hx = shape[0] + shape[2], hy = shape[1] + shape[3];
        else
            lx = shape[0], ly = shape[1], hx = shape[0] + shape[2], hy = shape[1] + shape[3];
    }
};

uint8_t AlphaAt(const Texture& t, int x, int y) { return static_cast<uint8_t>(t.texels[static_cast<size_t>(y) * static_cast<size_t>(t.width) + static_cast<size_t>(x)] >> 24); }

void CopyTexel(const Texture& t, int x, int y, Image& image, int ix, int iy) {
    const uint32_t c = t.texels[static_cast<size_t>(y) * static_cast<size_t>(t.width) + static_cast<size_t>(x)];
    uint8_t* p = &image.rgba[(static_cast<size_t>(iy) * static_cast<size_t>(image.width) + static_cast<size_t>(ix)) * 4];
    p[0] = static_cast<uint8_t>(c & 255), p[1] = static_cast<uint8_t>((c >> 8) & 255), p[2] = static_cast<uint8_t>((c >> 16) & 255), p[3] = static_cast<uint8_t>(c >> 24);
}

// A grid over the cut: every cell holding an opaque pixel becomes two triangles. Simple, even and deterministic;
// cells hanging over the outline only draw transparent texels.
void GridMesh(Part& part, int alphaThreshold, int maxVertices) {
    std::vector<int> vertices;
    std::vector<uint32_t> triangles;
    part.cell = Skeleton2DGridMesh(part.cut, part.cell, alphaThreshold, maxVertices, vertices, triangles);
    for (int v : vertices) part.vertices.push(v);
    for (uint32_t t : triangles) part.triangles.push(static_cast<int>(t));
}

Json Warning(const std::string& where, const std::string& message, const std::string& hint) {
    Json w = Json::MakeObject();
    w["where"] = where;
    w["message"] = message;
    w["hint"] = hint;
    return w;
}

const Rgba8 kPartColors[] = {{255, 99, 71, 255}, {65, 105, 225, 255}, {50, 205, 50, 255}, {255, 165, 0, 255}, {186, 85, 211, 255},
                             {0, 206, 209, 255}, {255, 215, 0, 255}, {255, 105, 180, 255}, {139, 195, 74, 255}, {121, 134, 203, 255}};

}  // namespace

int Skeleton2DGridMesh(const Image& image, int cell, int alphaThreshold, int maxVertices, std::vector<int>& vertices, std::vector<uint32_t>& triangles) {
    const int w = image.width, h = image.height;
    if (cell <= 0) cell = std::max(6, static_cast<int>(std::lround(static_cast<double>(std::max(w, h)) / 10.0)));
    for (;; cell += std::max(1, cell / 4)) {
        const int nx = (w + cell - 1) / cell, ny = (h + cell - 1) / cell;
        std::map<int, int> ids;
        std::vector<std::array<int, 2>> corners;
        std::vector<uint32_t> tris;
        auto vertex = [&](int i, int j) {
            const int key = j * (nx + 1) + i;
            auto found = ids.find(key);
            if (found != ids.end()) return static_cast<uint32_t>(found->second);
            const int id = static_cast<int>(corners.size());
            ids[key] = id;
            corners.push_back({std::min(i * cell, w), std::min(j * cell, h)});
            return static_cast<uint32_t>(id);
        };
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) {
                bool used = false;
                for (int y = j * cell; y < std::min(h, (j + 1) * cell) && !used; ++y)
                    for (int x = i * cell; x < std::min(w, (i + 1) * cell) && !used; ++x)
                        used = image.rgba[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4 + 3] > alphaThreshold;
                if (!used) continue;
                const uint32_t a = vertex(i, j), b = vertex(i + 1, j), c = vertex(i + 1, j + 1), d = vertex(i, j + 1);
                tris.insert(tris.end(), {a, d, c, a, c, b});
            }
        if (static_cast<int>(corners.size()) > maxVertices) continue;
        vertices.clear();
        for (const auto& corner : corners) vertices.insert(vertices.end(), {corner[0], corner[1]});
        triangles = tris;
        return cell;
    }
}

Json BuildSkeleton2D(Engine& engine, const Json& draft, const std::string& out, const std::string& partsDirArg, bool dryRun,
                     const std::string& overlayOut) {
    OnlyFields(draft, "draft", {"image", "pixelsPerUnit", "height", "origin", "alphaThreshold", "pixelArt", "joints", "bones", "parts"});
    if (AssetManager::KindOf(out) != "skeleton2d") throw ApiError("invalid_path", "out must end with .skel2d.json");
    const int alphaThreshold = draft.has("alphaThreshold") ? static_cast<int>(Number(draft["alphaThreshold"], "alphaThreshold", 0, 254)) : 8;

    // The illustration (optional when every part brings its own image).
    std::shared_ptr<const Texture> art;
    const std::string imagePath = draft["image"].asString("");
    if (!imagePath.empty()) {
        engine.Assets().Forget(imagePath);  // the agent may have just regenerated it
        std::string error;
        art = engine.Assets().GetTexture(imagePath, &error);
        if (!art) throw ApiError("not_found", "image does not load: " + error, "draft.image is a project-relative image path.");
    }

    // Joints.
    std::map<std::string, P2> joints;
    if (draft.has("joints")) {
        if (!draft["joints"].isObject()) Fail("joints", "must map joint names to [x, y]");
        for (const auto& j : draft["joints"].members()) {
            if (j.second.isNull()) Fail("joints." + j.first, "is not placed yet (null); give its [x, y] in image pixels");
            joints[j.first] = Point(j.second, "joints." + j.first);
        }
    }
    auto pointOf = [&](const Json& v, const std::string& where) {
        if (v.isString()) {
            auto found = joints.find(v.asString());
            if (found == joints.end()) Fail(where, "unknown joint '" + v.asString() + "'");
            return found->second;
        }
        return Point(v, where);
    };
    std::set<std::string> usedJoints;
    auto noteJoint = [&](const Json& v) { if (v.isString()) usedJoints.insert(v.asString()); };

    // Opaque bounds of the illustration: the default origin and size.
    int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    long long opaque = 0;
    if (art) {
        bx0 = art->width, by0 = art->height, bx1 = -1, by1 = -1;
        for (int y = 0; y < art->height; ++y)
            for (int x = 0; x < art->width; ++x)
                if (AlphaAt(*art, x, y) > alphaThreshold) {
                    ++opaque;
                    bx0 = std::min(bx0, x), by0 = std::min(by0, y), bx1 = std::max(bx1, x), by1 = std::max(by1, y);
                }
        if (opaque == 0) throw ApiError("invalid_draft", "the image has no opaque pixels", "Use an illustration on a transparent background.");
    }
    const float artHeight = art ? static_cast<float>(by1 - by0 + 1) : 0;
    float ppu = 0;
    if (draft.has("pixelsPerUnit")) ppu = Number(draft["pixelsPerUnit"], "pixelsPerUnit", 0.01f, 100000);
    else if (art) ppu = artHeight / (draft.has("height") ? Number(draft["height"], "height", 0.01f, 10000) : 2.0f);
    else Fail("pixelsPerUnit", "is needed when the draft has no image");
    P2 origin;
    if (draft.has("origin")) origin = pointOf(draft["origin"], "origin"), noteJoint(draft["origin"]);
    else if (joints.count("root")) origin = joints["root"], usedJoints.insert("root");
    else if (art) origin = {static_cast<float>(bx0 + bx1 + 1) * 0.5f, static_cast<float>(by1 + 1)};
    auto world = [&](P2 p) { return P2{(p.x - origin.x) / ppu, (origin.y - p.y) / ppu}; };

    // Bones: a root at the origin, then the draft's bones (parents first).
    std::vector<Bone> bones(1);
    bones[0].name = "root";
    bones[0].from = bones[0].to = origin;
    auto boneIndex = [&](const std::string& name) {
        for (size_t i = 0; i < bones.size(); ++i) if (bones[i].name == name) return static_cast<int>(i);
        return -1;
    };
    const Json& boneList = draft["bones"];
    if (!boneList.isArray() || boneList.size() == 0) Fail("bones", "must list bones {name, from, to, parent}");
    if (boneList.size() + 1 > Skeleton2DDef::kMaxBones) Fail("bones", "has more than 255 bones");
    for (size_t i = 0; i < boneList.size(); ++i) {
        const std::string at = "bones[" + std::to_string(i) + "]";
        const Json& b = boneList[static_cast<int>(i)];
        OnlyFields(b, at, {"name", "parent", "from", "to"});
        Bone bone;
        bone.name = b["name"].asString("");
        if (bone.name.empty() || boneIndex(bone.name) >= 0) Fail(at + ".name", bone.name.empty() ? "is missing" : "'" + bone.name + "' is listed twice (root is made for you)");
        bone.parent = boneIndex(b["parent"].asString("root"));
        if (bone.parent < 0) Fail(at + ".parent", "'" + b["parent"].asString() + "' is not a bone listed before this one");
        if (!b.has("from")) Fail(at + ".from", "is missing: the joint (or [x, y]) the bone starts at");
        bone.from = pointOf(b["from"], at + ".from");
        noteJoint(b["from"]);
        if (b.has("to")) {
            bone.to = pointOf(b["to"], at + ".to");
            noteJoint(b["to"]);
            const P2 d = bone.to - bone.from;
            bone.length = std::sqrt(d.x * d.x + d.y * d.y);
            bone.hasTo = bone.length > 0.5f;
            bone.angle = bone.hasTo ? Angle(bone.from, bone.to) : 0;
        }
        bones.push_back(bone);
    }

    // Parts.
    const Json& partList = draft["parts"];
    if (!partList.isArray() || partList.size() == 0) Fail("parts", "must list parts back to front");
    if (partList.size() > Skeleton2DDef::kMaxSlots) Fail("parts", "has more than 256 parts");
    std::vector<Part> parts;
    for (size_t i = 0; i < partList.size(); ++i) {
        const std::string at = "parts[" + std::to_string(i) + "]";
        const Json& p = partList[static_cast<int>(i)];
        OnlyFields(p, at, {"name", "polygon", "ellipse", "rect", "image", "at", "bone", "bones", "mesh", "cell"});
        Part part;
        part.name = p["name"].asString("");
        if (part.name.empty()) Fail(at + ".name", "is missing");
        for (const Part& other : parts) if (other.name == part.name) Fail(at + ".name", "'" + part.name + "' is listed twice");
        const int shapes = (p.has("polygon") ? 1 : 0) + (p.has("ellipse") ? 1 : 0) + (p.has("rect") ? 1 : 0) + (p.has("image") ? 1 : 0);
        if (shapes != 1) Fail(at, "needs exactly one of polygon, ellipse, rect (regions of draft.image) or image (its own picture)");
        if (p.has("polygon")) {
            const Json& poly = p["polygon"];
            if (!poly.isArray() || poly.size() < 3) Fail(at + ".polygon", "must be at least three [x, y] points");
            for (size_t k = 0; k < poly.size(); ++k) part.polygon.push_back(pointOf(poly[static_cast<int>(k)], at + ".polygon[" + std::to_string(k) + "]"));
        } else if (p.has("ellipse") || p.has("rect")) {
            const bool ellipse = p.has("ellipse");
            const Json& s = ellipse ? p["ellipse"] : p["rect"];
            if (!s.isArray() || s.size() != 4) Fail(at, ellipse ? "ellipse must be [cx, cy, rx, ry]" : "rect must be [x, y, w, h]");
            for (int k = 0; k < 4; ++k) part.shape[k] = Number(s[k], at);
            part.kind = ellipse ? Part::Ellipse : Part::Rect;
        } else {
            part.kind = Part::Picture;
            part.image = p["image"].asString();
            part.at = p.has("at") ? pointOf(p["at"], at + ".at") : P2{};
        }
        if (part.kind != Part::Picture && !art) Fail(at, "cuts a region but the draft has no image");
        std::vector<std::string> names;
        if (p.has("bone")) names.push_back(p["bone"].asString(""));
        if (p.has("bones")) {
            if (!p["bones"].isArray() || p["bones"].size() == 0 || p["bones"].size() > 8) Fail(at + ".bones", "must list 1..8 bone names");
            for (const Json& n : p["bones"].items()) names.push_back(n.asString(""));
        }
        if (names.empty()) Fail(at, "needs bone (it moves rigidly with it) or bones (a mesh bending between them)");
        for (const std::string& n : names) {
            const int b = boneIndex(n);
            if (b < 0) Fail(at + ".bones", "unknown bone '" + n + "'");
            if (std::find(part.bones.begin(), part.bones.end(), b) == part.bones.end()) part.bones.push_back(b);
        }
        part.mesh = p.has("mesh") ? p["mesh"].asBool() : part.bones.size() > 1;
        if (part.bones.size() > 1 && !part.mesh) Fail(at + ".mesh", "a part following several bones must be a mesh");
        part.cell = p.has("cell") ? static_cast<int>(Number(p["cell"], at + ".cell", 2, 512)) : 0;
        parts.push_back(std::move(part));
    }

    // Cut the parts. Coverage counts how many parts take each opaque pixel of the illustration.
    std::vector<uint8_t> takers(art ? static_cast<size_t>(art->width) * static_cast<size_t>(art->height) : 0, 0);
    Json warnings = Json::MakeArray();
    for (Part& part : parts) {
        if (part.kind == Part::Picture) {
            std::string error;
            engine.Assets().Forget(part.image);
            const auto picture = engine.Assets().GetTexture(part.image, &error);
            if (!picture) throw ApiError("not_found", "part '" + part.name + "' image does not load: " + error);
            part.x0 = static_cast<int>(std::lround(part.at.x)), part.y0 = static_cast<int>(std::lround(part.at.y));
            part.w = picture->width, part.h = picture->height;
            part.cut.width = part.w, part.cut.height = part.h;
            part.cut.rgba.assign(static_cast<size_t>(part.w) * static_cast<size_t>(part.h) * 4, 0);
            for (int y = 0; y < part.h; ++y)
                for (int x = 0; x < part.w; ++x) {
                    CopyTexel(*picture, x, y, part.cut, x, y);
                    if (AlphaAt(*picture, x, y) > alphaThreshold) ++part.pixels;
                }
            part.path = part.image;
            continue;
        }
        float lx, ly, hx, hy;
        part.Bounds(lx, ly, hx, hy);
        const int sx0 = std::max(0, static_cast<int>(std::floor(lx))), sy0 = std::max(0, static_cast<int>(std::floor(ly)));
        const int sx1 = std::min(art->width - 1, static_cast<int>(std::ceil(hx))), sy1 = std::min(art->height - 1, static_cast<int>(std::ceil(hy)));
        int cx0 = art->width, cy0 = art->height, cx1 = -1, cy1 = -1;
        std::vector<std::pair<int, int>> taken;
        for (int y = sy0; y <= sy1; ++y)
            for (int x = sx0; x <= sx1; ++x)
                if (AlphaAt(*art, x, y) > 0 && part.Contains(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f)) {
                    taken.emplace_back(x, y);
                    cx0 = std::min(cx0, x), cy0 = std::min(cy0, y), cx1 = std::max(cx1, x), cy1 = std::max(cy1, y);
                }
        if (taken.empty()) {
            warnings.push(Warning("parts." + part.name, "takes no opaque pixels of the image", "Move its region over the part of the picture it should cut."));
            continue;
        }
        const int pad = 2;
        part.x0 = cx0 - pad, part.y0 = cy0 - pad, part.w = cx1 - cx0 + 1 + 2 * pad, part.h = cy1 - cy0 + 1 + 2 * pad;
        part.cut.width = part.w, part.cut.height = part.h;
        part.cut.rgba.assign(static_cast<size_t>(part.w) * static_cast<size_t>(part.h) * 4, 0);
        for (const auto& xy : taken) {
            CopyTexel(*art, xy.first, xy.second, part.cut, xy.first - part.x0, xy.second - part.y0);
            if (AlphaAt(*art, xy.first, xy.second) > alphaThreshold) {
                ++part.pixels;
                uint8_t& n = takers[static_cast<size_t>(xy.second) * static_cast<size_t>(art->width) + static_cast<size_t>(xy.first)];
                n = static_cast<uint8_t>(std::min(255, n + 1));
            }
        }
    }

    // Repaints imported for a part (skeleton2d.variant_import) stay when the part is cut exactly as before: the part
    // then keeps its repainted picture (on a canvas grown evenly on every side) and is meshed over it.
    int keptFills = 0;
    {
        const std::string fullOut = engine.ResolvePath(out);
        std::string text, parseError;
        const Json old = ReadTextFile(fullOut, text) ? Json::parse(text, &parseError) : Json();
        const std::string dir = !partsDirArg.empty() ? partsDirArg : out.substr(0, out.size() - std::string(".skel2d.json").size()) + "-parts";
        for (Part& part : parts) {
            if (part.w == 0 || part.kind == Part::Picture || !parseError.empty()) continue;
            const Json* skins = old.find("skins");
            const Json* skin = skins ? skins->find("default") : nullptr;
            const Json* slot = skin ? skin->find(part.name) : nullptr;
            const Json* before = slot ? slot->find(part.name) : nullptr;
            if (!before || !(*before)["filled"].asBool()) continue;
            const std::string plainPath = dir + "/" + part.name + ".png", filledPath = (*before)["image"].asString();
            engine.Assets().Forget(plainPath);
            engine.Assets().Forget(filledPath);
            const auto plain = engine.Assets().GetTexture(plainPath), filledPicture = engine.Assets().GetTexture(filledPath);
            if (!plain || !filledPicture || plain->width != part.w || plain->height != part.h) continue;
            bool same = true;
            for (size_t i = 0; same && i < plain->texels.size(); ++i) {
                const uint8_t* p = &part.cut.rgba[i * 4];
                same = plain->texels[i] == (static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24);
            }
            const int grow = (filledPicture->width - part.w) / 2;
            if (!same || grow < 0 || filledPicture->width - part.w != 2 * grow || filledPicture->height - part.h != 2 * grow) continue;
            part.plain = part.cut;
            part.plainPath = plainPath;
            part.cut.width = filledPicture->width;
            part.cut.height = filledPicture->height;
            part.cut.rgba.resize(filledPicture->texels.size() * 4);
            for (size_t i = 0; i < filledPicture->texels.size(); ++i)
                for (int c = 0; c < 4; ++c) part.cut.rgba[i * 4 + static_cast<size_t>(c)] = static_cast<uint8_t>((filledPicture->texels[i] >> (8 * c)) & 255);
            part.x0 -= grow, part.y0 -= grow, part.w = filledPicture->width, part.h = filledPicture->height;
            part.path = filledPath;
            part.filled = true;
            ++keptFills;
        }
    }

    // Meshes and weights: each vertex leans on the part's bones by distance to them (closer = more).
    for (Part& part : parts) {
        if (part.w == 0 || !part.mesh) continue;
        GridMesh(part, alphaThreshold, static_cast<int>(Skeleton2DDef::kMaxMeshVertices));
        if (part.bones.size() < 2) continue;
        const float soft = std::max(2.0f, static_cast<float>(part.cell) * 0.5f);
        for (size_t v = 0; v * 2 + 1 < part.vertices.size(); ++v) {
            const P2 q{static_cast<float>(part.x0) + part.vertices[static_cast<int>(v * 2)].asFloat(),
                       static_cast<float>(part.y0) + part.vertices[static_cast<int>(v * 2 + 1)].asFloat()};
            std::vector<std::pair<float, int>> influence;
            float total = 0;
            for (int b : part.bones) {
                const Bone& bone = bones[static_cast<size_t>(b)];
                const float d = bone.hasTo ? SegmentDistance(q, bone.from, bone.to) : std::sqrt((q - bone.from).x * (q - bone.from).x + (q - bone.from).y * (q - bone.from).y);
                const float w = 1.0f / std::pow(d + soft, 4.0f);
                influence.emplace_back(w, b);
                total += w;
            }
            for (auto& i : influence) i.first /= total;
            std::stable_sort(influence.begin(), influence.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            if (influence.size() > 4) influence.resize(4);
            float kept = 0;
            for (const auto& i : influence) if (i.first >= 0.02f) kept += i.first;
            Json list = Json::MakeArray();
            for (const auto& i : influence) {
                if (i.first < 0.02f) continue;
                Json pair = Json::MakeArray();
                pair.push(bones[static_cast<size_t>(i.second)].name);
                pair.push(std::round(i.first / kept * 1000.0f) / 1000.0f);
                list.push(pair);
            }
            part.weights.push(list);
        }
    }

    // The skeleton: bones relative to their parents, attachments placed so the setup pose is the picture.
    Json skeleton = Json::MakeObject();
    skeleton["pixelsPerUnit"] = Round4(ppu);
    if (draft.has("pixelArt")) skeleton["pixelArt"] = draft["pixelArt"].asBool();
    Json boneJson = Json::MakeArray();
    for (size_t i = 0; i < bones.size(); ++i) {
        const Bone& b = bones[i];
        Json j = Json::MakeObject();
        j["name"] = b.name;
        if (i > 0) {
            const Bone& p = bones[static_cast<size_t>(b.parent)];
            j["parent"] = p.name;
            const P2 rel = world(b.from) - world(p.from);
            const float c = std::cos(-p.angle), s = std::sin(-p.angle);
            const float x = Round4(c * rel.x - s * rel.y), y = Round4(s * rel.x + c * rel.y);
            if (x != 0) j["x"] = x;
            if (y != 0) j["y"] = y;
            const float rotation = Round4(Wrap180(Degrees(b.angle - p.angle)));
            if (rotation != 0) j["rotation"] = rotation;
            if (b.length > 0) j["length"] = Round4(b.length / ppu);
        }
        boneJson.push(j);
    }
    skeleton["bones"] = boneJson;
    Json slots = Json::MakeArray(), skin = Json::MakeObject();
    std::string partsDir = partsDirArg;
    if (partsDir.empty()) partsDir = out.substr(0, out.size() - std::string(".skel2d.json").size()) + "-parts";
    for (Part& part : parts) {
        if (part.w == 0) continue;
        const Bone& bone = bones[static_cast<size_t>(part.bones[0])];
        Json slot = Json::MakeObject();
        slot["name"] = part.name;
        slot["bone"] = bone.name;
        slot["attachment"] = part.name;
        slots.push(slot);
        if (part.kind != Part::Picture && !part.filled) part.path = partsDir + "/" + part.name + ".png";
        // The cut's top-left corner, in the bone's space; the image stays upright in the setup pose.
        const P2 rel = world({static_cast<float>(part.x0), static_cast<float>(part.y0)}) - world(bone.from);
        const float c = std::cos(-bone.angle), s = std::sin(-bone.angle);
        Json a = Json::MakeObject();
        a["image"] = part.path;
        if (part.filled) a["filled"] = true;
        a["pivot"] = Json(Json::Array{0, 1});
        a["x"] = Round4(c * rel.x - s * rel.y);
        a["y"] = Round4(s * rel.x + c * rel.y);
        const float rotation = Round4(Wrap180(Degrees(-bone.angle)));
        if (rotation != 0) a["rotation"] = rotation;
        if (part.mesh) {
            a["vertices"] = part.vertices;
            a["triangles"] = part.triangles;
            if (part.bones.size() > 1) a["weights"] = part.weights;
        }
        Json entry = Json::MakeObject();
        entry[part.name] = a;
        skin[part.name] = entry;
    }
    skeleton["slots"] = slots;
    Json skins = Json::MakeObject();
    skins["default"] = skin;
    skeleton["skins"] = skins;
    // Clips, springs and IK of the skeleton being rebuilt stay (repaints were kept above).
    const std::string full = engine.ResolvePath(out);
    std::string previous;
    if (ReadTextFile(full, previous)) {
        std::string parseError;
        const Json old = Json::parse(previous, &parseError);
        if (parseError.empty())
            for (const char* kept : {"springs", "ik", "clips"})  // what was added to the asset after the build
                if (old[kept].size() > 0) skeleton[kept] = old[kept];
    }
    std::string error;
    if (!ParseSkeleton2D(skeleton, &error)) {
        if (skeleton.has("clips") || skeleton.has("springs") || skeleton.has("ik")) {
            throw ApiError("invalid_skeleton", "the rebuilt skeleton does not fit the clips, springs or ik of " + out + ": " + error,
                           "A bone or slot they name is gone. Keep its name in the draft, or remove what names it (skeleton2d.set {path, clip, remove: true} for a clip).");
        }
        throw ApiError("invalid_skeleton", error, kDraftHint);
    }

    // Report.
    for (const auto& j : joints)
        if (!usedJoints.count(j.first))
            warnings.push(Warning("joints." + j.first, "no bone uses this joint", "Use it as a bone's from/to, or remove it."));
    for (size_t i = 1; i < bones.size(); ++i) {
        bool used = false;
        for (const Part& p : parts) used = used || std::find(p.bones.begin(), p.bones.end(), static_cast<int>(i)) != p.bones.end();
        for (const Bone& child : bones) used = used || child.parent == static_cast<int>(i);
        if (!used) warnings.push(Warning("bones." + bones[i].name, "no part follows this bone and it has no children", "Give it to a part (bone/bones) or remove it."));
        if (!bones[i].hasTo) warnings.push(Warning("bones." + bones[i].name, "has no direction (to missing or equal to from)", "Give it a to joint: the bone points from -> to, and clips turn it around from."));
    }
    long long covered = 0, shared = 0;
    for (uint8_t n : takers) covered += n > 0, shared += n > 1;
    Json coverage = Json::MakeObject();
    if (art) {
        const long long uncovered = opaque - covered;
        coverage["opaque"] = static_cast<double>(opaque);
        coverage["uncovered"] = static_cast<double>(uncovered);
        coverage["uncoveredPercent"] = std::round(1000.0 * static_cast<double>(uncovered) / static_cast<double>(opaque)) / 10.0;
        coverage["shared"] = static_cast<double>(shared);
        if (uncovered * 100 > opaque)
            warnings.push(Warning("parts", std::to_string(uncovered) + " opaque pixels (" + coverage["uncoveredPercent"].dump() + "%) belong to no part (magenta in the draft picture)",
                                  "Grow the regions next to the magenta areas, or add a part for them."));
    }

    // The draft over the picture: regions, bones, joints, and what no part took.
    const std::string overlayPath = overlayOut.empty() ? std::string(".oe/vision/draft.png") : overlayOut;
    if (art) {
        const float zoom = std::clamp(1024.0f / static_cast<float>(std::max(art->width, art->height)), 0.25f, 4.0f);
        Image picture;
        picture.width = std::max(1, static_cast<int>(static_cast<float>(art->width) * zoom));
        picture.height = std::max(1, static_cast<int>(static_cast<float>(art->height) * zoom));
        picture.rgba.assign(static_cast<size_t>(picture.width) * static_cast<size_t>(picture.height) * 4, 255);
        for (int y = 0; y < picture.height; ++y)
            for (int x = 0; x < picture.width; ++x) {
                const int ax = std::min(art->width - 1, static_cast<int>(static_cast<float>(x) / zoom));
                const int ay = std::min(art->height - 1, static_cast<int>(static_cast<float>(y) / zoom));
                const uint8_t back = ((x / 8 + y / 8) % 2) ? 60 : 80;
                FillRect(picture, x, y, 1, 1, {back, back, back, 255});
                const uint32_t t = art->texels[static_cast<size_t>(ay) * static_cast<size_t>(art->width) + static_cast<size_t>(ax)];
                BlendPixel(picture, x, y, {static_cast<uint8_t>(t & 255), static_cast<uint8_t>((t >> 8) & 255), static_cast<uint8_t>((t >> 16) & 255), static_cast<uint8_t>(t >> 24)});
                if ((t >> 24) > static_cast<uint32_t>(alphaThreshold) && takers[static_cast<size_t>(ay) * static_cast<size_t>(art->width) + static_cast<size_t>(ax)] == 0)
                    BlendPixel(picture, x, y, {255, 0, 255, 170});
            }
        auto px = [&](P2 p) { return P2{p.x * zoom, p.y * zoom}; };
        for (size_t i = 0; i < parts.size(); ++i) {
            const Part& part = parts[i];
            const Rgba8 color = kPartColors[i % 10];
            P2 label;
            if (part.kind == Part::Polygon) {
                for (size_t k = 0; k < part.polygon.size(); ++k) {
                    const P2 a = px(part.polygon[k]), b = px(part.polygon[(k + 1) % part.polygon.size()]);
                    DrawLine(picture, a.x, a.y, b.x, b.y, color, 2);
                    label = label + part.polygon[k] * (1.0f / static_cast<float>(part.polygon.size()));
                }
            } else if (part.kind == Part::Ellipse) {
                for (int k = 0; k < 48; ++k) {
                    const float t0 = static_cast<float>(k) / 48 * 2 * kPi, t1 = static_cast<float>(k + 1) / 48 * 2 * kPi;
                    const P2 a = px({part.shape[0] + std::cos(t0) * part.shape[2], part.shape[1] + std::sin(t0) * part.shape[3]});
                    const P2 b = px({part.shape[0] + std::cos(t1) * part.shape[2], part.shape[1] + std::sin(t1) * part.shape[3]});
                    DrawLine(picture, a.x, a.y, b.x, b.y, color, 2);
                }
                label = {part.shape[0], part.shape[1]};
            } else {
                const float x = part.kind == Part::Rect ? part.shape[0] : static_cast<float>(part.x0);
                const float y = part.kind == Part::Rect ? part.shape[1] : static_cast<float>(part.y0);
                const float w = part.kind == Part::Rect ? part.shape[2] : static_cast<float>(part.w);
                const float h = part.kind == Part::Rect ? part.shape[3] : static_cast<float>(part.h);
                const P2 corners[] = {px({x, y}), px({x + w, y}), px({x + w, y + h}), px({x, y + h})};
                for (int k = 0; k < 4; ++k) DrawLine(picture, corners[k].x, corners[k].y, corners[(k + 1) % 4].x, corners[(k + 1) % 4].y, color, 2);
                label = {x + w * 0.5f, y + h * 0.5f};
            }
            const P2 l = px(label);
            DrawText(picture, static_cast<int>(l.x) - TextWidth(part.name) / 2, static_cast<int>(l.y) - 3, part.name, color);
        }
        for (size_t i = 1; i < bones.size(); ++i) {
            const P2 a = px(bones[i].from), b = px(bones[i].hasTo ? bones[i].to : bones[i].from);
            DrawLine(picture, a.x, a.y, b.x, b.y, {255, 255, 255, 230}, 2);
            DrawCircle(picture, b.x, b.y, 2.5f, {255, 255, 255, 230}, true);
        }
        for (const auto& j : joints) {
            const P2 p = px(j.second);
            DrawCircle(picture, p.x, p.y, 4, {0, 0, 0, 255}, true);
            DrawCircle(picture, p.x, p.y, 3, {255, 215, 0, 255}, true);
            DrawText(picture, static_cast<int>(p.x) + 6, static_cast<int>(p.y) - 3, j.first, {255, 215, 0, 255});
        }
        const P2 o = px(origin);
        DrawLine(picture, o.x - 8, o.y, o.x + 8, o.y, {255, 255, 255, 255}, 2);
        DrawLine(picture, o.x, o.y - 8, o.x, o.y + 8, {255, 255, 255, 255}, 2);
        Json args = Json::MakeObject();
        args["out"] = overlayPath;
        WriteToolImage(engine, args, overlayPath, picture);
    }

    if (!dryRun) {
        for (const Part& part : parts) {
            if (part.w == 0 || part.kind == Part::Picture) continue;
            const std::string& file = part.filled ? part.plainPath : part.path;
            const std::string partFull = engine.ResolvePath(file);
            CreateDirectories(ParentPath(partFull));
            if (!WritePng(partFull, part.filled ? part.plain : part.cut, true)) throw ApiError("write_failed", "cannot write " + file);
            engine.Assets().Forget(file);
        }
        CreateDirectories(ParentPath(full));
        if (!WriteTextFile(full, skeleton.dump(2) + "\n")) throw ApiError("write_failed", "cannot write " + out);
        engine.Assets().Forget(out);
    }

    Json result = Json::MakeObject();
    result["out"] = out;
    result["written"] = !dryRun;
    result["pixelsPerUnit"] = Round4(ppu);
    result["origin"] = Json(Json::Array{origin.x, origin.y});
    Json partReport = Json::MakeArray();
    for (const Part& part : parts) {
        Json p = Json::MakeObject();
        p["name"] = part.name;
        p["image"] = part.path;
        p["rect"] = Json(Json::Array{part.x0, part.y0, part.w, part.h});
        p["pixels"] = part.pixels;
        if (part.filled) p["filled"] = true;
        if (part.mesh) {
            p["vertices"] = static_cast<int>(part.vertices.size() / 2);
            p["triangles"] = static_cast<int>(part.triangles.size() / 3);
            p["cell"] = part.cell;
        }
        Json names = Json::MakeArray();
        for (int b : part.bones) names.push(bones[static_cast<size_t>(b)].name);
        p["bones"] = names;
        partReport.push(p);
    }
    result["parts"] = partReport;
    result["bones"] = static_cast<int>(bones.size());
    if (art) {
        result["coverage"] = coverage;
        result["draftImage"] = overlayPath;
    }
    if (skeleton.has("clips")) result["keptClips"] = static_cast<int>(skeleton["clips"].size());
    if (keptFills) result["keptFills"] = keptFills;
    result["warnings"] = warnings;
    result["next"] = dryRun ? "Look at draftImage; fix the draft, then build without dryRun."
                            : "Check the rig with skeleton2d.overlay {path: out} and the draft picture, then add clips (skeleton2d.set {path, clip, values}).";
    return result;
}

// ----- Templates ------------------------------------------------------------

namespace {

struct TemplateJoint {
    const char* name;
    float x, y;  // across from the centre line and down from the top, in character heights
    const char* note;
};

Json PointJson(P2 p) { return Json(Json::Array{std::round(p.x), std::round(p.y)}); }

}  // namespace

Json Skeleton2DDraftTemplate(Engine& engine, const std::string& kind, const std::string& image, const Json& given) {
    // A standing figure H tall: joints as (offset from the centre line, depth from the top) in H.
    static const TemplateJoint front[] = {
        {"head_top", 0, 0, "top of the head (hair included)"}, {"neck", 0, 0.19f, "where the neck meets the chin line"},
        {"chest", 0, 0.30f, "middle of the chest"}, {"hip", 0, 0.52f, "middle of the pelvis (the body turns around it)"},
        {"shoulder_l", 0.12f, 0.22f, "the character's left shoulder joint (on the image's right in a front view)"},
        {"elbow_l", 0.15f, 0.37f, "left elbow"}, {"wrist_l", 0.16f, 0.50f, "left wrist"}, {"hand_tip_l", 0.165f, 0.57f, "tip of the left hand"},
        {"shoulder_r", -0.12f, 0.22f, "the character's right shoulder joint (image left)"}, {"elbow_r", -0.15f, 0.37f, "right elbow"},
        {"wrist_r", -0.16f, 0.50f, "right wrist"}, {"hand_tip_r", -0.165f, 0.57f, "tip of the right hand"},
        {"hip_l", 0.06f, 0.53f, "left hip joint (top of the thigh)"}, {"knee_l", 0.065f, 0.74f, "left knee"},
        {"ankle_l", 0.07f, 0.94f, "left ankle"}, {"toe_l", 0.10f, 1.0f, "tip of the left foot"},
        {"hip_r", -0.06f, 0.53f, "right hip joint"}, {"knee_r", -0.065f, 0.74f, "right knee"},
        {"ankle_r", -0.07f, 0.94f, "right ankle"}, {"toe_r", -0.10f, 1.0f, "tip of the right foot"}};
    // Facing right (+x); the back limbs are a little behind the front ones.
    static const TemplateJoint side[] = {
        {"head_top", 0, 0, "top of the head (hair included)"}, {"neck", -0.01f, 0.19f, "where the neck meets the chin line"},
        {"chest", 0, 0.30f, "middle of the chest"}, {"hip", -0.01f, 0.52f, "middle of the pelvis"},
        {"shoulder_front", 0, 0.22f, "shoulder of the arm nearer the viewer"}, {"elbow_front", 0.01f, 0.37f, "front elbow"},
        {"wrist_front", 0.02f, 0.50f, "front wrist"}, {"hand_tip_front", 0.03f, 0.57f, "tip of the front hand"},
        {"shoulder_back", -0.02f, 0.22f, "shoulder of the far arm"}, {"elbow_back", -0.02f, 0.37f, "back elbow"},
        {"wrist_back", -0.01f, 0.50f, "back wrist"}, {"hand_tip_back", 0, 0.57f, "tip of the back hand"},
        {"hip_front", 0.01f, 0.53f, "hip joint of the nearer leg"}, {"knee_front", 0.02f, 0.74f, "front knee"},
        {"ankle_front", 0.01f, 0.94f, "front ankle"}, {"toe_front", 0.08f, 1.0f, "tip of the front foot (toes forward)"},
        {"hip_back", -0.02f, 0.53f, "hip joint of the far leg"}, {"knee_back", -0.02f, 0.74f, "back knee"},
        {"ankle_back", -0.03f, 0.94f, "back ankle"}, {"toe_back", 0.04f, 1.0f, "tip of the back foot"}};
    const bool isFront = kind == "humanoid-front";
    if (!isFront && kind != "humanoid-side") throw ApiError("invalid_args", "unknown template '" + kind + "'", "Use humanoid-front or humanoid-side.");
    const std::string l = isFront ? "_l" : "_front", r = isFront ? "_r" : "_back";

    // The figure's box: the opaque pixels of the illustration, or a nominal 400 x 1000 box.
    float x0 = 0, y0 = 0, w = 400, h = 1000;
    bool guessed = false;
    std::shared_ptr<const Texture> art;
    if (!image.empty()) {
        std::string error;
        engine.Assets().Forget(image);
        art = engine.Assets().GetTexture(image, &error);
        if (!art) throw ApiError("not_found", "image does not load: " + error);
        int bx0 = art->width, by0 = art->height, bx1 = -1, by1 = -1;
        for (int y = 0; y < art->height; ++y)
            for (int x = 0; x < art->width; ++x)
                if ((art->texels[static_cast<size_t>(y) * static_cast<size_t>(art->width) + static_cast<size_t>(x)] >> 24) > 8)
                    bx0 = std::min(bx0, x), by0 = std::min(by0, y), bx1 = std::max(bx1, x), by1 = std::max(by1, y);
        if (bx1 < 0) throw ApiError("invalid_args", "the image has no opaque pixels", "Use an illustration on a transparent background.");
        x0 = static_cast<float>(bx0), y0 = static_cast<float>(by0), w = static_cast<float>(bx1 - bx0 + 1), h = static_cast<float>(by1 - by0 + 1);
        guessed = true;
    }
    const float cx = x0 + w * 0.5f;
    std::map<std::string, P2> at;
    Json joints = Json::MakeObject(), notes = Json::MakeObject();
    for (const TemplateJoint& j : isFront ? front : side) {
        // Side views face right: offsets grow toward +x the same way.
        P2 p{cx + j.x * h, y0 + j.y * h};
        if (const Json* placed = given.isObject() ? given.find(j.name) : nullptr) p = Point(*placed, std::string("joints.") + j.name);
        at[j.name] = p;
        joints[j.name] = PointJson(p);
        notes[j.name] = j.note;
    }
    if (given.isObject())
        for (const auto& g : given.members())
            if (!at.count(g.first)) throw ApiError("invalid_args", "unknown joint '" + g.first + "' for " + kind, "The template's joints are listed in its notes.");
    // The figure across a -> b: opaque runs along the normal at five places; the second thinnest (not the median: a limb
    // touching the body measures it too) gives half the thickness plus a margin so the region holds the outline, and
    // how far the run's middle lies off the line (a joint placed on the edge of a limb). `fallback` without an image
    // or where nothing is opaque.
    struct Across {
        float half = 0;
        P2 shift;  // moves the line onto the middle of the limb
    };
    auto across = [&](P2 a, P2 b, float fallback) {
        const Across none{fallback, {}};
        if (!art) return none;
        P2 d = b - a;
        const float len = std::sqrt(d.x * d.x + d.y * d.y);
        if (len < 1) return none;
        d = d * (1 / len);
        const P2 n{-d.y, d.x};
        auto opaqueAt = [&](P2 p) {
            const int x = static_cast<int>(p.x), y = static_cast<int>(p.y);
            return x >= 0 && y >= 0 && x < art->width && y < art->height &&
                   (art->texels[static_cast<size_t>(y) * static_cast<size_t>(art->width) + static_cast<size_t>(x)] >> 24) > 8;
        };
        std::vector<std::pair<float, float>> runs;  // length, middle of the run along n
        for (float t : {0.2f, 0.35f, 0.5f, 0.65f, 0.8f}) {
            const P2 c = a + (b - a) * t;
            if (!opaqueAt(c)) continue;
            float reach[2] = {0, 0};
            for (int k = 0; k < 2; ++k)
                for (float s = 1; s < 0.3f * h && opaqueAt(c + n * (k == 0 ? s : -s)); ++s) reach[k] += 1;
            runs.emplace_back(reach[0] + reach[1] + 1, (reach[0] - reach[1]) * 0.5f);
        }
        if (runs.empty()) return none;
        std::sort(runs.begin(), runs.end());
        const auto run = runs[std::min<size_t>(1, runs.size() - 1)];
        return Across{run.first * 0.5f * 1.15f + 6, n * run.second};
    };
    auto halfWidth = [&](P2 a, P2 b, float fallback) { return across(a, b, fallback).half; };
    // A band around a chain of joints, moved onto the middle of the limb, as wide as its widest segment (up to `cap`).
    auto band = [&](const std::vector<P2>& chain, float fallback, float cap) {
        std::vector<Across> segments;
        float half = 0;
        for (size_t i = 0; i + 1 < chain.size(); ++i) {
            segments.push_back(across(chain[i], chain[i + 1], fallback));
            half = std::max(half, segments.back().half);
        }
        std::vector<P2> moved = chain;
        for (size_t i = 0; i < chain.size(); ++i) {
            P2 shift;
            float count = 0;
            if (i > 0) shift = shift + segments[i - 1].shift, count += 1;
            if (i < segments.size()) shift = shift + segments[i].shift, count += 1;
            moved[i] = chain[i] + shift * (1.0f / std::max(1.0f, count));
        }
        return Ribbon(moved, std::min(cap, half));
    };
    Json bones = Json::MakeArray();
    auto bone = [&](const std::string& name, const std::string& parent, const std::string& from, const std::string& to) {
        Json b = Json::MakeObject();
        b["name"] = name;
        if (!parent.empty()) b["parent"] = parent;
        b["from"] = from;
        b["to"] = to;
        bones.push(b);
    };
    bone("hip", "", "hip", "chest");
    bone("chest", "hip", "chest", "neck");
    bone("head", "chest", "neck", "head_top");
    for (const std::string& s : {r, l}) {
        bone("upper_arm" + s, "chest", "shoulder" + s, "elbow" + s);
        bone("forearm" + s, "upper_arm" + s, "elbow" + s, "wrist" + s);
        bone("hand" + s, "forearm" + s, "wrist" + s, "hand_tip" + s);
        bone("thigh" + s, "hip", "hip" + s, "knee" + s);
        bone("shin" + s, "thigh" + s, "knee" + s, "ankle" + s);
        bone("foot" + s, "shin" + s, "ankle" + s, "toe" + s);
    }
    auto poly = [&](std::vector<P2> points) {
        Json list = Json::MakeArray();
        for (P2 p : points) list.push(PointJson(p));
        return list;
    };
    auto part = [&](const std::string& name, const Json& polygon, const std::vector<std::string>& follow) {
        Json p = Json::MakeObject();
        p["name"] = name;
        p["polygon"] = polygon;
        if (follow.size() == 1) p["bone"] = follow[0];
        else {
            Json list = Json::MakeArray();
            for (const std::string& f : follow) list.push(f);
            p["bones"] = list;
        }
        return p;
    };
    auto limbParts = [&](const std::string& s, Json& list, bool arm) {
        if (arm) {
            list.push(part("arm" + s, poly(band({at["shoulder" + s], at["elbow" + s], at["wrist" + s]}, 0.035f * h, 0.06f * h)), {"upper_arm" + s, "forearm" + s}));
            list.push(part("hand" + s, poly(Ribbon({at["wrist" + s], at["hand_tip" + s]}, std::max(0.03f * h, halfWidth(at["wrist" + s], at["hand_tip" + s], 0.035f * h)))), {"hand" + s}));
        } else {
            list.push(part("leg" + s, poly(band({at["hip" + s], at["knee" + s], at["ankle" + s]}, 0.05f * h, 0.09f * h)), {"thigh" + s, "shin" + s}));
            // A foot is seen from the front or side: as wide as its boot, measured across the ankle-toe line.
            list.push(part("foot" + s, poly(band({at["ankle" + s], at["toe" + s]}, 0.035f * h, 0.07f * h)), {"foot" + s}));
        }
    };
    Json parts = Json::MakeArray();
    if (!isFront) limbParts(r, parts, true);  // the far arm behind everything
    limbParts(r, parts, false);
    limbParts(l, parts, false);
    // The torso from the shoulders to below the hips: as wide as the shoulders at the top, the measured waist in the
    // middle and the hips plus a leg's width at the bottom.
    const P2 neck = at["neck"], chest = at["chest"], hip = at["hip"];
    const float topY = neck.y, bottomY = hip.y + 0.04f * h;
    const float left = std::min(at["shoulder" + r].x, at["shoulder" + l].x), right = std::max(at["shoulder" + r].x, at["shoulder" + l].x);
    const float pad = 0.02f * h;
    const float legHalf = halfWidth(at["hip" + l], at["knee" + l], 0.05f * h);
    // At the bottom: the hips plus a leg, or what is opaque across the hip line (a tunic or skirt), no wider than the shoulders.
    const float hipHalf = std::min((right - left) * 0.5f + pad,
                                   std::max(std::max(std::fabs(at["hip" + l].x - hip.x), std::fabs(at["hip" + r].x - hip.x)) + legHalf,
                                            halfWidth({hip.x, hip.y - 1}, {hip.x, hip.y + 1}, 0)));
    const float shoulderY = std::max(at["shoulder" + r].y, at["shoulder" + l].y) + 0.03f * h;
    // halfWidth measures across a line: across a short vertical one is the horizontal run (no wider than the shoulders).
    const float waistHalf = std::min((right - left) * 0.5f + pad, halfWidth({hip.x, (chest.y + hip.y) * 0.5f - 1}, {hip.x, (chest.y + hip.y) * 0.5f + 1}, 0.09f * h));
    const float midY = (chest.y + hip.y) * 0.5f;
    if (isFront) {
        parts.push(part("torso", poly({{left - pad, topY}, {right + pad, topY}, {right + pad, shoulderY}, {hip.x + waistHalf, midY}, {hip.x + hipHalf, bottomY},
                                       {hip.x - hipHalf, bottomY}, {hip.x - waistHalf, midY}, {left - pad, shoulderY}}),
                        {"hip", "chest"}));
    } else {
        // Seen from the side the shoulders are one point: the body is as deep as the picture is wide across it, measured
        // at the chest, the waist and the hips, each band centred on what it measured.
        auto depth = [&](float y, float fallback) {
            const Across a = across({chest.x, y - 1}, {chest.x, y + 1}, fallback);
            return std::make_pair(chest.x + a.shift.x, std::min(a.half, 0.16f * h));
        };
        const auto upper = depth(chest.y, 0.08f * h), mid = depth(midY, 0.08f * h), low = depth(hip.y, 0.08f * h);
        parts.push(part("torso", poly({{upper.first - upper.second, topY}, {upper.first + upper.second, topY}, {mid.first + mid.second, midY},
                                       {low.first + low.second, bottomY}, {low.first - low.second, bottomY}, {mid.first - mid.second, midY}}),
                        {"hip", "chest"}));
    }
    // The head: an ellipse from the neck to the top of the head, as wide as the face is across its middle.
    const P2 top = at["head_top"], middle = (neck + top) * 0.5f;
    const float ry = std::fabs(neck.y - top.y) * 0.5f * 1.08f + 4;
    const float rx = halfWidth({middle.x, middle.y - 1}, {middle.x, middle.y + 1}, ry * 0.9f);
    Json head = Json::MakeObject();
    head["name"] = "head";
    head["ellipse"] = Json(Json::Array{std::round(middle.x), std::round(middle.y), std::round(rx), std::round(ry)});
    head["bone"] = "head";
    parts.push(head);
    if (isFront) limbParts(r, parts, true);
    limbParts(l, parts, true);

    Json draft = Json::MakeObject();
    if (!image.empty()) draft["image"] = image;
    draft["height"] = 2;
    draft["joints"] = joints;
    draft["bones"] = bones;
    draft["parts"] = parts;
    Json out = Json::MakeObject();
    out["draft"] = draft;
    out["notes"] = notes;
    out["guessed"] = guessed;
    out["next"] = guessed ? "Joints and regions are guesses from usual proportions. Check each joint with image.grid {path: image, rect, marks}, move it, "
                            "fit the regions to the parts of the picture (neighbours should overlap a little at joints), then skeleton2d.build {draft, dryRun: true} "
                            "and read its draftImage."
                          : "Positions are for a nominal 400 x 1000 figure; set draft.image and place every joint on it.";
    return out;
}

}  // namespace oe
