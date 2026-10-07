#include "app/Vfx.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

#include "app/Engine.h"
#include "core/FileSystem.h"
#include "scene/Components.h"
#include "scene/Systems.h"

namespace oe {
namespace {
std::string ModuleOf(const std::string& name) {
    if (name.find("OverLife") != std::string::npos) return "Curves";
    if (name == "delay" || name == "duration" || name == "loop" || name == "playing" || name == "lifetime" || name == "lifetimeVariation") return "Timing";
    if (name == "rate" || name == "burst" || name == "maxParticles" || name == "seed" || name == "emitting" || name == "maxPoints") return "Emission";
    if (name == "shape" || name == "shapeSize" || name == "shapeEdge" || name == "dimensions" || name == "spread") return "Shape";
    if (name == "texture" || name == "blend" || name == "layer" || name == "pixelArt" || name == "frame" || name == "frames" ||
        name == "columns" || name == "rows" || name == "frameRate" || name == "randomFrame") return "Render";
    if (name == "space" || name == "direction" || name == "gravity" || name == "drag" || name == "radialSpeed" ||
        name == "turbulence" || name == "turbulenceScale" || name == "speed" || name == "speedVariation" || name == "minDistance") return "Motion";
    return "Appearance";
}

size_t Distance(const std::string& a, const std::string& b) {
    std::vector<size_t> row(b.size() + 1);
    for (size_t i = 0; i < row.size(); ++i) row[i] = i;
    for (size_t i = 0; i < a.size(); ++i) {
        size_t diagonal = row[0]; row[0] = i + 1;
        for (size_t j = 0; j < b.size(); ++j) {
            const size_t old = row[j + 1];
            row[j + 1] = std::min({row[j] + 1, row[j + 1] + 1, diagonal + (a[i] == b[j] ? 0u : 1u)});
            diagonal = old;
        }
    }
    return row.back();
}

void ValidateFields(const ComponentType& type, void* value, const Json& values) {
    if (!values.isObject()) throw ApiError("invalid_vfx", type.name + " settings must be an object", "Use field names from vfx.schema.");
    for (const auto& entry : values.members()) {
        const FieldInfo* field = type.FindField(entry.first);
        if (!field) {
            const FieldInfo* closest = nullptr;
            size_t best = std::numeric_limits<size_t>::max();
            for (const FieldInfo& candidate : type.fields) {
                const size_t distance = Distance(entry.first, candidate.name);
                if (distance < best) { best = distance; closest = &candidate; }
            }
            throw ApiError("unknown_field", type.name + " has no field '" + entry.first + "'",
                           "Did you mean '" + closest->name + "'? Call vfx.schema for valid fields.");
        }
        const Json& v = entry.second;
        auto rangeError = [&] {
            throw ApiError("invalid_range", type.name + "." + field->name + " is outside its valid range",
                           "Use finite values within the min/max in vfx.schema; integer fields require whole numbers.");
        };
        if (field->type == FieldType::Float || field->type == FieldType::Int) {
            if (!v.isNumber() || !std::isfinite(v.asNumber()) || std::abs(v.asNumber()) > std::numeric_limits<float>::max()) rangeError();
            if (field->hasRange && (v.asFloat() < field->min || v.asFloat() > field->max)) rangeError();
            if (field->type == FieldType::Int && (std::floor(v.asNumber()) != v.asNumber() ||
                v.asNumber() < std::numeric_limits<int>::min() || v.asNumber() > std::numeric_limits<int>::max())) rangeError();
        }
        if (field->type == FieldType::Vec3 || field->type == FieldType::Color) {
            if (v.isArray()) {
                if (v.size() != 3) rangeError();
                for (const Json& coordinate : v.items())
                    if (!coordinate.isNumber() || !std::isfinite(coordinate.asFloat())) rangeError();
            } else if (v.isObject()) {
                const std::string keys = field->type == FieldType::Color ? "rgb" : "xyz";
                if (v.size() == 0) rangeError();
                for (const auto& coordinate : v.members())
                    if (coordinate.first.size() != 1 || keys.find(coordinate.first) == std::string::npos ||
                        !coordinate.second.isNumber() || !std::isfinite(coordinate.second.asFloat())) rangeError();
            } else if (field->type != FieldType::Color || !v.isString() || v.asString().empty()) rangeError();
        }
        std::string error;
        if (field->type == FieldType::Json) {
            ParticleCurve curve;
            if (!ParseParticleCurve(v, field->name == "colorOverLife", curve, &error))
                    throw ApiError("invalid_curve", field->name + ": " + error, "Use up to 16 keys [[t,value],...] with t nondecreasing in 0..1.");
            for (const Vec3& key : curve.values) {
                if ((field->name == "sizeOverLife" && key.x < 0) || (field->name == "opacityOverLife" && (key.x < 0 || key.x > 1)))
                    throw ApiError("invalid_curve", field->name + " has an invalid value", "Sizes must be nonnegative; opacity must be 0..1.");
            }
        }
        if (!FieldFromJson(*field, value, v, &error)) throw ApiError("invalid_vfx", error, "Use the type/options in vfx.schema.");
    }
    if (type.name == "ParticleEmitter") {
        const auto& emitter = *static_cast<ParticleEmitter*>(value);
        if (emitter.shapeSize.x < 0 || emitter.shapeSize.y < 0 || emitter.shapeSize.z < 0)
            throw ApiError("invalid_range", "shapeSize must be nonnegative", "Use a radius or nonnegative half extents.");
        if (!ParticleSettingsValid(emitter)) throw ApiError("invalid_vfx", "invalid particle settings", "Check fields and curves with vfx.schema.");
    }
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

void ValidateVfxSettings(const ComponentType& type, const void* current, const Json& values) {
    if (type.name != "ParticleEmitter" && type.name != "Trail") return;
    auto pool = type.makePool();
    void* copy = pool->Add(1);
    std::string error;
    if (current) ApplyComponentJson(type, copy, ComponentToJson(type, current), &error);
    ValidateFields(type, copy, values);
}

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
    out["params"]["available"] = false;
    out["params"]["syntax"] = "Phase 2: params[name] = {default, min?, max?, doc, targets:[{to:'emitterName.field', scale?}]}";
    out["limits"]["duration"] = 60; out["limits"]["previewFrames"] = 16; out["limits"]["analysisSamples"] = 241;
    out["limits"]["particleSteps"] = 20000000;
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
    for (const char* blocks : {"emitters", "trails"}) for (const Json& block : chosen[blocks].items()) {
        const bool trail = std::string(blocks) == "trails";
        const ComponentType& type = *TypeRegistry::Find(trail ? "Trail" : "ParticleEmitter");
        if (mode == "replace") scene.RemoveComponent(id, type);
        void* component = scene.AddComponent(id, type);
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
        const ParticleEmitter* emitter = engine.GetScene().Get<ParticleEmitter>(id);
        const Trail* trail = engine.GetScene().Get<Trail>(id);
        for (const char* name : {"ParticleEmitter", "Trail"}) {
            const ComponentType& type = *TypeRegistry::Find(name);
            const void* value = engine.GetScene().GetComponent(id, type);
            if (!value) continue;
            found = true; ValidateVfxSettings(type, nullptr, ComponentToJson(type, value));
            const std::string texture = name == std::string("Trail") ? trail->texture : emitter->texture;
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
            if ((trail->width == 0 && trail->endWidth == 0) || (trail->opacity == 0 && trail->endOpacity == 0))
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
