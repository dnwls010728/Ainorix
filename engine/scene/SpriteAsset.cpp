#include "scene/SpriteAsset.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "scene/Components.h"
#include "scene/Scene.h"

namespace oe {

bool IsSpriteAssetPath(const std::string& path) {
    if (path.size() < 12) return false;
    std::string tail = path.substr(path.size() - 12);
    for (char& c : tail) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return tail == ".sprite.json";
}

bool ParseSpriteSource(const std::string& asset, SpriteSource& out, std::string* error) {
    out = SpriteSource();
    if (IsSpriteAssetPath(asset)) return false;
    const size_t q = asset.find('?');
    out.path = asset.substr(0, q);
    if (q == std::string::npos) return true;
    auto fail = [&](const std::string& message) {
        if (error) *error = "sprite source '" + asset + "': " + message;
        return false;
    };
    const std::string query = asset.substr(q + 1);
    for (size_t at = 0; at <= query.size();) {
        size_t end = query.find('&', at);
        if (end == std::string::npos) end = query.size();
        const std::string item = query.substr(at, end - at);
        at = end + 1;
        if (item.empty()) continue;
        const size_t eq = item.find('=');
        const std::string key = item.substr(0, eq), value = eq == std::string::npos ? std::string() : item.substr(eq + 1);
        char* rest = nullptr;
        const double number = std::strtod(value.c_str(), &rest);
        const bool isNumber = !value.empty() && rest && *rest == '\0' && std::isfinite(number);
        if (key == "columns" || key == "rows") {
            if (!isNumber || number < 1 || number > 4096 || std::floor(number) != number) return fail(key + " must be 1..4096");
            (key == "columns" ? out.columns : out.rows) = static_cast<int>(number);
        } else if (key == "ppu") {
            if (!isNumber || number < 0.001 || number > 100000) return fail("ppu must be 0.001..100000");
            out.ppu = number;
        } else if (key == "pivot") {
            const size_t comma = value.find(',');
            char *ra = nullptr, *rb = nullptr;
            const std::string a = value.substr(0, comma), b = comma == std::string::npos ? std::string() : value.substr(comma + 1);
            out.pivotX = std::strtod(a.c_str(), &ra);
            out.pivotY = std::strtod(b.c_str(), &rb);
            if (comma == std::string::npos || a.empty() || b.empty() || *ra || *rb || !std::isfinite(out.pivotX) || !std::isfinite(out.pivotY))
                return fail("pivot must be x,y");
        } else if (key == "pixelArt") {
            if (value != "0" && value != "1") return fail("pixelArt must be 0 or 1");
            out.pixelArt = value == "1";
        } else if (key == "normal") {
            out.normal = value;
        } else {
            return fail("unknown setting '" + key + "' (columns, rows, ppu, pivot, pixelArt, normal)");
        }
    }
    return true;
}

std::string FormatSpriteSource(const SpriteSource& s) {
    std::string query;
    auto add = [&](const std::string& item) { query += (query.empty() ? "?" : "&") + item; };
    auto number = [](double v) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.9g", v);
        return std::string(buf);
    };
    if (s.columns != 1) add("columns=" + std::to_string(s.columns));
    if (s.rows != 1) add("rows=" + std::to_string(s.rows));
    if (s.ppu != 16) add("ppu=" + number(s.ppu));
    if (s.pivotX != 0.5 || s.pivotY != 0.5) add("pivot=" + number(s.pivotX) + "," + number(s.pivotY));
    if (!s.pixelArt) add("pixelArt=0");
    if (!s.normal.empty()) add("normal=" + s.normal);
    return s.path + query;
}

