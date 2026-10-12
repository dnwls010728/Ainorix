#pragma once
#include <map>
#include <string>
#include <vector>

#include "core/Json.h"
#include "render/Mesh.h"

namespace oe {

// Moves animation clips from one skeleton to another with different bone names, proportions and
// rest pose (mocap made for a T-pose rig onto a character modelled in an A-pose).
struct RetargetOptions {
    std::map<std::string, std::string> bones;  // target bone -> source bone
    std::string hips;                          // target bone that also takes the source's movement (bob, sway)
    std::vector<std::string> ends;             // target bones that are not laid along their source bone (feet, toes)
    std::map<std::string, std::string> clips;  // new clip name -> source clip; empty = every clip under its own name
    float rate = 30.0f;                        // keys per second
};

// Writes a glTF document (JSON with an embedded buffer) that holds one node per mapped target bone
// and the retargeted clips as linear keys: a clip file for the target's "<model>.clips.json".
// `report` receives {clips, bones, yaw, scale, missing}. False with `error` when nothing can be mapped.
bool RetargetClips(const Mesh& source, const Mesh& target, const RetargetOptions& options, Json& gltf, Json& report, std::string* error);

// A bone map for two humanoid rigs from their bone names (Mixamo, 3ds Max Biped, Unreal, Unity/VRM style
// names): {bones: {target: source}, hips, ends: [feet and toes], unmatched: [body parts found in only one
// of the two or in neither]}. A starting point for RetargetOptions; the caller checks and edits it.
Json GuessRetargetMap(const Mesh& source, const Mesh& target);

}  // namespace oe
