#pragma once
#include <string>
#include <vector>

#include "scene/Reflect.h"

namespace oe {

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
    static constexpr const char* kDoc = "Draws a mesh: a built-in shape (cube, sphere, plane, pyramid, quad) or a glTF model file. Unknown meshes render as a magenta cube.";
    std::string mesh = "cube";
    std::string material;
    Color color{0.8f, 0.8f, 0.8f};
    float opacity = 1.0f;
    std::string texture;
    std::string shading = "smooth";
    bool unlit = false;
    bool castShadows = true;
    bool visible = true;
    static void Reflect(FieldList& f) {
        f.Add("mesh", &MeshRenderer::mesh, "Built-in name (cube, sphere, plane, pyramid, quad) or model path, e.g. \"assets/models/fox.glb\" (.glb/.gltf).");
        f.Add("material", &MeshRenderer::material,
              "Material file (*.mat.json: PBR color/metallic/roughness/normal/emissive maps, transparency) used for every part of the mesh. "
              "Empty = the model's own materials (glTF) or the default (white, roughness 0.7).");
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
    static void Reflect(FieldList& f) {
        f.Add("clip", &Animator::clip, "Exact model clip name; empty = default pose. animation.play changes clips and restarts by default.");
        f.Add("speed", &Animator::speed, "Playback multiplier; negative plays backwards, zero holds the pose.");
        f.Add("loop", &Animator::loop, "Wrap time at the clip duration; otherwise clamp and stop at either endpoint.");
        f.Add("playing", &Animator::playing, "Advance time while true; false holds the current pose.");
        f.Add("time", &Animator::time, "Seconds into the clip; set to seek. Direct clip edits preserve time.");
    }
};

// Runtime particle values are snapshots of the emitter settings at birth.
struct Particle {
    Vec3 position, velocity, gravity;
    float age = 0, lifetime = 1;
    float startSize = 0.1f, endSize = 0;
    Color startColor, endColor;
    float startOpacity = 1, endOpacity = 0;
    bool worldSpace = false;
};

struct ParticleEmitter {
    static constexpr const char* kTypeName = "ParticleEmitter";
    static constexpr const char* kDoc = "Deterministic 2D/3D billboard particles; initial burst and continuous rate, bounded per emitter.";
    float rate = 10;
    int burst = 0;
    float lifetime = 1;
    float speed = 1;
    float spread = 30;
    Vec3 direction{0, 1, 0};
    Vec3 gravity{0, -9.8f, 0};
    float startSize = 0.1f, endSize = 0;
    Color startColor{1, 1, 1}, endColor{1, 1, 1};
    float startOpacity = 1, endOpacity = 0;
    std::string texture;
    int frame = 0, columns = 1, rows = 1;
    std::string space = "local";
    int dimensions = 3;
    int seed = 1;
    int maxParticles = 256;
    bool loop = true;
    bool playing = true;
    std::vector<Particle> particles;  // runtime, not serialized
    uint32_t randomState = 0;        // runtime xorshift stream; zero = uninitialized
    double carry = 0;               // fractional rate births, bounded to [0,1)
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
        f.Add("direction", &ParticleEmitter::direction, "Emission direction; zero falls back to +Y. 2D uses XY only.");
        f.Add("gravity", &ParticleEmitter::gravity, "Acceleration in particle coordinates (world or local), captured at birth.");
        range(f.Add("startSize", &ParticleEmitter::startSize, "Billboard size in meters at birth."), 0, 10000);
        range(f.Add("endSize", &ParticleEmitter::endSize, "Billboard size at expiry."), 0, 10000);
        f.Add("startColor", &ParticleEmitter::startColor, "RGB tint at birth.");
        f.Add("endColor", &ParticleEmitter::endColor, "RGB tint at expiry.");
        range(f.Add("startOpacity", &ParticleEmitter::startOpacity, "Opacity at birth."), 0, 1);
        range(f.Add("endOpacity", &ParticleEmitter::endOpacity, "Opacity at expiry."), 0, 1);
        f.Add("texture", &ParticleEmitter::texture, "Optional texture/sprite sheet; empty uses solid quads.");
        range(f.Add("frame", &ParticleEmitter::frame, "Sprite sheet frame, row-major."), 0, 1000000);
        range(f.Add("columns", &ParticleEmitter::columns, "Sprite sheet columns."), 1, 4096);
        range(f.Add("rows", &ParticleEmitter::rows, "Sprite sheet rows."), 1, 4096);
        f.Add("space", &ParticleEmitter::space, "Local particles follow the emitter; world particles remain where spawned.").options = {"local", "world"};
        range(f.Add("dimensions", &ParticleEmitter::dimensions, "2 = XY fan/planar motion; 3 = cone in 3D."), 2, 3);
        f.Add("seed", &ParticleEmitter::seed, "Fixed seed mixed with entity id; changes take effect after particles.clear.");
        range(f.Add("maxParticles", &ParticleEmitter::maxParticles, "Maximum live particles per emitter; excess births are dropped."), 0, 10000);
        f.Add("loop", &ParticleEmitter::loop, "Enable continuous rate births; false emits only the initial burst.");
        f.Add("playing", &ParticleEmitter::playing, "Automatic emission switch; existing particles continue to age while false.");
    }
};

