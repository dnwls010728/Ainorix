#include "scene/Systems.h"
#include "scene/VfxExpression.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "assets/Assets.h"
#include "core/Log.h"
#include "scene/Components.h"
#include "scene/Pose.h"
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
        // A new clip fades in from the one that was shown (Animator.blend), which keeps playing meanwhile.
        if (a.shown != a.clip) {
            const bool fade = !a.shown.empty() && !a.clip.empty() && std::isfinite(a.blend) && a.blend > 0;
            a.from = fade ? a.shown : std::string();
            a.fromTime = a.shownTime;
            a.fade = fade ? a.blend : 0.0f;
            a.shown = a.clip;
        }
        if (!renderer || !std::isfinite(a.time) || !std::isfinite(a.speed)) continue;
        if (!a.playing && !(a.fade > 0)) { a.shownTime = a.time; continue; }
        std::shared_ptr<const Mesh> mesh = assets.GetMesh(renderer->mesh);
        const AnimationClip* clip = mesh ? FindAnimationClip(*mesh, a.clip) : nullptr;
        if (a.fade > 0) {  // also while a script drives `time` itself (playing: false)
            const AnimationClip* from = mesh ? FindAnimationClip(*mesh, a.from) : nullptr;
            a.fade -= dt;
            if (!from || !(a.fade > 0)) {
                a.fade = 0;
                a.from.clear();
            } else if (from->duration > 0) {
                a.fromTime = std::fmod(a.fromTime + dt * a.speed, from->duration);
                if (a.fromTime < 0) a.fromTime += from->duration;
            }
        }
        if (!a.playing) { a.shownTime = a.time; continue; }
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
        a.shownTime = a.time;
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

}  // namespace

