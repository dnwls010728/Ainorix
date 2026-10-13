#pragma once
#include <memory>
#include <string>
#include <vector>

#include "scene/Reflect.h"

namespace oe {
struct VfxAsset;
struct VfxExpressionSet;

// Built-in components. To add one: declare a struct with kTypeName, kDoc and
// Reflect(), then register it in RegisterBuiltinComponents(). Serialization,
// the API, schemas and the editor inspector pick it up automatically.

struct Transform {
    static constexpr const char* kTypeName = "Transform";
    static constexpr const char* kDoc = "Position, rotation (Euler degrees, applied X then Y then Z) and scale relative to the parent entity.";
    Vec3 position{0, 0, 0};
    Vec3 rotation{0, 0, 0};
    Vec3 scale{1, 1, 1};
    static void Reflect(FieldList& f) {
        f.Add("position", &Transform::position, "Local position in meters. +Y is up.");
        f.Add("rotation", &Transform::rotation, "Local rotation as Euler angles in degrees.");
        f.Add("scale", &Transform::scale, "Local scale factors.");
    }
};

struct MeshRenderer {
    static constexpr const char* kTypeName = "MeshRenderer";
    static constexpr const char* kDoc = "Draws a mesh: a built-in shape (cube, sphere, plane, plane64, pyramid, quad) or a glTF model file. Unknown meshes render as a magenta cube.";
    std::string mesh = "cube";
    std::string material;
    Json materials = Json::MakeArray();
    Json blendShapes = Json::MakeObject();
    Json shaderUniforms = Json::MakeObject();
    Color color{0.8f, 0.8f, 0.8f};
    float opacity = 1.0f;
    std::string texture;
    std::string shading = "smooth";
    bool unlit = false;
    bool castShadows = true;
    bool visible = true;
    bool parentPose = false;
    static void Reflect(FieldList& f) {
        f.Add("mesh", &MeshRenderer::mesh, "Built-in name (cube, sphere, plane, plane64 = plane split into 64x64 cells for shader graph vertex offsets, pyramid, quad) or model path, e.g. \"assets/models/fox.glb\" (.glb/.gltf/.fbx).");
        f.Add("material", &MeshRenderer::material,
              "Material file (*.mat.json: PBR color/metallic/roughness/normal/emissive maps, transparency) used for every part of the mesh. "
              "Empty = the model's own materials (glTF) or the default (white, roughness 0.7).");
        f.Add("materials", &MeshRenderer::materials,
              "Material files per material of a model, in the order asset.info lists its `materials` (their `name` says which part): "
              "[\"materials/face.mat.json\", \"\", ...]. An empty or missing entry keeps `material`, or the model's own when that is empty.");
        f.Add("blendShapes", &MeshRenderer::blendShapes,
              "Weights of the model's blend shapes (morph targets), {name: weight}: {\"E_Close\": 1} closes the eyes, 0.5 half way; several add up. "
              "asset.info lists a model's `blendShapes`; unknown names are ignored. Scripts animate faces by setting this every frame.");
        f.Add("shaderUniforms", &MeshRenderer::shaderUniforms,
              "Per-entity values for the material's shader graph uniforms: {name: number or [x, y, z, w]}. Unnamed uniforms keep the "
              "material's values; scripts can change them every frame. An unknown name or a material without a graph renders magenta.");
        f.Add("color", &MeshRenderer::color, "Tint multiplied with the material/texture color, linear RGB 0..1 (or \"#rrggbb\").");
        FieldInfo& op = f.Add("opacity", &MeshRenderer::opacity, "Below 1 the mesh is drawn transparent (alpha blended, sorted back to front, no shadow).");
        op.hasRange = true;
        op.min = 0.0f;
        op.max = 1.0f;
        f.Add("texture", &MeshRenderer::texture, "Image (.png/.jpg) overriding the material's base color texture. Empty = use the material's.");
        f.Add("shading", &MeshRenderer::shading, "smooth = interpolated vertex normals, flat = faceted.").options = {"smooth", "flat"};
        f.Add("unlit", &MeshRenderer::unlit, "Ignore lighting and shadows (UI-like, emissive look). Overrides the material.");
        f.Add("castShadows", &MeshRenderer::castShadows, "Casts shadows from the directional light.");
        f.Add("visible", &MeshRenderer::visible, "Whether the mesh is drawn.");
        f.Add("parentPose", &MeshRenderer::parentPose, "Draw this model in the pose of the parent entity, which must show the same model: a second layer of a character (an outline shell, a glow) follows its Animator, BoneIK goals and SpringBones.");
    }
};

struct Animator {
    static constexpr const char* kTypeName = "Animator";
    static constexpr const char* kDoc = "Fixed-step glTF TRS animation on the entity's MeshRenderer; clips are listed by asset.info.";
    std::string clip;
    float speed = 1.0f;
    bool loop = true;
    bool playing = true;
    float time = 0.0f;
    float blend = 0.0f;
    // Runtime, not serialized: the clip that is shown and where it was, and the clip it fades from.
    std::string shown, from;
    float shownTime = 0.0f, fromTime = 0.0f, fade = 0.0f;
    bool eventsStarted = false;  // native snapshots copy this; seeks do not traverse skipped markers
    static void Reflect(FieldList& f) {
        f.Add("clip", &Animator::clip, "Exact model clip name; empty = default pose. animation.play changes clips and restarts by default.");
        f.Add("speed", &Animator::speed, "Playback multiplier; negative plays backwards, zero holds the pose.");
        f.Add("loop", &Animator::loop, "Wrap time at the clip duration; otherwise clamp and stop at either endpoint.");
        f.Add("playing", &Animator::playing, "Advance time while true; false holds the current pose.");
        f.Add("time", &Animator::time, "Seconds into the clip; set to seek. Direct clip edits preserve time.");
        FieldInfo& blend = f.Add("blend", &Animator::blend, "Seconds a change of `clip` takes: the pose fades from the old clip (which keeps playing) to the new one instead of jumping. 0 = at once. 0.2 suits idle/walk/run.");
        blend.hasRange = true; blend.min = 0; blend.max = 5;
    }
};

struct BoneAttachment {
    static constexpr const char* kTypeName = "BoneAttachment";
    static constexpr const char* kDoc = "Holds this entity on a bone of its parent's model (weapon in a hand, hat on a head): after physics each frame its Transform position and rotation are set from the posed bone, IK included. Bone names: animation.bones.";
    std::string bone;
    Vec3 position;
    Vec3 rotation;
    static void Reflect(FieldList& f) {
        f.Add("bone", &BoneAttachment::bone, "Exact node name in the parent's MeshRenderer model; an unknown name leaves the Transform alone.");
        f.Add("position", &BoneAttachment::position, "Offset from the bone in the bone's own axes, in model units.");
        f.Add("rotation", &BoneAttachment::rotation, "Rotation offset from the bone (Euler degrees). Transform.scale stays yours.");
    }
};

struct BoneIK {
    static constexpr const char* kTypeName = "BoneIK";
    static constexpr const char* kDoc = "Inverse kinematics goal for its parent's model: the bones above `bone` turn so that it reaches this entity's Transform position (move the entity to move the hand or foot). Applied after the Animator's clip in both renderers; goals run in entity id order.";
    std::string bone;
    int chain = 2;
    float weight = 1.0f;
    Vec3 pole;
    float soften = 0.0f;
    bool matchRotation = false;
    Vec3 turn;
    bool keepRotation = false;
    static void Reflect(FieldList& f) {
        f.Add("bone", &BoneIK::bone, "Exact name of the node that should reach the goal (hand, foot, head); see animation.bones.");
        f.Add("chain", &BoneIK::chain, "How many bones above it turn, 1..8. 2 = an arm or a leg (exact two-bone solve); 1 aims the parent bone; more bend a spine or tail step by step.");
        f.Add("weight", &BoneIK::weight, "0 = the animated pose, 1 = reach the goal; between blends the goal position.");
        f.Add("pole", &BoneIK::pole, "chain 2: direction in model space the elbow or knee points to, e.g. [0,0,1]; [0,0,0] keeps the side the limb bends to in the model's default pose.");
        f.Add("soften", &BoneIK::soften, "chain 2: share of the limb length (0..0.5) over which it eases into full extension, so a goal near or beyond reach does not snap the limb straight; the bone then stops slightly short of such goals. 0 = exact reach. Try 0.1 for feet.");
        f.Add("keepRotation", &BoneIK::keepRotation, "The bone keeps the orientation the clip gave it while the limb above it is solved; otherwise it turns with the limb. On for feet and hands whose goal only corrects a clip (foot placement): the ankle or wrist does not twist when the knee or elbow ends up elsewhere.");
        f.Add("turn", &BoneIK::turn, "Turns the bone by these Euler degrees around the model's axes on top of the orientation the pose gives it (scaled by weight): a foot tilted to the slope it stands on, from a script. Without matchRotation.");
        f.Add("matchRotation", &BoneIK::matchRotation, "Also turn the bone to this entity's Transform rotation (a foot flat on a slope, a palm on a wall).");
    }
};

struct SpringBone {
    static constexpr const char* kTypeName = "SpringBone";
    static constexpr const char* kDoc = "Secondary motion for bones of its parent's model (tail, ears, hair, a cape strip): `bone` and every bone below it lag behind and swing as the character moves, then settle back into the animated pose. Simulated after physics each fixed step while playing, on top of the Animator's clip and BoneIK; stopped scenes show the plain pose.";
    std::string bone;
    float stiffness = 100.0f;
    float damping = 10.0f;
    Vec3 gravity;
    float weight = 1.0f;
    float radius = 0.02f;
    float endLength = 0.0f;
    float limit = 0.0f;
    float air = 0.0f;
    bool collide = true;
    // Runtime, not serialized: where each swinging bone's end is and was one step ago and where the
    // pose last put it (world space), parents first, and the `bone` they were made for. Empty until
    // the first simulated step.
    std::vector<Vec3> tails, previous, rests;
    std::string simulated;
    Mat4 stepWorld;  // the model's world matrix at the step the tails belong to
    static void Reflect(FieldList& f) {
        auto range = [](FieldInfo& field, float lo, float hi) { field.hasRange = true; field.min = lo; field.max = hi; };
        f.Add("bone", &SpringBone::bone, "Exact name of the first bone that swings (tail root, ear base, first hair bone); the whole branch below it follows. A name ending in * takes every branch whose bones start with that text (\"Bone_Skirt*\": all strips of a skirt with one component). See animation.bones.");
        range(f.Add("stiffness", &SpringBone::stiffness, "Pull back to the animated pose. 20 = loose and slow (long hair, cloth), 100 = a tail, 400+ = short stiff parts (ears, antennae). 0 = hangs freely."), 0, 3600);
        range(f.Add("damping", &SpringBone::damping, "How fast the swing dies out, per second. 3 = long wobble, 10 = one soft overshoot, 25+ = follows late without swinging back. A clip that already moves these bones in a rhythm (a walk) gets exaggerated when this is low; raise it together with stiffness."), 0, 60);
        f.Add("gravity", &SpringBone::gravity, "World-space acceleration in m/s^2, e.g. [0,-9.8,0] so the chain sags; [0,0,0] keeps the authored pose at rest. Other directions act as steady wind.");
        range(f.Add("weight", &SpringBone::weight, "0 = the animated pose, 1 = the simulated swing; between blends them."), 0, 1);
        range(f.Add("radius", &SpringBone::radius, "Thickness of the swinging bones in meters when they meet a SpringCollider of the same model."), 0, 1);
        range(f.Add("limit", &SpringBone::limit, "Largest angle in degrees a bone may swing away from its animated direction; 0 = no limit. 25-40 keeps hair and cloth from standing out sideways when the character starts, stops or jumps. Colliders may push further."), 0, 180);
        range(f.Add("air", &SpringBone::air, "Share of the damping that works against the air instead of against the pose, 0..1. At 0 a damped bone moves with the character; above 0 it trails behind while the character moves and falls back when it stops: hair and cloth streaming in a run. Try 0.05-0.15; the trail grows with speed, damping and this value, and shrinks with stiffness."), 0, 1);
        f.Add("collide", &SpringBone::collide, "Whether the SpringColliders of the model stop these bones. Off for parts that hang clear of them (long hair behind legs that have colliders for the skirt).");
        range(f.Add("endLength", &SpringBone::endLength, "Meters of mesh that hang below the last bone of each branch (the hem of a skirt, hair tips). Above 0 the last bones swing and collide too, as if they had a bone that far along them; 0 = they stay rigid on the bone above."), 0, 5);
    }
};

struct SpringCollider {
    static constexpr const char* kTypeName = "SpringCollider";
    static constexpr const char* kDoc = "A body part that the SpringBones of its parent's model do not pass through: a sphere at `bone`, or a capsule from `bone` to `tail` (thigh to calf, calf to foot) so a raised leg lifts a skirt instead of cutting through it. A child of the model entity, like SpringBone. It moves with the pose (clip + BoneIK); it is not a physics collider.";
    std::string bone;
    std::string tail;
    float radius = 0.06f;
    Vec3 offset;
    // Runtime, not serialized: where the collider was one step ago (world space), for the direction it moves in.
    Vec3 previousA, previousB;
    bool tracked = false;
    static void Reflect(FieldList& f) {
        f.Add("bone", &SpringCollider::bone, "Exact name of the bone the collider sits on. See animation.bones.");
        f.Add("tail", &SpringCollider::tail, "Second bone: the collider is a capsule between the two. Empty = a sphere at `bone`.");
        FieldInfo& r = f.Add("radius", &SpringCollider::radius, "Radius in meters. Larger than the limb: the cloth between two swinging strips is not simulated.");
        r.hasRange = true; r.min = 0; r.max = 5;
        f.Add("offset", &SpringCollider::offset, "Moves the collider, in the model's own axes and meters (e.g. [0,0,0.03] = forward).");
    }
};

// Runtime particle values are snapshots of the emitter settings at birth.
enum class ParticleCollision { None, Stop, Bounce, Die };
struct Particle {
    Vec3 position, velocity, gravity;
    float age = 0, lifetime = 1;
    float drag = 0;
    float startSize = 0.1f, endSize = 0;
    Color startColor, endColor;
    float startOpacity = 1, endOpacity = 0;
    float rotation = 0, spin = 0;  // degrees around the view axis, degrees per second
    float sizeScale = 1;           // sizeVariation factor, applied to sizeOverLife keys
    int frameOffset = 0;           // randomFrame: first flipbook frame of this particle
    Vec3 birthPosition, initialVelocity, initialRotation, initialScale{1, 1, 1}; // captured birth inputs
    Vec3 integratedPosition; // physics position without the non-accumulating lifetime offset
    Vec3 rotation3D, axisScale{1, 1, 1}; // evaluated extra Euler rotation and scale multiplier
    Vec4 initialCustom{}, custom{}; // captured and evaluated material attributes
    Vec3 expressionBirthPosition, expressionBirthVelocity; // captured emitter coordinates for expressions
    Vec3 expressionColor; float expressionSize = 0, expressionOpacity = 0; // evaluated appearance overrides
    bool hasExpressionColor = false, hasExpressionSize = false, hasExpressionOpacity = false;
    Mat4 birthBasis; // emitter frame captured for world-space motion curves
    uint64_t index = 0; // accepted birth order, independent of channel evaluation
    int birthCount = 0; // accepted particles in this birth batch, immutable
    Mat4 meshBasis;    // world-space mesh orientation captured at birth
    float random = 0;  // stable birth hash, no extra random draws
    int depth = 0;     // bounded death-chain generation
    bool worldSpace = false;
    ParticleCollision collision = ParticleCollision::None;
    float collisionBounce = 0.5f, collisionFriction = 0;
    Vec3 collisionPreviousWorld, collisionAnchor;
    bool collisionStopped = false, collision2D = false;
};

struct ParticleEmitter {
    static constexpr const char* kTypeName = "ParticleEmitter";
    static constexpr const char* kDoc = "Deterministic 2D/3D billboard particles; initial burst and continuous rate, bounded per emitter.";
    float rate = 10;
    int burst = 0;
    float lifetime = 1;
    float speed = 1;
    float spread = 30;
    float speedVariation = 0, lifetimeVariation = 0, sizeVariation = 0;
    float drag = 0;
    std::string collision = "none";  // Point sweep against static colliders, captured at birth.
    float collisionBounce = 0.5f, collisionFriction = 0;
    Vec3 direction{0, 1, 0};
    Vec3 gravity{0, -9.8f, 0};
    float startSize = 0.1f, endSize = 0;
    Color startColor{1, 1, 1}, endColor{1, 1, 1};
    float startOpacity = 1, endOpacity = 0;
    std::string texture;
    std::string material;  // optional .mat.json surface graph
    std::string mesh;      // empty = billboard; otherwise static project/built-in mesh
    Vec3 meshScale{1, 1, 1};
    Vec3 positionOffset{}, rotation3D{}, scale{1, 1, 1}, pivot{}; // birth offset/rotation/scale; unscaled geometry pivot
    float custom0 = 0, custom1 = 0, custom2 = 0, custom3 = 0; // birth material data
    Json positionOverLife = Json::MakeArray(), velocityOverLife = Json::MakeArray(); // vector keys, emitter coordinates
    Json rotationOverLife = Json::MakeArray(), scaleOverLife = Json::MakeArray(); // additive degrees / scale multipliers
    Json custom0OverLife = Json::MakeArray(), custom1OverLife = Json::MakeArray(); // scalar material data keys
    Json custom2OverLife = Json::MakeArray(), custom3OverLife = Json::MakeArray();
    Json expressions = Json::MakeObject(); // canonical birth/update expression source text
    Json expressionParams = Json::MakeObject(); // runtime parameter values, not serialized
    mutable std::shared_ptr<const VfxExpressionSet> expressionProgram; // immutable compiled cache, shared by snapshots
    bool expressionInSystem = false; // distinguishes zero-parameter linked effects from standalone emitters
    double expressionSystemTime = 0; // fixed-step parent system clock (standalone uses emitter elapsed)
    bool batching = false;  // opt-in atomic emitter draw; legacy per-particle sorting otherwise
    std::string onDeath;   // named target emitter in the same VFX asset
    int deathCount = 1;
    int frame = 0, columns = 1, rows = 1;
    std::string space = "local";
    int dimensions = 3;
    int seed = 1;
    int maxParticles = 256;
    std::string blend = "alpha";
    int layer = 0;
    bool loop = true;
    bool playing = true;
    float delay = 0, duration = 0;
    std::string shape = "point";
    Vec3 shapeSize{1, 1, 1};
    bool shapeEdge = false;
    float radialSpeed = 0;
    float rotation = 0, rotationVariation = 0;
    float spin = 0, spinVariation = 0;
    std::string align = "camera";
    float stretch = 0;
    int frames = 1;
    float frameRate = 0;
    bool randomFrame = false;
    Json sizeOverLife = Json::MakeArray();
    Json colorOverLife = Json::MakeArray();
    Json opacityOverLife = Json::MakeArray();
    float turbulence = 0, turbulenceScale = 1;
    bool pixelArt = true;
    std::vector<Particle> particles;  // runtime, not serialized
    uint32_t randomState = 0;        // runtime xorshift stream; zero = uninitialized
    double carry = 0;               // fractional rate births, bounded to [0,1)
    double elapsed = 0;             // seconds simulated while playing; drives delay/duration
    int64_t cycle = 0;              // duration cycles started (loop with a duration re-fires the burst)
    bool initialBurstEmitted = false;
    uint64_t emitted = 0;            // accepted births since creation/clear
    bool invalidSettingsReported = false;  // suppress repeated runtime error logs
    static void Reflect(FieldList& f) {
        auto range = [](FieldInfo& field, float lo, float hi) { field.hasRange = true; field.min = lo; field.max = hi; };
        range(f.Add("rate", &ParticleEmitter::rate, "Automatic particles per second while loop and playing are true."), 0, 10000);
        range(f.Add("burst", &ParticleEmitter::burst, "Automatic initial burst on the first playing step."), 0, 10000);
        range(f.Add("lifetime", &ParticleEmitter::lifetime, "Lifetime in seconds, captured at birth."), 0.001f, 600);
        range(f.Add("speed", &ParticleEmitter::speed, "Initial speed in emitter coordinates, captured at birth."), 0, 10000);
        range(f.Add("spread", &ParticleEmitter::spread, "Cone half-angle in degrees around direction; planar fan in 2D."), 0, 180);
        range(f.Add("speedVariation", &ParticleEmitter::speedVariation, "0..1: each particle starts between speed * (1 - variation) and speed."), 0, 1);
        range(f.Add("lifetimeVariation", &ParticleEmitter::lifetimeVariation, "0..1: each particle lives between lifetime * (1 - variation) and lifetime."), 0, 1);
        range(f.Add("sizeVariation", &ParticleEmitter::sizeVariation, "0..1: each particle's start and end size are scaled between (1 - variation) and 1."), 0, 1);
        range(f.Add("drag", &ParticleEmitter::drag, "Velocity lost per second (0 = none, 4 = slows to a stop quickly), captured at birth."), 0, 60);
        f.Add("collision", &ParticleEmitter::collision, "Particle center sweep against static non-trigger colliders: none, stop, bounce or die. Captured at birth; dimensions selects 3D or 2D physics.").options = {"none","stop","bounce","die"};
        range(f.Add("collisionBounce", &ParticleEmitter::collisionBounce, "Normal velocity retained after a bounce (0..1), captured at birth."), 0, 1);
        range(f.Add("collisionFriction", &ParticleEmitter::collisionFriction, "Tangential velocity lost on collision (0..1), captured at birth."), 0, 1);
        f.Add("direction", &ParticleEmitter::direction, "Emission direction; zero falls back to +Y. 2D uses XY only.");
        f.Add("gravity", &ParticleEmitter::gravity, "Acceleration in particle coordinates (world or local), captured at birth.");
        range(f.Add("startSize", &ParticleEmitter::startSize, "Billboard size in meters at birth."), 0, 10000);
        range(f.Add("endSize", &ParticleEmitter::endSize, "Billboard size at expiry."), 0, 10000);
        f.Add("startColor", &ParticleEmitter::startColor, "RGB tint at birth.");
        f.Add("endColor", &ParticleEmitter::endColor, "RGB tint at expiry.");
        range(f.Add("startOpacity", &ParticleEmitter::startOpacity, "Opacity at birth."), 0, 1);
        range(f.Add("endOpacity", &ParticleEmitter::endOpacity, "Opacity at expiry."), 0, 1);
        f.Add("material", &ParticleEmitter::material, "Optional project-relative .mat.json, including surface graphs.");
        f.Add("mesh", &ParticleEmitter::mesh, "Static project mesh or built-in name; empty uses camera-facing quads.");
        f.Add("meshScale", &ParticleEmitter::meshScale, "Mesh XYZ scale multiplied by lifetime size; orientation follows emitter and rotation.");
        f.Add("expressions", &ParticleEmitter::expressions, "Typed source text {birth:{channel:text},update:{channel:text}}; vfx.schema.expressions defines inputs, operations and budgets.");
        f.Add("positionOffset", &ParticleEmitter::positionOffset, "Birth offset in emitter coordinates; captured before local/world conversion.");
        f.Add("rotation3D", &ParticleEmitter::rotation3D, "Birth XYZ Euler rotation in degrees, added to lifetime rotation and legacy Z spin.");
        f.Add("scale", &ParticleEmitter::scale, "Nonnegative birth XYZ multiplier; multiplied once by scaleOverLife, scalar size and meshScale (meshes only).");
        f.Add("pivot", &ParticleEmitter::pivot, "Unscaled mesh/billboard geometry point fixed at the particle center under scale and rotation.");
        range(f.Add("custom0", &ParticleEmitter::custom0, "Birth material custom0; captured per particle, not a shader uniform."), -65504, 65504);
        range(f.Add("custom1", &ParticleEmitter::custom1, "Birth material custom1; captured per particle, not a shader uniform."), -65504, 65504);
        range(f.Add("custom2", &ParticleEmitter::custom2, "Birth material custom2; captured per particle, not a shader uniform."), -65504, 65504);
        range(f.Add("custom3", &ParticleEmitter::custom3, "Birth material custom3; captured per particle, not a shader uniform."), -65504, 65504);
        f.Add("batching", &ParticleEmitter::batching, "Combine particles into one emitter draw, sorted internally; transparent emitters sort as whole groups.");
        f.Add("onDeath", &ParticleEmitter::onDeath, "VFX asset only: named emitter receiving a burst at each expired particle position; maximum depth 4.");
        range(f.Add("deathCount", &ParticleEmitter::deathCount, "Particles per death event, subject to target and system budgets."), 0, 256);
        f.Add("texture", &ParticleEmitter::texture, "Optional texture/sprite sheet; empty uses solid quads.");
        range(f.Add("frame", &ParticleEmitter::frame, "Sprite sheet frame, row-major."), 0, 1000000);
        range(f.Add("columns", &ParticleEmitter::columns, "Sprite sheet columns."), 1, 4096);
        range(f.Add("rows", &ParticleEmitter::rows, "Sprite sheet rows."), 1, 4096);
        f.Add("space", &ParticleEmitter::space, "Local particles follow the emitter; world particles remain where spawned.").options = {"local", "world"};
        range(f.Add("dimensions", &ParticleEmitter::dimensions, "2 = XY fan/planar motion; 3 = cone in 3D."), 2, 3);
        f.Add("seed", &ParticleEmitter::seed, "Fixed seed mixed with entity id; changes take effect after particles.clear.");
        range(f.Add("maxParticles", &ParticleEmitter::maxParticles, "Maximum live particles per emitter; excess births are dropped."), 0, 10000);
        f.Add("blend", &ParticleEmitter::blend, "alpha = normal; add = particles brighten what is behind them (sparks, fire).").options = {"alpha", "add"};
        f.Add("layer", &ParticleEmitter::layer, "2D sorting layer, like Sprite.layer: particles draw in front of lower layers.");
        f.Add("loop", &ParticleEmitter::loop, "Enable continuous rate births; false emits only the initial burst.");
        f.Add("playing", &ParticleEmitter::playing, "Automatic emission switch; existing particles continue to age while false.");
        range(f.Add("delay", &ParticleEmitter::delay, "Seconds of playing before the initial burst and the rate start (layered effects: flash, then smoke)."), 0, 600);
        range(f.Add("duration", &ParticleEmitter::duration,
                    "0 = no limit. Otherwise the rate emits for this many seconds after the delay; with loop the cycle repeats and re-fires the burst."), 0, 600);
        f.Add("shape", &ParticleEmitter::shape,
              "Where particles are born, in emitter coordinates: point, sphere (radius shapeSize.x), box (half extents shapeSize) or "
              "circle (radius shapeSize.x; in XY for 2D, on the ground plane XZ for 3D).").options = {"point", "sphere", "box", "circle"};
        f.Add("shapeSize", &ParticleEmitter::shapeSize, "Sphere/circle radius in x, or box half extents, in meters.");
        f.Add("shapeEdge", &ParticleEmitter::shapeEdge, "Spawn only on the shape's surface or rim (rings, shells).");
        range(f.Add("radialSpeed", &ParticleEmitter::radialSpeed,
                    "Extra speed away from the emitter origin (negative = toward it), for particles born off the origin; captured at birth."), -10000, 10000);
        range(f.Add("rotation", &ParticleEmitter::rotation, "Billboard rotation in degrees at birth, counter-clockwise on screen."), -36000, 36000);
        range(f.Add("rotationVariation", &ParticleEmitter::rotationVariation, "Each particle starts within +/- this many degrees of rotation."), 0, 360);
        range(f.Add("spin", &ParticleEmitter::spin, "Rotation speed in degrees per second, captured at birth."), -36000, 36000);
        range(f.Add("spinVariation", &ParticleEmitter::spinVariation, "Each particle spins within +/- this many degrees per second of spin."), 0, 36000);
        f.Add("align", &ParticleEmitter::align,
              "camera = upright billboards; velocity = the billboard's up axis follows the direction of travel on screen (sparks, rain).").options = {"camera", "velocity"};
        range(f.Add("stretch", &ParticleEmitter::stretch, "align velocity only: seconds of travel added to the billboard's length (0.05 = streaks)."), 0, 100);
        range(f.Add("frames", &ParticleEmitter::frames, "Flipbook length: frames played from `frame` on, row-major. 1 = a single frame."), 1, 1000000);
        range(f.Add("frameRate", &ParticleEmitter::frameRate, "Flipbook frames per second, looping; 0 plays the frames once over the particle's lifetime."), 0, 240);
        f.Add("randomFrame", &ParticleEmitter::randomFrame, "Each particle starts at a random flipbook frame and keeps it when frameRate is 0 (a sheet of variants).");
        f.Add("sizeOverLife", &ParticleEmitter::sizeOverLife,
              "Size keys [[t, meters], ...] with t ascending in 0..1; replaces startSize/endSize when not empty (sizeVariation still scales it). At most 16 keys.");
        f.Add("colorOverLife", &ParticleEmitter::colorOverLife,
              "Color keys [[t, [r, g, b]] or [t, \"#rrggbb\"], ...]; replaces startColor/endColor when not empty. At most 16 keys.");
        f.Add("opacityOverLife", &ParticleEmitter::opacityOverLife,
              "Opacity keys [[t, 0..1], ...]; replaces startOpacity/endOpacity when not empty (fade in, hold, fade out). At most 16 keys.");
        f.Add("positionOverLife", &ParticleEmitter::positionOverLife, "Vector keys [[age,[x,y,z]],...], meters in emitter coordinates; additive offset on integrated motion, never accumulated.");
        f.Add("velocityOverLife", &ParticleEmitter::velocityOverLife, "Vector keys in emitter meters/second; replaces velocity each step before gravity/drag. Empty retains integrated velocity.");
        f.Add("rotationOverLife", &ParticleEmitter::rotationOverLife, "Vector keys in degrees; added to captured rotation3D and legacy Z rotation/spin.");
        f.Add("scaleOverLife", &ParticleEmitter::scaleOverLife, "Nonnegative XYZ multiplier keys; normalized age 0..1, up to 16 keys. Empty means [1,1,1].");
        f.Add("custom0OverLife", &ParticleEmitter::custom0OverLife, "Scalar keys for particleCustom0; replaces the birth value over normalized age, at most 16 keys.");
        f.Add("custom1OverLife", &ParticleEmitter::custom1OverLife, "Scalar keys for particleCustom1; replaces the birth value over normalized age, at most 16 keys.");
        f.Add("custom2OverLife", &ParticleEmitter::custom2OverLife, "Scalar keys for particleCustom2; replaces the birth value over normalized age, at most 16 keys.");
        f.Add("custom3OverLife", &ParticleEmitter::custom3OverLife, "Scalar keys for particleCustom3; replaces the birth value over normalized age, at most 16 keys.");
        range(f.Add("turbulence", &ParticleEmitter::turbulence, "Strength in m/s^2 of a fixed noise field that pushes particles around (smoke, embers)."), 0, 10000);
        range(f.Add("turbulenceScale", &ParticleEmitter::turbulenceScale, "Noise cells per meter: small = wide swirls, large = jitter."), 0.001f, 1000);
        f.Add("pixelArt", &ParticleEmitter::pixelArt, "Sharp nearest-neighbour texels; false filters the texture smoothly (soft glows, smoke).");
    }
};

// One sample of the path a Trail follows, in world coordinates.
struct TrailPoint {
    Vec3 position;
    float age = 0;
    Vec3 tip;  // second world-space anchor in ribbon mode
};

struct Trail {
    static constexpr const char* kTypeName = "Trail";
    static constexpr const char* kDoc = "Camera-facing ribbon left behind the entity as it moves (sword swings, projectiles, dashes); it narrows and fades along its length.";
    float lifetime = 0.5f;
    float minDistance = 0.05f;
    float width = 0.2f, endWidth = 0;
    Color color{1, 1, 1}, endColor{1, 1, 1};
    float opacity = 1, endOpacity = 0;
    std::string texture;
    std::string material;
    std::string mode = "camera";  // ribbon sweeps base to tip, independent of camera
    std::string baseAnchor, tipAnchor;  // optional unique entity names; empty uses owner
    Vec3 baseOffset, tipOffset{0, 1, 0};
    float duration = 0;  // finite emission window; zero keeps emitting
    double elapsed = 0;  // runtime emission clock
    std::string blend = "alpha";
    int layer = 0;
    int maxPoints = 64;
    bool emitting = true;
    std::vector<TrailPoint> points;  // runtime, oldest first; not serialized
    static void Reflect(FieldList& f) {
        auto range = [](FieldInfo& field, float lo, float hi) { field.hasRange = true; field.min = lo; field.max = hi; };
        range(f.Add("lifetime", &Trail::lifetime, "Seconds a point of the path stays; with the entity's speed it sets the ribbon's length."), 0.01f, 60);
        range(f.Add("minDistance", &Trail::minDistance, "Meters the entity moves before the path gets a new point (smaller = smoother curves)."), 0.001f, 100);
        range(f.Add("width", &Trail::width, "Ribbon width in meters at the entity."), 0, 1000);
        range(f.Add("endWidth", &Trail::endWidth, "Width at the oldest point."), 0, 1000);
        f.Add("color", &Trail::color, "RGB tint at the entity.");
        f.Add("endColor", &Trail::endColor, "RGB tint at the oldest point.");
        range(f.Add("opacity", &Trail::opacity, "Opacity at the entity."), 0, 1);
        range(f.Add("endOpacity", &Trail::endOpacity, "Opacity at the oldest point."), 0, 1);
        f.Add("material", &Trail::material, "Optional .mat.json surface material/graph.");
        f.Add("mode", &Trail::mode, "camera faces the view; ribbon sweeps between two world-space anchors.").options = {"camera", "ribbon"};
        f.Add("baseAnchor", &Trail::baseAnchor, "Unique scene entity name for blade base; empty uses owner/local block transform.");
        f.Add("tipAnchor", &Trail::tipAnchor, "Unique scene entity name for blade tip; empty uses owner/local block transform.");
        f.Add("baseOffset", &Trail::baseOffset, "Base offset in anchor coordinates.");
        f.Add("tipOffset", &Trail::tipOffset, "Tip offset in anchor coordinates.");
        range(f.Add("duration", &Trail::duration, "Seconds of emission, followed by drain; zero emits indefinitely."), 0, 600);
        f.Add("texture", &Trail::texture, "Optional image stretched over the ribbon: left edge at the entity, right edge at the oldest point. Empty = solid.");
        f.Add("blend", &Trail::blend, "alpha = normal; add = the ribbon brightens what is behind it (energy, fire).").options = {"alpha", "add"};
        f.Add("layer", &Trail::layer, "2D sorting layer, like Sprite.layer.");
        range(f.Add("maxPoints", &Trail::maxPoints, "Most path points kept; the oldest are dropped first."), 2, 1024);
        f.Add("emitting", &Trail::emitting, "Add new points while true; false lets the existing ribbon fade out.");
    }
};

// A linked effect's authored fields; all instance arrays/counters are transient and copied by snapshots.
struct VFXSystem {
    static constexpr const char* kTypeName = "VFXSystem";
    static constexpr const char* kDoc = "Live-linked .vfx.json effect with named parameters, independent emitters and a shared particle budget.";
    std::string asset;  // project-relative .vfx.json
    Json params = Json::MakeObject();  // named instance overrides
    bool playing = true;  // enable automatic emission; existing particles/points still age
    bool autoDestroy = false;  // destroy the owner once finished
    std::shared_ptr<const VfxAsset> definition;  // immutable cached definition retained in native snapshots
    std::string loadedPath, error;  // last successful source and current reload diagnostic
    Json appliedParams;  // last validated overrides; avoids per-frame configuration work
    Json runtimeParams = Json::MakeObject();  // Lua/command overrides excluded from authored scene JSON
    bool playbackOverride = false, runtimePlaying = true;  // runtime controls leave authored playing unchanged
    std::vector<ParticleEmitter> emitters;
    std::vector<Trail> trails;
    double expressionTime = 0; // fixed-step parent clock, retained by native snapshots
    bool started = false;
    bool suppressDestroy = false;  // a clear operation can leave an empty reusable instance
    static void Reflect(FieldList& f) {
        f.Add("asset", &VFXSystem::asset, "Project-relative effect asset (*.vfx.json).");
        f.Add("params", &VFXSystem::params, "Named parameter overrides defined by the effect asset; unknown names/ranges are errors.");
        f.Add("playing", &VFXSystem::playing, "Automatic emission switch; existing particles and trail points keep aging while false.");
        f.Add("autoDestroy", &VFXSystem::autoDestroy, "Destroy the owner and its children after the effect finishes (vfx.spawn defaults to true).");
    }
};

// ----- 2D --------------------------------------------------------------------------

struct Sprite {
    static constexpr const char* kTypeName = "Sprite";
    static constexpr const char* kDoc = "Draws an image (or one frame of a sprite sheet) on a quad facing +Z, placed at the entity. Transparent pixels are cut out. For 2D games use an orthographic Camera looking down -Z.";
    std::string texture;
    std::string material;
    Color color{1, 1, 1};
    int frame = 0;
    int columns = 1;
    int rows = 1;
    float pixelsPerUnit = 16.0f;
    float width = 0.0f;
    float height = 0.0f;
    float pivotX = 0.5f;
    float pivotY = 0.5f;
    bool flipX = false;
    bool flipY = false;
    bool pixelArt = true;
    float alphaCutoff = 0.5f;
    float opacity = 1.0f;
    bool lit = false;
    float emission = 0.0f;
    std::string blend = "alpha";
    int layer = 0;
    int order = 0;
    bool castShadows = false;
    bool visible = true;
    static void Reflect(FieldList& f) {
        f.Add("texture", &Sprite::texture, "Image file (.png with transparency). A sprite sheet is a grid of equally sized frames.");
        f.Add("material", &Sprite::material,
              "Optional material file (*.mat.json) for the sprite's quad, e.g. a shader graph whose `offset` sways it or `fog: false`. "
              "`texture` stays its base image (graph uv includes the frame); the sprite's own cutoff, blend, lit, tint and emission still apply. "
              "Empty = the default sprite material.");
        f.Add("color", &Sprite::color, "Tint multiplied with the image.");
        f.Add("frame", &Sprite::frame, "Frame index in the sheet, row by row from the top-left (SpriteAnimation sets it).");
        f.Add("columns", &Sprite::columns, "Frames per row in the sheet.");
        f.Add("rows", &Sprite::rows, "Rows of frames in the sheet.");
        f.Add("pixelsPerUnit", &Sprite::pixelsPerUnit, "Image pixels per world unit; sets the size when width/height are 0 (16 = a 16 px frame is 1 unit).");
        f.Add("width", &Sprite::width, "Width in world units (0 = frame pixels / pixelsPerUnit). Transform.scale multiplies it.");
        f.Add("height", &Sprite::height, "Height in world units (0 = from pixels).");
        f.Add("pivotX", &Sprite::pivotX, "Point of the sprite placed at the entity position: 0 = left edge, 0.5 = center, 1 = right edge.");
        f.Add("pivotY", &Sprite::pivotY, "0 = bottom edge (feet), 0.5 = center, 1 = top edge.");
        f.Add("flipX", &Sprite::flipX, "Mirror horizontally (face left).");
        f.Add("flipY", &Sprite::flipY, "Mirror vertically.");
        f.Add("pixelArt", &Sprite::pixelArt, "Sharp nearest-neighbour pixels instead of smooth filtering.");
        f.Add("alphaCutoff", &Sprite::alphaCutoff, "Pixels with alpha below this are not drawn. 0 = soft edges: the image's alpha is blended (smoke, glows, UI-like art).");
        FieldInfo& op = f.Add("opacity", &Sprite::opacity, "Below 1 the sprite is drawn see-through (alpha blended).");
        op.hasRange = true;
        op.min = 0.0f;
        op.max = 1.0f;
        f.Add("lit", &Sprite::lit, "Apply scene lighting (default: full brightness, like classic 2D).");
        FieldInfo& em = f.Add("emission", &Sprite::emission,
                              "Lit sprites: this fraction of the image's own colour is added whatever the lighting, so sprites seen against "
                              "the light (night, backlit) stay readable. 0 = none.");
        em.hasRange = true;
        em.min = 0.0f;
        em.max = 4.0f;
        f.Add("blend", &Sprite::blend, "alpha = normal; add = the image brightens what is behind it (glows, fire, light beams) and is always blended.").options = {"alpha", "add"};
        f.Add("layer", &Sprite::layer,
              "2D sorting layer: sprites of a higher layer are drawn in front of lower ones at the same depth, whatever their "
              "position on screen (background 0, characters 1, effects 2, ...).");
        f.Add("order", &Sprite::order, "Sorting inside the layer: higher is drawn in front.");
        f.Add("castShadows", &Sprite::castShadows,
              "Cast a shadow shaped by the image (texels below alphaCutoff cast none). Needs lit: true and a cut-out sprite "
              "(alphaCutoff > 0); the sprite is then drawn from both sides.");
        f.Add("visible", &Sprite::visible, "Whether the sprite is drawn.");
    }
};

struct SpriteAnimation {
    static constexpr const char* kTypeName = "SpriteAnimation";
    static constexpr const char* kDoc = "Plays frame sequences on the entity's Sprite. clips = {\"run\": {\"frames\": [2,3,4,5], \"fps\": 10, \"loop\": true}}; set clip to switch (restarts it).";
    Json clips = Json::MakeObject();
    std::string clip;
    float speed = 1.0f;
    bool playing = true;
    float time = 0.0f;
    bool finished = false;
    std::string current;  // runtime: clip that `time` refers to (not reflected)
    bool eventsStarted = false;
    static void Reflect(FieldList& f) {
        f.Add("clips", &SpriteAnimation::clips, "Clip name -> {frames: [indices], fps: number, loop: bool (default true), events: [{frame: sequenceIndex, name: string}]}. animation.events lists/sets markers; onAnimationEvent(name, clip) receives crossings and finished.");
        f.Add("clip", &SpriteAnimation::clip, "Clip being played. Changing it starts the new clip from its first frame.");
        f.Add("speed", &SpriteAnimation::speed, "Playback speed multiplier.");
        f.Add("playing", &SpriteAnimation::playing, "Pause/resume.");
        f.Add("time", &SpriteAnimation::time, "Runtime: seconds into the current clip.");
        f.Add("finished", &SpriteAnimation::finished, "Runtime: true once a non-looping clip reached its last frame.");
    }
};

struct Tilemap {
    static constexpr const char* kTypeName = "Tilemap";
    static constexpr const char* kDoc = "Grid of tiles written as text rows (map): each character is a tile defined by a tileset file or the legend (fixed frame, random variants or autotile that picks frames from the neighbours). The entity position is the top-left corner; rows go down (-Y), columns right (+X). Tiles can be solid, one-way or shaped (slopes) for 2D and 3D physics.";
    std::string tileset;
    int columns = 1;
    int rows = 1;
    float tileSize = 1.0f;
    Json map = Json::MakeArray();
    Json legend = Json::MakeObject();
    std::string solid;
    Color color{1, 1, 1};
    bool pixelArt = true;
    bool lit = false;
    bool visible = true;
    static void Reflect(FieldList& f) {
        f.Add("tileset", &Tilemap::tileset, "Tileset: an image (grid of equally sized tiles, see columns/rows) or a *.tileset.json file (image, grid and tile rules shared by maps).");
        f.Add("columns", &Tilemap::columns, "Tiles per row in the tileset image (ignored with a .tileset.json).");
        f.Add("rows", &Tilemap::rows, "Rows of tiles in the tileset image (ignored with a .tileset.json).");
        f.Add("tileSize", &Tilemap::tileSize, "Size of one tile in world units.");
        f.Add("map", &Tilemap::map, "Array of strings, one per row from the top; one character per tile. Characters without a rule (space, '.') are empty.");
        f.Add("legend", &Tilemap::legend, "Character -> rule; overrides the tileset file. A rule is a frame number or {frame, variants, autotile: \"sides\"|\"blob\", frames, connects, edges, collision}, e.g. {\"#\": {\"autotile\": \"blob\", \"frame\": 0, \"collision\": \"solid\"}, \"=\": {\"frame\": 47, \"collision\": \"oneway\"}}.");
        f.Add("solid", &Tilemap::solid, "Characters that collide as full blocks, e.g. \"#=\" (shortcut for collision: \"solid\").");
        f.Add("color", &Tilemap::color, "Tint.");
        f.Add("pixelArt", &Tilemap::pixelArt, "Sharp nearest-neighbour pixels.");
        f.Add("lit", &Tilemap::lit, "Apply scene lighting.");
        f.Add("visible", &Tilemap::visible, "Whether the tiles are drawn (they still collide).");
    }
};

struct Camera {
    static constexpr const char* kTypeName = "Camera";
    static constexpr const char* kDoc = "Camera looking down its local -Z axis. The first active camera renders the game view.";
    std::string projection = "perspective";
    float fov = 60.0f;
    float orthoSize = 5.0f;
    float nearPlane = 0.1f;
    float farPlane = 500.0f;
    Color clearColor{0.12f, 0.14f, 0.18f};
    bool active = true;
    static void Reflect(FieldList& f) {
        f.Add("projection", &Camera::projection, "perspective or orthographic (no perspective shrink; good for 2D/isometric).").options = {"perspective", "orthographic"};
        f.Add("orthoSize", &Camera::orthoSize, "Orthographic: half of the visible height in meters.");
        FieldInfo& fov = f.Add("fov", &Camera::fov, "Perspective: vertical field of view in degrees.");
        fov.hasRange = true; fov.min = 1.0f; fov.max = 179.0f;
        f.Add("nearPlane", &Camera::nearPlane, "Near clip distance in meters.");
        f.Add("farPlane", &Camera::farPlane, "Far clip distance in meters.");
        f.Add("clearColor", &Camera::clearColor, "Background color.");
        f.Add("active", &Camera::active, "Only the first active camera (lowest entity id) is used.");
    }
};

struct PostProcess {
    static constexpr const char* kTypeName = "PostProcess";
    static constexpr const char* kDoc = "Optional screen effects on the active Camera. Defaults preserve existing frames; UI and selection outlines are unaffected.";
    float exposure = 1.0f;         // multiplier applied before tone mapping; one is neutral
    std::string toneMapping = "none";  // none or reinhard (per-channel c / (1 + c))
    float bloom = 0.0f;            // blurred highlight strength; zero disables bloom
    float bloomThreshold = 1.0f;   // threshold in unexposed scene RGB, using the largest channel
    int bloomRadius = 8;           // separable tent filter radius in scene pixels
    bool fxaa = false;             // directional edge smoothing after color effects, before overlays
    float ssr = 0.0f;              // screen-space reflected ambient strength; zero allocates no SSR buffers
    float ssrDistance = 30.0f;     // maximum reflection ray length in world meters
    float ssrThickness = 0.2f;     // view-depth tolerance for a screen-space hit
    float ssao = 0.0f;             // ambient occlusion strength; zero skips the depth prepass and sampling
    float ssaoRadius = 1.0f;       // world-space sampling radius in meters
    float ssaoBias = 0.02f;        // world-space self-occlusion bias
    float vignette = 0.0f;          // edge darkening strength; zero disables the effect
    float vignetteRadius = 0.75f;   // normalized distance from screen center
    float vignetteSoftness = 0.5f;  // smooth transition width
    int dofRadius = 0;             // largest depth-of-field blur radius in scene pixels; zero disables it
    float dofFocus = 10.0f;        // view depth in meters that is sharp
    float dofRange = 2.0f;         // depth on either side of dofFocus that stays sharp
    float dofFalloff = 8.0f;       // further depth over which the blur grows to dofRadius
    float fog = 0.0f;              // largest fog amount reached at fogEnd; zero disables fog
    Color fogColor{0.75f, 0.82f, 0.9f};
    float fogStart = 20.0f;        // distance from the camera where fog begins
    float fogEnd = 80.0f;          // distance where it reaches `fog`
    float saturation = 1.0f;       // color grading after tone mapping; all neutral values skip it
    float contrast = 1.0f;
    Color shadowTint{1, 1, 1};
    Color highlightTint{1, 1, 1};
    static void Reflect(FieldList& f) {
        FieldInfo& exposureField = f.Add("exposure", &PostProcess::exposure, "Scene brightness multiplier before tone mapping. 1 preserves brightness.");
        exposureField.hasRange = true; exposureField.min = 0; exposureField.max = 32;
        f.Add("toneMapping", &PostProcess::toneMapping,
              "none (disabled), reinhard (compress HDR channels as c / (1 + c)) or aces (filmic curve, Narkowicz fit of ACES).")
            .options = {"none", "reinhard", "aces"};
        FieldInfo& bloomStrength = f.Add("bloom", &PostProcess::bloom, "Blurred highlight strength before exposure/tone mapping. 0 disables bloom.");
        bloomStrength.hasRange = true; bloomStrength.min = 0; bloomStrength.max = 4;
        FieldInfo& threshold = f.Add("bloomThreshold", &PostProcess::bloomThreshold, "Highlight threshold in unexposed scene RGB (largest channel); extraction preserves hue.");
        threshold.hasRange = true; threshold.min = 0; threshold.max = 32;
        FieldInfo& bloomRadiusField = f.Add("bloomRadius", &PostProcess::bloomRadius, "Separable tent filter radius in scene pixels, 1..32. Full-resolution bloom is optional.");
        bloomRadiusField.hasRange = true; bloomRadiusField.min = 1; bloomRadiusField.max = 32;
        f.Add("fxaa", &PostProcess::fxaa, "Optional directional FXAA on display scene color before UI and selection outlines; disabled by default.");
        FieldInfo& ssrStrength = f.Add("ssr", &PostProcess::ssr, "Screen-space reflection strength (0..1) on opaque glossy surfaces. Missing rays keep probe/sky reflections. 0 disables it.");
        ssrStrength.hasRange = true; ssrStrength.min = 0; ssrStrength.max = 1;
        FieldInfo& ssrLength = f.Add("ssrDistance", &PostProcess::ssrDistance, "SSR maximum ray length in world meters (0.1..1000); 64 steps with five hit refinements.");
        ssrLength.hasRange = true; ssrLength.min = 0.1f; ssrLength.max = 1000;
        FieldInfo& ssrTolerance = f.Add("ssrThickness", &PostProcess::ssrThickness, "SSR view-depth hit tolerance in world meters (0.001..10). Thin values reduce false reflections.");
        ssrTolerance.hasRange = true; ssrTolerance.min = 0.001f; ssrTolerance.max = 10;
        FieldInfo& ssaoStrength = f.Add("ssao", &PostProcess::ssao, "Screen-space ambient occlusion strength (0..1). Darkens ambient light only; 0 disables the depth prepass.");
        ssaoStrength.hasRange = true; ssaoStrength.min = 0; ssaoStrength.max = 1;
        FieldInfo& ssaoRadiusField = f.Add("ssaoRadius", &PostProcess::ssaoRadius, "SSAO sampling radius in world meters (0.01..10), limited to 64 scene pixels per axis.");
        ssaoRadiusField.hasRange = true; ssaoRadiusField.min = 0.01f; ssaoRadiusField.max = 10;
        FieldInfo& ssaoBiasField = f.Add("ssaoBias", &PostProcess::ssaoBias, "SSAO self-occlusion bias in world meters (0..1).");
        ssaoBiasField.hasRange = true; ssaoBiasField.min = 0; ssaoBiasField.max = 1;
        FieldInfo& strength = f.Add("vignette", &PostProcess::vignette, "Edge darkening strength: 0 disables, 1 is fully dark outside the transition.");
        strength.hasRange = true; strength.min = 0; strength.max = 1;
        FieldInfo& radius = f.Add("vignetteRadius", &PostProcess::vignetteRadius, "Normalized radius: center 0, edge midpoint 1, corner sqrt(2).");
        radius.hasRange = true; radius.min = 0; radius.max = 1.5f;
        FieldInfo& softness = f.Add("vignetteSoftness", &PostProcess::vignetteSoftness, "Smooth transition width in normalized screen coordinates (minimum 0.01).");
        softness.hasRange = true; softness.min = 0.01f; softness.max = 2;
        FieldInfo& dof = f.Add("dofRadius", &PostProcess::dofRadius,
                               "Depth of field: largest blur radius in scene pixels (0..16) for surfaces far from dofFocus. 0 disables it.");
        dof.hasRange = true; dof.min = 0; dof.max = 16;
        FieldInfo& focus = f.Add("dofFocus", &PostProcess::dofFocus, "Depth of field: distance in front of the camera, in meters, that is sharp.");
        focus.hasRange = true; focus.min = 0; focus.max = 100000;
        FieldInfo& range = f.Add("dofRange", &PostProcess::dofRange, "Depth of field: depth on either side of dofFocus that stays fully sharp.");
        range.hasRange = true; range.min = 0; range.max = 100000;
        FieldInfo& falloff = f.Add("dofFalloff", &PostProcess::dofFalloff,
                                   "Depth of field: depth beyond dofRange over which the blur grows to dofRadius (minimum 0.01).");
        falloff.hasRange = true; falloff.min = 0.01f; falloff.max = 100000;
        FieldInfo& fogField = f.Add("fog", &PostProcess::fog,
                                    "Distance fog: amount of fogColor mixed into surfaces at fogEnd and beyond (0 disables). It grows linearly "
                                    "from fogStart; additive surfaces fade out instead. Materials with fog: false (sky domes) are not fogged.");
        fogField.hasRange = true; fogField.min = 0; fogField.max = 1;
        f.Add("fogColor", &PostProcess::fogColor, "Fog color (usually the sky's horizon color).");
        FieldInfo& fogStartField = f.Add("fogStart", &PostProcess::fogStart, "Fog: distance from the camera in meters where fog begins.");
        fogStartField.hasRange = true; fogStartField.min = 0; fogStartField.max = 100000;
        FieldInfo& fogEndField = f.Add("fogEnd", &PostProcess::fogEnd, "Fog: distance where the fog reaches its full amount (at least fogStart + 0.01).");
        fogEndField.hasRange = true; fogEndField.min = 0; fogEndField.max = 100000;
        FieldInfo& sat = f.Add("saturation", &PostProcess::saturation, "Color grading after tone mapping: 0 = grey, 1 = unchanged, above 1 = more vivid.");
        sat.hasRange = true; sat.min = 0; sat.max = 4;
        FieldInfo& con = f.Add("contrast", &PostProcess::contrast, "Color grading: contrast around mid grey 0.5; 1 = unchanged.");
        con.hasRange = true; con.min = 0; con.max = 4;
        f.Add("shadowTint", &PostProcess::shadowTint, "Color grading: multiplier for dark tones (split toning, e.g. cool shadows). White = unchanged.");
        f.Add("highlightTint", &PostProcess::highlightTint, "Color grading: multiplier for bright tones (e.g. warm highlights). White = unchanged.");
    }
};

struct Light2D {
    static constexpr const char* kTypeName = "Light2D";
    static constexpr const char* kDoc = "2D light: a soft round hole in the active camera's Darkness2D, centred on the entity. Without a Darkness2D it does nothing.";
    float radius = 5.0f;
    float inner = 0.25f;
    float strength = 1.0f;
    bool enabled = true;
    static void Reflect(FieldList& f) {
        f.Add("radius", &Light2D::radius, "World units where the light ends (Transform.scale does not change it).");
        FieldInfo& in = f.Add("inner", &Light2D::inner, "Fraction of the radius that is fully lit before the falloff starts.");
        in.hasRange = true; in.min = 0.0f; in.max = 0.99f;
        FieldInfo& st = f.Add("strength", &Light2D::strength, "How much darkness the centre removes: 1 = all, 0.5 = a dim glimpse.");
        st.hasRange = true; st.min = 0.0f; st.max = 1.0f;
        f.Add("enabled", &Light2D::enabled, "Off = no light, without removing the component.");
    }
};

struct Darkness2D {
    static constexpr const char* kTypeName = "Darkness2D";
    static constexpr const char* kDoc =
        "On an orthographic Camera: covers the 2D view with a colour that Light2D entities cut soft holes into (night, fog of war, caves). "
        "Drawn at `layer`: blended sprites and particles in higher layers stay visible on top of it.";
    Color color{0.02f, 0.02f, 0.06f};
    float opacity = 0.9f;
    int layer = 100;
    int resolution = 144;
    bool enabled = true;
    static void Reflect(FieldList& f) {
        f.Add("color", &Darkness2D::color, "Colour of the unlit area.");
        FieldInfo& op = f.Add("opacity", &Darkness2D::opacity, "How opaque the unlit area is (1 hides it completely).");
        op.hasRange = true; op.min = 0.0f; op.max = 1.0f;
        f.Add("layer", &Darkness2D::layer,
              "2D sorting layer of the overlay (see Sprite.layer). Blended sprites (alphaCutoff 0) and particles above it are not darkened; "
              "cut-out sprites are always under it.");
        FieldInfo& res = f.Add("resolution", &Darkness2D::resolution, "Light map rows across the view height; edges are smoothed when it is stretched.");
        res.hasRange = true; res.min = 16; res.max = 512;
        f.Add("enabled", &Darkness2D::enabled, "Off = no darkness.");
    }
};

struct DirectionalLight {
    static constexpr const char* kTypeName = "DirectionalLight";
    static constexpr const char* kDoc = "Sun-like light. Direction comes from the entity's rotation (local -Z).";
    Color color{1.0f, 0.97f, 0.9f};
    float intensity = 1.0f;
    Color ambient{0.18f, 0.2f, 0.25f};
    bool hemisphere = false;
    Color groundAmbient{0.1f, 0.09f, 0.08f};
    bool shadows = true;
    float shadowStrength = 0.75f;
    float shadowDistance = 0.0f;
    int shadowCascades = 1;  // One preserves the legacy map; 2..4 split the camera's view by depth.
    static void Reflect(FieldList& f) {
        f.Add("color", &DirectionalLight::color, "Light color.");
        f.Add("intensity", &DirectionalLight::intensity, "Brightness multiplier.");
        f.Add("ambient", &DirectionalLight::ambient, "Ambient light added to every surface (with hemisphere: the light from the sky above).");
        f.Add("hemisphere", &DirectionalLight::hemisphere,
              "Hemisphere ambient: surfaces facing up get `ambient`, surfaces facing down get groundAmbient, blended by the normal.");
        f.Add("groundAmbient", &DirectionalLight::groundAmbient, "Hemisphere ambient: light bounced from the ground, seen by downward faces.");
        f.Add("shadows", &DirectionalLight::shadows, "Cast shadows (first directional light only).");
        FieldInfo& s = f.Add("shadowStrength", &DirectionalLight::shadowStrength, "0 = no darkening, 1 = black shadows.");
        s.hasRange = true; s.min = 0.0f; s.max = 1.0f;
        FieldInfo& d = f.Add("shadowDistance", &DirectionalLight::shadowDistance,
                             "0 = whole scene with one map; with cascades, camera far plane (at most 1000 meters). "
                             "> 0: only what the camera sees up to this distance "
                             "(world units), so large scenes get sharper shadows and the shadow pass skips far casters.");
        d.hasRange = true; d.min = 0.0f; d.max = 100000.0f;
        FieldInfo& c = f.Add("shadowCascades", &DirectionalLight::shadowCascades,
                             "1 = legacy shadow map; 2..4 = stable cascades with blended boundaries. "
                             "shadowDistance is the far reach (0 = camera far plane, at most 1000 meters).");
        c.hasRange = true; c.min = 1.0f; c.max = 4.0f;
    }
};

// The environment of a scene: a sky that lights it, that smooth and metallic surfaces reflect
// (image-based lighting) and that is drawn behind it. Either three colors (an analytic gradient)
// or an equirectangular image (`texture`). One per scene (the first is used). Absent = the flat /
// hemisphere ambient of the directional light and the camera's clear color, unchanged.
struct Skybox {
    static constexpr const char* kTypeName = "Skybox";
    static constexpr const char* kDoc = "Sky environment: lights the scene, is reflected by smooth/metallic surfaces (image-based lighting) and drawn as the background. Three colors, or an equirectangular image. One per scene.";
    std::string texture;                 // equirectangular image (.hdr/.png/.jpg); empty = the three colors below
    float rotation = 0.0f;               // turns the image around the vertical axis, degrees
    Color sky{0.30f, 0.47f, 0.80f};      // zenith (straight up)
    Color horizon{0.70f, 0.78f, 0.90f};  // at the horizon
    Color ground{0.28f, 0.27f, 0.30f};   // below the horizon (bounced ground light)
    float intensity = 1.0f;              // multiplies the whole environment
    float reflections = 1.0f;            // 0 = light only, 1 = full environment reflection on smooth surfaces
    bool background = true;              // draw the sky behind the scene (else the flat clear color stays)
    bool sun = true;                     // draw a sun disc in the first directional light's direction
    float sunSize = 2.5f;                // sun disc angular size in degrees
    static void Reflect(FieldList& f) {
        f.Add("texture", &Skybox::texture, "Equirectangular (2:1 panorama) image of the surroundings, .hdr (keeps bright lights) or .png/.jpg. Lights the scene, is reflected (blurrier on rougher surfaces) and drawn as the background. Empty = the sky/horizon/ground colors.");
        FieldInfo& ro = f.Add("rotation", &Skybox::rotation, "Turns the image around the vertical axis, degrees.");
        ro.hasRange = true; ro.min = -360.0f; ro.max = 360.0f;
        f.Add("sky", &Skybox::sky, "Color straight up (zenith). Used without a texture.");
        f.Add("horizon", &Skybox::horizon, "Color at the horizon. Used without a texture.");
        f.Add("ground", &Skybox::ground, "Color below the horizon (light bounced from the ground). Used without a texture.");
        FieldInfo& in = f.Add("intensity", &Skybox::intensity, "Multiplies the whole environment.");
        in.hasRange = true; in.min = 0.0f; in.max = 8.0f;
        FieldInfo& rf = f.Add("reflections", &Skybox::reflections, "0 = ambient light only, 1 = full sky reflection on smooth metal/wet surfaces.");
        rf.hasRange = true; rf.min = 0.0f; rf.max = 1.0f;
        f.Add("background", &Skybox::background, "Draw the sky behind the scene (off = keep the camera's clear color).");
        f.Add("sun", &Skybox::sun, "Draw a sun disc in the first directional light's direction (not with a texture: the image has its own sun).");
        FieldInfo& ss = f.Add("sunSize", &Skybox::sunSize, "Sun disc size in degrees.");
        ss.hasRange = true; ss.min = 0.1f; ss.max = 30.0f;
    }
};

struct Decal {
    static constexpr const char* kTypeName = "Decal";
    static constexpr const char* kDoc = "A texture/tint projected along local -Y onto opaque/cutout surfaces in this oriented box, before lighting. First sixteen visible projectors by order then id.";
    std::string texture;           // Project texture; empty is a solid color.
    Vec3 size{1,1,1};              // Full local box dimensions; hierarchy/scale also apply.
    Color color{1,1,1};
    float opacity = 1;             // Texture alpha multiplier; scripts can fade it on simulated time.
    float edgeFade = 0.1f;         // Feather width as a fraction of each normalized box axis.
    float normalThreshold = 0.2f;  // Reject surfaces facing away from local +Y.
    int order = 0;                // Lower first, higher painted over; entity id breaks ties.
    bool enabled = true;
    static void Reflect(FieldList& f) {
        f.Add("texture", &Decal::texture, "Projected texture with alpha; empty paints solid color. Resampled to a 254-square atlas tile.");
        f.Add("size", &Decal::size, "Full local box dimensions (positive, at most 1000 per axis). Transform hierarchy and scale apply.");
        f.Add("color", &Decal::color, "RGB tint of the projected texture (bounded 0..1).");
        FieldInfo& opacityField = f.Add("opacity", &Decal::opacity, "Projected alpha multiplier (0..1). 0 skips this decal; scripts can animate it.");
        opacityField.hasRange = true; opacityField.min = 0; opacityField.max = 1;
        FieldInfo& fadeField = f.Add("edgeFade", &Decal::edgeFade, "Feather at all six box boundaries, as a fraction of the normalized box (0..0.5). 0 is a hard boundary.");
        fadeField.hasRange = true; fadeField.min = 0; fadeField.max = 0.5f;
        FieldInfo& thresholdField = f.Add("normalThreshold", &Decal::normalThreshold, "Surface normal dot local +Y below this rejects projection; above it fades toward full coverage (-1..0.99).");
        thresholdField.hasRange = true; thresholdField.min = -1; thresholdField.max = 0.99f;
        f.Add("order", &Decal::order, "Lower draws first, higher covers it. First sixteen visible enabled boxes by order then entity id.");
        f.Add("enabled", &Decal::enabled, "Enable this box projector; disabled decals cost no atlas/sample work.");
    }
};

struct PlanarReflection {
    static constexpr const char* kTypeName = "PlanarReflection";
    static constexpr const char* kDoc = "A live one-sided reflection on this entity's flat MeshRenderer. One visible surface per view; captures moving scenery without recursive mirrors.";
    Vec3 normal{0,1,0};            // Plane normal in local space, transformed by the inverse-transpose.
    Vec3 offset{0,0,0};            // Plane origin in local space (hierarchy/scale apply).
    float strength = 1;            // Blend toward captured scene color; zero disables capture.
    int resolution = 256;          // Square capture size in pixels, bounded 64..1024.
    float clipBias = 0.01f;        // World meters above the plane excluded to avoid self-clipping artifacts.
    bool enabled = true;
    static void Reflect(FieldList& f) {
        f.Add("normal", &PlanarReflection::normal, "Local-space plane normal; default +Y for the built-in plane. Zero/invalid normals disable capture.");
        f.Add("offset", &PlanarReflection::offset, "Local-space plane origin; should coincide with the flat mesh. Entity hierarchy and scale apply.");
        FieldInfo& strengthField = f.Add("strength", &PlanarReflection::strength, "Blend from ordinary material lighting to live reflection (0..1). 0 skips capture.");
        strengthField.hasRange = true; strengthField.min = 0; strengthField.max = 1;
        FieldInfo& resolutionField = f.Add("resolution", &PlanarReflection::resolution, "Square reflection capture resolution (64..1024). One surface per view, nearest origin then entity id.");
        resolutionField.hasRange = true; resolutionField.min = 64; resolutionField.max = 1024;
        FieldInfo& biasField = f.Add("clipBias", &PlanarReflection::clipBias, "World-space clipping offset above the plane (0.001..0.1 meters). Excludes geometry behind the mirror.");
        biasField.hasRange = true; biasField.min = 0.001f; biasField.max = 0.1f;
        f.Add("enabled", &PlanarReflection::enabled, "Enable live planar capture; disable to use the ordinary material.");
    }
};

struct ReflectionProbe {
    static constexpr const char* kTypeName = "ReflectionProbe";
    static constexpr const char* kDoc = "Local reflection: a picture of the surroundings taken from the entity's position. Surfaces inside its box reflect (and are ambient-lit by) that picture instead of the sky, so a room reflects its own walls. Shows the scenery, not moving bodies; taken when the scene loads, play starts, the stopped scene is edited, or on probe.bake.";
    Vec3 size{10.0f, 5.0f, 10.0f};  // the box (world axes) the probe covers
    Vec3 offset{0, 0, 0};           // box center relative to the entity (the picture is taken at the entity)
    float blend = 0.5f;             // fade distance outside the box, meters
    float intensity = 1.0f;
    bool boxProjection = true;      // reflections follow the box walls (rooms); off = infinitely far surroundings
    bool ambient = true;            // the picture also replaces the sky's diffuse light inside the box
    int resolution = 128;           // pixels along one of the six faces of the picture
    float nearPlane = 0.1f;
    float farPlane = 200.0f;
    static void Reflect(FieldList& f) {
        f.Add("size", &ReflectionProbe::size, "Size of the box the probe covers, meters along the world axes (the entity's rotation and scale are ignored). Make it the size of the room: with boxProjection its walls are where the reflections sit.");
        f.Add("offset", &ReflectionProbe::offset, "Box center relative to the entity. The picture is taken at the entity's position (keep that clear of walls and furniture).");
        FieldInfo& bl = f.Add("blend", &ReflectionProbe::blend, "Distance outside the box over which the probe fades into the sky or a larger probe, meters.");
        bl.hasRange = true; bl.min = 0.0f; bl.max = 10.0f;
        FieldInfo& in = f.Add("intensity", &ReflectionProbe::intensity, "Multiplies the picture.");
        in.hasRange = true; in.min = 0.0f; in.max = 8.0f;
        f.Add("boxProjection", &ReflectionProbe::boxProjection, "Reflections are placed on the box walls, so they line up in a room. Off = the surroundings are treated as infinitely far (outdoors).");
        f.Add("ambient", &ReflectionProbe::ambient, "The picture also replaces the sky's diffuse ambient light inside the box (an interior is lit by its own walls, not the sky).");
        FieldInfo& re = f.Add("resolution", &ReflectionProbe::resolution, "Pixels along one face of the picture (16..256). Larger = sharper mirror reflections, slower to take.");
        re.hasRange = true; re.min = 16.0f; re.max = 256.0f;
        f.Add("nearPlane", &ReflectionProbe::nearPlane, "Nearest distance the picture shows, meters.");
        f.Add("farPlane", &ReflectionProbe::farPlane, "Farthest distance the picture shows, meters.");
    }
};

struct SpotLight {
    static constexpr const char* kTypeName = "SpotLight";
    static constexpr const char* kDoc = "Cone light along local -Z (parent rotations included), with a soft edge and optional budgeted shadows.";
    Color color{1.0f, 0.85f, 0.6f};  // Light color.
    float intensity = 1.5f;        // Brightness on the cone axis at the source.
    float range = 6.0f;            // Radial reach in world meters, unaffected by scale.
    float innerAngle = 20.0f;      // Half-angle in degrees, full strength inside.
    float outerAngle = 30.0f;      // Half-angle in degrees, zero strength outside.
    bool shadows = false;         // Opt in to the shared two-local-light shadow budget.
    static void Reflect(FieldList& f) {
        f.Add("color", &SpotLight::color, "Light color.");
        f.Add("intensity", &SpotLight::intensity, "Brightness on the axis; negative values render as zero.");
        f.Add("range", &SpotLight::range, "Radial distance in meters where the light reaches zero; minimum 0.01.");
        f.Add("innerAngle", &SpotLight::innerAngle, "Full-strength cone half-angle in degrees; clamped to 0..outerAngle.");
        f.Add("outerAngle", &SpotLight::outerAngle, "Cone half-angle in degrees; clamped to 0.1..89. Equal angles give a hard edge.");
        f.Add("shadows", &SpotLight::shadows, "Cast shadows if selected: at most two local lights per view, in view and nearest first.");
    }
};

struct PointLight {
    static constexpr const char* kTypeName = "PointLight";
    static constexpr const char* kDoc = "Light bulb at the entity position, fading to zero at `range`.";
    Color color{1.0f, 0.85f, 0.6f};
    float intensity = 1.5f;
    float range = 6.0f;
    bool shadows = false;  // Opt in to the shared two-local-light shadow budget (six cube faces).
    static void Reflect(FieldList& f) {
        f.Add("color", &PointLight::color, "Light color.");
        f.Add("intensity", &PointLight::intensity, "Brightness at the center.");
        f.Add("range", &PointLight::range, "Distance in meters where the light reaches zero.");
        f.Add("shadows", &PointLight::shadows, "Cast six-face shadows if selected: at most two local lights per view, in view and nearest first.");
    }
};

struct CameraFollow {
    static constexpr const char* kTypeName = "CameraFollow";
    static constexpr const char* kDoc = "Moves this entity (usually the camera) to target + offset after physics each frame and aims it at the target.";
    EntityId target = kNullEntity;
    Vec3 offset{0, 4, 8};
    Vec3 lookOffset{0, 0.5f, 0};
    float smoothing = 8.0f;
    bool useBounds = false;
    Vec3 boundsMin{-1000, -1000, -1000};
    Vec3 boundsMax{1000, 1000, 1000};
    static void Reflect(FieldList& f) {
        f.Add("target", &CameraFollow::target, "Entity id to follow.");
        f.Add("offset", &CameraFollow::offset, "Position relative to the target (world axes).");
        f.Add("lookOffset", &CameraFollow::lookOffset, "Point to look at, relative to the target.");
        f.Add("smoothing", &CameraFollow::smoothing, "Catch-up speed (per second); 0 = snap instantly.");
        f.Add("useBounds", &CameraFollow::useBounds, "Clamp the camera position to boundsMin..boundsMax and keep its rotation (2D side-scrollers: the view stops at the level edges).");
        f.Add("boundsMin", &CameraFollow::boundsMin, "Lowest camera position when useBounds is on.");
        f.Add("boundsMax", &CameraFollow::boundsMax, "Highest camera position when useBounds is on.");
    }
};

struct Tag {
    static constexpr const char* kTypeName = "Tag";
    static constexpr const char* kDoc = "Free-form labels for gameplay queries (e.g. \"enemy\", \"pickup\").";
    std::string tags;
    static void Reflect(FieldList& f) { f.Add("tags", &Tag::tags, "Comma separated tags."); }
};

// ----- navigation ------------------------------------------------------------------

struct NavMesh {
    static constexpr const char* kTypeName = "NavMesh";
    static constexpr const char* kDoc = "Settings of the scene's navigation mesh: the ground an agent of this size can walk on, built from static colliders and/or meshes (nav.bake, nav.path, Lua nav.path). One per scene (the first in id order); a scene without one uses these defaults.";
    float agentRadius = 0.4f;
    float agentHeight = 1.8f;
    float maxSlope = 45.0f;
    float stepHeight = 0.3f;
    float cellSize = 0.2f;
    float minRegion = 1.0f;
    std::string source = "colliders";
    std::string collect = "all";
    static void Reflect(FieldList& f) {
        FieldInfo& r = f.Add("agentRadius", &NavMesh::agentRadius, "Agent radius in meters: the mesh keeps this far from walls and edges.");
        r.hasRange = true; r.min = 0.0f; r.max = 10.0f;
        FieldInfo& h = f.Add("agentHeight", &NavMesh::agentHeight, "Agent height in meters: lower passages are not walkable.");
        h.hasRange = true; h.min = 0.1f; h.max = 50.0f;
        FieldInfo& s = f.Add("maxSlope", &NavMesh::maxSlope, "Steepest walkable slope in degrees.");
        s.hasRange = true; s.min = 0.0f; s.max = 89.0f;
        FieldInfo& c = f.Add("stepHeight", &NavMesh::stepHeight, "Highest ledge the agent steps up or down (stairs, kerbs), meters.");
        c.hasRange = true; c.min = 0.0f; c.max = 10.0f;
        FieldInfo& cs = f.Add("cellSize", &NavMesh::cellSize, "Size of the cells the geometry is sampled with, meters. Smaller follows walls more closely and bakes slower; about agentRadius / 2.");
        cs.hasRange = true; cs.min = 0.05f; cs.max = 5.0f;
        FieldInfo& m = f.Add("minRegion", &NavMesh::minRegion, "Walkable islands smaller than this (square meters) are dropped, e.g. the tops of crates.");
        m.hasRange = true; m.min = 0.0f; m.max = 10000.0f;
        f.Add("source", &NavMesh::source, "Geometry taken as ground and obstacles: colliders (static Collider shapes, what characters stand on), meshes (static MeshRenderer models) or all.").options = {"colliders", "meshes", "all"};
        f.Add("collect", &NavMesh::collect, "all = every entity of the scene; children = only this entity and those below it.").options = {"all", "children"};
    }
};

struct NavModifier {
    static constexpr const char* kTypeName = "NavModifier";
    static constexpr const char* kDoc = "How this entity and its children take part in the navigation mesh.";
    std::string mode = "ignore";
    static void Reflect(FieldList& f) {
        f.Add("mode", &NavModifier::mode, "ignore = left out of the bake (sky domes, decoration); unwalkable = an obstacle agents never stand on (water, lava).").options = {"ignore", "unwalkable"};
    }
};

struct Data {
    static constexpr const char* kTypeName = "Data";
    static constexpr const char* kDoc = "Named game values kept on the entity (numbers, strings, booleans): cooldowns, charges, mana, a "
                                        "state name. Saved with the scene, editable in the inspector and replicable like any field "
                                        "(NetSync fields \"Data.values\"). With network.prediction \"owned\" a correction rewinds "
                                        "components, not script variables, so values the prediction depends on belong here.";
    Json values = Json::MakeObject();
    static void Reflect(FieldList& f) { f.Add("values", &Data::values, "Name -> number, string or boolean (at most 64 names, names up to 64 bytes).").flatValues = true; }
};

struct Script {
    static constexpr const char* kTypeName = "Script";
    static constexpr const char* kDoc = "Runs a Lua script while simulating. The file returns a table with optional onStart(self), onUpdate(self, dt) and onDestroy(self). See docs/SCRIPTING.md.";
    std::string path;
    Json params = Json::MakeObject();
    bool enabled = true;
    static void Reflect(FieldList& f) {
        f.Add("path", &Script::path, "Lua file relative to the project, e.g. \"scripts/rotator.lua\".");
        f.Add("params", &Script::params, "JSON object passed to the script as self.params.");
        f.Add("enabled", &Script::enabled, "Disabled scripts are not run.");
    }
};

// ----- Physics (simulated by Jolt, see engine/physics) ----------------------

struct Collider {
    static constexpr const char* kTypeName = "Collider";
    static constexpr const char* kDoc = "Collision shape. Alone it is static (walls, floors); add RigidBody to make it move. isTrigger makes it a non-solid volume that reports onTriggerEnter/onTriggerExit.";
    std::string shape = "box";
    Vec3 size{1, 1, 1};
    float radius = 0.5f;
    float height = 2.0f;
    Vec3 center{0, 0, 0};
    bool isTrigger = false;
    float friction = 0.5f;
    float bounciness = 0.0f;
    static void Reflect(FieldList& f) {
        f.Add("shape", &Collider::shape, "Shape type.").options = {"box", "sphere", "capsule"};
        f.Add("size", &Collider::size, "Box: full size in local units (multiplied by Transform.scale). Matches the 1x1x1 cube mesh by default.");
        f.Add("radius", &Collider::radius, "Sphere/capsule radius in local units.");
        f.Add("height", &Collider::height, "Capsule: total height along local Y, including the rounded ends.");
        f.Add("center", &Collider::center, "Offset of the shape from the entity origin (local units).");
        f.Add("isTrigger", &Collider::isTrigger, "Non-solid volume: reports overlaps instead of colliding.");
        FieldInfo& fr = f.Add("friction", &Collider::friction, "Surface friction (0 = ice, 1 = rubber).");
        fr.hasRange = true; fr.min = 0.0f; fr.max = 2.0f;
        FieldInfo& b = f.Add("bounciness", &Collider::bounciness, "Restitution (0 = no bounce, 1 = perfectly elastic).");
        b.hasRange = true; b.min = 0.0f; b.max = 1.0f;
    }
};

struct RigidBody {
    static constexpr const char* kTypeName = "RigidBody";
    static constexpr const char* kDoc = "Makes a Collider move. dynamic = driven by gravity and collisions; kinematic = follows its Transform (moving platforms, doors) and pushes dynamic bodies.";
    std::string type = "dynamic";
    float mass = 1.0f;
    Vec3 velocity{0, 0, 0};
    Vec3 angularVelocity{0, 0, 0};
    float gravityScale = 1.0f;
    float linearDamping = 0.05f;
    bool continuous = false;
    // Axis locks (Unity's constraints, Godot's axis_lock_*): no movement along / rotation around a world axis.
    bool lockPositionX = false, lockPositionY = false, lockPositionZ = false;
    bool lockRotationX = false, lockRotationY = false, lockRotationZ = false;
    static void Reflect(FieldList& f) {
        f.Add("type", &RigidBody::type, "Body type.").options = {"dynamic", "kinematic"};
        FieldInfo& m = f.Add("mass", &RigidBody::mass, "Mass in kg (dynamic bodies).");
        m.hasRange = true; m.min = 0.001f; m.max = 1.0e6f;
        f.Add("velocity", &RigidBody::velocity, "Linear velocity in m/s. Written by the simulation every step; set it to launch the body.");
        f.Add("angularVelocity", &RigidBody::angularVelocity, "Angular velocity in degrees/s (world axes). Written by the simulation.");
        f.Add("gravityScale", &RigidBody::gravityScale, "Multiplier for gravity (0 = floats).");
        f.Add("linearDamping", &RigidBody::linearDamping, "Air resistance.");
        f.Add("continuous", &RigidBody::continuous, "Continuous collision detection for fast objects (prevents tunneling through thin walls).");
        f.Add("lockPositionX", &RigidBody::lockPositionX, "Never move along the world X axis (dynamic bodies).");
        f.Add("lockPositionY", &RigidBody::lockPositionY, "Never move along the world Y axis (dynamic bodies).");
        f.Add("lockPositionZ", &RigidBody::lockPositionZ, "Never move along the world Z axis (dynamic bodies); with lockRotationX/Y it keeps a 2.5D body in its XY plane.");
        f.Add("lockRotationX", &RigidBody::lockRotationX, "Never rotate around the world X axis (dynamic bodies).");
        f.Add("lockRotationY", &RigidBody::lockRotationY, "Never rotate around the world Y axis (dynamic bodies).");
        f.Add("lockRotationZ", &RigidBody::lockRotationZ, "Never rotate around the world Z axis (dynamic bodies). All three keep the body upright.");
    }
    // Fields of older files and calls: lockRotation (all three rotation locks), plane2D (Z position plus X/Y rotation).
    static bool Upgrade(Json& values) {
        bool changed = false;
        if (values.has("lockRotation")) {
            const bool on = values["lockRotation"].asBool(false);
            for (const char* k : {"lockRotationX", "lockRotationY", "lockRotationZ"}) values[k] = Json(on);
            values.erase("lockRotation");
            changed = true;
        }
        if (values.has("plane2D")) {
            const bool on = values["plane2D"].asBool(false);
            values["lockPositionZ"] = Json(on);
            if (on) values["lockRotationX"] = values["lockRotationY"] = Json(true);
            values.erase("plane2D");
            changed = true;
        }
        return changed;
    }
};

struct CharacterBody {
    static constexpr const char* kTypeName = "CharacterBody";
    static constexpr const char* kDoc = "Physics character for players/NPCs: set velocity (x/z to walk, y to jump) and the engine moves it, sliding along walls, climbing steps and slopes, applying gravity. PlayerController uses it automatically when present.";
    std::string shape = "capsule";
    float radius = 0.4f;
    float height = 1.8f;
    float maxSlope = 50.0f;
    float stepHeight = 0.3f;
    float gravityScale = 1.0f;
    Vec3 velocity{0, 0, 0};
    bool grounded = false;
    bool lockPositionX = false, lockPositionY = false, lockPositionZ = false;
    static void Reflect(FieldList& f) {
        f.Add("shape", &CharacterBody::shape, "Shape centered on the entity origin.").options = {"capsule", "sphere"};
        f.Add("radius", &CharacterBody::radius, "Radius in meters.");
        f.Add("height", &CharacterBody::height, "Capsule total height in meters.");
        f.Add("maxSlope", &CharacterBody::maxSlope, "Steepest walkable slope in degrees.");
        f.Add("stepHeight", &CharacterBody::stepHeight, "Highest step the character walks up automatically.");
        f.Add("gravityScale", &CharacterBody::gravityScale, "Multiplier for gravity.");
        f.Add("velocity", &CharacterBody::velocity, "Desired velocity in m/s; after each step it holds the actual velocity.");
        f.Add("grounded", &CharacterBody::grounded, "Runtime state: true while standing on walkable ground.");
        f.Add("lockPositionX", &CharacterBody::lockPositionX, "Keep the character on its X (velocity.x is ignored).");
        f.Add("lockPositionY", &CharacterBody::lockPositionY, "Keep the character at its height: no gravity or jumps (velocity.y is ignored).");
        f.Add("lockPositionZ", &CharacterBody::lockPositionZ, "Keep the character on its Z (velocity.z is ignored); 2.5D side-scrollers.");
    }
    static bool Upgrade(Json& values) {  // plane2D of older files and calls
        if (!values.has("plane2D")) return false;
        values["lockPositionZ"] = Json(values["plane2D"].asBool(false));
        values.erase("plane2D");
        return true;
    }
};

// ----- 2D physics (simulated by Box2D, see engine/physics/Physics2D.cpp) ---------
// Bodies live in the XY plane of their entity: position x/y, rotation around Z,
// Transform.scale x/y scales the shape. 2D and 3D bodies do not collide with
// each other; Tilemaps collide with both.

struct Collider2D {
    static constexpr const char* kTypeName = "Collider2D";
    static constexpr const char* kDoc = "2D collision shape in the XY plane (Box2D). Alone it is static (walls, ground); add RigidBody2D to make it move. Shapes: box (optionally rounded), circle, capsule, polygon (any simple outline, concave is split), edge (line strip; loop closes it). isTrigger makes a sensor volume; oneWay makes a platform you can pass from below.";
    std::string shape = "box";
    Vec3 size{1, 1, 0};
    float radius = 0.5f;
    float height = 1.0f;
    float rounding = 0.0f;
    Vec3 center{0, 0, 0};
    float angle = 0.0f;
    Json points = Json::MakeArray();
    bool loop = false;
    bool isTrigger = false;
    bool oneWay = false;
    float friction = 0.6f;
    float bounciness = 0.0f;
    float density = 1.0f;
    int layer = 0;
    Json ignoreLayers = Json::MakeArray();
    static void Reflect(FieldList& f) {
        f.Add("shape", &Collider2D::shape, "Shape type.").options = {"box", "circle", "capsule", "polygon", "edge"};
        f.Add("size", &Collider2D::size, "Box: width and height in local units (x, y; z is ignored), multiplied by Transform.scale.");
        f.Add("radius", &Collider2D::radius, "Circle/capsule radius in local units.");
        f.Add("height", &Collider2D::height, "Capsule: total height along local Y, including the rounded ends.");
        f.Add("rounding", &Collider2D::rounding, "Box: corner radius (0 = sharp corners). Rounded boxes slide over bumps more smoothly.");
        f.Add("center", &Collider2D::center, "Offset of the shape from the entity origin (x, y).");
        f.Add("angle", &Collider2D::angle, "Box/capsule rotation around the shape center in degrees (added to the entity rotation).");
        f.Add("points", &Collider2D::points, "Polygon/edge points [[x, y], ...] in local units. Polygon: outline in any winding (concave is triangulated). Edge: a line strip, two-sided.");
        f.Add("loop", &Collider2D::loop, "Edge: close the strip into a loop (smooth one-sided outline, solid inside: good for terrain).");
        f.Add("isTrigger", &Collider2D::isTrigger, "Sensor: reports onTriggerEnter/onTriggerExit for moving bodies and characters instead of colliding.");
        f.Add("oneWay", &Collider2D::oneWay, "One-way platform: solid only from above (bodies and characters pass through from below and the sides).");
        FieldInfo& fr = f.Add("friction", &Collider2D::friction, "Surface friction (0 = ice, 1 = rubber).");
        fr.hasRange = true; fr.min = 0.0f; fr.max = 2.0f;
        FieldInfo& b = f.Add("bounciness", &Collider2D::bounciness, "Restitution (0 = no bounce, 1 = perfectly elastic).");
        b.hasRange = true; b.min = 0.0f; b.max = 1.0f;
        FieldInfo& d = f.Add("density", &Collider2D::density, "Relative density when a RigidBody2D has several shapes (the body's mass is RigidBody2D.mass).");
        d.hasRange = true; d.min = 0.0f; d.max = 1000.0f;
        FieldInfo& l = f.Add("layer", &Collider2D::layer, "Collision layer 0-15.");
        l.hasRange = true; l.min = 0; l.max = 15;
        f.Add("ignoreLayers", &Collider2D::ignoreLayers, "Layers this shape does not collide with, e.g. [2, 3]. Two shapes collide unless either ignores the other's layer.");
    }
};

struct RigidBody2D {
    static constexpr const char* kTypeName = "RigidBody2D";
    static constexpr const char* kDoc = "Makes a Collider2D move in the XY plane (Box2D). dynamic = gravity and collisions; kinematic = follows its Transform (moving platforms, doors) and pushes dynamic bodies. For top-down games set gravityScale 0 and some linearDamping.";
    std::string type = "dynamic";
    float mass = 1.0f;
    Vec3 velocity{0, 0, 0};
    float angularVelocity = 0.0f;
    float gravityScale = 1.0f;
    float linearDamping = 0.0f;
    float angularDamping = 0.05f;
    bool bullet = false;
    bool lockPositionX = false, lockPositionY = false;
    bool lockRotationZ = false;
    static void Reflect(FieldList& f) {
        f.Add("type", &RigidBody2D::type, "Body type.").options = {"dynamic", "kinematic"};
        FieldInfo& m = f.Add("mass", &RigidBody2D::mass, "Mass in kg (dynamic bodies).");
        m.hasRange = true; m.min = 0.001f; m.max = 1.0e6f;
        f.Add("velocity", &RigidBody2D::velocity, "Linear velocity in m/s (x, y). Written by the simulation every step; set it to launch the body.");
        f.Add("angularVelocity", &RigidBody2D::angularVelocity, "Spin in degrees/s (counter-clockwise). Written by the simulation.");
        f.Add("gravityScale", &RigidBody2D::gravityScale, "Multiplier for gravity (0 = floats; top-down games).");
        f.Add("linearDamping", &RigidBody2D::linearDamping, "Slows the body down over time (top-down friction).");
        f.Add("angularDamping", &RigidBody2D::angularDamping, "Slows spinning down over time.");
        f.Add("bullet", &RigidBody2D::bullet, "Continuous collision against other moving bodies too (fast projectiles). Static geometry is always continuous.");
        f.Add("lockPositionX", &RigidBody2D::lockPositionX, "Never move along X (dynamic bodies): it only slides up and down.");
        f.Add("lockPositionY", &RigidBody2D::lockPositionY, "Never move along Y (dynamic bodies): it only slides sideways.");
        f.Add("lockRotationZ", &RigidBody2D::lockRotationZ, "Never rotate around Z, the only 2D rotation (keeps it upright).");
    }
    static bool Upgrade(Json& values) {  // fixedRotation of older files and calls
        if (!values.has("fixedRotation")) return false;
        values["lockRotationZ"] = Json(values["fixedRotation"].asBool(false));
        values.erase("fixedRotation");
        return true;
    }
};

struct CharacterBody2D {
    static constexpr const char* kTypeName = "CharacterBody2D";
    static constexpr const char* kDoc = "2D character for players/NPCs (Box2D mover): set velocity and the engine moves it, sliding along walls and slopes, standing on moving platforms, passing one-way platforms from below and pushing dynamic bodies. platformer mode applies gravity and sets grounded; topdown mode has no gravity.";
    std::string mode = "platformer";
    std::string shape = "capsule";
    float radius = 0.4f;
    float height = 1.0f;
    Vec3 velocity{0, 0, 0};
    float gravityScale = 1.0f;
    float maxSlope = 50.0f;
    float pushStrength = 1.0f;
    bool grounded = false;
    bool onWall = false;
    bool onCeiling = false;
    bool dropThrough = false;
    int layer = 0;
    Json ignoreLayers = Json::MakeArray();
    static void Reflect(FieldList& f) {
        f.Add("mode", &CharacterBody2D::mode, "platformer: gravity pulls down -Y, grounded is tracked; topdown: no gravity, velocity x/y moves freely.").options = {"platformer", "topdown"};
        f.Add("shape", &CharacterBody2D::shape, "Shape centered on the entity origin.").options = {"capsule", "circle"};
        f.Add("radius", &CharacterBody2D::radius, "Radius in meters.");
        f.Add("height", &CharacterBody2D::height, "Capsule total height in meters (vertical).");
        f.Add("velocity", &CharacterBody2D::velocity, "Desired velocity in m/s (x, y); after each step it holds the actual velocity (stopped by walls, ceilings, ground).");
        f.Add("gravityScale", &CharacterBody2D::gravityScale, "Multiplier for gravity (platformer mode). 2-3 gives a snappy jump arc.");
        f.Add("maxSlope", &CharacterBody2D::maxSlope, "Steepest walkable slope in degrees (steeper surfaces are walls).");
        f.Add("pushStrength", &CharacterBody2D::pushStrength, "How hard the character pushes dynamic bodies it walks into (0 = not at all).");
        f.Add("grounded", &CharacterBody2D::grounded, "Runtime state: standing on walkable ground (platformer mode).");
        f.Add("onWall", &CharacterBody2D::onWall, "Runtime state: touching a wall (steeper than maxSlope) this step.");
        f.Add("onCeiling", &CharacterBody2D::onCeiling, "Runtime state: hit a ceiling this step.");
        f.Add("dropThrough", &CharacterBody2D::dropThrough, "Set true to fall through one-way platforms (e.g. Down + Jump); reset it when the character is below.");
        FieldInfo& l = f.Add("layer", &CharacterBody2D::layer, "Collision layer 0-15.");
        l.hasRange = true; l.min = 0; l.max = 15;
        f.Add("ignoreLayers", &CharacterBody2D::ignoreLayers, "Layers the character does not collide with.");
    }
};

// ----- Prefabs ------------------------------------------------------------------

struct Joint {
    static constexpr const char* kTypeName = "Joint";
    static constexpr const char* kDoc = "Connects this entity's body (Collider + RigidBody) to another body, or to the world when `connected` is 0 (Jolt): hinge = doors, wheels, levers; ball = ropes, chains, pendulums; slider = pistons, drawers, lifts; distance = rods, ropes, springs; fixed = glued parts that can break. The bodies are joined as they stand when the joint is made. One per entity: a body with two joints carries one, the other body carries the second. See physics.joints.";
    std::string type = "hinge";
    EntityId connected = kNullEntity;
    Vec3 anchor;
    Vec3 connectedAnchor;
    bool autoAnchor = true;
    Vec3 axis{0, 1, 0};
    bool useLimits = false;
    float min = 0.0f, max = 0.0f;
    bool useMotor = false;
    float motorSpeed = 0.0f;
    float maxMotorForce = 100.0f;
    float frequency = 0.0f;
    float damping = 0.5f;
    bool collideConnected = false;
    float breakForce = 0.0f;
    bool broken = false;
    static void Reflect(FieldList& f) {
        f.Add("type", &Joint::type, "fixed = no relative motion; hinge = turns around `axis` through `anchor`; ball = turns freely around `anchor`; slider = moves along `axis` only; distance = keeps `anchor` and `connectedAnchor` a length apart.").options = {"fixed", "hinge", "ball", "slider", "distance"};
        f.Add("connected", &Joint::connected, "Entity id of the other body (Collider, with or without RigidBody). 0 = the world: the joint holds this body in place.");
        f.Add("anchor", &Joint::anchor, "Joint point on this entity, in its local units (like Collider.center): the hinge pin, the ball, the rope's end.");
        f.Add("connectedAnchor", &Joint::connectedAnchor, "Joint point on the connected entity in its local units (world coordinates when connected is 0). Used when autoAnchor is false; a distance joint needs it.");
        f.Add("autoAnchor", &Joint::autoAnchor, "true = the connected anchor is the same world point as `anchor` when the joint is made. Set false for distance joints and to pull two bodies together.");
        f.Add("axis", &Joint::axis, "hinge: the pin's direction; slider: the direction of travel; ball with limits: the cone's middle. In this entity's local axes.");
        f.Add("useLimits", &Joint::useLimits, "Enables min/max. hinge: angles in degrees from the starting pose (min -180..0, max 0..180); slider: meters from the start (min <= 0 <= max); distance: shortest and longest length (a rope is min 0, max = its length); ball: max = how far it may swing from `axis` in degrees.");
        f.Add("min", &Joint::min, "Lower limit, see useLimits.");
        f.Add("max", &Joint::max, "Upper limit, see useLimits.");
        f.Add("useMotor", &Joint::useMotor, "hinge/slider: drive the joint at motorSpeed (wheels, fans, lifts). May be changed every frame.");
        f.Add("motorSpeed", &Joint::motorSpeed, "Target speed: degrees/s (hinge) or m/s (slider).");
        f.Add("maxMotorForce", &Joint::maxMotorForce, "Strongest torque (hinge, N m) or force (slider, N) the motor uses; a heavy body needs more.");
        f.Add("frequency", &Joint::frequency, "Spring in Hz. distance: above 0 the rod is a spring; hinge/slider: soft limits. 0 = rigid.");
        f.Add("damping", &Joint::damping, "Spring damping ratio (0 = keeps bouncing, 1 = no overshoot).");
        f.Add("collideConnected", &Joint::collideConnected, "Whether the two jointed bodies collide with each other (a door and its frame usually should not).");
        f.Add("breakForce", &Joint::breakForce, "The joint breaks when it has to hold more than this force in N (0 = never).");
        f.Add("broken", &Joint::broken, "Runtime state: true after the joint broke; set false to join the bodies again as they stand.");
    }
};

struct Joint2D {
    static constexpr const char* kTypeName = "Joint2D";
    static constexpr const char* kDoc = "Connects this entity's 2D body (Collider2D + RigidBody2D) to another 2D body, or to the world when `connected` is 0 (Box2D): hinge = swings, wheels, flippers; slider = lifts, pistons; distance = rods, ropes, springs; wheel = a car wheel with suspension; fixed = glued parts that can break. The bodies are joined as they stand when the joint is made. One per entity. See physics.joints.";
    std::string type = "hinge";
    EntityId connected = kNullEntity;
    Vec3 anchor;
    Vec3 connectedAnchor;
    bool autoAnchor = true;
    Vec3 axis{0, 1, 0};
    bool useLimits = false;
    float min = 0.0f, max = 0.0f;
    bool useMotor = false;
    float motorSpeed = 0.0f;
    float maxMotorForce = 100.0f;
    float frequency = 0.0f;
    float damping = 0.5f;
    bool collideConnected = false;
    float breakForce = 0.0f;
    bool broken = false;
    static void Reflect(FieldList& f) {
        f.Add("type", &Joint2D::type, "fixed = welded; hinge = turns around `anchor`; slider = moves along `axis` only, no turning; distance = keeps `anchor` and `connectedAnchor` a length apart; wheel = slides along `axis` on a spring and spins freely.").options = {"fixed", "hinge", "slider", "distance", "wheel"};
        f.Add("connected", &Joint2D::connected, "Entity id of the other 2D body (Collider2D, with or without RigidBody2D). 0 = the world: the joint holds this body in place.");
        f.Add("anchor", &Joint2D::anchor, "Joint point on this entity (x, y) in its local units: the hinge pin, the rope's end, the wheel axle.");
        f.Add("connectedAnchor", &Joint2D::connectedAnchor, "Joint point on the connected entity (x, y) in its local units (world coordinates when connected is 0). Used when autoAnchor is false; a distance joint needs it.");
        f.Add("autoAnchor", &Joint2D::autoAnchor, "true = the connected anchor is the same world point as `anchor` when the joint is made.");
        f.Add("axis", &Joint2D::axis, "slider/wheel: direction of travel (x, y) in this entity's local axes; [0,1,0] is a vertical suspension or lift.");
        f.Add("useLimits", &Joint2D::useLimits, "Enables min/max. hinge: angles in degrees from the starting pose; slider/wheel: meters from the start; distance: shortest and longest length (a rope is min 0, max = its length).");
        f.Add("min", &Joint2D::min, "Lower limit, see useLimits.");
        f.Add("max", &Joint2D::max, "Upper limit, see useLimits.");
        f.Add("useMotor", &Joint2D::useMotor, "hinge/slider/wheel: drive the joint at motorSpeed. May be changed every frame.");
        f.Add("motorSpeed", &Joint2D::motorSpeed, "Target speed: degrees/s counter-clockwise (hinge, wheel) or m/s (slider).");
        f.Add("maxMotorForce", &Joint2D::maxMotorForce, "Strongest torque (hinge, wheel: N m) or force (slider: N) the motor uses.");
        f.Add("frequency", &Joint2D::frequency, "Spring in Hz. distance: above 0 the rod is a spring; wheel: the suspension (0 = rigid); fixed: above 0 the weld flexes. 0 = rigid.");
        f.Add("damping", &Joint2D::damping, "Spring damping ratio (0 = keeps bouncing, 1 = no overshoot).");
        f.Add("collideConnected", &Joint2D::collideConnected, "Whether the two jointed bodies collide with each other.");
        f.Add("breakForce", &Joint2D::breakForce, "The joint breaks when it has to hold more than this force in N (0 = never).");
        f.Add("broken", &Joint2D::broken, "Runtime state: true after the joint broke; set false to join the bodies again as they stand.");
    }
};

struct Prefab {
    static constexpr const char* kTypeName = "Prefab";
    static constexpr const char* kDoc = "Marks the root of a prefab instance and remembers which prefab file it came from.";
    std::string path;
    static void Reflect(FieldList& f) { f.Add("path", &Prefab::path, "Prefab file, e.g. \"prefabs/coin.prefab.json\"."); }
};

// ----- In-game UI ----------------------------------------------------------------
// Screen-space UI laid out on a reference canvas (1280x720 unless a UICanvas
// says otherwise) scaled to the screen. Every element has the same placement
// fields: `anchor` picks the point of the parent rectangle the element hangs
// from and its pivot (or a stretch mode), x/y offset it in reference pixels
// (+x right, +y down), width/height size it. The parent rectangle is the
// nearest ancestor entity with a UI element, else the screen, so panels can
// hold buttons and text. Siblings draw in `order`, then id; children draw on
// top of their parent. A UILayout on the parent arranges children instead
// (in hierarchy order). A UIPanel with opacity 0 is an invisible container.

inline const std::vector<std::string>& UIAnchors() {
    static const std::vector<std::string> a = {"top-left", "top", "top-right", "left", "center", "right", "bottom-left", "bottom", "bottom-right",
                                               "stretch-top", "stretch-middle", "stretch-bottom", "stretch-left", "stretch-center", "stretch-right", "stretch"};
    return a;
}

constexpr const char* kUIAnchorDoc =
    "Where the element hangs in its parent (the screen or the parent UI element) and its pivot. stretch-* modes stretch along an axis: "
    "there width/height is added to the parent's size (-40 leaves 20 px on each side).";

// Placement fields shared by all UI components.
template <class T>
void ReflectUIPlacement(FieldList& f, const char* sizeDoc) {
    f.Add("anchor", &T::anchor, kUIAnchorDoc).options = UIAnchors();
    f.Add("x", &T::x, "Horizontal offset in reference pixels (canvas is 1280x720 by default).");
    f.Add("y", &T::y, "Vertical offset in reference pixels (+y is down).");
    f.Add("width", &T::width, sizeDoc);
    f.Add("height", &T::height, sizeDoc);
}

template <class T>
void ReflectUICommon(FieldList& f) {
    FieldInfo& o = f.Add("opacity", &T::opacity, "0 = invisible, 1 = opaque (this element only; children keep theirs).");
    o.hasRange = true;
    o.min = 0.0f;
    o.max = 1.0f;
    f.Add("visible", &T::visible, "Hidden elements (and their children) are not drawn and cannot be clicked.");
    f.Add("order", &T::order, "Draw order among siblings (higher on top).");
}

// Image look shared by UI elements (Unity's sliced Image, Unreal's box brush, Godot's StyleBoxTexture): a texture
// replaces the solid background and every other image of the element uses the same slicing and sampling.
template <class T>
void ReflectUISkin(FieldList& f) {
    f.Add("texture", &T::texture, "Background image (png/jpg/bmp/tga); the color then tints it (white = unchanged). Empty = solid color.");
    f.Add("slice", &T::slice, "9-slice border of the element's images in image pixels: corners keep their size, edges and center stretch (0 = plain stretch).");
    f.Add("sliceScale", &T::sliceScale, "Size of the 9-slice border on screen, in reference pixels per image pixel.");
    f.Add("pixelArt", &T::pixelArt, "Nearest-neighbour sampling of the element's images (sharp pixels).");
}

constexpr const char* kUIFontDoc =
    "\"default\" (built-in Roboto), \"pixel\" (built-in 5x7 pixel font, ASCII) or a font file in the project (assets/fonts/x.ttf, .otf, .ttc). "
    "Characters a font lacks fall back to the default font.";

struct UIText {
    static constexpr const char* kTypeName = "UIText";
    static constexpr const char* kDoc = "Screen-space text: TrueType fonts (any language the font covers), alignment, wrapping, outline, shadow, rich text.";
    std::string text = "Text";
    std::string font = "default";
    float size = 32.0f;
    Color color{1, 1, 1};
    std::string anchor = "top-left";
    float x = 24.0f;
    float y = 24.0f;
    float width = 0.0f;
    float height = 0.0f;
    std::string align = "auto";
    std::string verticalAlign = "top";
    bool wrap = true;
    float lineSpacing = 1.0f;
    float letterSpacing = 0.0f;
    bool bold = false;
    bool richText = true;
    float outlineWidth = 0.0f;
    Color outlineColor{0, 0, 0};
    float shadowDistance = 0.0f;
    Color shadowColor{0, 0, 0};
    int visibleCharacters = -1;
    float minSize = 0.0f;
    float opacity = 1.0f;
    bool visible = true;
    int order = 0;
    static void Reflect(FieldList& f) {
        f.Add("text", &UIText::text, "Text to show (UTF-8). \\n starts a new line. With richText: <color=#ff8800>..</color>, <b>..</b>.");
        f.Add("font", &UIText::font, kUIFontDoc);
        f.Add("size", &UIText::size, "Font size in reference pixels (em height; for \"pixel\" the line height).");
        f.Add("color", &UIText::color, "Text color.");
        ReflectUIPlacement<UIText>(f, "Box size in reference pixels; 0 = fit the text. A width makes text wrap and align inside it.");
        f.Add("align", &UIText::align, "Horizontal alignment of lines; auto follows the anchor (left/center/right).").options = {"auto", "left", "center", "right"};
        f.Add("verticalAlign", &UIText::verticalAlign, "Vertical alignment inside a box with a height.").options = {"top", "middle", "bottom"};
        f.Add("wrap", &UIText::wrap, "Break lines at word boundaries to fit width (when width > 0).");
        f.Add("lineSpacing", &UIText::lineSpacing, "Line height multiplier.");
        f.Add("letterSpacing", &UIText::letterSpacing, "Extra space between characters in reference pixels.");
        f.Add("bold", &UIText::bold, "Synthetic bold.");
        f.Add("richText", &UIText::richText, "Interpret <color=..> and <b> tags.");
        f.Add("outlineWidth", &UIText::outlineWidth, "Outline thickness in reference pixels (0 = none).");
        f.Add("outlineColor", &UIText::outlineColor, "Outline color.");
        f.Add("shadowDistance", &UIText::shadowDistance, "Drop shadow offset (right and down) in reference pixels (0 = none).");
        f.Add("shadowColor", &UIText::shadowColor, "Drop shadow color (drawn at half the text opacity).");
        f.Add("minSize", &UIText::minSize, "Shrink the font down to this size until the text fits its width/height box (0 = never shrink).");
        f.Add("visibleCharacters", &UIText::visibleCharacters, "Show only the first N characters (typewriter effects); -1 = all.");
        ReflectUICommon<UIText>(f);
    }
};

struct UIPanel {
    static constexpr const char* kTypeName = "UIPanel";
    static constexpr const char* kDoc = "Screen-space rectangle (backgrounds, windows, bars): a solid color or a 9-slice image, rounded corners, a border. Parent of other UI elements.";
    std::string anchor = "top-left";
    float x = 16.0f;
    float y = 16.0f;
    float width = 240.0f;
    float height = 64.0f;
    Color color{0, 0, 0};
    std::string texture;
    float slice = 0.0f;
    float sliceScale = 1.0f;
    bool pixelArt = false;
    float opacity = 0.5f;
    float radius = 0.0f;
    float borderWidth = 0.0f;
    Color borderColor{1, 1, 1};
    bool clip = false;
    bool blockInput = false;
    bool visible = true;
    int order = -1;
    static void Reflect(FieldList& f) {
        ReflectUIPlacement<UIPanel>(f, "Size in reference pixels.");
        f.Add("color", &UIPanel::color, "Fill color.");
        ReflectUISkin<UIPanel>(f);
        f.Add("radius", &UIPanel::radius, "Corner radius in reference pixels.");
        f.Add("borderWidth", &UIPanel::borderWidth, "Border thickness in reference pixels (0 = none).");
        f.Add("borderColor", &UIPanel::borderColor, "Border color.");
        f.Add("blockInput", &UIPanel::blockInput,
              "Modal backdrop: buttons and sliders drawn under this panel cannot be clicked or hovered; elements drawn over it (its children) can.");
        f.Add("clip", &UIPanel::clip, "Children are cut off at the panel's edges (and cannot be clicked outside it).");
        ReflectUICommon<UIPanel>(f);
    }
};

struct UIButton {
    static constexpr const char* kTypeName = "UIButton";
    static constexpr const char* kDoc =
        "Clickable screen-space button: a solid or textured (9-slice) background with a look per state (normal, hover, pressed, disabled, on), "
        "a label and an optional icon. A click calls onClick(self) on the entity's Script; the pointer entering/leaving calls "
        "onPointerEnter/onPointerExit; a toggle button calls onToggle(self, on). Click it from tools with input.click (ui.layout lists where it is).";
    std::string text = "Button";
    std::string font = "default";
    float size = 28.0f;
    Color textColor{1, 1, 1};
    std::string align = "center";
    float padding = 8.0f;
    std::string icon;
    float iconSize = 0.0f;
    std::string iconSide = "left";
    std::string anchor = "center";
    float x = 0.0f;
    float y = 0.0f;
    float width = 240.0f;
    float height = 64.0f;
    Color color{0.25f, 0.45f, 0.9f};
    std::string texture;
    float slice = 0.0f;
    float sliceScale = 1.0f;
    bool pixelArt = false;
    float radius = 8.0f;
    float borderWidth = 0.0f;
    Color borderColor{1, 1, 1};
    float hoverBrightness = 1.15f;
    float pressedBrightness = 0.85f;
    std::string hoverTexture;
    std::string pressedTexture;
    std::string disabledTexture;
    float hoverScale = 1.0f;
    float pressedScale = 1.0f;
    bool interactable = true;
    std::string clickOn = "press";
    bool toggle = false;
    bool on = false;
    std::string group;
    Color onColor{0.2f, 0.7f, 0.4f};
    std::string onTexture;
    std::string key;
    float opacity = 1.0f;
    bool visible = true;
    int order = 10;
    // Runtime state set by the simulation (not saved).
    bool hovered = false;
    bool pressed = false;
    bool focused = false;   // has the keyboard/gamepad focus (UICanvas.navigation)
    float scaleNow = 1.0f;  // eases toward hoverScale / pressedScale
    static void Reflect(FieldList& f) {
        f.Add("text", &UIButton::text, "Label (same rich text as UIText). Empty = no label (icon buttons).");
        f.Add("font", &UIButton::font, kUIFontDoc);
        f.Add("size", &UIButton::size, "Label font size in reference pixels.");
        f.Add("textColor", &UIButton::textColor, "Label color.");
        f.Add("align", &UIButton::align, "Where the label (and icon) sits across the button.").options = {"left", "center", "right"};
        f.Add("padding", &UIButton::padding, "Space kept free at the left and right edge in reference pixels.");
        f.Add("icon", &UIButton::icon, "Image drawn next to the label (png/jpg/bmp/tga). Empty = none.");
        f.Add("iconSize", &UIButton::iconSize, "Icon height in reference pixels, the width follows the image; 0 = the image's pixel size.");
        f.Add("iconSide", &UIButton::iconSide, "Side of the label the icon is on.").options = {"left", "right", "top", "bottom"};
        ReflectUIPlacement<UIButton>(f, "Size in reference pixels.");
        f.Add("color", &UIButton::color, "Background color.");
        ReflectUISkin<UIButton>(f);
        f.Add("radius", &UIButton::radius, "Corner radius in reference pixels.");
        f.Add("borderWidth", &UIButton::borderWidth, "Border thickness in reference pixels (0 = none).");
        f.Add("borderColor", &UIButton::borderColor, "Border color.");
        f.Add("hoverBrightness", &UIButton::hoverBrightness, "Background brightness while the pointer is over the button.");
        f.Add("pressedBrightness", &UIButton::pressedBrightness, "Background brightness while pressed.");
        f.Add("hoverTexture", &UIButton::hoverTexture, "Background image while the pointer is over the button. Empty = texture.");
        f.Add("pressedTexture", &UIButton::pressedTexture, "Background image while pressed. Empty = texture.");
        f.Add("disabledTexture", &UIButton::disabledTexture, "Background image while not interactable. Empty = texture.");
        f.Add("hoverScale", &UIButton::hoverScale, "Size multiplier while the pointer is over the button (1.05 = grows a little); eased, also while the game is paused.");
        f.Add("pressedScale", &UIButton::pressedScale, "Size multiplier while pressed (0.95 = squeezes).");
        f.Add("interactable", &UIButton::interactable, "Disabled buttons are drawn faded and ignore clicks.");
        f.Add("clickOn", &UIButton::clickOn,
              "When the click happens: press = as the pointer goes down; release = when it comes up while still over the button "
              "(moving off before releasing cancels).").options = {"press", "release"};
        f.Add("toggle", &UIButton::toggle, "A click flips `on` and calls onToggle(self, on) (check boxes, tabs, switches).");
        f.Add("on", &UIButton::on, "Toggle state. Scripts may set it; the group rule is applied on clicks only.");
        f.Add("group", &UIButton::group,
              "Radio group: turning a toggle on turns off every other toggle in the scene with the same group name, and a click cannot "
              "turn the one that is on off. Empty = independent.");
        f.Add("onColor", &UIButton::onColor, "Background color while on (replaces color).");
        f.Add("onTexture", &UIButton::onTexture, "Background image while on. Empty = texture.");
        f.Add("key", &UIButton::key,
              "On-screen control: while the mouse or any finger holds the button, this key is down (input.down / input.pressed, "
              "CharacterBody controls), e.g. \"Left\", \"Space\". Several fingers hold several buttons at once. Empty = none.");
        ReflectUICommon<UIButton>(f);
    }
};

struct UIImage {
    static constexpr const char* kTypeName = "UIImage";
    static constexpr const char* kDoc = "Screen-space image: icons, portraits, 9-slice frames, fill bars (fill + fillOrigin), sprite sheet frames.";
    std::string texture;
    Color color{1, 1, 1};
    std::string anchor = "center";
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    int frame = 0;
    int columns = 1;
    int rows = 1;
    float slice = 0.0f;
    float sliceScale = 1.0f;
    bool preserveAspect = false;
    bool pixelArt = false;
    float fill = 1.0f;
    std::string fillOrigin = "left";
    float radius = 0.0f;
    float opacity = 1.0f;
    bool visible = true;
    int order = 0;
    static void Reflect(FieldList& f) {
        f.Add("texture", &UIImage::texture, "Image file (png/jpg/bmp/tga). Empty = solid color.");
        f.Add("color", &UIImage::color, "Tint (multiplies the image).");
        ReflectUIPlacement<UIImage>(f, "Size in reference pixels; 0 = the image's (frame's) pixel size.");
        f.Add("frame", &UIImage::frame, "Frame of a sprite sheet (row by row from the top-left).");
        f.Add("columns", &UIImage::columns, "Sheet columns.");
        f.Add("rows", &UIImage::rows, "Sheet rows.");
        f.Add("slice", &UIImage::slice, "9-slice border in image pixels: corners keep their size, edges and center stretch (0 = plain stretch).");
        f.Add("sliceScale", &UIImage::sliceScale, "Size of the 9-slice border on screen, in reference pixels per image pixel.");
        f.Add("preserveAspect", &UIImage::preserveAspect, "Fit the image inside the box without distorting it.");
        f.Add("pixelArt", &UIImage::pixelArt, "Nearest-neighbour sampling (sharp pixels).");
        FieldInfo& fl = f.Add("fill", &UIImage::fill, "Visible fraction (health bars, cooldowns).");
        fl.hasRange = true;
        fl.min = 0.0f;
        fl.max = 1.0f;
        f.Add("fillOrigin", &UIImage::fillOrigin, "Side the fill grows from.").options = {"left", "right", "top", "bottom"};
        f.Add("radius", &UIImage::radius, "Corner radius in reference pixels (round avatars, pills).");
        ReflectUICommon<UIImage>(f);
    }
};

struct UISlider {
    static constexpr const char* kTypeName = "UISlider";
    static constexpr const char* kDoc =
        "Bar showing value in [min, max]: a draggable slider (calls onValueChanged(self, value) on the entity's Script) or, with interactable "
        "false, a progress/health bar.";
    float value = 0.5f;
    float min = 0.0f;
    float max = 1.0f;
    float step = 0.0f;
    std::string direction = "left-to-right";
    std::string anchor = "center";
    float x = 0.0f;
    float y = 0.0f;
    float width = 320.0f;
    float height = 24.0f;
    Color color{0.12f, 0.13f, 0.16f};
    Color fillColor{0.25f, 0.45f, 0.9f};
    bool handle = true;
    Color handleColor{1, 1, 1};
    std::string texture;
    float slice = 0.0f;
    float sliceScale = 1.0f;
    bool pixelArt = false;
    std::string fillTexture;
    std::string handleTexture;
    float radius = 12.0f;
    bool interactable = true;
    float opacity = 1.0f;
    bool visible = true;
    int order = 10;
    // Runtime state set by the simulation (not saved).
    bool hovered = false;
    bool pressed = false;
    bool focused = false;  // has the keyboard/gamepad focus (UICanvas.navigation)
    static void Reflect(FieldList& f) {
        f.Add("value", &UISlider::value, "Current value.");
        f.Add("min", &UISlider::min, "Value at the start.");
        f.Add("max", &UISlider::max, "Value at the end.");
        f.Add("step", &UISlider::step, "Snap dragged values to multiples of this (0 = continuous).");
        f.Add("direction", &UISlider::direction, "Fill direction.").options = {"left-to-right", "right-to-left", "bottom-to-top", "top-to-bottom"};
        ReflectUIPlacement<UISlider>(f, "Size in reference pixels.");
        f.Add("color", &UISlider::color, "Track (background) color.");
        f.Add("fillColor", &UISlider::fillColor, "Filled part color.");
        f.Add("handle", &UISlider::handle, "Draw a round handle at the value.");
        f.Add("handleColor", &UISlider::handleColor, "Handle color.");
        ReflectUISkin<UISlider>(f);
        f.Add("fillTexture", &UISlider::fillTexture, "Image of the filled part (tinted by fillColor), laid over the whole bar and cut at the value. Empty = solid color.");
        f.Add("handleTexture", &UISlider::handleTexture, "Image of the handle (tinted by handleColor), drawn at its own proportions. Empty = a round handle.");
        f.Add("radius", &UISlider::radius, "Corner radius in reference pixels.");
        f.Add("interactable", &UISlider::interactable, "Drag to change the value; false = display only (progress bar).");
        ReflectUICommon<UISlider>(f);
    }
};

struct UIInput {
    static constexpr const char* kTypeName = "UIInput";
    static constexpr const char* kDoc =
        "Text field (player names, chat, codes, notes). A click gives it the focus and places the caret; arrows, Home/End, Shift+arrows or a drag "
        "select, Ctrl+A/C/X/V use the clipboard, Enter calls onSubmit(self, text), Escape or a click elsewhere leaves it. Every change calls "
        "onTextChanged(self, text). While it has the focus scripts do not see the keyboard. Tools type with input.text.";
    std::string text;
    std::string placeholder = "Enter text";
    std::string font = "default";
    float size = 24.0f;
    Color textColor{1, 1, 1};
    std::string anchor = "center";
    float x = 0.0f;
    float y = 0.0f;
    float width = 320.0f;
    float height = 48.0f;
    Color color{0.12f, 0.13f, 0.16f};
    std::string texture;
    float slice = 0.0f;
    float sliceScale = 1.0f;
    bool pixelArt = false;
    float radius = 8.0f;
    float borderWidth = 2.0f;
    Color borderColor{0.4f, 0.45f, 0.55f};
    int maxLength = 0;
    bool password = false;
    bool multiline = false;
    bool interactable = true;
    float opacity = 1.0f;
    bool visible = true;
    int order = 10;
    // Runtime state set by the simulation (not saved).
    bool hovered = false;
    bool focused = false;
    float caret = 0.0f;  // seconds since the caret last moved (blink)
    int caretAt = 0;     // byte offset of the caret in `text`
    int selectAt = 0;    // other end of the selection (== caretAt: nothing selected)
    std::string composing;  // what an input method is composing at the caret (InputState::composing): drawn underlined, not in `text` yet
    static void Reflect(FieldList& f) {
        f.Add("text", &UIInput::text, "What the player typed (UTF-8). Scripts may set it.");
        f.Add("placeholder", &UIInput::placeholder, "Hint shown faded while the field is empty and not focused.");
        f.Add("font", &UIInput::font, kUIFontDoc);
        f.Add("size", &UIInput::size, "Font size in reference pixels.");
        f.Add("textColor", &UIInput::textColor, "Text and caret color.");
        ReflectUIPlacement<UIInput>(f, "Size in reference pixels.");
        f.Add("color", &UIInput::color, "Background color.");
        ReflectUISkin<UIInput>(f);
        f.Add("radius", &UIInput::radius, "Corner radius in reference pixels.");
        f.Add("borderWidth", &UIInput::borderWidth, "Border thickness in reference pixels (0 = none).");
        f.Add("borderColor", &UIInput::borderColor, "Border color.");
        f.Add("maxLength", &UIInput::maxLength, "Most characters the player can type (0 = no limit).");
        f.Add("password", &UIInput::password, "Show every character as * (and never copy it).");
        f.Add("multiline", &UIInput::multiline, "Several lines: the text wraps at the width, Enter starts a new line and Ctrl+Enter submits.");
        f.Add("interactable", &UIInput::interactable, "Disabled fields are drawn faded and cannot be focused.");
        ReflectUICommon<UIInput>(f);
    }
};

struct UIDropdown {
    static constexpr const char* kTypeName = "UIDropdown";
    static constexpr const char* kDoc =
        "Drop-down list (language, difficulty, resolution): shows the selected option; a click opens the list on top of everything else, a click "
        "on a row selects it and calls onValueChanged(self, index) on the entity's Script, a click elsewhere closes it.";
    Json options = Json::MakeArray();
    int selected = 0;
    std::string font = "default";
    float size = 24.0f;
    Color textColor{1, 1, 1};
    std::string anchor = "center";
    float x = 0.0f;
    float y = 0.0f;
    float width = 240.0f;
    float height = 48.0f;
    Color color{0.18f, 0.2f, 0.26f};
    std::string texture;
    float slice = 0.0f;
    float sliceScale = 1.0f;
    bool pixelArt = false;
    float radius = 8.0f;
    float borderWidth = 0.0f;
    Color borderColor{1, 1, 1};
    bool interactable = true;
    float opacity = 1.0f;
    bool visible = true;
    int order = 10;
    // Runtime state set by the simulation (not saved).
    bool hovered = false;
    bool focused = false;
    bool open = false;
    int hoverItem = -1;  // row under the pointer while open
    int first = 0;       // first row shown when the list is longer than the screen
    static void Reflect(FieldList& f) {
        f.Add("options", &UIDropdown::options, "The choices, e.g. [\"Easy\", \"Normal\", \"Hard\"]. The open list has one row per option, each as tall as the element; a list longer than the screen scrolls with the wheel or a drag.");
        f.Add("selected", &UIDropdown::selected, "Index of the chosen option (0 = the first).");
        f.Add("font", &UIDropdown::font, kUIFontDoc);
        f.Add("size", &UIDropdown::size, "Font size in reference pixels.");
        f.Add("textColor", &UIDropdown::textColor, "Text color.");
        ReflectUIPlacement<UIDropdown>(f, "Size in reference pixels.");
        f.Add("color", &UIDropdown::color, "Background color (the list is a little darker, the row under the pointer lighter).");
        ReflectUISkin<UIDropdown>(f);
        f.Add("radius", &UIDropdown::radius, "Corner radius in reference pixels.");
        f.Add("borderWidth", &UIDropdown::borderWidth, "Border thickness in reference pixels (0 = none).");
        f.Add("borderColor", &UIDropdown::borderColor, "Border color.");
        f.Add("interactable", &UIDropdown::interactable, "Disabled drop-downs are drawn faded and cannot be opened.");
        ReflectUICommon<UIDropdown>(f);
    }
};

struct UIJoystick {
    static constexpr const char* kTypeName = "UIJoystick";
    static constexpr const char* kDoc =
        "On-screen stick for touch screens: while a finger (or the mouse) that went down on it is held, two gamepad axes follow it, so "
        "input.axis(\"LeftX\") works the same with a gamepad and on a phone. Releasing returns the axes to 0.";
    static constexpr int kNoPointer = -1;
    static constexpr int kMousePointer = -2;
    std::string anchor = "bottom-left";
    float x = 40.0f;
    float y = -40.0f;
    float width = 200.0f;
    float height = 200.0f;
    std::string axisX = "LeftX";
    std::string axisY = "LeftY";
    Color color{1, 1, 1};
    Color handleColor{1, 1, 1};
    std::string texture;
    std::string handleTexture;
    bool pixelArt = false;
    float opacity = 0.3f;
    bool visible = true;
    int order = 10;
    // Runtime state set by the simulation (not saved).
    float valueX = 0.0f;      // -1 left .. 1 right
    float valueY = 0.0f;      // -1 down .. 1 up
    int held = kNoPointer;    // touch id holding the stick, or kMousePointer
    static void Reflect(FieldList& f) {
        ReflectUIPlacement<UIJoystick>(f, "Size in reference pixels (the stick is the circle inside this box).");
        f.Add("axisX", &UIJoystick::axisX, "Axis the horizontal position drives (+ = right).").options = {"LeftX", "RightX"};
        f.Add("axisY", &UIJoystick::axisY, "Axis the vertical position drives (+ = up).").options = {"LeftY", "RightY"};
        f.Add("color", &UIJoystick::color, "Base color.");
        f.Add("handleColor", &UIJoystick::handleColor, "Handle color (drawn at twice the base opacity).");
        f.Add("texture", &UIJoystick::texture, "Image of the base (tinted by color). Empty = a disc.");
        f.Add("handleTexture", &UIJoystick::handleTexture, "Image of the handle (tinted by handleColor). Empty = a disc.");
        f.Add("pixelArt", &UIJoystick::pixelArt, "Nearest-neighbour sampling of the images (sharp pixels).");
        ReflectUICommon<UIJoystick>(f);
    }
};

struct UILayout {
    static constexpr const char* kTypeName = "UILayout";
    static constexpr const char* kDoc =
        "Arranges the UI children of this entity's UI element in a column, row or grid (their anchor/x/y are ignored). "
        "fit resizes the element to its content (menus, lists, inventories).";
    std::string direction = "vertical";
    float spacing = 8.0f;
    float padding = 0.0f;
    int columns = 3;
    std::string align = "start";
    std::string crossAlign = "start";
    bool fit = false;
    static void Reflect(FieldList& f) {
        f.Add("direction", &UILayout::direction, "vertical (column), horizontal (row) or grid (rows of `columns` equal cells).").options = {"vertical", "horizontal", "grid"};
        f.Add("spacing", &UILayout::spacing, "Gap between children in reference pixels.");
        f.Add("padding", &UILayout::padding, "Inner margin on every side in reference pixels.");
        f.Add("columns", &UILayout::columns, "Cells per row for grid.");
        f.Add("align", &UILayout::align, "Where the children sit along the layout direction.").options = {"start", "center", "end"};
        f.Add("crossAlign", &UILayout::crossAlign, "Placement across the direction; stretch makes children as wide (tall) as the element.").options = {"start", "center", "end", "stretch"};
        f.Add("fit", &UILayout::fit, "Resize the element to wrap its children.");
    }
};

struct UIScroll {
    static constexpr const char* kTypeName = "UIScroll";
    static constexpr const char* kDoc =
        "Makes a UIPanel a scroll view: its children are clipped to the panel and move with `scroll`. The mouse wheel over the panel, "
        "dragging its empty area (or a finger) and the scroll bar change `scroll`. Combine with UILayout for lists.";
    std::string direction = "vertical";
    float scroll = 0.0f;
    float wheelStep = 60.0f;
    bool bar = true;
    Color barColor{1, 1, 1};
    static void Reflect(FieldList& f) {
        f.Add("direction", &UIScroll::direction, "Axis the content scrolls along.").options = {"vertical", "horizontal"};
        f.Add("scroll", &UIScroll::scroll, "Offset in reference pixels from the start of the content; kept inside the content by the engine while simulating.");
        f.Add("wheelStep", &UIScroll::wheelStep, "Reference pixels per mouse wheel notch.");
        f.Add("bar", &UIScroll::bar, "Draw a scroll bar thumb when the content is larger than the panel.");
        f.Add("barColor", &UIScroll::barColor, "Scroll bar colour.");
    }
};

struct UIMotion {
    static constexpr const char* kTypeName = "UIMotion";
    static constexpr const char* kDoc =
        "Entrance animation of a UI element and its children: plays each time the element becomes visible (screens, dialogs, toasts). "
        "Runs on real frames, so it also plays while the game is paused. Outside a play session the element shows its final state.";
    std::string enter = "fade";
    float duration = 0.25f;
    float delay = 0.0f;
    float distance = 40.0f;
    float time = 1e9f;        // runtime: seconds since the element became visible
    bool wasVisible = false;  // runtime
    static void Reflect(FieldList& f) {
        f.Add("enter", &UIMotion::enter, "fade = opacity; pop = grows from 85% with a small overshoot; slide-* = moves in from `distance` away, all with a fade.")
            .options = {"none", "fade", "pop", "slide-up", "slide-down", "slide-left", "slide-right"};
        f.Add("duration", &UIMotion::duration, "Seconds the animation takes.");
        f.Add("delay", &UIMotion::delay, "Seconds to wait first (stagger a row of cards with increasing delays).");
        f.Add("distance", &UIMotion::distance, "Slide distance in reference pixels.");
        f.Add("time", &UIMotion::time, "Runtime: seconds since the element became visible; set 0 to replay.");
    }
};

struct UICanvas {
    static constexpr const char* kTypeName = "UICanvas";
    static constexpr const char* kDoc = "Optional, one per scene: the reference resolution the UI is authored for and how it scales to other screen sizes.";
    float referenceWidth = 1280.0f;
    float referenceHeight = 720.0f;
    float match = 1.0f;
    bool navigation = false;
    Color focusColor{1, 1, 1};
    static void Reflect(FieldList& f) {
        f.Add("referenceWidth", &UICanvas::referenceWidth, "Reference canvas width in pixels.");
        f.Add("referenceHeight", &UICanvas::referenceHeight, "Reference canvas height in pixels.");
        FieldInfo& m = f.Add("match", &UICanvas::match, "0 = scale with the screen width, 1 = with the height (default), in between = blend.");
        m.hasRange = true;
        m.min = 0.0f;
        m.max = 1.0f;
        f.Add("navigation", &UICanvas::navigation,
              "Keyboard/gamepad control of the UI: arrows or the D-pad move the focus between buttons and sliders (left/right changes a focused "
              "horizontal slider), Enter or GamepadA clicks the focused button. Off = pointer only.");
        f.Add("focusColor", &UICanvas::focusColor, "Color of the frame around the focused element.");
    }
};

// ----- Audio -----------------------------------------------------------------------

struct AudioSource {
    static constexpr const char* kTypeName = "AudioSource";
    static constexpr const char* kDoc = "Plays a WAV clip while simulating (music, ambience). One-shot effects are easier from Lua: audio.play(path).";
    std::string clip;
    float volume = 1.0f;
    float pitch = 1.0f;
    bool loop = false;
    bool playOnStart = true;
    static void Reflect(FieldList& f) {
        f.Add("clip", &AudioSource::clip, "WAV file relative to the project, e.g. \"sounds/music.wav\".");
        FieldInfo& v = f.Add("volume", &AudioSource::volume, "0..1 (up to 2 for boost).");
        v.hasRange = true; v.min = 0.0f; v.max = 2.0f;
        FieldInfo& p = f.Add("pitch", &AudioSource::pitch, "Playback speed (1 = normal).");
        p.hasRange = true; p.min = 0.1f; p.max = 4.0f;
        f.Add("loop", &AudioSource::loop, "Repeat forever.");
        f.Add("playOnStart", &AudioSource::playOnStart, "Start when the entity first appears in a play session.");
    }
};

// ----- Authoritative replication ---------------------------------------------------
struct NetSync {
    static constexpr const char* kTypeName = "NetSync";
    static constexpr const char* kDoc = "Server-owned replication policy. Fields use Component.field keys; clients cannot write authority.";
    int owner = 0;
    Json fields = Json::parse(R"({"Transform.position":{"onChange":true},"Transform.rotation":{"onChange":true}})");
    std::string prefab;
    float distance = 0;
    int team = 0;
    bool teamOnly = false;
    bool predict = true;
    static void Reflect(FieldList& f) {
        f.Add("owner", &NetSync::owner, "Controlling player id; 0 means server.");
        f.Add("fields", &NetSync::fields, "Component.field -> {always,onChange,ownerOnly,quantize}; at most 64 fields.");
        f.Add("prefab", &NetSync::prefab, "Optional project-relative prefab path for reliable spawn.");
        f.Add("distance", &NetSync::distance, "Relevance distance from receiver's NetPlayer (0 = unlimited).");
        f.Add("team", &NetSync::team, "Relevance team.");
        f.Add("teamOnly", &NetSync::teamOnly, "Show only to matching team, or owner.");
        f.Add("predict", &NetSync::predict, "Client predicts its owned entity; others are interpolated.");
    }
};
struct NetPlayer {
    static constexpr const char* kTypeName = "NetPlayer";
    static constexpr const char* kDoc = "Player entity assigned by the authoritative server; network.playerPrefab spawns one per ready participant.";
    int player = 0;
    int team = 0;
    static void Reflect(FieldList& f) {
        f.Add("player", &NetPlayer::player, "Authenticated player id assigned by the server.");
        f.Add("team", &NetPlayer::team, "Player's relevance team.");
    }
};

void RegisterBuiltinComponents();

}  // namespace oe
