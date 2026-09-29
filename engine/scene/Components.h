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
    static constexpr const char* kDoc = "Draws a mesh: a built-in shape (cube, sphere, plane, pyramid) or a glTF model file. Unknown meshes render as a magenta cube.";
    std::string mesh = "cube";
    Color color{0.8f, 0.8f, 0.8f};
    std::string texture;
    std::string shading = "smooth";
    bool unlit = false;
    bool castShadows = true;
    bool visible = true;
    static void Reflect(FieldList& f) {
        f.Add("mesh", &MeshRenderer::mesh, "Built-in name (cube, sphere, plane, pyramid) or model path, e.g. \"assets/models/fox.glb\" (.glb/.gltf).");
        f.Add("color", &MeshRenderer::color, "Tint multiplied with the model/texture color, linear RGB 0..1 (or \"#rrggbb\").");
        f.Add("texture", &MeshRenderer::texture, "Image (.png/.jpg) overriding the model's own base color texture. Empty = use the model's.");
        f.Add("shading", &MeshRenderer::shading, "smooth = interpolated vertex normals, flat = faceted.").options = {"smooth", "flat"};
        f.Add("unlit", &MeshRenderer::unlit, "Ignore lighting and shadows (UI-like, emissive look).");
        f.Add("castShadows", &MeshRenderer::castShadows, "Casts shadows from the directional light.");
        f.Add("visible", &MeshRenderer::visible, "Whether the mesh is drawn.");
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
    static void Reflect(FieldList& f) {
        f.Add("target", &CameraFollow::target, "Entity id to follow.");
        f.Add("offset", &CameraFollow::offset, "Position relative to the target (world axes).");
        f.Add("lookOffset", &CameraFollow::lookOffset, "Point to look at, relative to the target.");
        f.Add("smoothing", &CameraFollow::smoothing, "Catch-up speed (per second); 0 = snap instantly.");
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
    static void Reflect(FieldList& f) {
        f.Add("shape", &CharacterBody::shape, "Shape centered on the entity origin.").options = {"capsule", "sphere"};
        f.Add("radius", &CharacterBody::radius, "Radius in meters.");
        f.Add("height", &CharacterBody::height, "Capsule total height in meters.");
        f.Add("maxSlope", &CharacterBody::maxSlope, "Steepest walkable slope in degrees.");
        f.Add("stepHeight", &CharacterBody::stepHeight, "Highest step the character walks up automatically.");
        f.Add("gravityScale", &CharacterBody::gravityScale, "Multiplier for gravity.");
        f.Add("velocity", &CharacterBody::velocity, "Desired velocity in m/s; after each step it holds the actual velocity.");
        f.Add("grounded", &CharacterBody::grounded, "Runtime state: true while standing on walkable ground.");
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
// UI is laid out on a 1280x720 reference canvas scaled to the screen height.
// `anchor` picks both the screen point and the element's pivot; x/y are
// offsets in reference pixels (+x right, +y down). Drawn in `order`, then id.

inline const std::vector<std::string>& UIAnchors() {
    static const std::vector<std::string> a = {"top-left", "top", "top-right", "left", "center", "right", "bottom-left", "bottom", "bottom-right"};
    return a;
}

struct UIText {
    static constexpr const char* kTypeName = "UIText";
    static constexpr const char* kDoc = "Screen-space text (ASCII, built-in pixel font). Multi-line with \n.";
    std::string text = "Text";
    std::string anchor = "top-left";
    float x = 24.0f;
    float y = 24.0f;
    float size = 32.0f;
    Color color{1, 1, 1};
    bool visible = true;
    int order = 0;
    static void Reflect(FieldList& f) {
        f.Add("text", &UIText::text, "Text to show. Non-ASCII characters render as '?'.");
        f.Add("anchor", &UIText::anchor, "Screen anchor and pivot.").options = UIAnchors();
        f.Add("x", &UIText::x, "Horizontal offset in reference pixels (canvas is 1280x720).");
        f.Add("y", &UIText::y, "Vertical offset in reference pixels (+y is down).");
        f.Add("size", &UIText::size, "Line height in reference pixels.");
        f.Add("color", &UIText::color, "Text color.");
        f.Add("visible", &UIText::visible, "Hidden elements are not drawn.");
        f.Add("order", &UIText::order, "Draw order (higher on top).");
    }
};

struct UIPanel {
    static constexpr const char* kTypeName = "UIPanel";
    static constexpr const char* kDoc = "Screen-space colored rectangle (backgrounds, bars).";
    std::string anchor = "top-left";
    float x = 16.0f;
    float y = 16.0f;
    float width = 240.0f;
    float height = 64.0f;
    Color color{0, 0, 0};
    float opacity = 0.5f;
    bool visible = true;
    int order = -1;
    static void Reflect(FieldList& f) {
        f.Add("anchor", &UIPanel::anchor, "Screen anchor and pivot.").options = UIAnchors();
        f.Add("x", &UIPanel::x, "Horizontal offset in reference pixels.");
        f.Add("y", &UIPanel::y, "Vertical offset in reference pixels.");
        f.Add("width", &UIPanel::width, "Width in reference pixels.");
        f.Add("height", &UIPanel::height, "Height in reference pixels.");
        f.Add("color", &UIPanel::color, "Fill color.");
        FieldInfo& o = f.Add("opacity", &UIPanel::opacity, "0 = invisible, 1 = opaque.");
        o.hasRange = true; o.min = 0.0f; o.max = 1.0f;
        f.Add("visible", &UIPanel::visible, "Hidden elements are not drawn.");
        f.Add("order", &UIPanel::order, "Draw order (higher on top).");
    }
};

struct UIButton {
    static constexpr const char* kTypeName = "UIButton";
    static constexpr const char* kDoc = "Clickable screen-space button. A click calls onClick(self) on the entity's Script. Click it from tools with input.click.";
    std::string text = "Button";
    std::string anchor = "center";
    float x = 0.0f;
    float y = 0.0f;
    float width = 240.0f;
    float height = 64.0f;
    float size = 28.0f;
    Color color{0.25f, 0.45f, 0.9f};
    Color textColor{1, 1, 1};
    bool visible = true;
    int order = 10;
    static void Reflect(FieldList& f) {
        f.Add("text", &UIButton::text, "Label.");
        f.Add("anchor", &UIButton::anchor, "Screen anchor and pivot.").options = UIAnchors();
        f.Add("x", &UIButton::x, "Horizontal offset in reference pixels.");
        f.Add("y", &UIButton::y, "Vertical offset in reference pixels.");
        f.Add("width", &UIButton::width, "Width in reference pixels.");
        f.Add("height", &UIButton::height, "Height in reference pixels.");
        f.Add("size", &UIButton::size, "Label line height in reference pixels.");
        f.Add("color", &UIButton::color, "Background color.");
        f.Add("textColor", &UIButton::textColor, "Label color.");
        f.Add("visible", &UIButton::visible, "Hidden buttons are not drawn and cannot be clicked.");
        f.Add("order", &UIButton::order, "Draw order (higher on top).");
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
