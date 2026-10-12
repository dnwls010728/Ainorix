#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "core/Math.h"

namespace oe {

// Math used by the native editor (scene camera, gizmo, drop placement).
// Header-only so tests can check it without a GPU.

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
    // Keep the 3D orbit independent of edits made while viewing the XY plane.
    struct OrbitState {
        Vec3 target{0, 0.5f, 0};
        float yaw = -35, pitch = -28, distance = 11;
    } saved3D, saved2D;
    bool hasSaved3D = false, hasSaved2D = false;

    // Match the perspective footprint at the orbit target when first entering orthographic 2D.
    float OrthoHalfHeight() const { return std::max(0.001f, distance * std::tan(Radians(fov) * 0.5f)); }
    Mat4 Projection(float aspect) const {
        return mode2D ? Mat4::Orthographic(OrthoHalfHeight(), aspect, 0.05f, 1000.0f)
                      : Mat4::Perspective(Radians(fov), aspect, 0.05f, 1000.0f);
    }

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
        if (on == mode2D) return;
        if (on) {
            saved3D = {target, yaw, pitch, distance};
            hasSaved3D = true;
            if (hasSaved2D) {
                target = saved2D.target;
                distance = saved2D.distance;
            }
            yaw = 0.0f;
            pitch = 0.0f;
        } else {
            saved2D = {target, 0, 0, distance};
            hasSaved2D = true;
            if (hasSaved3D) {
                target = saved3D.target;
                yaw = saved3D.yaw;
                pitch = saved3D.pitch;
                distance = saved3D.distance;
            }
        }
        mode2D = on;
    }
};

// Spacing of the 2D Scene view grid: the power of ten that keeps neighbouring
// lines at least `minPixels` apart (every tenth line is drawn stronger).
inline float Grid2DStep(float unitsPerPixel, float minPixels = 10.0f) {
    float units = std::max(1e-6f, unitsPerPixel * minPixels);
    return std::pow(10.0f, std::ceil(std::log10(units) - 1e-4f));
}

// A grid level appears once its cells are this many pixels wide and is fully
// drawn at ten times that (where the next finer level starts to appear).
constexpr float kGrid2DMinPixels = 8.0f;
inline float Grid2DAlpha(float cellPixels) { return Clamp((cellPixels - kGrid2DMinPixels) / (kGrid2DMinPixels * 9.0f), 0.0f, 1.0f); }

// Inverse-project the actual camera clip box, preserving perspective/orthographic and parent transforms.
// Corners 0..3 are near, 4..7 far; each face goes bottom-left, bottom-right, top-right, top-left.
inline bool CameraFrustumCorners(const Mat4& view, const Mat4& projection, std::array<Vec3, 8>& corners) {
    const Mat4 inverse = (projection * view).Inverse();
    for (int i = 0; i < 8; ++i) {
        const int face = i % 4;
        const Vec4 clip(face == 1 || face == 2 ? 1.0f : -1.0f, face >= 2 ? 1.0f : -1.0f, i >= 4 ? 1.0f : -1.0f, 1);
        const Vec4 world = inverse * clip;
        if (!std::isfinite(world.w) || std::fabs(world.w) < 1e-8f) return false;
        const Vec3 point = world.xyz() * (1.0f / world.w);
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) return false;
        corners[static_cast<size_t>(i)] = point;
    }
    return true;
}

// Intersect the finite camera clip volume with the XY editing plane. Unlike
// projecting all eight corners, this outlines only content visible on that plane.
inline std::vector<Vec3> CameraPlaneFootprint(const Mat4& view, const Mat4& projection, float planeZ = 0) {
    std::array<Vec3, 8> corners;
    if (!CameraFrustumCorners(view, projection, corners)) return {};
    std::vector<Vec3> points;
    auto add = [&](Vec3 point) {
        point.z = planeZ;
        for (const Vec3& existing : points) if (Length(existing - point) < 1e-4f) return;
        points.push_back(point);
    };
    auto edge = [&](const Vec3& a, const Vec3& b) {
        const float da = a.z - planeZ, db = b.z - planeZ;
        if (std::fabs(da) < 1e-5f) add(a);
        if (std::fabs(db) < 1e-5f) add(b);
        if ((da < 0 && db > 0) || (da > 0 && db < 0)) add(a + (b - a) * (da / (da - db)));
    };
    for (size_t i = 0; i < 4; ++i) {
        const size_t next = (i + 1) % 4;
        edge(corners[i], corners[next]);
        edge(corners[i + 4], corners[next + 4]);
        edge(corners[i], corners[i + 4]);
    }
    if (points.size() < 3) return {};
    Vec3 center;
    for (const Vec3& point : points) center = center + point;
    center = center * (1.0f / static_cast<float>(points.size()));
    std::sort(points.begin(), points.end(), [&](const Vec3& a, const Vec3& b) {
        return std::atan2(a.y - center.y, a.x - center.x) < std::atan2(b.y - center.y, b.x - center.x);
    });
    return points;
}

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
