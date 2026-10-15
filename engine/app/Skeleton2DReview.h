#pragma once
#include "core/Json.h"

namespace oe {

class Engine;

// Review of a 2D skeleton in motion (docs/SKELETON2D.md, "Reviewing a rig"). Both take the skeleton from `path` or
// an entity `id` and a set of poses: a clip sampled at `times` or `samples` evenly spaced times, every clip
// (`clip: "*"`), or - with no clip - stress poses that bend each joint by +-`angle` degrees one at a time.
// Poses are drawn by a small rasterizer that knows, for every pixel, which slot shows it and from which texel.

// Measures what goes wrong in those poses and returns problems with hints:
//  revealed  - picture that was hidden under another part in the setup pose shows (cut edges, a copy of the other
//              part); `masks: true` writes the revealed texels of each slot as a PNG mask (white = revealed);
//  gap       - the figure opens around a joint (less opaque than in the setup pose);
//  flipped   - mesh triangles fold over; stretched - triangles stretch or squash beyond 1.6x / 0.5x;
//  seam      - a looping clip ends in a different pose than it starts.
// `render` (default true) writes a picture of the worst poses with the problems marked.
Json AnalyzeSkeleton2D(Engine& engine, const Json& args);

// A contact sheet of the poses (`columns`, `cell` pixels), each labeled; `focus: "bone"` zooms every cell
// around that bone's joint (`zoom`, default 3).
Json Skeleton2DContactSheet(Engine& engine, const Json& args);

}  // namespace oe
