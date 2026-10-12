#pragma once
#include <memory>
#include <vector>

#include "core/Math.h"

namespace oe {

// A textured environment: an equirectangular image prepared for image-based lighting.
//
// levels[0] is the image (resized to a power of two, at most kEnvMaxWidth wide); each further level
// is half the size and blurred more, so a surface of roughness r reads level r * (levels - 1): the
// sharp image for a mirror, a wide blur for a rough surface. `sh` is the diffuse light arriving from
// the whole image (irradiance / pi as nine spherical-harmonic terms), so a white image gives white
// ambient like the engine's flat ambient color.
//
// Directions are in map space: +Y up, -Z at the center of the image, +X to its right.
// Both renderers sample the same data with the same rules (GPU: textureLod on the uploaded levels
// with repeat-U / clamp-V linear filtering; software: Sample below), so they agree.
constexpr int kEnvMaxWidth = 2048;

struct EnvironmentMap {
    struct Level {
        int width = 0, height = 0;
        std::vector<float> rgb;  // width * height * 3, rows top (up) to bottom
    };
    std::vector<Level> levels;
    Vec3 sh[9];  // evaluated by Irradiance; premultiplied (band weights, basis constants, 1/pi)

    float MaxLod() const { return levels.empty() ? 0.0f : static_cast<float>(levels.size() - 1); }
    // Bilinear within a level (U wraps, V clamps), linear between the two nearest levels.
    Color Sample(const Vec3& dir, float lod) const;
    // Diffuse light on a surface facing `normal` (never negative).
    Color Irradiance(const Vec3& normal) const;
};

// Image coordinates of a map-space direction: u 0..1 left to right (-Z at 0.5), v 0 (up) .. 1 (down).
inline void EnvUV(const Vec3& dir, float& u, float& v) {
    u = std::atan2(dir.x, -dir.z) * (0.5f / kPi) + 0.5f;
    v = std::acos(Clamp(dir.y, -1.0f, 1.0f)) * (1.0f / kPi);
}

// The nine irradiance terms as a polynomial of the normal (the same expression runs in mesh_fs).
inline Vec3 EnvIrradianceSH(const Vec3 sh[9], const Vec3& n) {
    return sh[0] + sh[1] * n.y + sh[2] * n.z + sh[3] * n.x + sh[4] * (n.x * n.y) + sh[5] * (n.y * n.z) +
           sh[6] * (3.0f * n.z * n.z - 1.0f) + sh[7] * (n.x * n.z) + sh[8] * (n.x * n.x - n.y * n.y);
}

// Builds the levels and the irradiance terms from an equirectangular image (`rgb`: width * height * 3
// floats in display colors, rows top to bottom; values above 1 are kept). nullptr for an empty image.
std::shared_ptr<EnvironmentMap> BuildEnvironmentMap(const float* rgb, int width, int height);

// The blur of a level: standard deviation (radians) of the lobe a surface of this roughness reflects.
inline float EnvLobeSigma(float roughness) { return 0.75f * roughness * roughness; }

}  // namespace oe