namespace {
VfxExpressionInputs ExpressionInputs(const ParticleEmitter& emitter, const Particle& p) {
    VfxExpressionInputs input;
    input.age = Clamp(p.age / p.lifetime, 0, 1); input.lifetime = p.lifetime;
    input.emitterTime = static_cast<float>(emitter.elapsed); input.systemTime = static_cast<float>(emitter.expressionInSystem ? emitter.expressionSystemTime : emitter.elapsed);
    input.index = static_cast<float>(p.index); input.count = static_cast<float>(p.birthCount); input.random = p.random;
    input.speed = Length(p.velocity); input.birthPosition = p.expressionBirthPosition; input.birthVelocity = p.expressionBirthVelocity;
    input.birthRotation = p.initialRotation; input.birthScale = p.initialScale;
    input.birthColor = Vec3(p.startColor.r,p.startColor.g,p.startColor.b); input.birthSize = p.sizeScale > 0 ? p.startSize / p.sizeScale : 0; input.birthOpacity = p.startOpacity;
    input.params = &emitter.expressionParams; return input;
}
void UpdateExpressionChannels(const ParticleEmitter& emitter, Particle& p, const VfxExpressionInputs& input) {
    p.hasExpressionColor = p.hasExpressionSize = p.hasExpressionOpacity = false;
    if (!emitter.expressionProgram) return;
    const auto& programs = emitter.expressionProgram->update;
    auto value = [&](size_t channel) { return EvaluateVfxExpression(programs[channel],input); };
    auto present = [&](size_t channel) { return !programs[channel].code.empty(); };
    auto coordinates = [&](Vec3 v) { return p.worldSpace ? p.birthBasis.TransformDir(v) : v; };
    if (present(0)) p.position = p.integratedPosition + coordinates(value(0));
    if (present(2)) p.rotation3D = p.initialRotation + value(2);
    if (present(3)) { const Vec3 multiplier = value(3); p.axisScale = Mul(p.initialScale,Vec3(std::max(0.0f,multiplier.x),std::max(0.0f,multiplier.y),std::max(0.0f,multiplier.z))); }
    if (present(4)) { p.hasExpressionColor=true;p.expressionColor=value(4); }
    if (present(5)) { p.hasExpressionSize=true;p.expressionSize=std::max(0.0f,value(5).x)*p.sizeScale; }
    if (present(6)) { p.hasExpressionOpacity=true;p.expressionOpacity=Clamp(value(6).x,0,1); }
    float custom[] = {p.custom.x,p.custom.y,p.custom.z,p.custom.w};
    for (size_t i=0;i<4;++i) if (present(7+i)) custom[i]=value(7+i).x;
    p.custom=Vec4(custom[0],custom[1],custom[2],custom[3]);
}
void BirthExpressionChannels(const ParticleEmitter& emitter, Particle& p) {
    if (!emitter.expressionProgram) return;
    const VfxExpressionInputs input=ExpressionInputs(emitter,p);
    const auto& programs=emitter.expressionProgram->birth;
    for (size_t channel=0;channel<programs.size();++channel) {
        if (programs[channel].code.empty()) continue;
        const Vec3 v=EvaluateVfxExpression(programs[channel],input);
        switch (static_cast<VfxChannel>(channel)) {
            case VfxChannel::Position: p.position=v; break;
            case VfxChannel::Velocity: p.velocity=v; break;
            case VfxChannel::Rotation: p.initialRotation=v; break;
            case VfxChannel::Scale: p.initialScale=Vec3(std::max(0.0f,v.x),std::max(0.0f,v.y),std::max(0.0f,v.z)); break;
            case VfxChannel::Color: p.startColor=p.endColor=Color(v.x,v.y,v.z); break;
            case VfxChannel::Size: p.startSize=p.endSize=std::max(0.0f,v.x)*p.sizeScale; break;
            case VfxChannel::Opacity: p.startOpacity=p.endOpacity=Clamp(v.x,0,1); break;
            case VfxChannel::Custom0: p.initialCustom.x=v.x; break;
            case VfxChannel::Custom1: p.initialCustom.y=v.x; break;
            case VfxChannel::Custom2: p.initialCustom.z=v.x; break;
            case VfxChannel::Custom3: p.initialCustom.w=v.x; break;
            default: break;
        }
    }
}
struct MotionCurves {
    ParticleCurve position, velocity, rotation, scale;
    std::array<ParticleCurve, 4> custom;
    explicit MotionCurves(const ParticleEmitter& emitter) {
        ParseParticleVectorCurve(emitter.positionOverLife, position, nullptr);
        ParseParticleVectorCurve(emitter.velocityOverLife, velocity, nullptr);
        ParseParticleVectorCurve(emitter.rotationOverLife, rotation, nullptr);
        ParseParticleVectorCurve(emitter.scaleOverLife, scale, nullptr);
        const Json* keys[] = {&emitter.custom0OverLife, &emitter.custom1OverLife, &emitter.custom2OverLife, &emitter.custom3OverLife};
        for (int i = 0; i < 4; ++i) ParseParticleCurve(*keys[i], false, custom[static_cast<size_t>(i)], nullptr);
    }
    Vec3 Coordinates(const Particle& particle, Vec3 value) const {
        return particle.worldSpace ? particle.birthBasis.TransformDir(value) : value;
    }
    void Evaluate(Particle& particle) const {
        const float t = Clamp(particle.age / particle.lifetime, 0, 1);
        particle.position = position.Empty() ? particle.integratedPosition :
            particle.integratedPosition + Coordinates(particle, position.Sample(t));
        particle.rotation3D = particle.initialRotation + (rotation.Empty() ? Vec3() : rotation.Sample(t));
        const Vec3 multiplier = scale.Empty() ? Vec3(1, 1, 1) : scale.Sample(t);
        particle.axisScale = Vec3(particle.initialScale.x * multiplier.x, particle.initialScale.y * multiplier.y, particle.initialScale.z * multiplier.z);
        const float birth[] = {particle.initialCustom.x, particle.initialCustom.y, particle.initialCustom.z, particle.initialCustom.w};
        float values[4];
        for (int i = 0; i < 4; ++i) values[i] = custom[static_cast<size_t>(i)].Empty() ? birth[i] : custom[static_cast<size_t>(i)].Sample(t).x;
        particle.custom = Vec4(values[0], values[1], values[2], values[3]);
    }
};
}

