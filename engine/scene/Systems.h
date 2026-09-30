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
    // Mouse/touch: position normalized to the game view (0..1, origin top-left)
    // plus the view size in pixels so UI hit tests match what was rendered.
    // Buttons use the key names "MouseLeft" / "MouseRight".
    float mouseX = 0.5f;
    float mouseY = 0.5f;
    int viewWidth = 1280;
    int viewHeight = 720;
    // Relative mouse motion in pixels since the last simulation step (mouse
    // look). Platforms accumulate it; it is cleared after every step.
    float mouseDX = 0.0f;
    float mouseDY = 0.0f;
    // Set by the game (input.lockMouse): hide the cursor and keep it in the
    // view so only relative motion matters. Platforms clear it when the player
    // presses Escape or the window loses focus.
    bool mouseLocked = false;
};

// Advances all behavior components by dt seconds (before physics).
void UpdateSystems(Scene& scene, InputState& input, float dt);
// Runs after physics: CameraFollow.
void UpdateLateSystems(Scene& scene, float dt);

}  // namespace oe
