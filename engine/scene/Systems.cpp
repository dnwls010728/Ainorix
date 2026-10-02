#include "scene/Systems.h"

#include <algorithm>
#include <cmath>

#include "assets/Assets.h"
#include "core/Log.h"
#include "scene/Components.h"

namespace oe {

bool InputState::SetAxis(const std::string& name, float value) {
    bool known = false;
    for (const char* axis : kAxisNames) if (name == axis) known = true;
    if (!known || !std::isfinite(value)) return false;
    axes[name] = Clamp(value, name == "LT" || name == "RT" ? 0.0f : -1.0f, 1.0f);
    return true;
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
float WrapDegrees(float d) {
    d = std::fmod(d, 360.0f);
    if (d < 0) d += 360.0f;
    return d;
}

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

void UpdateParticles(Scene& scene, float dt) {
    for (auto& kv : scene.Pool<ParticleEmitter>()) {
        ParticleEmitter& emitter = kv.second;
        size_t limit = static_cast<size_t>(std::max(0, std::min(10000, emitter.maxParticles)));
        if (emitter.particles.size() > limit) emitter.particles.resize(limit);
        for (Particle& particle : emitter.particles) {
            particle.age += dt;
            particle.velocity += particle.gravity * dt;
            particle.position += particle.velocity * dt;
        }
        emitter.particles.erase(std::remove_if(emitter.particles.begin(), emitter.particles.end(),
            [](const Particle& particle) { return particle.age >= particle.lifetime; }), emitter.particles.end());
        if (!emitter.playing) continue;
        if (!emitter.initialBurstEmitted) {
            emitter.initialBurstEmitted = true;
            BurstParticles(scene, kv.first, emitter.burst);
        }
        if (!emitter.loop || !std::isfinite(emitter.rate)) continue;
        emitter.carry += static_cast<double>(Clamp(emitter.rate, 0, 10000)) * dt;
        int births = static_cast<int>(std::floor(emitter.carry));
        emitter.carry -= births;
        BurstParticles(scene, kv.first, births);
    }
}
}  // namespace

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
    return std::isfinite(emitter.rate) && std::isfinite(emitter.lifetime) && std::isfinite(emitter.speed) &&
           std::isfinite(emitter.spread) && std::isfinite(emitter.startSize) && std::isfinite(emitter.endSize) &&
           std::isfinite(emitter.startOpacity) && std::isfinite(emitter.endOpacity) && finiteVector(emitter.direction) &&
           finiteVector(emitter.gravity) && finiteColor(emitter.startColor) && finiteColor(emitter.endColor);
}

int BurstParticles(Scene& scene, EntityId id, int count) {
    ParticleEmitter* emitter = scene.Get<ParticleEmitter>(id);
    if (!emitter || count <= 0) return 0;
    if (!ParticleSettingsValid(*emitter)) {
        if (!emitter->invalidSettingsReported) OE_LOG_WARN("particles", "entity %u: emission skipped; particle settings must be finite", id);
        emitter->invalidSettingsReported = true;
        return 0;
    }
    emitter->invalidSettingsReported = false;
    const size_t limit = static_cast<size_t>(std::max(0, std::min(10000, emitter->maxParticles)));
    const size_t available = emitter->particles.size() < limit ? limit - emitter->particles.size() : 0;
    count = static_cast<int>(std::min(available, static_cast<size_t>(count)));
    Mat4 world = scene.WorldMatrix(id);
    for (int i = 0; i < count; ++i) {
        Particle particle;
        particle.velocity = ParticleDirection(*emitter, id) * emitter->speed;
        particle.gravity = emitter->gravity;
        if (emitter->dimensions == 2) particle.gravity.z = 0;
        particle.lifetime = Clamp(emitter->lifetime, 0.001f, 600);
        particle.startSize = emitter->startSize;
        particle.endSize = emitter->endSize;
        particle.startColor = emitter->startColor;
        particle.endColor = emitter->endColor;
        particle.startOpacity = emitter->startOpacity;
        particle.endOpacity = emitter->endOpacity;
        particle.worldSpace = emitter->space == "world";
        if (particle.worldSpace) {
            particle.position = world.TransformPoint(Vec3());
            particle.velocity = world.TransformDir(particle.velocity);
        }
        emitter->particles.push_back(particle);
    }
    emitter->emitted += static_cast<uint64_t>(count);
    return count;
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
    UpdateSpriteAnimations(scene, dt);
    for (auto& kv : scene.Pool<Rotator>()) {
        if (Transform* t = scene.Get<Transform>(kv.first)) {
            t->rotation += kv.second.degreesPerSecond * dt;
            t->rotation = Vec3(WrapDegrees(t->rotation.x), WrapDegrees(t->rotation.y), WrapDegrees(t->rotation.z));
        }
    }

    for (auto& kv : scene.Pool<Velocity>()) {
        if (Transform* t = scene.Get<Transform>(kv.first)) t->position += kv.second.linear * dt;
    }

    for (auto& kv : scene.Pool<PlayerController>()) {
        Transform* t = scene.Get<Transform>(kv.first);
        if (!t) continue;
        PlayerController& pc = kv.second;
        CharacterBody* body = scene.Get<CharacterBody>(kv.first);
        Vec3 move(0, 0, 0);
        if (input.IsDown("W") || input.IsDown("Up")) move.z -= 1;
        if (input.IsDown("S") || input.IsDown("Down")) move.z += 1;
        if (input.IsDown("A") || input.IsDown("Left")) move.x -= 1;
        if (input.IsDown("D") || input.IsDown("Right")) move.x += 1;
        if (body) {
            // Physics path: express intent as velocity; the physics step moves
            // the character, handles walls, slopes, gravity and landing.
            Vec3 v = Length(move) > 0 ? Normalize(move) * pc.speed : Vec3(0, 0, 0);
            body->velocity.x = v.x;
            body->velocity.z = v.z;
            if (body->grounded && (input.pressedThisFrame.count("Space") || input.IsDown("Space"))) body->velocity.y = pc.jumpSpeed;
            continue;
        }
        if (Length(move) > 0) t->position += Normalize(move) * (pc.speed * dt);

        float groundY = 0.5f * t->scale.y;
        bool grounded = t->position.y <= groundY + 1e-4f;
        if (grounded && (input.pressedThisFrame.count("Space") || input.IsDown("Space")) && pc.verticalVelocity <= 0) {
            pc.verticalVelocity = pc.jumpSpeed;
            grounded = false;
        }
        if (!grounded || pc.verticalVelocity > 0) {
            pc.verticalVelocity -= pc.gravity * dt;
            t->position.y += pc.verticalVelocity * dt;
            if (t->position.y <= groundY) {
                t->position.y = groundY;
                pc.verticalVelocity = 0;
            }
        }
    }

    input.pressedThisFrame.clear();
    input.mouseDX = input.mouseDY = 0.0f;
    for (InputState::Touch& t : input.touches) t.began = false;
}

}  // namespace oe
