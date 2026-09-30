#pragma once
#include <algorithm>
#include <cmath>

#include "core/Math.h"

namespace oe {

// Math used by the native editor (scene camera, gizmo, drop placement).
// Header-only so tests can check it without a GPU.

// Splits a matrix built like Mat4::TRS (T * Rz * Ry * Rx * S) back into
// position, Euler degrees (X then Y then Z, the Transform convention) and
// scale. Negative scale is folded into the rotation.
inline void DecomposeTRS(const Mat4& m, Vec3& position, Vec3& eulerDeg, Vec3& scale) {
    position = Vec3(m.at(0, 3), m.at(1, 3), m.at(2, 3));
    Vec3 c0(m.at(0, 0), m.at(1, 0), m.at(2, 0));
    Vec3 c1(m.at(0, 1), m.at(1, 1), m.at(2, 1));
    Vec3 c2(m.at(0, 2), m.at(1, 2), m.at(2, 2));
    scale = Vec3(Length(c0), Length(c1), Length(c2));
    if (scale.x > 1e-8f) c0 = c0 * (1.0f / scale.x);
    if (scale.y > 1e-8f) c1 = c1 * (1.0f / scale.y);
    if (scale.z > 1e-8f) c2 = c2 * (1.0f / scale.z);
    // R = [c0 c1 c2]; element R(row, col).
    float r00 = c0.x, r10 = c0.y, r20 = c0.z;
    float r11 = c1.y, r21 = c1.z;
    float r12 = c2.y, r22 = c2.z;
    float sy = Clamp(-r20, -1.0f, 1.0f);
    float x, y, z;
    if (std::fabs(sy) < 0.99999f) {
        y = std::asin(sy);
        x = std::atan2(r21, r22);
        z = std::atan2(r10, r00);
    } else {  // gimbal lock: Y = +-90 degrees, put everything into X
        y = sy > 0 ? kPi * 0.5f : -kPi * 0.5f;
        x = std::atan2(-r12, r11);
        z = 0.0f;
    }
    eulerDeg = Vec3(Degrees(x), Degrees(y), Degrees(z));
}

// Brings `angle` (degrees) within 180 of `reference` so re-decomposed Euler
// angles stay close to what the inspector showed before (no 179 -> -181 jumps).
inline float NearestAngle(float angle, float reference) {
    while (angle - reference > 180.0f) angle -= 360.0f;
    while (angle - reference < -180.0f) angle += 360.0f;
    return angle;
}

// Orbit / fly camera of the Scene view. Yaw 0 looks along -Z; pitch < 0 looks down.
struct EditorCamera {
    Vec3 target{0, 0.5f, 0};
    float yaw = -35.0f;     // degrees around +Y
    float pitch = -28.0f;   // degrees
    float distance = 11.0f;
    float fov = 60.0f;
    bool mode2D = false;    // looks along -Z, no rotation

    Vec3 Forward() const {
        if (mode2D) return Vec3(0, 0, -1);
        float cy = std::cos(Radians(yaw)), sy = std::sin(Radians(yaw));
        float cp = std::cos(Radians(pitch)), sp = std::sin(Radians(pitch));
        return Normalize(Vec3(-sy * cp, sp, -cy * cp));
    }
    Vec3 Right() const { return mode2D ? Vec3(1, 0, 0) : Normalize(Cross(Forward(), Vec3(0, 1, 0))); }
    Vec3 Up() const { return mode2D ? Vec3(0, 1, 0) : Cross(Right(), Forward()); }
    Vec3 Eye() const { return target - Forward() * distance; }

    void Orbit(float dxDeg, float dyDeg) {
        if (mode2D) return;
        yaw += dxDeg;
        pitch = Clamp(pitch + dyDeg, -89.0f, 89.0f);
    }
    // Moves the target in the view plane; dx/dy in pixels of a view `viewHeight` tall.
    void Pan(float dxPx, float dyPx, float viewHeight) {
        float unitsPerPixel = 2.0f * distance * std::tan(Radians(fov) * 0.5f) / std::max(1.0f, viewHeight);
        target = target - Right() * (dxPx * unitsPerPixel) + Up() * (dyPx * unitsPerPixel);
    }
    void Zoom(float notches) { distance = Clamp(distance * std::pow(0.85f, notches), 0.05f, 5000.0f); }
    // Fly mode (right mouse held): rotate around the eye instead of the target.
    void Look(float dxDeg, float dyDeg) {
        if (mode2D) return;
        Vec3 eye = Eye();
        Orbit(dxDeg, dyDeg);
        target = eye + Forward() * distance;
    }
    void Fly(const Vec3& localMove) { target = target + Right() * localMove.x + Up() * localMove.y + Forward() * localMove.z; }
    void Frame(const Vec3& center, float radius) {
        target = center;
        distance = std::max(1.0f, radius * 2.5f);
    }
    void Set2D(bool on) {
        mode2D = on;
        if (on) {
            yaw = 0.0f;
            pitch = 0.0f;
        }
    }
};

// World-space ray through a pixel of a view (x, y from the top-left).
inline void ScreenRay(const Mat4& view, const Mat4& proj, float x, float y, float width, float height, Vec3& origin, Vec3& dir) {
    Mat4 inv = (proj * view).Inverse();
    float nx = x / std::max(1.0f, width) * 2.0f - 1.0f;
    float ny = 1.0f - y / std::max(1.0f, height) * 2.0f;
    Vec4 a = inv * Vec4(nx, ny, -1.0f, 1.0f);
    Vec4 b = inv * Vec4(nx, ny, 1.0f, 1.0f);
    Vec3 pa = a.xyz() * (1.0f / a.w), pb = b.xyz() * (1.0f / b.w);
    origin = pa;
    dir = Normalize(pb - pa);
}

// Where a ray meets the plane through `point` with `normal`; false when it
// runs parallel or points away.
inline bool RayPlane(const Vec3& origin, const Vec3& dir, const Vec3& point, const Vec3& normal, Vec3& hit) {
    float d = Dot(dir, normal);
    if (std::fabs(d) < 1e-6f) return false;
    float t = Dot(point - origin, normal) / d;
    if (t < 0) return false;
    hit = origin + dir * t;
    return true;
}

}  // namespace oe