std::string SpriteAssetOf(const Scene& scene, EntityId id, const SpriteAssets* assets) {
    const Sprite* sp = scene.Get<Sprite>(id);
    if (!sp) return std::string();
    // A shared .anim.json names its own image, which the sprite then draws (as RenderScene does).
    if (const SpriteAnimation* animation = scene.Get<SpriteAnimation>(id); animation && !animation->asset.empty()) {
        if (!assets || !assets->animation) return std::string();
        const auto definition = assets->animation(animation->asset);
        if (!definition) return std::string();
        // Its image with the Sprite.asset settings (pixels per unit, pivot, ...) when that is not a file.
        SpriteSource settings;
        ParseSpriteSource(sp->asset, settings);
        const Json& sheet = (*definition)["sheet"];
        if (sheet.isObject()) {
            settings.path = sheet["texture"].asString();
            settings.columns = std::max(1, sheet["columns"].asInt(1));
            settings.rows = std::max(1, sheet["rows"].asInt(1));
            return FormatSpriteSource(settings);
        }
        const std::string path = (*definition)["atlas"]["path"].asString();
        if (path.empty()) return std::string();  // rectangles listed in the definition (RenderScene draws those)
        if (IsSpriteAssetPath(path)) return path;
        settings.path = path;
        settings.columns = settings.rows = 1;
        return FormatSpriteSource(settings);
    }
    return sp->asset;
}

const Json* SpriteAssetFrame(const Json& asset, const SpriteFrame& frame) {
    const Json& frames = asset["frames"];
    if (!frames.isArray()) return nullptr;
    if (!frame.name.empty()) {
        for (const Json& f : frames.items())
            if (f["name"].asString() == frame.name) return &f;
        return nullptr;
    }
    if (asset["grid"].isArray() && frames.size() > 0) {  // an image cut by columns/rows: indices wrap, as they always did
        const int count = static_cast<int>(frames.size());
        return &frames[((frame.index % count) + count) % count];
    }
    return frame.index >= 0 && static_cast<size_t>(frame.index) < frames.size() ? &frames[frame.index] : nullptr;
}

SpritePixelSpace SpriteFramePixelSpace(const Json& asset, const Json& frame, float width, float height, bool flipX, bool flipY) {
    const float canvasW = std::max(1.0f, frame["size"][0].asFloat(1)), canvasH = std::max(1.0f, frame["size"][1].asFloat(1));
    const float ppu = std::max(0.001f, asset["pixelsPerUnit"].asFloat(16));
    SpritePixelSpace space;
    space.originX = frame["pivot"][0].asFloat(0.5f) * canvasW;
    space.originY = (1.0f - frame["pivot"][1].asFloat(0.5f)) * canvasH;
    space.scaleX = (width > 0 ? width / canvasW : 1.0f / ppu) * (flipX ? -1.0f : 1.0f);
    space.scaleY = (height > 0 ? height / canvasH : 1.0f / ppu) * (flipY ? -1.0f : 1.0f);
    return space;
}

bool SpriteColliderOutline(const Scene& scene, EntityId id, const SpriteAssets* assets, Json& points, std::string* key, std::string* error) {
    auto fail = [&](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    const Sprite* sp = scene.Get<Sprite>(id);
    if (!sp) return fail("Collider2D shape sprite needs a Sprite on the same entity");
    const std::string path = SpriteAssetOf(scene, id, assets);
    if (path.empty()) return fail("Collider2D shape sprite needs a Sprite with a sprite asset (Sprite.asset, a .sprite.json)");
    const auto asset = assets && assets->sprite ? assets->sprite(path) : nullptr;
    if (!asset) return fail("cannot read sprite asset " + path);
    const Json* frame = SpriteAssetFrame(*asset, sp->frame);
    if (!frame) return fail("sprite asset " + path + " has no frame " + SpriteFrameKey(sp->frame));
    const SpritePixelSpace space = SpriteFramePixelSpace(*asset, *frame, sp->width, sp->height, sp->flipX, sp->flipY);
    Json source = (*frame)["collision"];
    if (!source.isArray() || source.size() < 3) {
        // No outline drawn for this frame: the rectangle of its pixels.
        const bool rotated = (*frame)["rotated"].asBool();
        const float x = (*frame)["trim"][0].asFloat(), y = (*frame)["trim"][1].asFloat();
        const float w = (*frame)["rect"][rotated ? 3 : 2].asFloat(), h = (*frame)["rect"][rotated ? 2 : 3].asFloat();
        source = Json::Array{Json::Array{x, y}, Json::Array{x + w, y}, Json::Array{x + w, y + h}, Json::Array{x, y + h}};
    }
    points = Json::MakeArray();
    for (const Json& p : source.items())
        points.push(Json::Array{space.ToLocalX(p[0].asFloat()), space.ToLocalY(p[1].asFloat())});
    if (key) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "|%p|%g,%g|%d%d", static_cast<const void*>(asset.get()), sp->width, sp->height, sp->flipX ? 1 : 0, sp->flipY ? 1 : 0);
        *key = path + "#" + SpriteFrameKey(sp->frame) + buf;
    }
    return true;
}

