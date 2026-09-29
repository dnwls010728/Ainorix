#include "scene/Systems.h"

#include "scene/Components.h"

namespace oe {

namespace {
float WrapDegrees(float d) {
    d = std::fmod(d, 360.0f);
    if (d < 0) d += 360.0f;
    return d;
}
}  // namespace

void UpdateLateSystems(Scene& scene, float dt) {
    for (auto& kv : scene.Pool<CameraFollow>()) {
        const CameraFollow& f = kv.second;
        Transform* t = scene.Get<Transform>(kv.first);
        if (!t || f.target == kv.first || !scene.Exists(f.target)) continue;
        Vec3 target = scene.WorldMatrix(f.target).TransformPoint(Vec3(0, 0, 0));
        Vec3 desired = target + f.offset;
        // Frame-rate independent exponential smoothing.
        float k = f.smoothing <= 0 ? 1.0f : 1.0f - std::exp(-f.smoothing * dt);
        t->position = t->position + (desired - t->position) * k;
        Vec3 look = target + f.lookOffset - t->position;
        if (Length(look) > 1e-4f) t->rotation = EulerLookDirection(look);
    }
}

void UpdateSystems(Scene& scene, InputState& input, float dt) {
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
}

}  // namespace oe
