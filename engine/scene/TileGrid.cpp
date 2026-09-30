#include "scene/TileGrid.h"

#include <cmath>
#include <map>
#include <string>

#include "scene/Scene.h"

namespace oe {

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

bool IsSolid(const Tilemap& tm, char c) { return c != '\0' && tm.solid.find(c) != std::string::npos; }

std::vector<TileRect> SolidRects(const Tilemap& tm) {
    std::vector<TileRect> done;
    // Open rectangles keyed by their column span; extended while the next row
    // has exactly the same run.
    std::map<std::pair<int, int>, TileRect> open;
    int rows = tm.map.isArray() ? static_cast<int>(tm.map.size()) : 0;
    for (int row = 0; row <= rows; ++row) {
        std::map<std::pair<int, int>, TileRect> next;
        if (row < rows) {
            const std::string& line = tm.map[row].asString();
            for (int col = 0; col < static_cast<int>(line.size());) {
                if (!IsSolid(tm, line[static_cast<size_t>(col)])) {
                    ++col;
                    continue;
                }
                int start = col;
                while (col < static_cast<int>(line.size()) && IsSolid(tm, line[static_cast<size_t>(col)])) ++col;
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
