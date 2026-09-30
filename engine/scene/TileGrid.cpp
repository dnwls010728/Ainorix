#include "scene/TileGrid.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>

#include "scene/Scene.h"

namespace oe {

// ----- Rules -------------------------------------------------------------------

bool IsTilesetFile(const std::string& path) {
    const std::string ext = ".tileset.json";
    return path.size() > ext.size() && path.compare(path.size() - ext.size(), ext.size(), ext) == 0;
}

namespace {

bool ParseCollision(const Json& v, TileDef& out, std::string* error) {
    auto poly = [&](std::initializer_list<TilePoint> pts) {
        out.collision = TileCollision::Shape;
        out.shape.assign(pts.begin(), pts.end());
        return true;
    };
    if (v.isString()) {
        const std::string& s = v.asString();
        if (s == "none") return out.collision = TileCollision::None, out.shape.clear(), true;
        if (s == "solid") return out.collision = TileCollision::Solid, out.shape.clear(), true;
        if (s == "oneway") return out.collision = TileCollision::OneWay, out.shape.clear(), true;
        if (s == "slope-up") return poly({{0, 0}, {1, 0}, {1, 1}});
        if (s == "slope-down") return poly({{0, 0}, {1, 0}, {0, 1}});
        if (s == "half-bottom") return poly({{0, 0}, {1, 0}, {1, 0.5f}, {0, 0.5f}});
        if (s == "half-top") return poly({{0, 0.5f}, {1, 0.5f}, {1, 1}, {0, 1}});
        if (error) *error = "unknown collision \"" + s + "\" (none, solid, oneway, slope-up, slope-down, half-bottom, half-top or [[x,y],...])";
        return false;
    }
    if (!v.isArray() || v.size() < 3 || v.size() > 8) {
        if (error) *error = "collision polygon needs 3 to 8 points [[x,y], ...] in tile units";
        return false;
    }
    std::vector<TilePoint> pts;
    for (const Json& p : v.items()) {
        if (!p.isArray() || p.size() != 2 || !p[0].isNumber() || !p[1].isNumber()) {
            if (error) *error = "collision polygon points must be [x, y] pairs";
            return false;
        }
        pts.push_back({p[0].asFloat(), p[1].asFloat()});
    }
    float area = 0;
    for (size_t i = 0; i < pts.size(); ++i) {
        const TilePoint& a = pts[i];
        const TilePoint& b = pts[(i + 1) % pts.size()];
        area += a.x * b.y - b.x * a.y;
    }
    if (std::fabs(area) < 1e-4f) {
        if (error) *error = "collision polygon has no area";
        return false;
    }
    if (area < 0) std::reverse(pts.begin(), pts.end());
    for (size_t i = 0; i < pts.size(); ++i) {
        const TilePoint& a = pts[i];
        const TilePoint& b = pts[(i + 1) % pts.size()];
        const TilePoint& c = pts[(i + 2) % pts.size()];
        if ((b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x) < -1e-5f) {
            if (error) *error = "collision polygon must be convex (split concave shapes over several tiles)";
            return false;
        }
    }
    out.collision = TileCollision::Shape;
    out.shape = std::move(pts);
    return true;
}

bool ParseFrameList(const Json& v, std::vector<int>& out, std::string* error, const char* what) {
    if (!v.isArray()) {
        if (error) *error = std::string(what) + " must be an array of frame numbers";
        return false;
    }
    out.clear();
    for (const Json& f : v.items()) {
        if (!f.isNumber()) {
            if (error) *error = std::string(what) + " must contain frame numbers";
            return false;
        }
        out.push_back(f.asInt());
    }
    return true;
}

uint32_t CellHash(int col, int row) {
    uint32_t h = static_cast<uint32_t>(col) * 0x9E3779B1u ^ (static_cast<uint32_t>(row) + 0x7F4A7C15u) * 0x85EBCA77u;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

}  // namespace

bool ParseTileRule(const Json& v, TileDef& out, std::string* error) {
    if (v.isNumber()) {
        out.frame = v.asInt();
        out.variants.clear();
        out.autotile = AutoTile::None;
        out.autoFrames.clear();
        return true;
    }
    if (!v.isObject()) {
        if (error) *error = "a tile rule is a frame number or an object {frame, variants, autotile, frames, connects, edges, collision}";
        return false;
    }
    for (const auto& kv : v.members()) {
        const std::string& k = kv.first;
        if (k != "frame" && k != "variants" && k != "autotile" && k != "frames" && k != "connects" && k != "edges" && k != "collision") {
            if (error) *error = "unknown tile rule key \"" + k + "\" (frame, variants, autotile, frames, connects, edges, collision)";
            return false;
        }
    }
    if (const Json* f = v.find("frame")) {
        if (!f->isNumber()) {
            if (error) *error = "frame must be a number";
            return false;
        }
        out.frame = f->asInt();
    }
    if (const Json* f = v.find("variants")) {
        if (!ParseFrameList(*f, out.variants, error, "variants")) return false;
    }
    if (const Json* a = v.find("autotile")) {
        std::string mode = a->asString("");
        if (mode == "sides") out.autotile = AutoTile::Sides;
        else if (mode == "blob") out.autotile = AutoTile::Blob;
        else if (mode == "none") out.autotile = AutoTile::None;
        else {
            if (error) *error = "autotile must be \"sides\" (16 frames) or \"blob\" (47 frames)";
            return false;
        }
    }
    if (out.autotile != AutoTile::None) {
        size_t need = out.autotile == AutoTile::Sides ? 16 : static_cast<size_t>(kBlobTileCount);
        if (const Json* f = v.find("frames")) {
            if (!ParseFrameList(*f, out.autoFrames, error, "frames")) return false;
            if (out.autoFrames.size() != need) {
                if (error) *error = "autotile \"" + std::string(out.autotile == AutoTile::Sides ? "sides" : "blob") + "\" needs " + std::to_string(need) + " frames, got " +
                                    std::to_string(out.autoFrames.size());
                return false;
            }
        } else {
            if (out.frame < 0) {
                if (error) *error = "autotile needs \"frame\" (the first of consecutive frames) or \"frames\"";
                return false;
            }
            out.autoFrames.clear();
            for (size_t i = 0; i < need; ++i) out.autoFrames.push_back(out.frame + static_cast<int>(i));
        }
    } else {
        out.autoFrames.clear();
    }
    if (const Json* c = v.find("connects")) {
        if (!c->isString()) {
            if (error) *error = "connects must be a string of characters";
            return false;
        }
        out.connects = c->asString();
    }
    if (const Json* e = v.find("edges")) out.edges = e->asBool(true);
    if (const Json* c = v.find("collision")) {
        if (!ParseCollision(*c, out, error)) return false;
    }
    return true;
}

bool ParseTileset(const Json& j, Tileset& out, std::string* error) {
    if (!j.isObject()) {
        if (error) *error = "a tileset file is a JSON object {image, columns, rows, tiles}";
        return false;
    }
    out = Tileset();
    out.image = j["image"].asString("");
    out.columns = std::max(1, j["columns"].asInt(1));
    out.rows = std::max(1, j["rows"].asInt(1));
    const Json& tiles = j["tiles"];
    if (!tiles.isNull() && !tiles.isObject()) {
        if (error) *error = "tiles must be an object {\"#\": rule, ...}";
        return false;
    }
    if (tiles.isObject()) {
        for (const auto& kv : tiles.members()) {
            if (kv.first.size() != 1) {
                if (error) *error = "tile key \"" + kv.first + "\" must be exactly one ASCII character";
                return false;
            }
            std::string err;
            if (!ParseTileRule(kv.second, out.tiles[kv.first[0]], &err)) {
                if (error) *error = "tile \"" + kv.first + "\": " + err;
                return false;
            }
        }
    }
    out.key = j.dump();
    return true;
}

TileRules BuildTileRules(const Tilemap& tm, const TilesetLookup* lookup) {
    TileRules r;
    r.image = tm.tileset;
    r.columns = std::max(1, tm.columns);
    r.rows = std::max(1, tm.rows);
    if (IsTilesetFile(tm.tileset)) {
        r.image.clear();
        std::shared_ptr<const Tileset> ts;
        std::string err;
        if (lookup && *lookup) ts = (*lookup)(tm.tileset, &err);
        else err = "tileset files are not available here";
        if (ts) {
            r.image = ts->image;
            r.columns = ts->columns;
            r.rows = ts->rows;
            r.tiles = ts->tiles;
            r.key = ts->key;
        } else {
            r.error = tm.tileset + ": " + err;
        }
    }
    if (tm.legend.isObject()) {
        for (const auto& kv : tm.legend.members()) {
            if (kv.first.size() != 1) {
                r.error = "legend key \"" + kv.first + "\" must be exactly one ASCII character";
                continue;
            }
            std::string err;
            TileDef def = r.tiles[kv.first[0]];
            if (ParseTileRule(kv.second, def, &err)) r.tiles[kv.first[0]] = def;
            else r.error = "legend \"" + kv.first + "\": " + err;
        }
    }
    for (char c : tm.solid) {
        TileDef& def = r.tiles[c];
        if (def.collision != TileCollision::Shape && def.collision != TileCollision::OneWay) def.collision = TileCollision::Solid;
    }
    r.key += "|" + tm.legend.dump() + "|" + tm.solid + "|" + r.image + "|" + std::to_string(r.columns) + "x" + std::to_string(r.rows);
    return r;
}

// ----- Autotiling ------------------------------------------------------------------

namespace {
enum : int { kN = 1, kNE = 2, kE = 4, kSE = 8, kS = 16, kSW = 32, kW = 64, kNW = 128 };

int CleanBlobMask(int m) {
    if (!(m & kN) || !(m & kE)) m &= ~kNE;
    if (!(m & kS) || !(m & kE)) m &= ~kSE;
    if (!(m & kS) || !(m & kW)) m &= ~kSW;
    if (!(m & kN) || !(m & kW)) m &= ~kNW;
    return m;
}

struct BlobTable {
    std::array<int, 256> index{};
    std::vector<int> masks;
    BlobTable() {
        for (int m = 0; m < 256; ++m) {
            if (CleanBlobMask(m) == m) masks.push_back(m);
        }
        for (int m = 0; m < 256; ++m) {
            int clean = CleanBlobMask(m);
            index[static_cast<size_t>(m)] = static_cast<int>(std::lower_bound(masks.begin(), masks.end(), clean) - masks.begin());
        }
    }
};
const BlobTable& Blob() {
    static const BlobTable table;
    return table;
}
}  // namespace

int BlobIndex(int mask) { return Blob().index[static_cast<size_t>(mask & 255)]; }
const std::vector<int>& BlobMasks() { return Blob().masks; }

void MapSize(const Tilemap& tm, int& width, int& height) {
    width = 0;
    height = tm.map.isArray() ? static_cast<int>(tm.map.size()) : 0;
    for (int r = 0; r < height; ++r) width = std::max(width, static_cast<int>(tm.map[r].asString("").size()));
}

TileFrames ResolveFrames(const Tilemap& tm, const TileRules& rules) {
    TileFrames out;
    MapSize(tm, out.width, out.height);
    out.frames.assign(static_cast<size_t>(out.width) * static_cast<size_t>(out.height), -1);
    for (int row = 0; row < out.height; ++row) {
        const std::string& line = tm.map[row].asString();
        for (int col = 0; col < static_cast<int>(line.size()); ++col) {
            char c = line[static_cast<size_t>(col)];
            auto it = rules.tiles.find(c);
            if (it == rules.tiles.end()) continue;
            const TileDef& def = it->second;
            int frame = def.frame;
            if (def.autotile != AutoTile::None && !def.autoFrames.empty()) {
                auto same = [&](int dc, int dr) {
                    int cc = col + dc, rr = row + dr;
                    if (rr < 0 || rr >= out.height || cc < 0 || cc >= out.width) return def.edges;
                    char n = TileAt(tm, cc, rr);
                    return n == c || (n != '\0' && def.connects.find(n) != std::string::npos);
                };
                int mask = 0;
                if (def.autotile == AutoTile::Sides) {
                    mask = (same(0, -1) ? 1 : 0) | (same(1, 0) ? 2 : 0) | (same(0, 1) ? 4 : 0) | (same(-1, 0) ? 8 : 0);
                    frame = def.autoFrames[static_cast<size_t>(mask)];
                } else {
                    mask = (same(0, -1) ? kN : 0) | (same(1, -1) ? kNE : 0) | (same(1, 0) ? kE : 0) | (same(1, 1) ? kSE : 0) | (same(0, 1) ? kS : 0) |
                           (same(-1, 1) ? kSW : 0) | (same(-1, 0) ? kW : 0) | (same(-1, -1) ? kNW : 0);
                    frame = def.autoFrames[static_cast<size_t>(BlobIndex(mask))];
                }
            } else if (!def.variants.empty()) {
                frame = def.variants[CellHash(col, row) % def.variants.size()];
            }
            out.frames[static_cast<size_t>(row) * static_cast<size_t>(out.width) + static_cast<size_t>(col)] = frame;
        }
    }
    return out;
}

// ----- Cells -------------------------------------------------------------------------

char TileAt(const Tilemap& tm, int col, int row) {
    if (!tm.map.isArray() || row < 0 || col < 0 || row >= static_cast<int>(tm.map.size())) return '\0';
    const std::string& line = tm.map[row].asString();
    return col < static_cast<int>(line.size()) ? line[static_cast<size_t>(col)] : '\0';
}

bool SetTile(Tilemap& tm, int col, int row, char c) {
    if (row < 0 || col < 0 || row > 4096 || col > 4096) return false;
    if (!tm.map.isArray()) tm.map = Json::MakeArray();
    while (static_cast<int>(tm.map.size()) <= row) tm.map.push(std::string());
    std::string line = tm.map[row].asString("");
    if (static_cast<int>(line.size()) <= col) line.resize(static_cast<size_t>(col) + 1, ' ');
    line[static_cast<size_t>(col)] = c;
    tm.map[row] = line;
    return true;
}

const TileDef* RuleAt(const Tilemap& tm, const TileRules& rules, int col, int row) {
    char c = TileAt(tm, col, row);
    if (c == '\0') return nullptr;
    auto it = rules.tiles.find(c);
    return it == rules.tiles.end() ? nullptr : &it->second;
}

TileCollision CollisionAt(const Tilemap& tm, const TileRules& rules, int col, int row) {
    const TileDef* d = RuleAt(tm, rules, col, row);
    return d ? d->collision : TileCollision::None;
}

// ----- Collision geometry ------------------------------------------------------------

std::vector<TileRect> SolidRects(const Tilemap& tm, const TileRules& rules) {
    std::vector<TileRect> done;
    // Open rectangles keyed by their column span; extended while the next row
    // has exactly the same run.
    std::map<std::pair<int, int>, TileRect> open;
    int width = 0, rows = 0;
    MapSize(tm, width, rows);
    for (int row = 0; row <= rows; ++row) {
        std::map<std::pair<int, int>, TileRect> next;
        if (row < rows) {
            for (int col = 0; col < width;) {
                if (CollisionAt(tm, rules, col, row) != TileCollision::Solid) {
                    ++col;
                    continue;
                }
                int start = col;
                while (col < width && CollisionAt(tm, rules, col, row) == TileCollision::Solid) ++col;
                std::pair<int, int> span(start, col - start);
                auto it = open.find(span);
                if (it != open.end()) {
                    TileRect r = it->second;
                    ++r.height;
                    next[span] = r;
                    open.erase(it);
                } else {
                    next[span] = TileRect{start, row, col - start, 1};
                }
            }
        }
        for (const auto& kv : open) done.push_back(kv.second);  // runs that did not continue
        open = std::move(next);
    }
    return done;
}

std::vector<std::vector<TilePoint>> SolidOutlines(const Tilemap& tm, const TileRules& rules) {
    int width = 0, height = 0;
    MapSize(tm, width, height);
    auto solid = [&](int c, int r) { return CollisionAt(tm, rules, c, r) == TileCollision::Solid; };
    // Directed boundary edges between integer corners (x = column, y = -row),
    // solid on the left.
    struct Edge {
        int x0, y0, x1, y1;
        bool used = false;
    };
    std::vector<Edge> edges;
    std::map<std::pair<int, int>, std::vector<size_t>> from;
    auto add = [&](int x0, int y0, int x1, int y1) {
        from[{x0, y0}].push_back(edges.size());
        edges.push_back({x0, y0, x1, y1});
    };
    for (int r = 0; r < height; ++r) {
        for (int c = 0; c < width; ++c) {
            if (!solid(c, r)) continue;
            if (!solid(c, r + 1)) add(c, -r - 1, c + 1, -r - 1);
            if (!solid(c + 1, r)) add(c + 1, -r - 1, c + 1, -r);
            if (!solid(c, r - 1)) add(c + 1, -r, c, -r);
            if (!solid(c - 1, r)) add(c, -r, c, -r - 1);
        }
    }
    std::vector<std::vector<TilePoint>> loops;
    for (size_t start = 0; start < edges.size(); ++start) {
        if (edges[start].used) continue;
        std::vector<std::pair<int, int>> pts;
        size_t cur = start;
        while (true) {
            Edge& e = edges[cur];
            e.used = true;
            pts.push_back({e.x0, e.y0});
            if (e.x1 == edges[start].x0 && e.y1 == edges[start].y0) break;
            // Next edge from the end corner; at a corner shared by two diagonal
            // cells prefer the left turn so each region keeps its own loop.
            int dx = e.x1 - e.x0, dy = e.y1 - e.y0;
            size_t best = SIZE_MAX;
            int bestRank = 99;
            for (size_t cand : from[{e.x1, e.y1}]) {
                const Edge& n = edges[cand];
                if (n.used) continue;
                int ndx = n.x1 - n.x0, ndy = n.y1 - n.y0;
                int rank = (ndx == -dy && ndy == dx) ? 0 : (ndx == dx && ndy == dy) ? 1 : 2;
                if (rank < bestRank) bestRank = rank, best = cand;
            }
            if (best == SIZE_MAX) break;  // open chain (cannot happen on a grid)
            cur = best;
        }
        // Drop corners where the direction does not change.
        std::vector<TilePoint> loop;
        size_t n = pts.size();
        for (size_t i = 0; i < n; ++i) {
            const auto& a = pts[(i + n - 1) % n];
            const auto& b = pts[i];
            const auto& c = pts[(i + 1) % n];
            long cross = static_cast<long>(b.first - a.first) * (c.second - b.second) - static_cast<long>(b.second - a.second) * (c.first - b.first);
            if (cross != 0) loop.push_back({static_cast<float>(b.first), static_cast<float>(b.second)});
        }
        if (loop.size() >= 4) loops.push_back(std::move(loop));
    }
    return loops;
}

std::vector<TileRect> OneWayRuns(const Tilemap& tm, const TileRules& rules) {
    std::vector<TileRect> out;
    int width = 0, height = 0;
    MapSize(tm, width, height);
    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width;) {
            if (CollisionAt(tm, rules, col, row) != TileCollision::OneWay) {
                ++col;
                continue;
            }
            int start = col;
            while (col < width && CollisionAt(tm, rules, col, row) == TileCollision::OneWay) ++col;
            out.push_back({start, row, col - start, 1});
        }
    }
    return out;
}

