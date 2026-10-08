#include "app/Vfx.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

#include "app/Engine.h"
#include "assets/Assets.h"
#include "core/FileSystem.h"
#include "scene/Components.h"
#include "scene/Systems.h"
#include "scene/Vfx.h"

namespace oe {
namespace {
std::string ModuleOf(const std::string& name) {
    if (name.find("OverLife") != std::string::npos) return "Curves";
    if (name == "delay" || name == "duration" || name == "loop" || name == "playing" || name == "lifetime" || name == "lifetimeVariation") return "Timing";
    if (name == "onDeath" || name == "deathCount" || name == "rate" || name == "burst" || name == "maxParticles" || name == "seed" || name == "emitting" || name == "maxPoints") return "Emission";
    if (name == "mode" || name == "baseAnchor" || name == "tipAnchor" || name == "baseOffset" || name == "tipOffset" || name == "shape" || name == "shapeSize" || name == "shapeEdge" || name == "dimensions" || name == "spread") return "Shape";
    if (name == "material" || name == "mesh" || name == "batching" || name == "texture" || name == "blend" || name == "layer" || name == "pixelArt" || name == "frame" || name == "frames" ||
        name == "columns" || name == "rows" || name == "frameRate" || name == "randomFrame") return "Render";
    if (name == "positionOffset" || name == "space" || name == "direction" || name == "gravity" || name == "drag" || name == "radialSpeed" ||
        name == "turbulence" || name == "turbulenceScale" || name == "speed" || name == "speedVariation" || name == "minDistance") return "Motion";
    return "Appearance";
}

std::set<EntityId> EffectIds(const Scene& scene, EntityId root) {
    std::set<EntityId> ids{root};
    std::vector<EntityId> pending{root};
    while (!pending.empty()) {
        const EntityId id = pending.back(); pending.pop_back();
        for (EntityId child : scene.Children(id)) if (ids.insert(child).second) pending.push_back(child);
    }
    return ids;
}
}  // namespace

Json VfxSchema() {
    Json out = Json::MakeObject();
    out["modules"] = Json(Json::Array{"Emission", "Shape", "Motion", "Appearance", "Curves", "Timing", "Render"});
    out["components"] = Json::MakeArray();
    for (const char* name : {"ParticleEmitter", "Trail"}) {
        const ComponentType& type = *TypeRegistry::Find(name);
        auto pool = type.makePool(); const void* defaults = pool->Add(1);
        Json component = Json::MakeObject(); component["name"] = name; component["fields"] = Json::MakeArray();
        for (const FieldInfo& field : type.fields) {
            Json value = Json::MakeObject();
            value["name"] = field.name; value["type"] = ToString(field.type); value["default"] = FieldToJson(field, defaults);
            value["min"] = field.hasRange ? Json(field.min) : Json(); value["max"] = field.hasRange ? Json(field.max) : Json();
            value["options"] = Json::MakeArray(); for (const std::string& option : field.options) value["options"].push(option);
            value["doc"] = field.doc; value["module"] = ModuleOf(field.name);
            component["fields"].push(value);
        }
        out["components"].push(component);
    }
    out["channels"]["curveTime"] = "normalized age in 0..1; endpoint clamp; linear interpolation; at most 16 keys";
    out["channels"]["vectorCurve"] = "[[age,[x,y,z]],...] (signed finite values within -65504..65504; scale nonnegative)";
    out["channels"]["position"] = "positionOffset is an emitter-space birth offset; positionOverLife is added to integratedPosition without accumulating previous offsets. World particles use the captured birth emitter basis; local particles follow their owner.";
    out["channels"]["velocity"] = "velocityOverLife replaces velocity in emitter m/s before gravity, turbulence and drag each step. Empty curves preserve integrated velocity.";
    out["channels"]["rotation"] = "rotation3D + rotationOverLife in XYZ degrees, composed inside legacy Z rotation/spin; billboard axes or captured mesh emitter axes";
    out["channels"]["scale"] = "scale * scaleOverLife per axis, then legacy scalar size and meshScale each once; empty scale curve multiplies by one";
    out["channels"]["pivot"] = "unscaled geometry point mapped to particle center before scale/rotation; cube base is [0,-0.5,0]";
    out["channels"]["custom"] = "custom0..3 captured at birth; scalar curves replace values; particleCustom0..3 material ops; finite -65504..65504; no graph uniform slots";
    out["channels"]["index"] = "zero-based accepted birth order per emitter, reset by clear; count is accepted batch size captured at birth";
    out["channels"]["order"] = "age, velocity curve, gravity/turbulence/drag, velocity integration, position offset, rotation/scale/custom curves, expiry, births";
    out["params"]["available"] = true;
    out["params"]["syntax"] = "params[name] = {default, min?, max?, doc, targets:[{to:'emitterName.field', scale?}]}";
    out["limits"]["duration"] = 60; out["limits"]["previewFrames"] = 16; out["limits"]["analysisSamples"] = 241;
    out["limits"]["particleSteps"] = 20000000;
    out["limits"]["deathDepth"] = 4;
    out["limits"]["meshVerticesPerEmitter"] = 1000000;
    return out;
}

Json ApplyVfxPreset(Engine& engine, EntityId id, const Json& args) {
    Json chosen;
    const Json presets = VfxPresets("");
    for (const Json& preset : presets.items()) if (preset["name"] == args["preset"]) { chosen = preset; break; }
    if (chosen.isNull()) throw ApiError("unknown_preset", "unknown VFX preset '" + args["preset"].asString() + "'", "Call vfx.presets to list names and intent.");
    const std::string mode = args["mode"].asString("replace");
    if (mode != "replace" && mode != "merge") throw ApiError("invalid_vfx", "mode must be replace or merge", "replace resets the affected component; merge keeps unspecified settings.");
    Scene& scene = engine.GetScene();
    Json fields = Json::MakeArray();
    // Validate every block before touching the live entity.
    for (const char* blocks : {"emitters", "trails"}) for (const Json& block : chosen[blocks].items()) {
        const bool trail = std::string(blocks) == "trails";
        const ComponentType& type = *TypeRegistry::Find(trail ? "Trail" : "ParticleEmitter");
        ValidateVfxSettings(type, mode == "merge" ? scene.GetComponent(id, type) : nullptr, block[trail ? "trail" : "emitter"]);
    }
    int emitterIndex = 0, trailIndex = 0;
    for (const char* blocks : {"emitters", "trails"}) for (const Json& block : chosen[blocks].items()) {
        const bool trail = std::string(blocks) == "trails";
        const ComponentType& type = *TypeRegistry::Find(trail ? "Trail" : "ParticleEmitter");
        const int index = trail ? trailIndex++ : emitterIndex++;
        EntityId target = id;
        if (index > 0) {
            const std::string name = scene.Record(id)->name + "_" + block["name"].asString();
            target = kNullEntity;
            for (EntityId child : scene.Children(id)) if (scene.Record(child)->name == name) target = child;
            if (!target) target = scene.Create(name, id);
            if (!scene.Get<Transform>(target)) scene.Add<Transform>(target);
        }
        if (mode == "replace") scene.RemoveComponent(target, type);
        void* component = scene.AddComponent(target, type);
        const Json& values = block[trail ? "trail" : "emitter"];
        std::string error;
        if (!ApplyComponentJson(type, component, values, &error)) throw ApiError("invalid_vfx", error);
        for (const FieldInfo& field : type.fields) if (mode == "replace" || values.has(field.name)) fields.push(type.name + "." + field.name);
    }
    if (!scene.Get<Transform>(id)) scene.Add<Transform>(id);
    Json out = Json::MakeObject(); out["id"] = id; out["preset"] = chosen["name"]; out["mode"] = mode; out["appliedFields"] = fields;
    return out;
}

Json CheckVfx(Engine& engine, EntityId root) {
    Json out = Json::MakeObject(); out["ok"] = true; out["warnings"] = Json::MakeArray();
    bool found = false;
    auto warn = [&](EntityId id, const char* code, const char* message, const char* hint) {
        Json warning = Json::MakeObject(); warning["code"] = code; warning["message"] = message; warning["hint"] = hint;
        warning["emitter"] = engine.GetScene().Record(id)->name; warning["id"] = id; out["warnings"].push(warning);
    };
    for (EntityId id : EffectIds(engine.GetScene(), root)) {
        if (const VFXSystem* system = engine.GetScene().Get<VFXSystem>(id)) {
            found = true;
            ValidateVfxSystem(engine, system, Json::MakeObject());
            Json checked = CheckVfxAsset(engine, system->asset, EffectiveVfxParams(*system));
            for (const Json& warning : checked["warnings"].items()) { Json item = warning; item["id"] = id; out["warnings"].push(item); }
            uint64_t live = 0;
            for (const auto& block : system->emitters) live += block.particles.size();
            if (live > static_cast<uint64_t>(checked["budget"].asInt()))
                warn(id, "above_budget", "Live particles exceed the system budget.", "Step once to enforce the new cap, or call vfx.clear.");
        }
        const ParticleEmitter* emitter = engine.GetScene().Get<ParticleEmitter>(id);
        const Trail* trail = engine.GetScene().Get<Trail>(id);
        for (const char* name : {"ParticleEmitter", "Trail"}) {
            const ComponentType& type = *TypeRegistry::Find(name);
            const void* value = engine.GetScene().GetComponent(id, type);
            if (!value) continue;
            found = true; ValidateVfxSettings(type, nullptr, ComponentToJson(type, value));
            const std::string texture = name == std::string("Trail") ? trail->texture : emitter->texture;
            const std::string material = name == std::string("Trail") ? trail->material : emitter->material;
            if (!material.empty()) {
                engine.ResolvePath(material);
                std::string error;
                if (!engine.Assets().GetMaterial(material, &error))
                    throw ApiError("invalid_vfx", "Invalid effect material: " + error, "Create or repair the .mat.json and its shader graph.");
            }
            if (emitter && name == std::string("ParticleEmitter") && !emitter->mesh.empty()) {
                if (!GetBuiltinMesh(emitter->mesh)) engine.ResolvePath(emitter->mesh);
                const auto mesh = engine.Assets().GetMesh(emitter->mesh);
                if (!mesh || !mesh->joints.empty() || mesh->positions.size() > 65536 || mesh->indices.size() > 196608 ||
                    mesh->positions.size() * static_cast<size_t>(emitter->maxParticles) > 1000000)
                    throw ApiError("invalid_vfx", "Mesh particle geometry is missing, animated or exceeds its vertex budget",
                        "Use a static mesh up to 65536 vertices/196608 indices and at most 1000000 vertices times maxParticles.");
            }
            if (!texture.empty() && !FileExists(engine.ResolvePath(texture)))
                warn(id, "missing_texture", "Effect texture does not exist.", "Import the texture or clear the texture field to use solid geometry.");
        }
        if (emitter) {
            auto allZero = [](const Json& keys, float a, float b) {
                if (keys.size() == 0) return a <= 0 && b <= 0;
                for (const Json& key : keys.items()) if (key[1].asFloat() > 0) return false;
                return true;
            };
            if (allZero(emitter->sizeOverLife, emitter->startSize, emitter->endSize) ||
                allZero(emitter->opacityOverLife, emitter->startOpacity, emitter->endOpacity))
                warn(id, "invisible_effect", "Size or opacity is zero throughout the particle lifetime.", "Give size and opacity a positive value or curve key.");
            if (emitter->maxParticles == 0 || (emitter->burst == 0 && (emitter->rate == 0 || (!emitter->loop && emitter->duration == 0))))
                warn(id, "no_births", "This emitter has no automatic births.", "Set burst, enable loop and rate, or use a finite duration with rate.");
            if (!emitter->loop && !emitter->playing && !emitter->initialBurstEmitted)
                warn(id, "never_finishes", "A stopped one-shot cannot reach its startup burst and finished state.", "Set playing:true to start it, then let particles expire.");
            if (emitter->lifetime > 30) warn(id, "long_lifetime", "Particle lifetime exceeds 30 seconds.", "Reduce lifetime for a short effect or explicitly allow a long-lived effect.");
            if (emitter->particles.size() > static_cast<size_t>(emitter->maxParticles))
                warn(id, "above_budget", "Live count exceeds the configured cap.", "Step once to enforce a reduced cap or clear particles.");
            if ((emitter->loop || emitter->duration > 0) && emitter->rate * emitter->lifetime > static_cast<float>(emitter->maxParticles) * 2)
                warn(id, "capacity_too_small", "Capacity is far below rate times lifetime; births will be dropped.", "Increase maxParticles or reduce rate/lifetime.");
            if (emitter->space == "world" && !engine.GetScene().Get<Script>(id) && !engine.GetScene().Get<RigidBody>(id) && !engine.GetScene().Get<RigidBody2D>(id))
                warn(id, "stationary_world_space", "World-space emission has no movement driver on this entity.", "Use local space for attached effects, or move the owner/parent from gameplay; isolated previews accept motion keys.");
        }
        if (trail) {
            Vec3 base, tip;
            if (!TrailAnchors(engine.GetScene(), *trail, engine.GetScene().WorldMatrix(id), base, tip))
                warn(id, "missing_anchor", "Ribbon anchor entity is missing.", "Use unique existing entity names, or empty anchors with local offsets.");
            if ((trail->mode != "ribbon" && trail->width == 0 && trail->endWidth == 0) || (trail->opacity == 0 && trail->endOpacity == 0))
                warn(id, "invisible_effect", "Trail width or opacity is always zero.", "Set a positive width and opacity.");
            if (trail->points.size() > static_cast<size_t>(trail->maxPoints))
                warn(id, "above_budget", "Trail points exceed the configured cap.", "Step once or call trail.clear.");
            warn(id, "trail_requires_motion", "A stationary trail produces no ribbon.", "Move the owner in gameplay; supply motion keys for isolated preview/analyze.");
        }
    }
    if (!found) throw ApiError("no_emitter", "entity subtree has no ParticleEmitter or Trail", "Apply a preset or add ParticleEmitter/Trail first.");
    return out;
}
}  // namespace oe