Json Collider2DOutline(const Scene& scene, EntityId id, const Collider2D& collider, const SpriteAssets* assets, std::string* key) {
    if (key) key->clear();
    if (collider.shape == "sprite") {
        Json points;
        if (SpriteColliderOutline(scene, id, assets, points, key)) return points;
        return Json::MakeArray();
    }
    if (collider.shape != "polygon" && collider.shape != "edge") return Json::MakeArray();
    const Sprite* sp = scene.Get<Sprite>(id);
    const Json& outline = Collider2DActivePoints(collider, sp);
    if (key && &outline != &collider.points) *key = SpriteFrameKey(sp->frame);
    return outline.isArray() ? outline : Json::MakeArray();
}

// ----- Outline tracing ---------------------------------------------------------------------------

namespace {

struct Pt {
    int x = 0, y = 0;
    bool operator==(const Pt& o) const { return x == o.x && y == o.y; }
    bool operator<(const Pt& o) const { return x != o.x ? x < o.x : y < o.y; }
};

long long Cross(const Pt& o, const Pt& a, const Pt& b) {
    return static_cast<long long>(a.x - o.x) * (b.y - o.y) - static_cast<long long>(a.y - o.y) * (b.x - o.x);
}

std::vector<Pt> Hull(std::vector<Pt> points) {
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());
    if (points.size() < 3) return points;
    std::vector<Pt> hull(points.size() * 2);
    size_t k = 0;
    for (size_t i = 0; i < points.size(); ++i) {
        while (k >= 2 && Cross(hull[k - 2], hull[k - 1], points[i]) <= 0) --k;
        hull[k++] = points[i];
    }
    for (size_t i = points.size() - 1, lower = k + 1; i-- > 0;) {
        while (k >= lower && Cross(hull[k - 2], hull[k - 1], points[i]) <= 0) --k;
        hull[k++] = points[i];
    }
    hull.resize(k - 1);
    return hull;
}

float SegmentDistance(const Pt& p, const Pt& a, const Pt& b) {
    const float dx = static_cast<float>(b.x - a.x), dy = static_cast<float>(b.y - a.y);
    const float len = dx * dx + dy * dy;
    float t = len > 0 ? ((static_cast<float>(p.x - a.x)) * dx + (static_cast<float>(p.y - a.y)) * dy) / len : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const float qx = static_cast<float>(a.x) + dx * t - static_cast<float>(p.x), qy = static_cast<float>(a.y) + dy * t - static_cast<float>(p.y);
    return std::sqrt(qx * qx + qy * qy);
}

// Ramer-Douglas-Peucker on the open chain loop[first..last] (indices modulo the loop size).
void Reduce(const std::vector<Pt>& loop, size_t first, size_t last, float tolerance, std::vector<bool>& keep) {
    const size_t n = loop.size();
    if ((last + n - first) % n < 2) return;
    float worst = -1;
    size_t at = first;
    for (size_t i = (first + 1) % n; i != last; i = (i + 1) % n) {
        const float d = SegmentDistance(loop[i], loop[first], loop[last]);
        if (d > worst) worst = d, at = i;
    }
    if (worst <= tolerance) return;
    keep[at] = true;
    Reduce(loop, first, at, tolerance, keep);
    Reduce(loop, at, last, tolerance, keep);
}

