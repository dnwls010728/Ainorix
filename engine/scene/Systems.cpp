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
        Vec3 move(0, 0, 0);
        if (input.IsDown("W") || input.IsDown("Up")) move.z -= 1;
        if (input.IsDown("S") || input.IsDown("Down")) move.z += 1;
        if (input.IsDown("A") || input.IsDown("Left")) move.x -= 1;
        if (input.IsDown("D") || input.IsDown("Right")) move.x += 1;
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