void StepEmitter(ParticleEmitter& emitter, EntityId owner, const Mat4& world, uint32_t seedMix, float dt, std::vector<Particle>* deaths) {
    VfxExpressionError expressionError;
    if (!PrepareVfxExpressions(emitter,&expressionError)) {
        if (!emitter.invalidSettingsReported) OE_LOG_WARN("particles", "entity %u: %s",owner,expressionError.message.c_str());
        emitter.invalidSettingsReported=true; return;
    }
    size_t limit = static_cast<size_t>(std::max(0, std::min(10000, emitter.maxParticles)));
    if (emitter.particles.size() > limit) emitter.particles.resize(limit);
    const float turbulence = std::isfinite(emitter.turbulence) && std::isfinite(emitter.turbulenceScale) ? Clamp(emitter.turbulence, 0, 10000) : 0;
    const float turbulenceScale = Clamp(emitter.turbulenceScale, 0.001f, 1000);
    const uint32_t turbulenceSeed = static_cast<uint32_t>(emitter.seed) ^ (owner * 0x9e3779b9u) ^ seedMix;
    const MotionCurves curves(emitter);
    for (Particle& particle : emitter.particles) {
        particle.age += dt;
        const VfxExpressionInputs expressionInput=ExpressionInputs(emitter,particle);
        if (!curves.velocity.Empty()) particle.velocity = curves.Coordinates(particle, curves.velocity.Sample(Clamp(particle.age / particle.lifetime, 0, 1)));
        if (emitter.expressionProgram && !emitter.expressionProgram->update[1].code.empty()) {
            const Vec3 prescribed=EvaluateVfxExpression(emitter.expressionProgram->update[1],expressionInput);
            particle.velocity=curves.Coordinates(particle,prescribed);
        }
        particle.velocity += particle.gravity * dt;
        if (turbulence > 0 && std::isfinite(particle.position.x) && std::isfinite(particle.position.y) && std::isfinite(particle.position.z)) {
            Vec3 push = TurbulenceAt(particle.position, turbulenceScale, turbulenceSeed) * (turbulence * dt);
            if (emitter.dimensions == 2) push.z = 0;
            particle.velocity += push;
        }
        if (particle.drag > 0) particle.velocity = particle.velocity * std::max(0.0f, 1.0f - particle.drag * dt);
        particle.integratedPosition += particle.velocity * dt;
        if (particle.spin != 0) particle.rotation += particle.spin * dt;
        curves.Evaluate(particle);
        UpdateExpressionChannels(emitter,particle,expressionInput);
    }
    if (deaths) for (const Particle& particle : emitter.particles)
        if (particle.age >= particle.lifetime) deaths->push_back(particle);
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

namespace {

// A node's local transform (the names and parents stay in the model).
struct LocalNode {
    Vec3 translation;
    Vec4 rotation;
    Vec3 scale;
};

// The model's nodes with the clip's channels sampled into their local translation/rotation/scale.
std::vector<LocalNode> SampleNodes(const Mesh& mesh, const AnimationClip* clip, float time) {
    std::vector<LocalNode> nodes(mesh.nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) nodes[i] = {mesh.nodes[i].translation, mesh.nodes[i].rotation, mesh.nodes[i].scale};
    if (clip && std::isfinite(time)) {
        for (const AnimationChannel& channel : clip->channels) {
            LocalNode& node = nodes[static_cast<size_t>(channel.node)];
            Vec4 value = SampleChannel(channel, time);
            if (channel.path == AnimationPath::Translation) node.translation = value.xyz();
            else if (channel.path == AnimationPath::Scale) node.scale = value.xyz();
            else node.rotation = value;
        }
    }
    return nodes;
}

std::vector<Mat4> ComposeNodes(const Mesh& mesh, const std::vector<LocalNode>& nodes);

}  // namespace

std::vector<Mat4> EvaluateNodePose(const Mesh& mesh, const AnimationClip* clip, float time) {
    if (!clip && mesh.restPose.size() == mesh.nodes.size()) return mesh.restPose;  // the default pose of a loaded model
    return ComposeNodes(mesh, SampleNodes(mesh, clip, time));
}

std::vector<Mat4> EvaluateNodePose(const Mesh& mesh, const AnimationClip* clip, float time,
                                   const AnimationClip* from, float fromTime, float weight) {
    if (!(weight < 1) || !std::isfinite(weight)) return EvaluateNodePose(mesh, clip, time);
    std::vector<LocalNode> nodes = SampleNodes(mesh, from, fromTime);
    if (weight <= 0) return ComposeNodes(mesh, nodes);
    const std::vector<LocalNode> to = SampleNodes(mesh, clip, time);
    for (size_t i = 0; i < nodes.size(); ++i) {
        nodes[i].translation = nodes[i].translation * (1 - weight) + to[i].translation * weight;
        nodes[i].scale = nodes[i].scale * (1 - weight) + to[i].scale * weight;
        Vec4 a = NormalizeQuaternion(nodes[i].rotation), b = NormalizeQuaternion(to[i].rotation);
        if (QuaternionDot(a, b) < 0) b = b * -1;  // the short way round
        nodes[i].rotation = NormalizeQuaternion(a * (1 - weight) + b * weight);
    }
    return ComposeNodes(mesh, nodes);
}

namespace {

std::vector<Mat4> ComposeNodes(const Mesh& mesh, const std::vector<LocalNode>& locals) {
    const std::vector<ModelNode>& nodes = mesh.nodes;
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
            const LocalNode& pose = locals[current];
            Mat4 local = node.usesMatrix ? node.matrix :
                Mat4::Translation(pose.translation) * QuaternionMatrix(pose.rotation) * Mat4::Scale(pose.scale);
            world[current] = node.parent < 0 ? local : world[static_cast<size_t>(node.parent)] * local;
            evaluated[current] = true;
        }
    }
    return world;
}

}  // namespace

