#pragma once
#include <cmath>

namespace oe {

constexpr float kPi = 3.14159265358979323846f;
inline float Radians(float deg) { return deg * (kPi / 180.0f); }
inline float Degrees(float rad) { return rad * (180.0f / kPi); }
inline float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
    bool operator==(const Vec3& o) const { return x == o.x && y == o.y && z == o.z; }
};

inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float Length(const Vec3& v) { return std::sqrt(Dot(v, v)); }
inline Vec3 Normalize(const Vec3& v) {
    float len = Length(v);
    return len > 1e-8f ? v / len : Vec3(0, 0, 0);
}
inline Vec3 Mul(const Vec3& a, const Vec3& b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }

struct Vec4 {
    float x = 0, y = 0, z = 0, w = 0;
    Vec4() = default;
    Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    Vec4(const Vec3& v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
    Vec4 operator+(const Vec4& o) const { return {x + o.x, y + o.y, z + o.z, w + o.w}; }
    Vec4 operator-(const Vec4& o) const { return {x - o.x, y - o.y, z - o.z, w - o.w}; }
    Vec4 operator*(float s) const { return {x * s, y * s, z * s, w * s}; }
    Vec3 xyz() const { return {x, y, z}; }
};

// Linear RGB color, components 0..1.
struct Color {
    float r = 1, g = 1, b = 1;
    Color() = default;
    Color(float r_, float g_, float b_) : r(r_), g(g_), b(b_) {}
    Color operator*(float s) const { return {r * s, g * s, b * s}; }
    Color operator+(const Color& o) const { return {r + o.r, g + o.g, b + o.b}; }
    Color operator*(const Color& o) const { return {r * o.r, g * o.g, b * o.b}; }
    bool operator==(const Color& o) const { return r == o.r && g == o.g && b == o.b; }
};

// Column-major 4x4 matrix (m[col * 4 + row]); right-handed, OpenGL clip space.
struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    static Mat4 Identity() { return Mat4(); }
    float& at(int row, int col) { return m[col * 4 + row]; }
    float at(int row, int col) const { return m[col * 4 + row]; }

    Mat4 operator*(const Mat4& o) const {
        Mat4 r;
        for (int c = 0; c < 4; ++c) {
            for (int rr = 0; rr < 4; ++rr) {
                float s = 0;
                for (int k = 0; k < 4; ++k) s += at(rr, k) * o.at(k, c);
                r.at(rr, c) = s;
            }
        }
        return r;
    }
    Vec4 operator*(const Vec4& v) const {
        return {at(0, 0) * v.x + at(0, 1) * v.y + at(0, 2) * v.z + at(0, 3) * v.w,
                at(1, 0) * v.x + at(1, 1) * v.y + at(1, 2) * v.z + at(1, 3) * v.w,
                at(2, 0) * v.x + at(2, 1) * v.y + at(2, 2) * v.z + at(2, 3) * v.w,
                at(3, 0) * v.x + at(3, 1) * v.y + at(3, 2) * v.z + at(3, 3) * v.w};
    }
    Vec3 TransformPoint(const Vec3& p) const { return ((*this) * Vec4(p, 1.0f)).xyz(); }
    Vec3 TransformDir(const Vec3& d) const { return ((*this) * Vec4(d, 0.0f)).xyz(); }