std::vector<TileShape> ShapedCells(const Tilemap& tm, const TileRules& rules) {
    std::vector<TileShape> out;
    int width = 0, height = 0;
    MapSize(tm, width, height);
    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
            const TileDef* d = RuleAt(tm, rules, col, row);
            if (!d || d->collision != TileCollision::Shape) continue;
            TileShape s{col, row, {}};
            for (const TilePoint& p : d->shape) s.points.push_back({static_cast<float>(col) + p.x, -static_cast<float>(row + 1) + p.y});
            out.push_back(std::move(s));
        }
    }
    return out;
}

void WorldToCell(const Scene& scene, EntityId id, const Vec3& world, int& col, int& row) {
    const Tilemap* tm = scene.Get<Tilemap>(id);
    float ts = tm ? std::max(0.001f, tm->tileSize) : 1.0f;
    Vec3 local = scene.WorldMatrix(id).Inverse().TransformPoint(world);
    col = static_cast<int>(std::floor(local.x / ts));
    row = static_cast<int>(std::floor(-local.y / ts));
}

Vec3 CellToWorld(const Scene& scene, EntityId id, int col, int row) {
    const Tilemap* tm = scene.Get<Tilemap>(id);
    float ts = tm ? std::max(0.001f, tm->tileSize) : 1.0f;
    return scene.WorldMatrix(id).TransformPoint(Vec3((static_cast<float>(col) + 0.5f) * ts, -(static_cast<float>(row) + 0.5f) * ts, 0.0f));
}

}  // namespace oe
