#pragma once
#include <set>
#include <string>

#include "scene/Scene.h"

namespace oe {

// Logical key state. Key names: "W","A","S","D","Up","Down","Left","Right",
// "Space","Shift","Escape", letters "A".."Z", digits "0".."9".
// Platforms feed it from real devices; the API can inject keys so agents can
// play-test without a window.
struct InputState {
    std::set<std::string> down;
    bool IsDown(const std::string& key) const { return down.count(key) > 0; }
    std::set<std::string> pressedThisFrame;  // edge-triggered, cleared each tick
};

// Advances all behavior components by dt seconds.
void UpdateSystems(Scene& scene, InputState& input, float dt);

}  // namespace oe