    static Mat4 Translation(const Vec3& t) {
        Mat4 r;
        r.at(0, 3) = t.x; r.at(1, 3) = t.y; r.at(2, 3) = t.z;
        return r;
    }
    static Mat4 Scale(const Vec3& s) {
        Mat4 r;
        r.at(0, 0) = s.x; r.at(1, 1) = s.y; r.at(2, 2) = s.z;
        return r;
    }
    static Mat4 RotationX(float rad) {
        Mat4 r;
        float c = std::cos(rad), s = std::sin(rad);
        r.at(1, 1) = c; r.at(1, 2) = -s; r.at(2, 1) = s; r.at(2, 2) = c;
        return r;
    }
    static Mat4 RotationY(float rad) {
        Mat4 r;
        float c = std::cos(rad), s = std::sin(rad);
        r.at(0, 0) = c; r.at(0, 2) = s; r.at(2, 0) = -s; r.at(2, 2) = c;
        return r;
    }
    static Mat4 RotationZ(float rad) {
        Mat4 r;
        float c = std::cos(rad), s = std::sin(rad);
        r.at(0, 0) = c; r.at(0, 1) = -s; r.at(1, 0) = s; r.at(1, 1) = c;
        return r;
    }
    // Euler angles in degrees, applied X then Y then Z (R = Rz * Ry * Rx).
    static Mat4 RotationEuler(const Vec3& deg) {
        return RotationZ(Radians(deg.z)) * RotationY(Radians(deg.y)) * RotationX(Radians(deg.x));
    }
    static Mat4 TRS(const Vec3& t, const Vec3& eulerDeg, const Vec3& s) {
        return Translation(t) * RotationEuler(eulerDeg) * Scale(s);
    }
    static Mat4 Perspective(float fovYRad, float aspect, float zNear, float zFar) {
        Mat4 r;
        float f = 1.0f / std::tan(fovYRad * 0.5f);
        for (float& v : r.m) v = 0;
        r.at(0, 0) = f / aspect;
        r.at(1, 1) = f;
        r.at(2, 2) = (zFar + zNear) / (zNear - zFar);
        r.at(2, 3) = (2.0f * zFar * zNear) / (zNear - zFar);
        r.at(3, 2) = -1.0f;
        return r;
    }
    static Mat4 Orthographic(float halfHeight, float aspect, float zNear, float zFar) {
        Mat4 r;
        float halfWidth = halfHeight * aspect;
        r.at(0, 0) = 1.0f / halfWidth;
        r.at(1, 1) = 1.0f / halfHeight;
        r.at(2, 2) = -2.0f / (zFar - zNear);
        r.at(2, 3) = -(zFar + zNear) / (zFar - zNear);
        return r;
    }
    Mat4 Transposed() const {
        Mat4 r;
        for (int c = 0; c < 4; ++c)
            for (int rr = 0; rr < 4; ++rr) r.at(rr, c) = at(c, rr);
        return r;
    }
    // General inverse (cofactor expansion). Returns identity if singular.
    Mat4 Inverse() const {
        const float* a = m;
        float inv[16];
        inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
        inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
        inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
        inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
        inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
        inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
        inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
        inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
        inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
        inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
        inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
        inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
        inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
        inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
        inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
        inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
        float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
        Mat4 r;
        if (std::fabs(det) < 1e-12f) return r;
        for (int i = 0; i < 16; ++i) r.m[i] = inv[i] / det;
        return r;
    }
    static Mat4 LookAt(const Vec3& eye, const Vec3& target, const Vec3& up) {
        Vec3 f = Normalize(target - eye);
        Vec3 s = Normalize(Cross(f, up));
        if (Length(s) < 1e-6f) s = Normalize(Cross(f, Vec3(0, 0, 1)));
        Vec3 u = Cross(s, f);
        Mat4 r;
        r.at(0, 0) = s.x; r.at(0, 1) = s.y; r.at(0, 2) = s.z; r.at(0, 3) = -Dot(s, eye);
        r.at(1, 0) = u.x; r.at(1, 1) = u.y; r.at(1, 2) = u.z; r.at(1, 3) = -Dot(u, eye);
        r.at(2, 0) = -f.x; r.at(2, 1) = -f.y; r.at(2, 2) = -f.z; r.at(2, 3) = Dot(f, eye);
        return r;
    }
};

// Euler angles (degrees, no roll) that point the local -Z axis along `dir`.
inline Vec3 EulerLookDirection(const Vec3& dir) {
    Vec3 d = Normalize(dir);
    return Vec3(Degrees(std::asin(Clamp(d.y, -1.0f, 1.0f))), Degrees(std::atan2(-d.x, -d.z)), 0.0f);
}

// Forward direction (-Z rotated) for euler rotation in degrees.
inline Vec3 ForwardFromEuler(const Vec3& deg) { return Normalize(Mat4::RotationEuler(deg).TransformDir(Vec3(0, 0, -1))); }

}  // namespace oe
