#pragma once
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "scene/Scene.h"
#include "scene/AnimationEvents.h"

namespace oe {

class AssetManager;
struct Mesh;
struct AnimationClip;

// Exact named clip lookup; nullptr means the model's default pose.
const AnimationClip* FindAnimationClip(const Mesh& mesh, const std::string& name);
// Model-space matrix of every node of an immutable model with the clip sampled at `time`
// (clamped to each channel's keys); nullptr gives the default pose. One entry per Mesh::nodes.
std::vector<Mat4> EvaluateNodePose(const Mesh& mesh, const AnimationClip* clip, float time);
// The same for a pose between two clips: `weight` 0 = `from` at `fromTime`, 1 = `clip` at `time`
// (each node's translation, rotation and scale are blended before the hierarchy is applied).
std::vector<Mat4> EvaluateNodePose(const Mesh& mesh, const AnimationClip* clip, float time,
                                   const AnimationClip* from, float fromTime, float weight);
// Joint palette (node matrix * inverse bind) for node matrices from EvaluateNodePose or scene/Pose.h.
std::vector<Mat4> JointPalette(const Mesh& mesh, const std::vector<Mat4>& nodes);
// Evaluates an immutable model's joint palette in model space, clamping channel times.
std::vector<Mat4> EvaluateAnimationPose(const Mesh& mesh, const AnimationClip* clip, float time);
struct Particle;
struct ParticleEmitter;
class PhysicsWorld;
struct ParticleCollisionHit { bool hit = false; Vec3 point, normal; };
using ParticleCollisionQuery = std::function<ParticleCollisionHit(const Vec3& from, const Vec3& to, bool planar)>;
// Rejects non-finite birth settings before they enter simulation/rendering.
bool ParticleSettingsValid(const ParticleEmitter& emitter);

// Emits up to the configured capacity immediately; returns accepted births.
// True once a non-looping emitter has emitted everything it will and its particles are gone
// (effect entities can be destroyed). Looping emitters never finish.
bool ParticlesFinished(const ParticleEmitter& emitter);
// Shared fixed-step/birth implementation; seedMix zero preserves the plain component's exact stream.
void StepEmitter(ParticleEmitter& emitter, EntityId owner, const Mat4& world, uint32_t seedMix, float dt, std::vector<Particle>* deaths = nullptr,
                 const ParticleCollisionQuery* collisionQuery = nullptr);
int BurstEmitter(ParticleEmitter& emitter, EntityId owner, const Mat4& world, uint32_t seedMix, int count);

// Keys of a ParticleEmitter sizeOverLife/colorOverLife/opacityOverLife field, ascending in
// t (0..1). Scalars are in x; colors in xyz.
struct ParticleCurve {
    static constexpr size_t kMaxKeys = 16;
    std::vector<float> times;
    std::vector<Vec3> values;
    bool Empty() const { return times.empty(); }
    // Linear between keys; the first/last key holds outside their times.
    Vec3 Sample(float t) const;
};
// Parses [[t, value], ...]; `color` takes [r, g, b] or "#rrggbb" values, otherwise a number.
// An empty array gives an empty curve; malformed keys return false with a message.
bool ParseParticleCurve(const Json& keys, bool color, ParticleCurve& out, std::string* error);
// Strict XYZ keys, including signed motion/rotation values; no color-string syntax.
bool ParseParticleVectorCurve(const Json& keys, ParticleCurve& out, std::string* error);


struct Trail;
// Forgets a Trail's path (teleports, respawns).
void ClearTrail(Trail& trail);
// Shared trail aging/sampling at an explicit world-space head.
void StepTrail(Trail& trail, const Vec3& position, float dt);
// Resolve ribbon anchor names/offsets; missing names return false rather than drawing at the origin.
bool TrailAnchors(const Scene& scene, const Trail& trail, const Mat4& owner, Vec3& base, Vec3& tip);
// Step a camera trail or two-anchor ribbon with a shared finite emission window.
void StepAnchoredTrail(Trail& trail, const Vec3& base, const Vec3& tip, float dt);
bool TrailEmitting(const Trail& trail);

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
    // Mouse wheel notches since the last simulation step (> 0 = away from the user); cleared like mouseDX.
    float wheel = 0.0f;
    // Set by the game (input.lockMouse): hide the cursor and keep it in the
    // view so only relative motion matters. Platforms clear it when the player
    // presses Escape or the window loses focus.
    bool mouseLocked = false;
    // Look direction a local script accumulates from relative mouse motion
    // (input.setLook), each in [-1,1] on the int16 grid of a network axis. It is
    // device state: no checkpoint restores it, and a match carries it in the input
    // stream when the project declares the LookX/LookY axes.
    float lookX = 0.0f;
    float lookY = 0.0f;
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
    // Characters typed since the last simulation step (UTF-8, no control characters; IME results
    // included), read by a focused UIInput and cleared like pressedThisFrame. The API adds to it with input.text.
    std::string text;
    // What an input method (Korean, Japanese, Chinese) is composing now: not typed yet, shown at the
    // caret of the focused UIInput. Device state like the mouse position: it never enters a match.
    std::string composing;
    void AddText(uint32_t codepoint);
};

// Advances all behavior components by dt seconds (before physics).
void UpdateSystems(Scene& scene, InputState& input, float dt, AssetManager* assets = nullptr, PhysicsWorld* physics = nullptr,
                   std::vector<AnimationEvent>* animationEvents = nullptr);
// Runs after physics: SpringBone and BoneAttachment (need `assets`), CameraFollow, Trail paths (so they follow physics bodies too).
void UpdateLateSystems(Scene& scene, float dt, AssetManager* assets = nullptr);

}  // namespace oe
