#include "app/Vfx.h"

#include "scene/Systems.h"

namespace oe {
Json DescribeVfxEmitter(const ParticleEmitter& emitter, bool particles) {
    Json out = Json::MakeObject();
    out["count"] = static_cast<uint64_t>(emitter.particles.size());
    out["emitted"] = emitter.emitted;
    out["playing"] = emitter.playing;
    out["loop"] = emitter.loop;
    out["maxParticles"] = emitter.maxParticles;
    out["finished"] = ParticlesFinished(emitter);
    if (particles) {
        out["particles"] = Json::MakeArray();
        for (const Particle& particle : emitter.particles) {
            Json p = Json::MakeObject();
            p["age"] = particle.age;
            p["lifetime"] = particle.lifetime;
            p["position"] = Json(Json::Array{particle.position.x, particle.position.y, particle.position.z});
            p["velocity"] = Json(Json::Array{particle.velocity.x, particle.velocity.y, particle.velocity.z});
            p["rotation"] = particle.rotation;
            auto vector = [](Vec3 v) { return Json(Json::Array{v.x, v.y, v.z}); };
            p["birthPosition"] = vector(particle.birthPosition);
            p["initialVelocity"] = vector(particle.initialVelocity);
            p["initialRotation"] = vector(particle.initialRotation);
            p["initialScale"] = vector(particle.initialScale);
            p["integratedPosition"] = vector(particle.integratedPosition);
            p["rotation3D"] = vector(particle.rotation3D);
            p["scale"] = vector(particle.axisScale);
            p["custom"] = Json(Json::Array{particle.custom.x, particle.custom.y, particle.custom.z, particle.custom.w});
            p["initialCustom"] = Json(Json::Array{particle.initialCustom.x, particle.initialCustom.y, particle.initialCustom.z, particle.initialCustom.w});
            p["index"] = particle.index; p["count"] = particle.birthCount;
            p["random"] = particle.random;
            p["worldSpace"] = particle.worldSpace;
            out["particles"].push(p);
        }
    }
    return out;
}
}  // namespace oe
