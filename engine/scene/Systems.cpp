#include "scene/Systems.h"

#include <algorithm>
#include <cmath>

#include "assets/Assets.h"
#include "core/Log.h"
#include "scene/Components.h"
#include "scene/Vfx.h"

namespace oe {

bool InputState::SetAxis(const std::string& name, float value) {
    bool known = false;
    for (const char* axis : kAxisNames) if (name == axis) known = true;
    if (!known || !std::isfinite(value)) return false;
    axes[name] = Clamp(value, name == "LT" || name == "RT" ? 0.0f : -1.0f, 1.0f);
    return true;
}

void InputState::AddText(uint32_t c) {
    if (c < 32 || c == 127 || c > 0x10FFFF || (c >= 0xD800 && c < 0xE000)) return;
    if (c < 0x80) {
        text += static_cast<char>(c);
    } else if (c < 0x800) {
        text += static_cast<char>(0xC0 | (c >> 6));
        text += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        text += static_cast<char>(0xE0 | (c >> 12));
        text += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        text += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        text += static_cast<char>(0xF0 | (c >> 18));
        text += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        text += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        text += static_cast<char>(0x80 | (c & 0x3F));
    }
}

float InputState::Axis(const std::string& name) const {
    auto found = axes.find(name);
    if (found == axes.end() || !std::isfinite(found->second)) return 0;
    float value = Clamp(found->second, name == "LT" || name == "RT" ? 0.0f : -1.0f, 1.0f);
    float magnitude = std::fabs(value);
    if (magnitude <= kGamepadDeadZone) return 0;
    float output = (magnitude - kGamepadDeadZone) / (1 - kGamepadDeadZone);
    return value < 0 ? -output : output;
}

namespace {
float QuaternionDot(const Vec4& a, const Vec4& b) { return a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w; }

Vec4 NormalizeQuaternion(const Vec4& q) {
    float length = std::sqrt(QuaternionDot(q, q));
    return length > 1e-8f ? q * (1.0f / length) : Vec4(0, 0, 0, 1);
}

Mat4 QuaternionMatrix(Vec4 q) {
    q = NormalizeQuaternion(q);
    Mat4 m;
    m.at(0,0) = 1 - 2*(q.y*q.y + q.z*q.z);
    m.at(0,1) = 2*(q.x*q.y - q.z*q.w);
    m.at(0,2) = 2*(q.x*q.z + q.y*q.w);
    m.at(1,0) = 2*(q.x*q.y + q.z*q.w);
    m.at(1,1) = 1 - 2*(q.x*q.x + q.z*q.z);
    m.at(1,2) = 2*(q.y*q.z - q.x*q.w);
    m.at(2,0) = 2*(q.x*q.z - q.y*q.w);
    m.at(2,1) = 2*(q.y*q.z + q.x*q.w);
    m.at(2,2) = 1 - 2*(q.x*q.x + q.y*q.y);
    return m;
}

Vec4 SampleChannel(const AnimationChannel& c, float time) {
    const bool cubic = c.interpolation == AnimationInterpolation::CubicSpline;
    auto keyValue = [&](size_t key) { return c.values[cubic ? key * 3 + 1 : key]; };
    if (time <= c.times.front()) return keyValue(0);
    if (time >= c.times.back()) return keyValue(c.times.size() - 1);
    size_t next = static_cast<size_t>(std::upper_bound(c.times.begin(), c.times.end(), time) - c.times.begin());
    size_t prev = next - 1;
    Vec4 a = keyValue(prev), b = keyValue(next);
    if (c.interpolation == AnimationInterpolation::Step) return a;
    float interval = c.times[next] - c.times[prev];
    float t = (time - c.times[prev]) / interval;
    if (cubic) {
        // glTF tangents are derivatives per second, scaled by this key interval.
        float t2 = t*t, t3 = t2*t;
        Vec4 value = a * (2*t3 - 3*t2 + 1) + c.values[prev*3 + 2] * ((t3 - 2*t2 + t)*interval) +
                     b * (-2*t3 + 3*t2) + c.values[next*3] * ((t3 - t2)*interval);
        return c.path == AnimationPath::Rotation ? NormalizeQuaternion(value) : value;
    }
    if (c.path != AnimationPath::Rotation) return a * (1 - t) + b * t;
    a = NormalizeQuaternion(a);
    b = NormalizeQuaternion(b);
    float dot = QuaternionDot(a, b);
    if (dot < 0) { b = b * -1; dot = -dot; }
    if (dot > 0.9995f) return NormalizeQuaternion(a * (1 - t) + b * t);
    float angle = std::acos(Clamp(dot, -1, 1));
    float denominator = std::sin(angle);
    return NormalizeQuaternion(a * (std::sin((1 - t)*angle) / denominator) + b * (std::sin(t*angle) / denominator));
}

void UpdateAnimators(Scene& scene, AssetManager& assets, float dt) {
    for (auto& kv : scene.Pool<Animator>()) {
        Animator& a = kv.second;
        const MeshRenderer* renderer = scene.Get<MeshRenderer>(kv.first);
        if (!renderer || !a.playing || !std::isfinite(a.time) || !std::isfinite(a.speed)) continue;
        std::shared_ptr<const Mesh> mesh = assets.GetMesh(renderer->mesh);
        const AnimationClip* clip = mesh ? FindAnimationClip(*mesh, a.clip) : nullptr;
        if (!clip) continue;
        if (clip->duration <= 0) { a.time = 0; if (!a.loop) a.playing = false; continue; }
        double next = static_cast<double>(a.time) + static_cast<double>(dt) * a.speed;
        if (a.loop) {
            next = std::fmod(next, static_cast<double>(clip->duration));
            if (next < 0) next += clip->duration;
        } else {
            if ((a.speed > 0 && next >= clip->duration) || (a.speed < 0 && next <= 0)) a.playing = false;
            next = std::max(0.0, std::min(next, static_cast<double>(clip->duration)));
        }
        a.time = static_cast<float>(next);
    }
}

float ParticleRandom(ParticleEmitter& emitter, EntityId id) {
    uint32_t& state = emitter.randomState;
    if (state == 0) {
        state = static_cast<uint32_t>(emitter.seed) ^ (id * 0x9e3779b9u) ^ 0x85ebca6bu;
        if (state == 0) state = 0x6d2b79f5u;
    }
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return static_cast<float>(state >> 8) * (1.0f / 16777216.0f);
}

Vec3 ParticleDirection(ParticleEmitter& emitter, EntityId id) {
    Vec3 direction = emitter.direction;
    if (emitter.dimensions == 2) direction.z = 0;
    float largest = std::max(std::fabs(direction.x), std::max(std::fabs(direction.y), std::fabs(direction.z)));
    if (largest > 0) direction = direction / largest;  // avoid length overflow for large finite vectors
    direction = Normalize(direction);
    if (Length(direction) < 1e-8f) direction = Vec3(0,1,0);
    float spread = Radians(Clamp(emitter.spread, 0, 180));
    if (emitter.dimensions == 2) {
        float angle = (ParticleRandom(emitter, id) * 2 - 1) * spread;
        float c = std::cos(angle), s = std::sin(angle);
        return Vec3(direction.x*c - direction.y*s, direction.x*s + direction.y*c, 0);
    }
    float z = 1 - ParticleRandom(emitter, id) * (1 - std::cos(spread));
    float radius = std::sqrt(std::max(0.0f, 1 - z*z));
    float angle = ParticleRandom(emitter, id) * (2*kPi);
    Vec3 axis = std::fabs(direction.y) < 0.9f ? Vec3(0,1,0) : Vec3(1,0,0);
    Vec3 right = Normalize(Cross(direction, axis));
    Vec3 up = Cross(right, direction);
    return direction*z + right*(radius*std::cos(angle)) + up*(radius*std::sin(angle));
}

// Value noise in [-1, 1] on an integer lattice: integer hashes and float lerps only, so every
// platform gets the same field (no libm).
float LatticeValue(int x, int y, int z, uint32_t seed) {
    uint32_t h = static_cast<uint32_t>(x) * 0x8da6b343u ^ static_cast<uint32_t>(y) * 0xd8163841u ^
                 static_cast<uint32_t>(z) * 0xcb1ab31fu ^ seed * 0x9e3779b9u;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    h *= 0x297a2d39u;
    h ^= h >> 15;
    return static_cast<float>(h >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

float ValueNoise(const Vec3& p, uint32_t seed) {
    const float fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
    const int x = static_cast<int>(fx), y = static_cast<int>(fy), z = static_cast<int>(fz);
    auto fade = [](float t) { return t * t * (3.0f - 2.0f * t); };
    const float tx = fade(p.x - fx), ty = fade(p.y - fy), tz = fade(p.z - fz);
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    auto row = [&](int yy, int zz) { return lerp(LatticeValue(x, yy, zz, seed), LatticeValue(x + 1, yy, zz, seed), tx); };
    return lerp(lerp(row(y, z), row(y + 1, z), ty), lerp(row(y, z + 1), row(y + 1, z + 1), ty), tz);
}

// Acceleration of the emitter's turbulence field at a particle position.
Vec3 TurbulenceAt(const Vec3& position, float scale, uint32_t seed) {
    const float limit = 1.0e6f;  // keeps the lattice coordinates inside int range
    Vec3 p(Clamp(position.x * scale, -limit, limit), Clamp(position.y * scale, -limit, limit), Clamp(position.z * scale, -limit, limit));
    return Vec3(ValueNoise(p, seed), ValueNoise(p, seed + 1), ValueNoise(p, seed + 2));
}

void UpdateParticles(Scene& scene, float dt) {
    for (auto& pair : scene.Pool<ParticleEmitter>()) StepEmitter(pair.second, pair.first, scene.WorldMatrix(pair.first), 0, dt);
}
}  // namespace

void StepEmitter(ParticleEmitter& emitter, EntityId owner, const Mat4& world, uint32_t seedMix, float dt) {
    size_t limit = static_cast<size_t>(std::max(0, std::min(10000, emitter.maxParticles)));
    if (emitter.particles.size() > limit) emitter.particles.resize(limit);
    const float turbulence = std::isfinite(emitter.turbulence) && std::isfinite(emitter.turbulenceScale) ? Clamp(emitter.turbulence, 0, 10000) : 0;
    const float turbulenceScale = Clamp(emitter.turbulenceScale, 0.001f, 1000);
    const uint32_t turbulenceSeed = static_cast<uint32_t>(emitter.seed) ^ (owner * 0x9e3779b9u) ^ seedMix;
    for (Particle& particle : emitter.particles) {
        particle.age += dt;
        particle.velocity += particle.gravity * dt;
        if (turbulence > 0 && std::isfinite(particle.position.x) && std::isfinite(particle.position.y) && std::isfinite(particle.position.z)) {
            Vec3 push = TurbulenceAt(particle.position, turbulenceScale, turbulenceSeed) * (turbulence * dt);
            if (emitter.dimensions == 2) push.z = 0;
            particle.velocity += push;
        }
        if (particle.drag > 0) particle.velocity = particle.velocity * std::max(0.0f, 1.0f - particle.drag * dt);
        particle.position += particle.velocity * dt;
        if (particle.spin != 0) particle.rotation += particle.spin * dt;
    }
    emitter.particles.erase(std::remove_if(emitter.particles.begin(), emitter.particles.end(),
        [](const Particle& particle) { return particle.age >= particle.lifetime; }), emitter.particles.end());
    if (!emitter.playing) return;
    // Emission runs on the time played before this step, so delay 0 starts on the first one.
    const double local = emitter.elapsed - static_cast<double>(Clamp(emitter.delay, 0, 600));
    emitter.elapsed += dt;
    if (local < 0) return;
    const double duration = std::isfinite(emitter.duration) ? static_cast<double>(Clamp(emitter.duration, 0, 600)) : 0;
    if (duration > 0 && emitter.loop) {
        const int64_t cycle = static_cast<int64_t>(std::floor(local / duration));
        if (cycle > emitter.cycle) {  // a new cycle fires the burst again
            emitter.cycle = cycle;
            emitter.initialBurstEmitted = false;
        }
    }
    if (!emitter.initialBurstEmitted) {
        emitter.initialBurstEmitted = true;
        BurstEmitter(emitter, owner, world, seedMix, emitter.burst);
    }
    const bool rateActive = duration > 0 ? (emitter.loop || local < duration) : emitter.loop;
    if (!rateActive || !std::isfinite(emitter.rate)) return;
    emitter.carry += static_cast<double>(Clamp(emitter.rate, 0, 10000)) * dt;
    int births = static_cast<int>(std::floor(emitter.carry));
    emitter.carry -= births;
    BurstEmitter(emitter, owner, world, seedMix, births);
}

const AnimationClip* FindAnimationClip(const Mesh& mesh, const std::string& name) {
    for (const AnimationClip& clip : mesh.clips) if (clip.name == name) return &clip;
    return nullptr;
}

std::vector<Mat4> EvaluateAnimationPose(const Mesh& mesh, const AnimationClip* clip, float time) {
    std::vector<ModelNode> nodes = mesh.nodes;
    if (clip && std::isfinite(time)) {
        for (const AnimationChannel& channel : clip->channels) {
            ModelNode& node = nodes[static_cast<size_t>(channel.node)];
            Vec4 value = SampleChannel(channel, time);
            if (channel.path == AnimationPath::Translation) node.translation = value.xyz();
            else if (channel.path == AnimationPath::Scale) node.scale = value.xyz();
            else node.rotation = value;
        }
    }
    std::vector<Mat4> world(nodes.size());
    std::vector<bool> evaluated(nodes.size(), false);
    std::vector<size_t> chain;
    // Source node order need not put parents first. Iterative chains avoid recursion depth limits.
    for (size_t i = 0; i < nodes.size(); ++i) {
        int index = static_cast<int>(i);
        while (index >= 0 && !evaluated[static_cast<size_t>(index)]) {
            chain.push_back(static_cast<size_t>(index));
            index = nodes[static_cast<size_t>(index)].parent;
        }
        while (!chain.empty()) {
            size_t current = chain.back();
            chain.pop_back();
            const ModelNode& node = nodes[current];
            Mat4 local = node.usesMatrix ? node.matrix :
                Mat4::Translation(node.translation) * QuaternionMatrix(node.rotation) * Mat4::Scale(node.scale);
            world[current] = node.parent < 0 ? local : world[static_cast<size_t>(node.parent)] * local;
            evaluated[current] = true;
        }
    }
    std::vector<Mat4> palette;
    palette.reserve(mesh.joints.size());
    for (const ModelJoint& joint : mesh.joints) palette.push_back(world[static_cast<size_t>(joint.node)] * joint.inverseBind);
    return palette;
}

bool ParticleSettingsValid(const ParticleEmitter& emitter) {
    auto finiteVector = [](const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
    auto finiteColor = [](const Color& c) { return std::isfinite(c.r) && std::isfinite(c.g) && std::isfinite(c.b); };
    ParticleCurve curve;
    return std::isfinite(emitter.rate) && std::isfinite(emitter.lifetime) && std::isfinite(emitter.speed) &&
           std::isfinite(emitter.speedVariation) && std::isfinite(emitter.lifetimeVariation) && std::isfinite(emitter.sizeVariation) &&
           std::isfinite(emitter.drag) && std::isfinite(emitter.spread) && std::isfinite(emitter.startSize) && std::isfinite(emitter.endSize) &&
           std::isfinite(emitter.startOpacity) && std::isfinite(emitter.endOpacity) && finiteVector(emitter.direction) &&
           finiteVector(emitter.gravity) && finiteColor(emitter.startColor) && finiteColor(emitter.endColor) &&
           finiteVector(emitter.shapeSize) && std::isfinite(emitter.radialSpeed) && std::isfinite(emitter.rotation) &&
           std::isfinite(emitter.rotationVariation) && std::isfinite(emitter.spin) && std::isfinite(emitter.spinVariation) &&
           std::isfinite(emitter.stretch) && std::isfinite(emitter.frameRate) && std::isfinite(emitter.delay) &&
           std::isfinite(emitter.duration) && std::isfinite(emitter.turbulence) && std::isfinite(emitter.turbulenceScale) &&
           ParseParticleCurve(emitter.sizeOverLife, false, curve, nullptr) && ParseParticleCurve(emitter.colorOverLife, true, curve, nullptr) &&
           ParseParticleCurve(emitter.opacityOverLife, false, curve, nullptr);
}

Vec3 ParticleCurve::Sample(float t) const {
    if (times.empty()) return Vec3();
    if (!(t > times.front())) return values.front();
    for (size_t i = 1; i < times.size(); ++i) {
        if (t > times[i]) continue;
        const float span = times[i] - times[i - 1];
        const float k = span > 0 ? (t - times[i - 1]) / span : 1.0f;
        return values[i - 1] * (1 - k) + values[i] * k;
    }
    return values.back();
}

bool ParseParticleCurve(const Json& keys, bool color, ParticleCurve& out, std::string* error) {
    out.times.clear();
    out.values.clear();
    auto fail = [&](const std::string& message) {
        if (error) *error = message;
        out.times.clear();
        out.values.clear();
        return false;
    };
    if (keys.isNull()) return true;
    if (!keys.isArray()) return fail("expected an array of [t, value] keys");
    if (keys.size() > ParticleCurve::kMaxKeys) return fail("at most 16 keys");
    for (const Json& key : keys.items()) {
        if (!key.isArray() || key.size() != 2 || !key[0].isNumber()) return fail("each key is [t, value], got " + key.dump());
        const float t = key[0].asFloat();
        if (!std::isfinite(t) || t < 0 || t > 1) return fail("key time must be 0..1, got " + key[0].dump());
        if (!out.times.empty() && t < out.times.back()) return fail("key times must ascend");
        Vec3 value;
        if (color) {
            FieldInfo field;  // the component color syntax: [r, g, b] or "#rrggbb"
            field.name = "color";
            field.type = FieldType::Color;
            Color parsed;
            std::string message;
            if (!FieldFromJson(field, &parsed, key[1], &message)) return fail(message);
            value = Vec3(parsed.r, parsed.g, parsed.b);
        } else {
            if (!key[1].isNumber()) return fail("key value must be a number, got " + key[1].dump());
            value = Vec3(key[1].asFloat(), 0, 0);
        }
        if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z)) return fail("key value must be finite");
        out.times.push_back(t);
        out.values.push_back(value);
    }
    return true;
}

bool ParticlesFinished(const ParticleEmitter& emitter) {
    if (emitter.loop || !emitter.particles.empty() || !emitter.initialBurstEmitted) return false;
    return !(emitter.duration > 0) || emitter.elapsed >= static_cast<double>(Clamp(emitter.delay, 0, 600)) + static_cast<double>(emitter.duration);
}

void ClearTrail(Trail& trail) { trail.points.clear(); }

int BurstParticles(Scene& scene, EntityId id, int count) {
    ParticleEmitter* emitter = scene.Get<ParticleEmitter>(id);
    return emitter ? BurstEmitter(*emitter, id, scene.WorldMatrix(id), 0, count) : 0;
}

int BurstEmitter(ParticleEmitter& value, EntityId id, const Mat4& world, uint32_t seedMix, int count) {
    ParticleEmitter* emitter = &value;
    if (count <= 0) return 0;
    if (!ParticleSettingsValid(*emitter)) {
        if (!emitter->invalidSettingsReported) OE_LOG_WARN("particles", "entity %u: emission skipped; particle settings must be finite", id);
        emitter->invalidSettingsReported = true;
        return 0;
    }
    emitter->invalidSettingsReported = false;
    const size_t limit = static_cast<size_t>(std::max(0, std::min(10000, emitter->maxParticles)));
    const size_t available = emitter->particles.size() < limit ? limit - emitter->particles.size() : 0;
    count = static_cast<int>(std::min(available, static_cast<size_t>(count)));
    if (emitter->randomState == 0 && seedMix != 0) {
        emitter->randomState = static_cast<uint32_t>(emitter->seed) ^ (id * 0x9e3779b9u) ^ 0x85ebca6bu ^ seedMix;
        if (emitter->randomState == 0) emitter->randomState = 0x6d2b79f5u;
    }
    for (int i = 0; i < count; ++i) {
        Particle particle;
        particle.velocity = ParticleDirection(*emitter, id) * emitter->speed;
        particle.gravity = emitter->gravity;
        if (emitter->dimensions == 2) particle.gravity.z = 0;
        particle.lifetime = Clamp(emitter->lifetime, 0.001f, 600);
        particle.startSize = emitter->startSize;
        particle.endSize = emitter->endSize;
        // Variations draw from the emitter's stream only when used, so emitters without them keep their sequence.
        auto vary = [&](float variation) { return variation > 0 ? 1.0f - Clamp(variation, 0, 1) * ParticleRandom(*emitter, id) : 1.0f; };
        particle.velocity = particle.velocity * vary(emitter->speedVariation);
        particle.lifetime = std::max(0.001f, particle.lifetime * vary(emitter->lifetimeVariation));
        const float sizeScale = vary(emitter->sizeVariation);
        particle.startSize *= sizeScale;
        particle.endSize *= sizeScale;
        particle.drag = Clamp(emitter->drag, 0, 60);
        particle.startColor = emitter->startColor;
        particle.endColor = emitter->endColor;
        particle.startOpacity = emitter->startOpacity;
        particle.endOpacity = emitter->endOpacity;
        particle.sizeScale = sizeScale;
        // Later features draw after the older ones for the same reason.
        if (emitter->shape != "point") {
            const bool planar = emitter->dimensions == 2;
            const float radius = std::fabs(emitter->shapeSize.x);
            Vec3 offset;
            if (emitter->shape == "box") {
                Vec3 unit;  // one statement per draw: argument evaluation order differs between compilers
                unit.x = ParticleRandom(*emitter, id) * 2 - 1;
                unit.y = ParticleRandom(*emitter, id) * 2 - 1;
                unit.z = planar ? 0.0f : ParticleRandom(*emitter, id) * 2 - 1;
                if (emitter->shapeEdge) {  // push the coordinate nearest a face onto it
                    const float ax = std::fabs(unit.x), ay = std::fabs(unit.y), az = std::fabs(unit.z);
                    if (ax >= ay && ax >= az) unit.x = unit.x < 0 ? -1.0f : 1.0f;
                    else if (ay >= az) unit.y = unit.y < 0 ? -1.0f : 1.0f;
                    else unit.z = unit.z < 0 ? -1.0f : 1.0f;
                }
                offset = Vec3(unit.x * emitter->shapeSize.x, unit.y * emitter->shapeSize.y, unit.z * emitter->shapeSize.z);
            } else if (emitter->shape == "circle" || planar) {
                const float angle = ParticleRandom(*emitter, id) * (2 * kPi);
                const float r = radius * (emitter->shapeEdge ? 1.0f : std::sqrt(ParticleRandom(*emitter, id)));
                offset = planar ? Vec3(std::cos(angle) * r, std::sin(angle) * r, 0) : Vec3(std::cos(angle) * r, 0, std::sin(angle) * r);
            } else {  // sphere
                const float z = ParticleRandom(*emitter, id) * 2 - 1;
                const float angle = ParticleRandom(*emitter, id) * (2 * kPi);
                const float ring = std::sqrt(std::max(0.0f, 1 - z * z));
                float r = radius;
                // The largest of three uniform draws is distributed like a cube root: uniform in the volume without cbrt.
                if (!emitter->shapeEdge) {
                    float largest = 0;
                    for (int draw = 0; draw < 3; ++draw) largest = std::max(largest, ParticleRandom(*emitter, id));
                    r *= largest;
                }
                offset = Vec3(ring * std::cos(angle), z, ring * std::sin(angle)) * r;
            }
            particle.position = offset;
            const float distance = Length(offset);
            if (emitter->radialSpeed != 0 && distance > 1e-6f) particle.velocity += offset * (emitter->radialSpeed / distance);
        }
        particle.rotation = emitter->rotation;
        if (emitter->rotationVariation > 0) particle.rotation += (ParticleRandom(*emitter, id) * 2 - 1) * emitter->rotationVariation;
        particle.spin = emitter->spin;
        if (emitter->spinVariation > 0) particle.spin += (ParticleRandom(*emitter, id) * 2 - 1) * emitter->spinVariation;
        if (emitter->randomFrame && emitter->frames > 1)
            particle.frameOffset = std::min(emitter->frames - 1, static_cast<int>(ParticleRandom(*emitter, id) * static_cast<float>(emitter->frames)));
        particle.worldSpace = emitter->space == "world";
        if (particle.worldSpace) {
            particle.position = world.TransformPoint(particle.position);
            particle.velocity = world.TransformDir(particle.velocity);
        }
        emitter->particles.push_back(particle);
    }
    emitter->emitted += static_cast<uint64_t>(count);
    return count;
}

void StepTrail(Trail& trail, const Vec3& position, float dt) {
    const float lifetime = std::isfinite(trail.lifetime) ? Clamp(trail.lifetime, 0.01f, 60) : 0.01f;
    for (TrailPoint& point : trail.points) point.age += dt;
    size_t expired = 0;  // points are oldest first
    while (expired < trail.points.size() && trail.points[expired].age >= lifetime) ++expired;
    trail.points.erase(trail.points.begin(), trail.points.begin() + static_cast<std::ptrdiff_t>(expired));
    if (!trail.emitting) return;
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) return;
    const float step = std::isfinite(trail.minDistance) ? Clamp(trail.minDistance, 0.001f, 100) : 0.001f;
    if (!trail.points.empty() && Length(position - trail.points.back().position) < step) return;
    const size_t limit = static_cast<size_t>(std::max(2, std::min(1024, trail.maxPoints)));
    if (trail.points.size() >= limit) trail.points.erase(trail.points.begin(), trail.points.begin() + static_cast<std::ptrdiff_t>(trail.points.size() - limit + 1));
    trail.points.push_back({position, 0.0f});
}

void UpdateLateSystems(Scene& scene, float dt) {
    for (auto& kv : scene.Pool<CameraFollow>()) {
        const CameraFollow& f = kv.second;
        Transform* t = scene.Get<Transform>(kv.first);
        if (!t || f.target == kv.first || !scene.Exists(f.target)) continue;
        Vec3 target = scene.WorldMatrix(f.target).TransformPoint(Vec3(0, 0, 0));
        Vec3 desired = target + f.offset;
        if (f.useBounds) {  // keep the view inside the level (2D side-scrollers)
            desired = Vec3(Clamp(desired.x, f.boundsMin.x, std::max(f.boundsMin.x, f.boundsMax.x)),
                           Clamp(desired.y, f.boundsMin.y, std::max(f.boundsMin.y, f.boundsMax.y)),
                           Clamp(desired.z, f.boundsMin.z, std::max(f.boundsMin.z, f.boundsMax.z)));
        }
        // Frame-rate independent exponential smoothing.
        float k = f.smoothing <= 0 ? 1.0f : 1.0f - std::exp(-f.smoothing * dt);
        t->position = t->position + (desired - t->position) * k;
        if (f.useBounds) continue;  // clamped cameras keep their rotation (look straight ahead)
        Vec3 look = target + f.lookOffset - t->position;
        if (Length(look) > 1e-4f) t->rotation = EulerLookDirection(look);
    }
    for (auto& pair : scene.Pool<Trail>()) StepTrail(pair.second, scene.WorldMatrix(pair.first).TransformPoint(Vec3()), dt);
    UpdateVfxTrails(scene, dt);
}

void UpdateSpriteAnimations(Scene& scene, float dt) {
    for (auto& kv : scene.Pool<SpriteAnimation>()) {
        SpriteAnimation& a = kv.second;
        Sprite* sprite = scene.Get<Sprite>(kv.first);
        if (a.clip != a.current) {  // switching clips restarts them
            a.current = a.clip;
            a.time = 0.0f;
            a.finished = false;
        }
        const Json* clip = a.clips.isObject() ? a.clips.find(a.clip) : nullptr;
        if (!sprite || !clip || !clip->isObject()) continue;
        const Json& c = *clip;
        const Json& frames = c["frames"];
        size_t count = frames.isArray() ? frames.size() : 0;
        if (count == 0) continue;
        float fps = std::max(0.001f, c["fps"].asFloat(8.0f));
        bool loop = c["loop"].asBool(true);
        if (a.playing && !a.finished) a.time += dt * a.speed;
        size_t index = static_cast<size_t>(std::max(0.0f, std::floor(a.time * fps + 1e-4f)));
        if (loop) {
            index %= count;
        } else if (index >= count - 1) {
            index = count - 1;
            a.finished = true;
        }
        sprite->frame = frames[static_cast<int>(index)].asInt(0);
    }
}

void UpdateSystems(Scene& scene, InputState& input, float dt, AssetManager* assets) {
    if (assets) UpdateAnimators(scene, *assets, dt);
    UpdateParticles(scene, dt);
    UpdateVfxSystems(scene, assets, dt);
    UpdateSpriteAnimations(scene, dt);
    input.pressedThisFrame.clear();
    input.text.clear();
    input.mouseDX = input.mouseDY = 0.0f;
    input.wheel = 0.0f;
    for (InputState::Touch& t : input.touches) t.began = false;
}

}  // namespace oe
