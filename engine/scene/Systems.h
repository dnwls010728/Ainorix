#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

#include "scene/Scene.h"

namespace oe {

class AssetManager;
struct Mesh;
struct AnimationClip;

// Exact named clip lookup; nullptr means the model's default pose.
const AnimationClip* FindAnimationClip(const Mesh& mesh, const std::string& name);
// Evaluates an immutable model's joint palette in model space, clamping channel times.
std::vector<Mat4> EvaluateAnimationPose(const Mesh& mesh, const AnimationClip* clip, float time);
struct ParticleEmitter;
// Rejects non-finite birth settings before they enter simulation/rendering.
bool ParticleSettingsValid(const ParticleEmitter& emitter);

// Emits up to the configured capacity immediately; returns accepted births.
int BurstParticles(Scene& scene, EntityId id, int count);

// Logical key state. Key names: "W","A","S","D","Up","Down","Left","Right",
// "Space","Shift","Escape", letters "A".."Z", digits "0".."9".
// Platforms feed it from real devices; the API can inject keys so agents can
// play-test without a window.
struct InputState {
    static constexpr float kGamepadDeadZone = 0.15f;
    inline static constexpr const char* kAxisNames[] = {"LeftX", "LeftY", "RightX", "RightY", "LT", "RT"};
    std::map<std::string, float> axes;  // raw sticks [-1,1], triggers [0,1]; +X right, +Y up
    // Updates a known raw axis, clamped to its device range; false for unknown/non-finite input.
    bool SetAxis(const std::string& name, float value);
    // Reads a scalar dead zone, rescaled to full range; absent/unknown axes return zero.
    float Axis(const std::string& name) const;
    std::set<std::string> down;
    bool IsDown(const std::string& key) const { return down.count(key) > 0; }
    std::set<std::string> pressedThisFrame;  // edge-triggered, cleared each tick
    // Mouse/touch: position normalized to the game view (0..1, origin top-left)
    // plus the view size in pixels so UI hit tests match what was rendered.
    // Buttons use MouseLeft/MouseRight or GamepadA/B/X/Y/LB/RB/Start/Back,
    // GamepadLeftStick/RightStick and GamepadDPadUp/Down/Left/Right.
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
    // Touch screens: every finger on the view, in the order they went down.
    // Positions are normalized like the mouse; `began` is set on the step the
    // finger went down. Platforms also report the first finger as the mouse
    // (MouseLeft), so taps work like clicks. The API injects them with input.touch.
    struct Touch {
        int id = 0;
        float x = 0.5f;
        float y = 0.5f;
        bool began = false;
    };
    std::vector<Touch> touches;
};

// Advances all behavior components by dt seconds (before physics).
void UpdateSystems(Scene& scene, InputState& input, float dt, AssetManager* assets = nullptr);
// Runs after physics: CameraFollow.
void UpdateLateSystems(Scene& scene, float dt);

}  // namespace oe