// ----- 2D --------------------------------------------------------------------------

struct Sprite {
    static constexpr const char* kTypeName = "Sprite";
    static constexpr const char* kDoc = "Draws an image (or one frame of a sprite sheet) on a quad facing +Z, placed at the entity. Transparent pixels are cut out. For 2D games use an orthographic Camera looking down -Z.";
    std::string texture;
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
    int order = 0;
    bool visible = true;
    static void Reflect(FieldList& f) {
        f.Add("texture", &Sprite::texture, "Image file (.png with transparency). A sprite sheet is a grid of equally sized frames.");
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
        f.Add("order", &Sprite::order, "Sorting among sprites at the same depth: higher is drawn in front.");
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
    static void Reflect(FieldList& f) {
        f.Add("clips", &SpriteAnimation::clips, "Clip name -> {frames: [indices], fps: number, loop: bool (default true)}.");
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
    float vignette = 0.0f;          // edge darkening strength; zero disables the effect
    float vignetteRadius = 0.75f;   // normalized distance from screen center
    float vignetteSoftness = 0.5f;  // smooth transition width
    static void Reflect(FieldList& f) {
        FieldInfo& exposureField = f.Add("exposure", &PostProcess::exposure, "Scene brightness multiplier before tone mapping. 1 preserves brightness.");
        exposureField.hasRange = true; exposureField.min = 0; exposureField.max = 32;
        f.Add("toneMapping", &PostProcess::toneMapping, "none (disabled) or reinhard (compress HDR channels as c / (1 + c)).").options = {"none", "reinhard"};
        FieldInfo& strength = f.Add("vignette", &PostProcess::vignette, "Edge darkening strength: 0 disables, 1 is fully dark outside the transition.");
        strength.hasRange = true; strength.min = 0; strength.max = 1;
        FieldInfo& radius = f.Add("vignetteRadius", &PostProcess::vignetteRadius, "Normalized radius: center 0, edge midpoint 1, corner sqrt(2).");
        radius.hasRange = true; radius.min = 0; radius.max = 1.5f;
        FieldInfo& softness = f.Add("vignetteSoftness", &PostProcess::vignetteSoftness, "Smooth transition width in normalized screen coordinates (minimum 0.01).");
        softness.hasRange = true; softness.min = 0.01f; softness.max = 2;
    }
};

struct DirectionalLight {
    static constexpr const char* kTypeName = "DirectionalLight";
    static constexpr const char* kDoc = "Sun-like light. Direction comes from the entity's rotation (local -Z).";
    Color color{1.0f, 0.97f, 0.9f};
    float intensity = 1.0f;
    Color ambient{0.18f, 0.2f, 0.25f};
    bool shadows = true;
    float shadowStrength = 0.75f;
    static void Reflect(FieldList& f) {
        f.Add("color", &DirectionalLight::color, "Light color.");
        f.Add("intensity", &DirectionalLight::intensity, "Brightness multiplier.");
        f.Add("ambient", &DirectionalLight::ambient, "Ambient light added to every surface.");
        f.Add("shadows", &DirectionalLight::shadows, "Cast shadows (first directional light only).");
        FieldInfo& s = f.Add("shadowStrength", &DirectionalLight::shadowStrength, "0 = no darkening, 1 = black shadows.");
        s.hasRange = true; s.min = 0.0f; s.max = 1.0f;
    }
};

struct PointLight {
    static constexpr const char* kTypeName = "PointLight";
    static constexpr const char* kDoc = "Light bulb at the entity position, fading to zero at `range`.";
    Color color{1.0f, 0.85f, 0.6f};
    float intensity = 1.5f;
    float range = 6.0f;
    static void Reflect(FieldList& f) {
        f.Add("color", &PointLight::color, "Light color.");
        f.Add("intensity", &PointLight::intensity, "Brightness at the center.");
        f.Add("range", &PointLight::range, "Distance in meters where the light reaches zero.");
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

struct Rotator {
    static constexpr const char* kTypeName = "Rotator";
    static constexpr const char* kDoc = "Behavior: spins the entity at a constant angular speed while simulating.";
    Vec3 degreesPerSecond{0, 45, 0};
    static void Reflect(FieldList& f) { f.Add("degreesPerSecond", &Rotator::degreesPerSecond, "Rotation speed per axis in degrees/second."); }
};

struct Velocity {
    static constexpr const char* kTypeName = "Velocity";
    static constexpr const char* kDoc = "Behavior: moves the entity linearly while simulating.";
    Vec3 linear{0, 0, 0};
    static void Reflect(FieldList& f) { f.Add("linear", &Velocity::linear, "Velocity in meters/second (world axes)."); }
};

struct PlayerController {
    static constexpr const char* kTypeName = "PlayerController";
    static constexpr const char* kDoc = "Behavior: moves the entity on the XZ plane with W/A/S/D (or arrow keys) and jumps with Space. Input can be injected through the input.* API.";
    float speed = 4.0f;
    float jumpSpeed = 5.0f;
    float gravity = 12.0f;
    float verticalVelocity = 0.0f;
    static void Reflect(FieldList& f) {
        f.Add("speed", &PlayerController::speed, "Move speed in meters/second.");
        f.Add("jumpSpeed", &PlayerController::jumpSpeed, "Initial upward speed when jumping.");
        f.Add("gravity", &PlayerController::gravity, "Downward acceleration while airborne (lands at y = 0 + half scale).");
        f.Add("verticalVelocity", &PlayerController::verticalVelocity, "Runtime state: current vertical speed.");
    }
};

struct Tag {
    static constexpr const char* kTypeName = "Tag";
    static constexpr const char* kDoc = "Free-form labels for gameplay queries (e.g. \"enemy\", \"pickup\").";
    std::string tags;
    static void Reflect(FieldList& f) { f.Add("tags", &Tag::tags, "Comma separated tags."); }
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
    bool lockRotation = false;
    bool continuous = false;
    bool plane2D = false;
    static void Reflect(FieldList& f) {
        f.Add("type", &RigidBody::type, "Body type.").options = {"dynamic", "kinematic"};
        FieldInfo& m = f.Add("mass", &RigidBody::mass, "Mass in kg (dynamic bodies).");
        m.hasRange = true; m.min = 0.001f; m.max = 1.0e6f;
        f.Add("velocity", &RigidBody::velocity, "Linear velocity in m/s. Written by the simulation every step; set it to launch the body.");
        f.Add("angularVelocity", &RigidBody::angularVelocity, "Angular velocity in degrees/s (world axes). Written by the simulation.");
        f.Add("gravityScale", &RigidBody::gravityScale, "Multiplier for gravity (0 = floats).");
        f.Add("linearDamping", &RigidBody::linearDamping, "Air resistance.");
        f.Add("lockRotation", &RigidBody::lockRotation, "Prevent the body from rotating (keeps it upright).");
        f.Add("continuous", &RigidBody::continuous, "Continuous collision detection for fast objects (prevents tunneling through thin walls).");
        f.Add("plane2D", &RigidBody::plane2D, "2D physics: move only in the XY plane and rotate only around Z.");
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
    bool plane2D = false;
    static void Reflect(FieldList& f) {
        f.Add("shape", &CharacterBody::shape, "Shape centered on the entity origin.").options = {"capsule", "sphere"};
        f.Add("radius", &CharacterBody::radius, "Radius in meters.");
        f.Add("height", &CharacterBody::height, "Capsule total height in meters.");
        f.Add("maxSlope", &CharacterBody::maxSlope, "Steepest walkable slope in degrees.");
        f.Add("stepHeight", &CharacterBody::stepHeight, "Highest step the character walks up automatically.");
        f.Add("gravityScale", &CharacterBody::gravityScale, "Multiplier for gravity.");
        f.Add("velocity", &CharacterBody::velocity, "Desired velocity in m/s; after each step it holds the actual velocity.");
        f.Add("grounded", &CharacterBody::grounded, "Runtime state: true while standing on walkable ground.");
        f.Add("plane2D", &CharacterBody::plane2D, "2D games: keep the character on its starting Z (velocity.z is ignored).");
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
    bool fixedRotation = false;
    bool bullet = false;
    static void Reflect(FieldList& f) {
        f.Add("type", &RigidBody2D::type, "Body type.").options = {"dynamic", "kinematic"};
        FieldInfo& m = f.Add("mass", &RigidBody2D::mass, "Mass in kg (dynamic bodies).");
        m.hasRange = true; m.min = 0.001f; m.max = 1.0e6f;
        f.Add("velocity", &RigidBody2D::velocity, "Linear velocity in m/s (x, y). Written by the simulation every step; set it to launch the body.");
        f.Add("angularVelocity", &RigidBody2D::angularVelocity, "Spin in degrees/s (counter-clockwise). Written by the simulation.");
        f.Add("gravityScale", &RigidBody2D::gravityScale, "Multiplier for gravity (0 = floats; top-down games).");
        f.Add("linearDamping", &RigidBody2D::linearDamping, "Slows the body down over time (top-down friction).");
        f.Add("angularDamping", &RigidBody2D::angularDamping, "Slows spinning down over time.");
        f.Add("fixedRotation", &RigidBody2D::fixedRotation, "Never rotate (keeps it upright).");
        f.Add("bullet", &RigidBody2D::bullet, "Continuous collision against other moving bodies too (fast projectiles). Static geometry is always continuous.");
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
        f.Add("visibleCharacters", &UIText::visibleCharacters, "Show only the first N characters (typewriter effects); -1 = all.");
        ReflectUICommon<UIText>(f);
    }
};

struct UIPanel {
    static constexpr const char* kTypeName = "UIPanel";
    static constexpr const char* kDoc = "Screen-space rectangle (backgrounds, windows, bars) with rounded corners and a border. Parent of other UI elements.";
    std::string anchor = "top-left";
    float x = 16.0f;
    float y = 16.0f;
    float width = 240.0f;
    float height = 64.0f;
    Color color{0, 0, 0};
    float opacity = 0.5f;
    float radius = 0.0f;
    float borderWidth = 0.0f;
    Color borderColor{1, 1, 1};
    bool clip = false;
    bool visible = true;
    int order = -1;
    static void Reflect(FieldList& f) {
        ReflectUIPlacement<UIPanel>(f, "Size in reference pixels.");
        f.Add("color", &UIPanel::color, "Fill color.");
        f.Add("radius", &UIPanel::radius, "Corner radius in reference pixels.");
        f.Add("borderWidth", &UIPanel::borderWidth, "Border thickness in reference pixels (0 = none).");
        f.Add("borderColor", &UIPanel::borderColor, "Border color.");
        f.Add("clip", &UIPanel::clip, "Children are cut off at the panel's edges (and cannot be clicked outside it).");
        ReflectUICommon<UIPanel>(f);
    }
};

struct UIButton {
    static constexpr const char* kTypeName = "UIButton";
    static constexpr const char* kDoc =
        "Clickable screen-space button. A click calls onClick(self) on the entity's Script; the pointer entering/leaving calls onPointerEnter/onPointerExit. "
        "Click it from tools with input.click (ui.layout lists where it is).";
    std::string text = "Button";
    std::string font = "default";
    float size = 28.0f;
    Color textColor{1, 1, 1};
    std::string anchor = "center";
    float x = 0.0f;
    float y = 0.0f;
    float width = 240.0f;
    float height = 64.0f;
    Color color{0.25f, 0.45f, 0.9f};
    float radius = 8.0f;
    float borderWidth = 0.0f;
    Color borderColor{1, 1, 1};
    float hoverBrightness = 1.15f;
    float pressedBrightness = 0.85f;
    bool interactable = true;
    std::string key;
    float opacity = 1.0f;
    bool visible = true;
    int order = 10;
    // Runtime state set by the simulation (not saved).
    bool hovered = false;
    bool pressed = false;
    static void Reflect(FieldList& f) {
        f.Add("text", &UIButton::text, "Label (same rich text as UIText).");
        f.Add("font", &UIButton::font, kUIFontDoc);
        f.Add("size", &UIButton::size, "Label font size in reference pixels.");
        f.Add("textColor", &UIButton::textColor, "Label color.");
        ReflectUIPlacement<UIButton>(f, "Size in reference pixels.");
        f.Add("color", &UIButton::color, "Background color.");
        f.Add("radius", &UIButton::radius, "Corner radius in reference pixels.");
        f.Add("borderWidth", &UIButton::borderWidth, "Border thickness in reference pixels (0 = none).");
        f.Add("borderColor", &UIButton::borderColor, "Border color.");
        f.Add("hoverBrightness", &UIButton::hoverBrightness, "Background brightness while the pointer is over the button.");
        f.Add("pressedBrightness", &UIButton::pressedBrightness, "Background brightness while pressed.");
        f.Add("interactable", &UIButton::interactable, "Disabled buttons are drawn faded and ignore clicks.");
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
    float radius = 12.0f;
    bool interactable = true;
    float opacity = 1.0f;
    bool visible = true;
    int order = 10;
    // Runtime state set by the simulation (not saved).
    bool hovered = false;
    bool pressed = false;
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
        f.Add("radius", &UISlider::radius, "Corner radius in reference pixels.");
        f.Add("interactable", &UISlider::interactable, "Drag to change the value; false = display only (progress bar).");
        ReflectUICommon<UISlider>(f);
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

struct UICanvas {
    static constexpr const char* kTypeName = "UICanvas";
    static constexpr const char* kDoc = "Optional, one per scene: the reference resolution the UI is authored for and how it scales to other screen sizes.";
    float referenceWidth = 1280.0f;
    float referenceHeight = 720.0f;
    float match = 1.0f;
    static void Reflect(FieldList& f) {
        f.Add("referenceWidth", &UICanvas::referenceWidth, "Reference canvas width in pixels.");
        f.Add("referenceHeight", &UICanvas::referenceHeight, "Reference canvas height in pixels.");
        FieldInfo& m = f.Add("match", &UICanvas::match, "0 = scale with the screen width, 1 = with the height (default), in between = blend.");
        m.hasRange = true;
        m.min = 0.0f;
        m.max = 1.0f;
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

void RegisterBuiltinComponents();

}  // namespace oe