bool SegmentsCross(const Pt& a, const Pt& b, const Pt& c, const Pt& d) {
    const long long d1 = Cross(a, b, c), d2 = Cross(a, b, d), d3 = Cross(c, d, a), d4 = Cross(c, d, b);
    return ((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0));
}

bool Simple(const std::vector<Pt>& p) {
    const size_t n = p.size();
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 2; j < n; ++j) {
            if (i == 0 && j == n - 1) continue;  // neighbours through the closing edge
            if (SegmentsCross(p[i], p[(i + 1) % n], p[j], p[(j + 1) % n])) return false;
        }
    return true;
}

std::vector<Pt> Simplify(const std::vector<Pt>& loop, float tolerance) {
    const size_t n = loop.size();
    if (n <= 3 || tolerance <= 0) return loop;
    // Split at the point farthest from the first one: two chains that each bend less than the whole loop.
    size_t far = 0;
    long long best = -1;
    for (size_t i = 1; i < n; ++i) {
        const long long dx = loop[i].x - loop[0].x, dy = loop[i].y - loop[0].y;
        if (dx * dx + dy * dy > best) best = dx * dx + dy * dy, far = i;
    }
    std::vector<bool> keep(n, false);
    keep[0] = keep[far] = true;
    Reduce(loop, 0, far, tolerance, keep);
    Reduce(loop, far, 0, tolerance, keep);
    std::vector<Pt> out;
    for (size_t i = 0; i < n; ++i)
        if (keep[i]) out.push_back(loop[i]);
    return out;
}

}  // namespace

