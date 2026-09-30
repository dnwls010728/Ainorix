#pragma once
#include <vector>

#include "core/Math.h"
#include "scene/Components.h"

namespace oe {

class Scene;

// Helpers for the Tilemap component, shared by physics, rendering overlays and
// the Lua `tilemap` API. Cells are (column, row) with row 0 at the top; the
// entity origin is the top-left corner of cell (0, 0).

// Axis-aligned block of solid cells (in cells).
struct TileRect {
    int col = 0, row = 0, width = 0, height = 0;
};

// Character at a cell, or '\0' outside the map.
char TileAt(const Tilemap& tm, int col, int row);
// Replaces one character (the row grows with spaces when needed). False when row < 0 or col < 0.
bool SetTile(Tilemap& tm, int col, int row, char c);
bool IsSolid(const Tilemap& tm, char c);
// Solid cells merged into as few rectangles as possible (rows first, then
// identical row spans stacked), deterministic for a given map.
std::vector<TileRect> SolidRects(const Tilemap& tm);
// World point -> cell of the tilemap entity (uses its world transform).
void WorldToCell(const Scene& scene, EntityId id, const Vec3& world, int& col, int& row);
// Center of a cell in world space.
Vec3 CellToWorld(const Scene& scene, EntityId id, int col, int row);

}  // namespace oe