std::vector<Mat4> JointPalette(const Mesh& mesh, const std::vector<Mat4>& nodes) {
    std::vector<Mat4> palette;
    palette.reserve(mesh.joints.size());
    for (const ModelJoint& joint : mesh.joints) palette.push_back(nodes[static_cast<size_t>(joint.node)] * joint.inverseBind);
    return palette;
}

std::vector<Mat4> EvaluateAnimationPose(const Mesh& mesh, const AnimationClip* clip, float time) {
    return JointPalette(mesh, EvaluateNodePose(mesh, clip, time));
}

bool ParticleSettingsValid(const ParticleEmitter& emitter) {
    if (!PrepareVfxExpressions(emitter)) return false;
    auto finiteVector = [](const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
    auto boundedVector = [&](const Vec3& v) { return finiteVector(v) && std::abs(v.x) <= 65504 && std::abs(v.y) <= 65504 && std::abs(v.z) <= 65504; };
    auto finiteColor = [](const Color& c) { return std::isfinite(c.r) && std::isfinite(c.g) && std::isfinite(c.b); };
    const Json* vectors[] = {&emitter.positionOverLife, &emitter.velocityOverLife, &emitter.rotationOverLife, &emitter.scaleOverLife};
    ParticleCurve vector;
    for (const Json* keys : vectors) if (!ParseParticleVectorCurve(*keys, vector, nullptr)) return false;
    for (const Vec3& value : vector.values) if (value.x < 0 || value.y < 0 || value.z < 0) return false;
    const Json* custom[] = {&emitter.custom0OverLife, &emitter.custom1OverLife, &emitter.custom2OverLife, &emitter.custom3OverLife};
    for (const Json* keys : custom) {
        if (!ParseParticleCurve(*keys, false, vector, nullptr)) return false;
        for (const Vec3& value : vector.values) if (std::abs(value.x) > 65504) return false;
    }
    if (!boundedVector(emitter.positionOffset) || !boundedVector(emitter.rotation3D) || !boundedVector(emitter.scale) || !boundedVector(emitter.pivot) ||
        emitter.scale.x < 0 || emitter.scale.y < 0 || emitter.scale.z < 0 || !std::isfinite(emitter.custom0) || !std::isfinite(emitter.custom1) ||
        !std::isfinite(emitter.custom2) || !std::isfinite(emitter.custom3) ||
        std::abs(emitter.custom0) > 65504 || std::abs(emitter.custom1) > 65504 || std::abs(emitter.custom2) > 65504 || std::abs(emitter.custom3) > 65504) return false;
    ParticleCurve curve;
    return std::isfinite(emitter.rate) && std::isfinite(emitter.lifetime) && std::isfinite(emitter.speed) &&
           std::isfinite(emitter.speedVariation) && std::isfinite(emitter.lifetimeVariation) && std::isfinite(emitter.sizeVariation) &&
           std::isfinite(emitter.drag) && std::isfinite(emitter.spread) && std::isfinite(emitter.startSize) && std::isfinite(emitter.endSize) &&
           std::isfinite(emitter.startOpacity) && std::isfinite(emitter.endOpacity) && finiteVector(emitter.direction) &&
           finiteVector(emitter.gravity) && finiteColor(emitter.startColor) && finiteColor(emitter.endColor) &&
           finiteVector(emitter.meshScale) && finiteVector(emitter.shapeSize) && std::isfinite(emitter.radialSpeed) && std::isfinite(emitter.rotation) &&
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

bool ParseParticleVectorCurve(const Json& keys, ParticleCurve& out, std::string* error) {
    out.times.clear(); out.values.clear();
    auto fail = [&](const char* message) {
        if (error) *error = message;
        out.times.clear(); out.values.clear(); return false;
    };
    if (keys.isNull()) return true;
    if (!keys.isArray() || keys.size() > ParticleCurve::kMaxKeys) return fail("expected up to 16 [age,[x,y,z]] keys");
    for (const Json& key : keys.items()) {
        if (!key.isArray() || key.size() != 2 || !key[0].isNumber() || !std::isfinite(key[0].asFloat()) ||
            key[0].asFloat() < 0 || key[0].asFloat() > 1 || (!out.times.empty() && key[0].asFloat() < out.times.back()))
            return fail("key age must be finite, ascending and in 0..1");
        if (!key[1].isArray() || key[1].size() != 3) return fail("vector key value must be [x,y,z]");
        for (const Json& coordinate : key[1].items())
            if (!coordinate.isNumber() || !std::isfinite(coordinate.asFloat()) || std::abs(coordinate.asFloat()) > 65504)
                return fail("vector values must be finite numbers in -65504..65504");
        out.times.push_back(key[0].asFloat()); out.values.emplace_back(key[1][0].asFloat(), key[1][1].asFloat(), key[1][2].asFloat());
    }
    return true;
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
    const MotionCurves curves(*emitter);
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
        if (!emitter->mesh.empty()) particle.meshBasis = world;
        particle.position += emitter->positionOffset;
        particle.index = emitter->emitted + static_cast<uint64_t>(i); particle.birthCount = count;
        particle.initialRotation=emitter->rotation3D;particle.initialScale=emitter->scale;
        particle.initialCustom=Vec4(emitter->custom0,emitter->custom1,emitter->custom2,emitter->custom3);
        particle.expressionBirthPosition=particle.position;particle.expressionBirthVelocity=particle.velocity;
        // Hash accepted birth index without consuming the established PRNG stream.
        uint32_t hash = emitter->randomState ^ static_cast<uint32_t>(emitter->emitted + static_cast<uint64_t>(i));
        hash ^= hash >> 16; hash *= 0x7feb352du; hash ^= hash >> 15; hash *= 0x846ca68bu; hash ^= hash >> 16;
        particle.random = static_cast<float>(hash >> 8) / 16777216.0f;
        BirthExpressionChannels(*emitter,particle);
        particle.expressionBirthPosition=particle.position;particle.expressionBirthVelocity=particle.velocity;
        particle.worldSpace = emitter->space == "world";
        if (particle.worldSpace) {
            particle.position = world.TransformPoint(particle.position);
            particle.velocity = world.TransformDir(particle.velocity);
        }
        particle.birthPosition = particle.integratedPosition = particle.position;
        particle.initialVelocity = particle.velocity;
        particle.birthBasis = world; particle.index = emitter->emitted + static_cast<uint64_t>(i); particle.birthCount = count;
        if (!curves.velocity.Empty()) particle.velocity = curves.Coordinates(particle, curves.velocity.Sample(0));
        particle.initialVelocity = particle.velocity;
        curves.Evaluate(particle);
        const VfxExpressionInputs expressionInput=ExpressionInputs(*emitter,particle);
        if (emitter->expressionProgram && !emitter->expressionProgram->update[1].code.empty()) {
            particle.velocity=curves.Coordinates(particle,EvaluateVfxExpression(emitter->expressionProgram->update[1],expressionInput));
        }
        UpdateExpressionChannels(*emitter,particle,expressionInput);
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
    trail.points.push_back({position, 0.0f, Vec3()});
}

bool TrailEmitting(const Trail& trail) {
    return trail.emitting && (trail.duration <= 0 || trail.elapsed < static_cast<double>(trail.duration));
}

bool TrailAnchors(const Scene& scene, const Trail& trail, const Mat4& owner, Vec3& base, Vec3& tip) {
    auto anchor = [&](const std::string& name, const Vec3& offset, Vec3& out) {
        if (name.empty()) { out = owner.TransformPoint(offset); return true; }
        EntityId id = kNullEntity;
        for (const auto& pair : scene.Entities()) if (pair.second.name == name) {
            if (id) return false;  // an ambiguous name cannot identify a blade anchor
            id = pair.first;
        }
        if (!id) return false;
        out = scene.WorldMatrix(id).TransformPoint(offset); return true;
    };
    if (trail.mode != "ribbon") { base = owner.TransformPoint(Vec3()); tip = base; return true; }
    return anchor(trail.baseAnchor, trail.baseOffset, base) && anchor(trail.tipAnchor, trail.tipOffset, tip);
}

void StepAnchoredTrail(Trail& trail, const Vec3& base, const Vec3& tip, float dt) {
    const bool authored = trail.emitting;
    trail.emitting = TrailEmitting(trail);
    if (trail.emitting) trail.elapsed += dt;
    if (trail.mode != "ribbon") StepTrail(trail, base, dt);
    else {
        for (TrailPoint& point : trail.points) point.age += dt;
        trail.points.erase(std::remove_if(trail.points.begin(), trail.points.end(), [&](const TrailPoint& p) {
            return p.age >= Clamp(trail.lifetime, 0.01f, 60);
        }), trail.points.end());
        const bool finite = std::isfinite(base.x) && std::isfinite(base.y) && std::isfinite(base.z) &&
                            std::isfinite(tip.x) && std::isfinite(tip.y) && std::isfinite(tip.z);
        if (trail.emitting && finite && (trail.points.empty() ||
            std::max(Length(base-trail.points.back().position), Length(tip-trail.points.back().tip)) >= trail.minDistance)) {
            const size_t limit = static_cast<size_t>(std::clamp(trail.maxPoints, 2, 1024));
            if (trail.points.size() >= limit) trail.points.erase(trail.points.begin());
            trail.points.push_back({base, 0, tip});
        }
    }
    trail.emitting = authored;
}

void UpdateLateSystems(Scene& scene, float dt, AssetManager* assets) {
    // First: springs swing behind where physics left the model, and cameras and trails below may
    // follow an entity held by a bone.
    if (assets) UpdateSpringBones(scene, *assets, dt);
    if (assets) UpdateBoneAttachments(scene, *assets);
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
    for (auto& pair : scene.Pool<Trail>()) {
        Vec3 base, tip;
        if (!TrailAnchors(scene, pair.second, scene.WorldMatrix(pair.first), base, tip)) {
            const bool emitting = pair.second.emitting; pair.second.emitting = false;
            StepAnchoredTrail(pair.second, Vec3(), Vec3(), dt); pair.second.emitting = emitting;
        } else StepAnchoredTrail(pair.second, base, tip, dt);
    }
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
    UpdateVfxSystems(scene, assets, dt);
    UpdateSpriteAnimations(scene, dt);
    input.pressedThisFrame.clear();
    input.text.clear();
    input.mouseDX = input.mouseDY = 0.0f;
    input.wheel = 0.0f;
    for (InputState::Touch& t : input.touches) t.began = false;
}

}  // namespace oe
