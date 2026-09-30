#pragma once
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/Json.h"
#include "core/Math.h"
#include "scene/Components.h"

namespace oe {

class Scene;

// Helpers for the Tilemap component, shared by physics, rendering, the editor
// and the Lua `tilemap` API. Cells are (column, row) with row 0 at the top;
// the entity origin is the top-left corner of cell (0, 0).

// ----- Tile rules ------------------------------------------------------------
// What each map character draws and how it collides. Rules come from a
// tileset file (Tilemap.tileset = "x.tileset.json", its "tiles" object) and
// from the component's own legend + solid, which override the file.
//
// A rule is a frame number or an object:
//   {"frame": 3}                           fixed frame
//   {"variants": [3, 4, 5]}                one of them per cell (stable pseudo-random)
//   {"autotile": "sides", "frame": 16}     16 frames from 16: picked by the 4 neighbours (N=1 E=2 S=4 W=8)
//   {"autotile": "blob", "frame": 32}      47 frames from 32: 8 neighbours, corners only count next to two sides
//   {"autotile": ..., "frames": [...]}     explicit frame list (16 or 47) instead of a first frame
//   "connects": "=%"                       other characters that count as the same terrain
//   "edges": false                         cells outside the map do not connect (default: they do)
//   "collision": "none" | "solid" | "oneway" | "slope-up" | "slope-down" | "half-bottom" | "half-top" | [[x,y], ...]
// Polygons are convex, in tile units with (0,0) the cell's bottom-left and (1,1) its top-right.

enum class TileCollision { None, Solid, OneWay, Shape };
enum class AutoTile { None, Sides, Blob };

struct TilePoint {
    float x = 0, y = 0;
};

struct TileDef {
    int frame = -1;  // -1 = draws nothing
    std::vector<int> variants;
    AutoTile autotile = AutoTile::None;
    std::vector<int> autoFrames;  // 16 (sides) or 47 (blob) frames, one per neighbour pattern
    std::string connects;         // extra characters of the same terrain
    bool edges = true;
    TileCollision collision = TileCollision::None;
    std::vector<TilePoint> shape;  // TileCollision::Shape: convex polygon (counter-clockwise)
};

struct TileRules {
    std::string image;  // tileset image ("" = untextured)
    int columns = 1, rows = 1;
    std::map<char, TileDef> tiles;
    std::string key;  // changes whenever the rules change (cache key)
    std::string error;  // tileset file problems (the component still works)
};

// A parsed *.tileset.json: {"image", "columns", "rows", "tileSize"?, "tiles": {char: rule}}.
struct Tileset {
    std::string image;
    int columns = 1, rows = 1;
    std::map<char, TileDef> tiles;
    std::string key;
};

bool IsTilesetFile(const std::string& path);
bool ParseTileset(const Json& j, Tileset& out, std::string* error);
// One rule (number or object) for character `c`. Collision stays None unless given.
bool ParseTileRule(const Json& v, TileDef& out, std::string* error);

// Looks up a tileset file by project-relative path (nullptr + error if it cannot be loaded).
using TilesetLookup = std::function<std::shared_ptr<const Tileset>(const std::string& path, std::string* error)>;
TileRules BuildTileRules(const Tilemap& tm, const TilesetLookup* lookup);

// Number of blob patterns (47) and the pattern index of an 8-neighbour mask
// (N=1 NE=2 E=4 SE=8 S=16 SW=32 W=64 NW=128; corners without both sides are dropped).
constexpr int kBlobTileCount = 47;
int BlobIndex(int mask);
// The 47 masks in frame order (ascending), for documentation and tools.
const std::vector<int>& BlobMasks();

// Frame per cell, row-major (width = widest row); -1 = empty.
struct TileFrames {
    int width = 0, height = 0;
    std::vector<int> frames;
};
TileFrames ResolveFrames(const Tilemap& tm, const TileRules& rules);

// ----- Cells -------------------------------------------------------------------

// Character at a cell, or '\0' outside the map.
char TileAt(const Tilemap& tm, int col, int row);
// Replaces one character (the row grows with spaces when needed). False when row < 0 or col < 0.
bool SetTile(Tilemap& tm, int col, int row, char c);
// Map size in cells: rows and the widest row.
void MapSize(const Tilemap& tm, int& width, int& height);
const TileDef* RuleAt(const Tilemap& tm, const TileRules& rules, int col, int row);
TileCollision CollisionAt(const Tilemap& tm, const TileRules& rules, int col, int row);

// ----- Collision geometry (tile units, x right, y up: cell (c, r) spans x c..c+1, y -r-1..-r) ----

// Axis-aligned block of cells.
struct TileRect {
    int col = 0, row = 0, width = 0, height = 0;
};
// Solid cells merged into as few rectangles as possible (rows first, then
// identical row spans stacked), deterministic for a given map.
std::vector<TileRect> SolidRects(const Tilemap& tm, const TileRules& rules);
// Outlines of the solid regions: closed loops with the solid on the left
// (outer boundaries counter-clockwise, holes clockwise), collinear points removed.
std::vector<std::vector<TilePoint>> SolidOutlines(const Tilemap& tm, const TileRules& rules);
// One-way cells merged along rows (height is always 1): the top edge is the platform.
std::vector<TileRect> OneWayRuns(const Tilemap& tm, const TileRules& rules);
// Cells with a polygon: the polygon in tile units (already offset to the cell).
struct TileShape {
    int col = 0, row = 0;
    std::vector<TilePoint> points;
};
std::vector<TileShape> ShapedCells(const Tilemap& tm, const TileRules& rules);

// World point -> cell of the tilemap entity (uses its world transform).
void WorldToCell(const Scene& scene, EntityId id, const Vec3& world, int& col, int& row);
// Center of a cell in world space.
Vec3 CellToWorld(const Scene& scene, EntityId id, int col, int row);

}  // namespace oe
