#include "scene/Vfx.h"
#include "scene/VfxExpression.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

#include "api/Commands.h"
#include "assets/Assets.h"
#include "core/Log.h"
#include "scene/Systems.h"

namespace oe {
namespace {
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
        if (field->type == FieldType::Json && field->name != "expressions") {
            ParticleCurve curve;
            const bool vector = field->name == "positionOverLife" || field->name == "velocityOverLife" ||
                field->name == "rotationOverLife" || field->name == "scaleOverLife";
            if (!(vector ? ParseParticleVectorCurve(v, curve, &error) : ParseParticleCurve(v, field->name == "colorOverLife", curve, &error)))
                    throw ApiError("invalid_curve", field->name + ": " + error, vector ? "Use up to 16 keys [[t,[x,y,z]],...] with t nondecreasing in 0..1." : "Use up to 16 keys [[t,value],...] with t nondecreasing in 0..1.");
            for (const Vec3& key : curve.values) {
                if (field->name == "scaleOverLife" && (key.x < 0 || key.y < 0 || key.z < 0))
                    throw ApiError("invalid_curve", "scaleOverLife must be nonnegative", "Use nonnegative XYZ multipliers.");
                if (field->name.compare(0, 6, "custom") == 0 && std::abs(key.x) > 65504)
                    throw ApiError("invalid_curve", "custom curve exceeds finite range", "Use values within -65504..65504.");
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
        VfxExpressionError expressionError;
        if (!PrepareVfxExpressions(emitter,&expressionError)) throw ApiError(expressionError.code,"expressions: "+expressionError.message,expressionError.hint);
        if (!ParticleSettingsValid(emitter)) throw ApiError("invalid_vfx", "invalid particle settings", "Check fields and curves with vfx.schema.");
    }
}

}  // namespace

void ValidateVfxSettings(const ComponentType& type, const void* current, const Json& values, const Json* expressionParams) {
    if (type.name != "ParticleEmitter" && type.name != "Trail") return;
    auto pool = type.makePool();
    void* copy = pool->Add(1);
    std::string error;
    if (current) ApplyComponentJson(type, copy, ComponentToJson(type, current), &error);
    if (type.name == "ParticleEmitter") {
        auto& emitter=*static_cast<ParticleEmitter*>(copy);
        if (expressionParams) emitter.expressionParams=*expressionParams;
        else if (current) emitter.expressionParams=static_cast<const ParticleEmitter*>(current)->expressionParams;
    }
    ValidateFields(type, copy, values);
}


namespace {
void Keys(const Json& value, std::initializer_list<const char*> allowed, const std::string& path) {
    if (!value.isObject()) throw ApiError("invalid_vfx", path + " must be an object", "Use the .vfx.json format in docs/VFX.md.");
    for (const auto& entry : value.members()) {
        bool known = false; std::string closest; size_t best = std::numeric_limits<size_t>::max();
        for (const char* key : allowed) {
            known = known || entry.first == key;
            const size_t distance = Distance(entry.first, key);
            if (distance < best) { best = distance; closest = key; }
        }
        if (!known) throw ApiError("unknown_field", path + "." + entry.first + " is unknown", "Did you mean '" + closest + "'? See docs/VFX.md.");
    }
}
bool NameValid(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    for (char c : name) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}
Json Sparse(const ComponentType& type, const void* value) {
    auto pool = type.makePool(); const void* defaults = pool->Add(1);
    Json out = Json::MakeObject();
    for (const FieldInfo& field : type.fields) {
        const Json json = FieldToJson(field, value);
        if (json != FieldToJson(field, defaults)) out[field.name] = json;
    }
    return out;
}
Mat4 LocalMatrix(const Json& block, const std::string& path) {
    if (block.has("matrix") && block.has("transform")) throw ApiError("invalid_vfx", path + " has both matrix and transform", "Use only one local transform representation.");
    Mat4 matrix;
    if (block.has("matrix")) {
        const Json& values = block["matrix"];
        if (!values.isArray() || values.size() != 16) throw ApiError("invalid_vfx", path + ".matrix needs 16 numbers", "Use column-major affine matrix values.");
        for (size_t i = 0; i < 16; ++i) {
            if (!values[i].isNumber() || !std::isfinite(values[i].asFloat()) || std::abs(values[i].asFloat()) > 1e6f)
                throw ApiError("invalid_range", path + ".matrix is non-finite or too large", "Use finite values within +/-1000000.");
            matrix.m[i] = values[i].asFloat();
        }
        if (matrix.at(3,0) != 0 || matrix.at(3,1) != 0 || matrix.at(3,2) != 0 || matrix.at(3,3) != 1)
            throw ApiError("invalid_vfx", path + ".matrix must be affine", "Last row must be [0,0,0,1].");
    } else if (block.has("transform")) {
        Keys(block["transform"], {"position", "rotation", "scale"}, path + ".transform");
        Transform transform;
        for (const auto& entry : block["transform"].members()) {
            if (!entry.second.isArray() || entry.second.size() != 3) throw ApiError("invalid_vfx", path + ".transform." + entry.first, "Use three finite numbers.");
            for (const Json& coordinate : entry.second.items()) if (!coordinate.isNumber() || !std::isfinite(coordinate.asFloat()) || std::abs(coordinate.asFloat()) > 1e6f)
                throw ApiError("invalid_range", path + ".transform is too large or non-finite", "Use finite numbers within +/-1000000.");
        }
        std::string error;
        if (!ApplyComponentJson(*TypeRegistry::Find("Transform"), &transform, block["transform"], &error)) throw ApiError("invalid_vfx", error);
        matrix = Mat4::TRS(transform.position, transform.rotation, transform.scale);
    }
    return matrix;
}
// Target parsing is shared by validation and parameter application; names never depend on array indices.
struct Target { bool trail = false; size_t index = 0; const FieldInfo* field = nullptr; };
Target FindTarget(const VfxAsset& asset, const std::string& to) {
    const size_t dot = to.find('.');
    if (dot == std::string::npos) throw ApiError("invalid_vfx", "invalid parameter target '" + to + "'", "Use blockName.fieldName.");
    const std::string name = to.substr(0, dot), field = to.substr(dot + 1);
    Target target; bool found = false;
    for (size_t i = 0; i < asset.emitters.size(); ++i) if (asset.emitters[i].name == name) { target.index = i; found = true; }
    for (size_t i = 0; i < asset.trails.size(); ++i) if (asset.trails[i].name == name) { target.trail = true; target.index = i; found = true; }
    if (!found) throw ApiError("invalid_vfx", "parameter target has no block '" + name + "'", "Use one of the named emitters/trails in this asset.");
    const ComponentType& type = *TypeRegistry::Find(target.trail ? "Trail" : "ParticleEmitter");
    target.field = type.FindField(field);
    if (!target.field) {
        Json bad = Json::MakeObject(); bad[field] = 0;
        ValidateVfxSettings(type, nullptr, bad);  // supplies the standard closest-field hint
    }
    return target;
}
}

void ConfigureVfx(const VfxAsset& asset, const Json& overrides, std::vector<ParticleEmitter>& emitters, std::vector<Trail>& trails) {
    if (!overrides.isObject()) throw ApiError("invalid_vfx", "params must be an object", "Use named values defined by this VFX asset.");
    const Json& parameters = asset.json["params"];
    for (const auto& entry : overrides.members()) if (!parameters.has(entry.first))
        throw ApiError("unknown_field", "unknown VFX parameter '" + entry.first + "'", "Call vfx.get to see params and their targets.");
    emitters.clear(); trails.clear();
    for (const auto& block : asset.emitters) emitters.push_back(block.emitter);
    for (const auto& block : asset.trails) trails.push_back(block.trail);
    Json expressionParams=Json::MakeObject();
    for (const auto& parameter : parameters.members()) expressionParams[parameter.first]=overrides.has(parameter.first)?overrides[parameter.first]:parameter.second["default"];
    for (auto& emitter : emitters) { emitter.expressionParams=expressionParams; emitter.expressionInSystem=true; }
    for (const auto& parameter : parameters.members()) {
        const Json& spec = parameter.second;
        const Json& value = overrides.has(parameter.first) ? overrides[parameter.first] : spec["default"];
        if (spec["default"].isNumber()) {
            if (!value.isNumber() || !std::isfinite(value.asNumber()) ||
                (spec.has("min") && value.asNumber() < spec["min"].asNumber()) || (spec.has("max") && value.asNumber() > spec["max"].asNumber()))
                throw ApiError("invalid_range", "params." + parameter.first + " is outside its range", "Use the min/max in vfx.get.");
        }
        for (const Json& mapping : spec["targets"].items()) {
            const Target target = FindTarget(asset, mapping["to"].asString());
            Json applied = value;
            if (mapping.has("scale")) {
                if (!value.isNumber()) throw ApiError("invalid_vfx", "scale requires a numeric parameter", "Remove scale for color, string, vector or boolean targets.");
                applied = value.asNumber() * mapping["scale"].asNumber();
            }
            const ComponentType& type = *TypeRegistry::Find(target.trail ? "Trail" : "ParticleEmitter");
            void* component = target.trail ? static_cast<void*>(&trails[target.index]) : static_cast<void*>(&emitters[target.index]);
            Json patch = Json::MakeObject(); patch[target.field->name] = applied;
            ValidateVfxSettings(type, component, patch);
            std::string error;
            if (!FieldFromJson(*target.field, component, applied, &error)) throw ApiError("invalid_vfx", error);
        }
    }
    int instructionCost=0;int64_t stepCost=0;
    for (auto& emitter : emitters) {
        VfxExpressionError error;
        if (!PrepareVfxExpressions(emitter,&error)) throw ApiError(error.code,error.message,error.hint);
        if (emitter.expressionProgram) {
            const int cost=emitter.expressionProgram->birthCost+emitter.expressionProgram->updateCost;
            instructionCost+=cost;stepCost+=static_cast<int64_t>(emitter.maxParticles)*5*cost;
        }
    }
    if (instructionCost>VfxExpressionSet::kEffectInstructions || stepCost>VfxExpressionSet::kStepInstructions)
        throw ApiError("expression_budget","effect expression budget exceeded","Use at most 512 instructions per asset and 1000000 worst-case instructions/step; reduce block capacities or expression cost.");
    for (const auto& emitter : emitters) if (!emitter.onDeath.empty()) {
        bool found = false; for (const auto& target : asset.emitters) found = found || target.name == emitter.onDeath;
        if (!found) throw ApiError("invalid_vfx", "unknown death target '" + emitter.onDeath + "'", "Use an emitter name from this asset.");
    }

}

VfxAsset ParseVfxAsset(const Json& json) {
    Keys(json, {"format", "version", "name", "description", "params", "emitters", "trails", "budget"}, "vfx");
    if (json["format"].asString() != "oe.vfx" || json["version"].asNumber() != 1)
        throw ApiError("invalid_vfx", "expected oe.vfx version 1", "Set format:'oe.vfx' and version:1.");
    if (json.dump().size() > 1024 * 1024) throw ApiError("invalid_range", "VFX asset exceeds 1 MiB", "Reduce blocks/parameters/strings.");
    for (const char* key : {"name", "description"}) if (json.has(key) && (!json[key].isString() || json[key].asString().size() > 4096))
        throw ApiError("invalid_vfx", std::string("vfx.") + key + " must be a bounded string", "Use text up to 4096 bytes.");
    VfxAsset asset; asset.json = json;
    asset.json["emitters"] = Json::MakeArray(); asset.json["trails"] = Json::MakeArray();
    Json expressionDefaults=Json::MakeObject();
    for (const auto& parameter : json["params"].members()) expressionDefaults[parameter.first]=parameter.second["default"];
    std::set<std::string> names;
    for (const char* key : {"emitters", "trails"}) {
        if (json.has(key) && (!json[key].isArray() || json[key].size() > 32)) throw ApiError("invalid_range", std::string("vfx.") + key, "Use arrays of at most 32 blocks.");
        const bool trail = std::string(key) == "trails";
        for (const Json& block : json[key].items()) {
            const std::string location = std::string("vfx.") + key + "[" + std::to_string(asset.json[key].size()) + "]";
            Keys(block, {"name", trail ? "trail" : "emitter", "transform", "matrix"}, location);
            const std::string name = block["name"].asString();
            if (!NameValid(name) || !names.insert(name).second) throw ApiError("invalid_vfx", location + ".name is invalid or repeated", "Use unique 1..64 character names containing letters, digits, '_' or '-'.");
            const ComponentType& type = *TypeRegistry::Find(trail ? "Trail" : "ParticleEmitter");
            const char* settings = trail ? "trail" : "emitter";
            const Json values = block.has(settings) ? block[settings] : Json::MakeObject();
            try { ValidateVfxSettings(type, nullptr, values, &expressionDefaults); }
            catch (const ApiError& ex) { throw ApiError(ex.code, location + "." + settings + " (" + name + "): " + ex.what(), ex.hint); }
            const Mat4 local = LocalMatrix(block, location);
            Json canonical = block;
            std::string error;
            if (trail) {
                VfxTrailDefinition definition; definition.name = name; definition.local = local;
                ApplyComponentJson(type, &definition.trail, values, &error);
                canonical[settings] = Sparse(type, &definition.trail); asset.trails.push_back(definition);
            } else {
                VfxEmitterDefinition definition; definition.name = name; definition.local = local;
                ApplyComponentJson(type, &definition.emitter, values, &error);
                definition.emitter.expressionParams=expressionDefaults;
                canonical[settings] = Sparse(type, &definition.emitter); asset.emitters.push_back(definition);
            }
            asset.json[key].push(canonical);
        }
    }
    if (names.empty() || names.size() > 32) throw ApiError("invalid_vfx", "VFX needs 1..32 total blocks", "Add emitters or trails.");
    if (json.has("budget")) {
        Keys(json["budget"], {"maxParticles"}, "vfx.budget");
        const Json& budget = json["budget"]["maxParticles"];
        if (!budget.isNumber() || !std::isfinite(budget.asNumber()) || budget.asNumber() < 0 || budget.asNumber() > 40000 || std::floor(budget.asNumber()) != budget.asNumber())
            throw ApiError("invalid_range", "budget.maxParticles must be 0..40000", "Use a whole number.");
        asset.maxParticles = budget.asInt();
    }
    if (json.has("params") && (!json["params"].isObject() || json["params"].size() > 32)) throw ApiError("invalid_range", "params must have at most 32 entries", "Use named parameter definitions.");
    if (!asset.json.has("params")) asset.json["params"] = Json::MakeObject();
    for (const auto& parameter : asset.json["params"].members()) {
        if (!NameValid(parameter.first)) throw ApiError("invalid_vfx", "invalid parameter name", "Use the same naming rules as blocks.");
        const Json& spec = parameter.second;
        Keys(spec, {"default", "min", "max", "doc", "targets"}, "params." + parameter.first);
        if (!spec.has("default") || !spec["targets"].isArray() || spec["targets"].size() > 32)
            throw ApiError("invalid_vfx", "parameter needs default and 0..32 targets", "Use {default:1,targets:[{to:'flames.rate',scale:40}]}.");
        if (spec.has("doc") && !spec["doc"].isString()) throw ApiError("invalid_vfx", "parameter doc must be text");
        for (const char* bound : {"min", "max"}) if (spec.has(bound) && (!spec["default"].isNumber() || !spec[bound].isNumber() || !std::isfinite(spec[bound].asNumber())))
            throw ApiError("invalid_range", "numeric parameter bounds must be finite", "Use min/max only with a numeric default.");
        if (spec.has("min") && spec.has("max") && spec["min"].asNumber() > spec["max"].asNumber()) throw ApiError("invalid_range", "parameter min exceeds max");
        for (const Json& mapping : spec["targets"].items()) {
            Keys(mapping, {"to", "scale"}, "params." + parameter.first + ".targets");
            if (!mapping["to"].isString()) throw ApiError("invalid_vfx", "parameter target needs a string 'to'", "Use blockName.fieldName.");
            FindTarget(asset, mapping["to"].asString());
            if (mapping.has("scale") && (!mapping["scale"].isNumber() || !std::isfinite(mapping["scale"].asNumber()))) throw ApiError("invalid_range", "target scale must be finite");
        }
    }
    for (const auto& block : asset.emitters) if (!block.emitter.onDeath.empty()) {
        bool found = false;
        for (const auto& target : asset.emitters) found = found || target.name == block.emitter.onDeath;
        if (!found) throw ApiError("invalid_vfx", "unknown death target '" + block.emitter.onDeath + "'", "Use an emitter name in this asset; chains stop after depth 4.");
    }
    std::vector<ParticleEmitter> emitters; std::vector<Trail> trails;
    ConfigureVfx(asset, Json::MakeObject(), emitters, trails);
    return asset;
}

uint32_t VfxSeedMix(size_t index) { return static_cast<uint32_t>(index) * 0x27d4eb2du; }

Json EffectiveVfxParams(const VFXSystem& system) {
    Json params = system.params;
    if (!params.isObject()) return params;  // ConfigureVfx reports malformed authored settings.
    for (const auto& pair : system.runtimeParams.members()) params[pair.first] = pair.second;
    return params;
}

bool VfxPlaying(const VFXSystem& system) { return system.playbackOverride ? system.runtimePlaying : system.playing; }

bool RefreshVfx(VFXSystem& system, AssetManager& assets, std::string* error) {
    std::string message;
    const auto definition = assets.GetVfx(system.asset, &message);
    if (!definition) { system.error = message; if (error) *error = message; return false; }
    const bool changedPath = !system.loadedPath.empty() && system.loadedPath != system.asset;
    const Json params = changedPath ? system.params : EffectiveVfxParams(system);
    if (definition == system.definition && system.loadedPath == system.asset && system.appliedParams == params) {
        system.error.clear(); return true;
    }
    std::vector<ParticleEmitter> emitters; std::vector<Trail> trails;
    try { ConfigureVfx(*definition, params, emitters, trails); }
    catch (const ApiError& ex) { system.error = ex.what(); if (error) *error = system.error; return false; }
    bool restart = !system.definition || system.loadedPath != system.asset || system.emitters.size() != emitters.size() || system.trails.size() != trails.size();
    if (!restart) {
        for (size_t i = 0; i < emitters.size(); ++i) restart = restart || definition->emitters[i].name != system.definition->emitters[i].name;
        for (size_t i = 0; i < trails.size(); ++i) restart = restart || definition->trails[i].name != system.definition->trails[i].name;
    }
    if (restart) { system.emitters = std::move(emitters); system.trails = std::move(trails); system.started = false; system.expressionTime=0; }
    else {
        std::string ignored;
        const ComponentType& emitterType = *TypeRegistry::Find("ParticleEmitter"), &trailType = *TypeRegistry::Find("Trail");
        for (size_t i = 0; i < emitters.size(); ++i) {
            ApplyComponentJson(emitterType, &system.emitters[i], ComponentToJson(emitterType, &emitters[i]), &ignored);
            system.emitters[i].expressionParams=emitters[i].expressionParams;system.emitters[i].expressionInSystem=true;
            system.emitters[i].expressionProgram=emitters[i].expressionProgram;
        }
        for (size_t i = 0; i < trails.size(); ++i) ApplyComponentJson(trailType, &system.trails[i], ComponentToJson(trailType, &trails[i]), &ignored);
    }
    system.definition = definition; system.loadedPath = system.asset; system.appliedParams = params; system.error.clear();
    if (changedPath) { system.runtimeParams = Json::MakeObject(); system.playbackOverride = false; system.suppressDestroy = false; }
    return true;
}

bool VfxFinished(const VFXSystem& system) {
    if (!system.definition || !system.started) return false;
    for (const auto& emitter : system.emitters)
        if (!emitter.particles.empty() || (VfxPlaying(system) && emitter.playing && !ParticlesFinished(emitter))) return false;
    for (const auto& trail : system.trails) if (!trail.points.empty() || (VfxPlaying(system) && TrailEmitting(trail))) return false;
    return true;
}

void UpdateVfxSystems(Scene& scene, AssetManager* assets, float dt) {
    for (auto& pair : scene.Pool<VFXSystem>()) {
        VFXSystem& system = pair.second;
        if (assets) RefreshVfx(system, *assets);
        if (!system.definition) continue;
        int remaining = system.definition->maxParticles;
        for (auto& emitter : system.emitters) {
            if (emitter.particles.size() > static_cast<size_t>(remaining)) emitter.particles.resize(static_cast<size_t>(remaining));
            remaining -= static_cast<int>(emitter.particles.size());
        }
        const Mat4 root = scene.WorldMatrix(pair.first);
        std::vector<std::vector<Particle>> deaths(system.emitters.size());
        for (size_t i = 0; i < system.emitters.size(); ++i) {
            ParticleEmitter& emitter = system.emitters[i];
            size_t otherCount = 0;
            for (size_t j = 0; j < system.emitters.size(); ++j) if (j != i) otherCount += system.emitters[j].particles.size();
            const int originalCap = emitter.maxParticles; const bool originalPlaying = emitter.playing;
            emitter.maxParticles = std::min(originalCap, std::max(0, system.definition->maxParticles - static_cast<int>(otherCount)));
            emitter.playing = originalPlaying && VfxPlaying(system);
            emitter.expressionSystemTime=system.expressionTime;
            StepEmitter(emitter, pair.first, root * system.definition->emitters[i].local, VfxSeedMix(i), dt, emitter.onDeath.empty() ? nullptr : &deaths[i]);
            emitter.maxParticles = originalCap; emitter.playing = originalPlaying;
        }
        // Queue death bursts only after all emitters stepped: targets never age a newborn
        // on the same step, regardless of block order. Depth is carried by each particle.
        for (size_t i = 0; VfxPlaying(system) && i < deaths.size(); ++i) {
            const auto& source = system.emitters[i];
            if (source.onDeath.empty() || source.deathCount <= 0) continue;
            size_t targetIndex = system.emitters.size();
            for (size_t j = 0; j < system.emitters.size(); ++j)
                if (system.definition->emitters[j].name == source.onDeath) targetIndex = j;
            if (targetIndex == system.emitters.size()) continue;
            auto& target = system.emitters[targetIndex];
            for (const Particle& particle : deaths[i]) {
                if (particle.depth >= 4) continue;
                size_t live = 0; for (const auto& block : system.emitters) live += block.particles.size();
                const int cap = target.maxParticles;
                target.maxParticles = std::min(cap, static_cast<int>(target.particles.size()) +
                    std::max(0, system.definition->maxParticles - static_cast<int>(live)));
                const Mat4 world = root * system.definition->emitters[i].local;
                const Vec3 position = particle.worldSpace ? particle.position : world.TransformPoint(particle.position);
                const std::string space = target.space; target.space = "world";
                const size_t before = target.particles.size();
                // Keep target orientation/shape, but place its origin at the death position.
                Mat4 spawn = root * system.definition->emitters[targetIndex].local;
                spawn.at(0,3) = position.x; spawn.at(1,3) = position.y; spawn.at(2,3) = position.z;
                BurstEmitter(target, pair.first, spawn, VfxSeedMix(targetIndex), source.deathCount);
                for (size_t k = before; k < target.particles.size(); ++k) target.particles[k].depth = particle.depth + 1;
                target.space = space; target.maxParticles = cap;
            }
        }
        system.expressionTime+=dt;
        system.started = true;
    }
}

void UpdateVfxTrails(Scene& scene, float dt) {
    std::vector<EntityId> destroy;
    for (auto& pair : scene.Pool<VFXSystem>()) {
        VFXSystem& system = pair.second;
        if (!system.definition) continue;
        const Mat4 root = scene.WorldMatrix(pair.first);
        for (size_t i = 0; i < system.trails.size(); ++i) {
            Trail& trail = system.trails[i]; const bool emitting = trail.emitting;
            trail.emitting = emitting && VfxPlaying(system);
            Vec3 base, tip;
            if (!TrailAnchors(scene, trail, root * system.definition->trails[i].local, base, tip)) trail.emitting = false;
            StepAnchoredTrail(trail, base, tip, dt);
            trail.emitting = emitting;
        }
        if (system.autoDestroy && !system.suppressDestroy && VfxFinished(system)) destroy.push_back(pair.first);
    }
    for (EntityId id : destroy) if (scene.Exists(id)) scene.Destroy(id);
}
}  // namespace oe
