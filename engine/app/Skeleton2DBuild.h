#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/Json.h"

namespace oe {

class Engine;
struct Image;

// A grid mesh over an image: every cell (`cell` pixels; <= 0 picks about a tenth of the image) holding a pixel with
// alpha above `alphaThreshold` becomes two triangles; cells grow until there are at most `maxVertices` vertices.
// Fills `vertices` with image pixels (x0, y0, x1, y1, ...) and returns the cell size used.
int Skeleton2DGridMesh(const Image& image, int cell, int alphaThreshold, int maxVertices, std::vector<int>& vertices, std::vector<uint32_t>& triangles);

// From a rig draft to a skeleton asset (docs/SKELETON2D.md, "Building from a picture"). A draft names joints as
// pixel points on one illustration, bones between joints, and parts: regions of the illustration (polygon,
// ellipse or rectangle) or separate images, each following one bone (rigid) or several (a weighted mesh).
// The build cuts each part into its own image, covers it with a grid mesh inside its opaque pixels, weights the
// vertices by their distance to the part's bones, places everything so the setup pose is the illustration, and
// reports coverage (opaque pixels no part took, pixels two parts share) with a picture of the draft.
//
// `out` is the *.skel2d.json to write; part images go to `partsDir` (default "<out without .skel2d.json>-parts/").
// Clips of an existing `out` are kept (rebuilding after moving a joint keeps the animation). `dryRun` writes only
// the draft picture. Throws ApiError with a hint for caller mistakes.
Json BuildSkeleton2D(Engine& engine, const Json& draft, const std::string& out, const std::string& partsDir, bool dryRun,
                     const std::string& overlayOut);

// A starting draft for a kind of character ("humanoid-front", "humanoid-side"): its joints (with a note on where
// each goes), bones and parts in draw order. With an illustration, joints and part regions are first guesses from
// the bounds of its opaque pixels and usual body proportions, for the agent to correct with image.grid.
// `joints` (optional) places some or all joints; the part regions are then fitted to them: limb widths are measured
// across the bones in the opaque pixels of the image.
Json Skeleton2DDraftTemplate(Engine& engine, const std::string& kind, const std::string& image, const Json& joints);

}  // namespace oe
