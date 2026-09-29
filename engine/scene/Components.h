#pragma once
#include <string>

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
    static constexpr const char* kDoc = "Draws a built-in mesh with a flat-shaded color.";
    std::string mesh = "cube";
    Color color{0.8f, 0.8f, 0.8f};
    bool visible = true;
    static void Reflect(FieldList& f) {
        f.Add("mesh", &MeshRenderer::mesh, "Built-in mesh name.").options = {"cube", "sphere", "plane", "pyramid"};
        f.Add("color", &MeshRenderer::color, "Base color, linear RGB 0..1 (or \"#rrggbb\" when writing).");
        f.Add("visible", &MeshRenderer::visible, "Whether the mesh is drawn.");
    }
};

struct Camera {
    static constexpr const char* kTypeName = "Camera";
    static constexpr const char* kDoc = "Perspective camera looking down its local -Z axis. The first active camera renders the game view.";
    float fov = 60.0f;
    float nearPlane = 0.1f;
    float farPlane = 500.0f;
    Color clearColor{0.12f, 0.14f, 0.18f};
    bool active = true;
    static void Reflect(FieldList& f) {
        FieldInfo& fov = f.Add("fov", &Camera::fov, "Vertical field of view in degrees.");
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
    static void Reflect(FieldList& f) {
        f.Add("color", &DirectionalLight::color, "Light color.");
        f.Add("intensity", &DirectionalLight::intensity, "Brightness multiplier.");
        f.Add("ambient", &DirectionalLight::ambient, "Ambient light added to every surface.");
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

void RegisterBuiltinComponents();

}  // namespace oe
