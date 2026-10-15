#pragma once
#include "core/Json.h"

namespace oe {

class Engine;

// Clips written from a recipe and a few numbers (docs/SKELETON2D.md, "Motion recipes"): idle, walk, run, jump,
// attack, hit, death, for a humanoid seen from the front or from the side (facing right). Bones are found by role
// (the names skeleton2d.draft_template gives: hip, chest, head, upper_arm_l, thigh_front, ...; `bones` maps roles to
// other names) and turned by direction - which way is forward, up or outward is read from each bone's setup pose -
// so a recipe fits any rig drawn that way. The result is an ordinary clip: tune it with skeleton2d.set, check it
// with skeleton2d.analyze.
Json Skeleton2DMotion(Engine& engine, const Json& args);

}  // namespace oe