Json TraceSpriteOutline(const uint32_t* texels, int textureWidth, int textureHeight, int x, int y, int width, int height, float cutoff, float detail,
                        bool convex) {
    Json out = Json::MakeArray();
    const int x0 = std::max(0, x), y0 = std::max(0, y);
    const int w = std::min(textureWidth, x + width) - x0, h = std::min(textureHeight, y + height) - y0;
    if (!texels || w <= 0 || h <= 0) return out;
    const int ox = x0 - x, oy = y0 - y;  // the rectangle may start outside the texture
    const uint32_t threshold = static_cast<uint32_t>(std::clamp(std::ceil(cutoff * 255.0f), 1.0f, 255.0f));
    std::vector<int> label(static_cast<size_t>(w) * static_cast<size_t>(h), -1);
    auto opaque = [&](int px, int py) {
        return (texels[static_cast<size_t>(y0 + py) * static_cast<size_t>(textureWidth) + static_cast<size_t>(x0 + px)] >> 24) >= threshold;
    };
    // The largest 4-connected part (a stray pixel or a detached spark does not get the collision).
    int bestLabel = -1, bestCount = 0, labels = 0;
    Pt bestStart;
    std::vector<Pt> stack;
    for (int py = 0; py < h; ++py)
        for (int px = 0; px < w; ++px) {
            if (label[static_cast<size_t>(py) * w + px] >= 0 || !opaque(px, py)) continue;
            const int id = labels++;
            int count = 0;
            stack.push_back({px, py});
            label[static_cast<size_t>(py) * w + px] = id;
            while (!stack.empty()) {
                const Pt p = stack.back();
                stack.pop_back();
                ++count;
                const int next[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (const auto& d : next) {
                    const int nx = p.x + d[0], ny = p.y + d[1];
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h || label[static_cast<size_t>(ny) * w + nx] >= 0 || !opaque(nx, ny)) continue;
                    label[static_cast<size_t>(ny) * w + nx] = id;
                    stack.push_back({nx, ny});
                }
            }
            if (count > bestCount) bestCount = count, bestLabel = id, bestStart = {px, py};  // first pixel found: top row, leftmost
        }
    if (bestLabel < 0) return out;
    auto inside = [&](int px, int py) { return px >= 0 && py >= 0 && px < w && py < h && label[static_cast<size_t>(py) * w + px] == bestLabel; };
    std::vector<Pt> loop;
    if (convex) {
        std::vector<Pt> corners;
        for (int py = 0; py < h; ++py)
            for (int px = 0; px < w; ++px)
                if (inside(px, py) && (!inside(px - 1, py) || !inside(px + 1, py) || !inside(px, py - 1) || !inside(px, py + 1)))
                    corners.insert(corners.end(), {{px, py}, {px + 1, py}, {px, py + 1}, {px + 1, py + 1}});
        loop = Hull(corners);
    } else {
        // Walk the part's outer edge along pixel borders with the part on the left (Y down), starting at the
        // top edge of its first pixel, which is on the outer edge. At a corner shared with a diagonal pixel the
        // walk turns left first, staying around the pixels it came along.
        Pt at{bestStart.x + 1, bestStart.y}, dir{-1, 0};
        const Pt start = at, startDir = dir;
        auto edgeExists = [&](const Pt& from, const Pt& d) {
            // The pixel left of the edge is in the part, the one right of it is not.
            const Pt leftPixel{d.x == 1 ? from.x : d.x == -1 ? from.x - 1 : d.y == 1 ? from.x : from.x - 1,
                               d.x == 1 ? from.y - 1 : d.x == -1 ? from.y : d.y == 1 ? from.y : from.y - 1};
            const Pt rightPixel{d.x == 1 ? from.x : d.x == -1 ? from.x - 1 : d.y == 1 ? from.x - 1 : from.x,
                                d.x == 1 ? from.y : d.x == -1 ? from.y - 1 : d.y == 1 ? from.y : from.y - 1};
            return inside(leftPixel.x, leftPixel.y) && !inside(rightPixel.x, rightPixel.y);
        };
        const size_t limit = static_cast<size_t>(w + 1) * static_cast<size_t>(h + 1) * 4;
        do {
            if (loop.empty() || !(loop.back() == at)) loop.push_back(at);
            at = {at.x + dir.x, at.y + dir.y};
            const Pt left{dir.y, -dir.x}, right{-dir.y, dir.x};
            if (edgeExists(at, left)) dir = left;
            else if (edgeExists(at, dir)) {
            } else dir = right;
        } while (!(at == start && dir == startDir) && loop.size() < limit);
        // Corners only: drop points in the middle of straight runs.
        std::vector<Pt> corners;
        for (size_t i = 0; i < loop.size(); ++i) {
            const Pt& a = loop[(i + loop.size() - 1) % loop.size()];
            const Pt& b = loop[i];
            const Pt& c = loop[(i + 1) % loop.size()];
            if (Cross(a, b, c) != 0) corners.push_back(b);
        }
        loop = corners;
        // Simplify as much as `detail` allows while the outline stays simple and small enough to edit.
        float tolerance = std::max(0.0f, detail);
        std::vector<Pt> simple = Simplify(loop, tolerance);
        while (simple.size() > 3 && !Simple(simple) && tolerance > 0.05f) simple = Simplify(loop, tolerance *= 0.5f);
        for (float more = std::max(1.0f, tolerance); simple.size() > 64 && more < 1e6f; more *= 1.5f) simple = Simplify(loop, more);
        if (simple.size() >= 3 && Simple(simple)) loop = simple;
        if (loop.size() > 64 || loop.size() < 3) {
            std::vector<Pt> corners2;
            for (const Pt& p : loop) corners2.push_back(p);
            loop = Hull(corners2);
        }
    }
    for (const Pt& p : loop) out.push(Json::Array{p.x + ox, p.y + oy});
    return out;
}

}  // namespace oe
