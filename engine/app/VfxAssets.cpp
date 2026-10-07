#include "app/Vfx.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "app/Engine.h"
#include "assets/Assets.h"
#include "core/FileSystem.h"
#include "scene/Components.h"
#include "scene/Systems.h"
#include "scene/Vfx.h"

namespace oe {
namespace {
std::string AssetPath(Engine& engine, const Json& args) {
    const std::string path = args["path"].asString();
    engine.ResolvePath(path);
    if (AssetManager::KindOf(path) != "vfx")
        throw ApiError("invalid_vfx", "VFX path must end in .vfx.json", "Use effects/fire.vfx.json inside the project.");
    return path;
}

void TexturePaths(Engine& engine, const VfxAsset& asset, const Json& params = Json::MakeObject()) {
    std::vector<ParticleEmitter> emitters; std::vector<Trail> trails;
    ConfigureVfx(asset, params, emitters, trails);
    for (const auto& emitter : emitters) if (!emitter.texture.empty()) engine.ResolvePath(emitter.texture);
    for (const auto& trail : trails) if (!trail.texture.empty()) engine.ResolvePath(trail.texture);
}

VfxAsset ReadAsset(Engine& engine, const std::string& path) {
    Json args = Json::MakeObject(); args["path"] = path; AssetPath(engine, args);
    const Json* document = engine.VfxDocument(path);
    const VfxAsset asset = ParseVfxAsset(document ? *document : engine.ReadProjectJson(path));
    TexturePaths(engine, asset);
    return asset;
}

Json WriteAsset(Engine& engine, const std::string& path, const Json& json) {
    const VfxAsset asset = ParseVfxAsset(json); TexturePaths(engine, asset);
    const std::string full = engine.ResolvePath(path); std::string error;
    if (!WriteTextFileAtomic(full, asset.json.dump(2) + "\n", &error))
        throw ApiError("write_failed", error, "Choose a writable project-relative .vfx.json path.");
    engine.Assets().Forget(path);
    return asset.json;
}

void Diff(const Json& before, const Json& after, const std::string& field, Json& out) {
    if (before == after) return;
    if (before.isObject() && after.isObject()) {
        std::set<std::string> keys;
        for (const auto& pair : before.members()) keys.insert(pair.first);
        for (const auto& pair : after.members()) keys.insert(pair.first);
        for (const auto& key : keys) Diff(before[key], after[key], field.empty() ? key : field + "." + key, out);
    } else {
        Json change = Json::MakeObject(); change["field"] = field; change["before"] = before; change["after"] = after; out.push(change);
    }
}

Json Semantic(const VfxAsset& asset) {
    Json result = Json::MakeObject(); result["$params"] = asset.json["params"]; result["$budget"] = asset.maxParticles;
    for (size_t i = 0; i < asset.emitters.size(); ++i) {
        const std::string& name = asset.emitters[i].name;
        result[name] = ComponentToJson(*TypeRegistry::Find("ParticleEmitter"), &asset.emitters[i].emitter);
        Json matrix = Json::MakeArray(); for (float value : asset.emitters[i].local.m) matrix.push(value);
        result[name]["matrix"] = matrix;
    }
    for (const auto& block : asset.trails) {
        result[block.name] = ComponentToJson(*TypeRegistry::Find("Trail"), &block.trail);
        Json matrix = Json::MakeArray(); for (float value : block.local.m) matrix.push(value);
        result[block.name]["matrix"] = matrix;
    }
    return result;
}
}  // namespace

Json SaveVfxDocument(Engine& engine, const std::string& path, const Json& document) {
    return WriteAsset(engine, path, document);
}

void ValidateVfxSystem(Engine& engine, const void* current, const Json& values) {
    VFXSystem candidate = current ? *static_cast<const VFXSystem*>(current) : VFXSystem{};
    std::string error;
    if (!ApplyComponentJson(*TypeRegistry::Find("VFXSystem"), &candidate, values, &error)) throw ApiError("invalid_vfx", error);
    if (candidate.asset.empty()) throw ApiError("invalid_vfx", "VFXSystem needs an asset", "Create an asset with vfx.create first.");
    const auto asset = ReadAsset(engine, candidate.asset);
    TexturePaths(engine, asset, candidate.params);
}

Json CheckVfxAsset(Engine& engine, const std::string& path, const Json& params) {
    const auto asset = ReadAsset(engine, path);
    // Reuse the component checker in a temporary engine scene, never touching the caller's scene.
    Engine isolated; std::string error;
    if (!isolated.Open(engine.ProjectDir(), &error)) throw ApiError("invalid_project", error);
    isolated.NewScene("VFX check");
    const EntityId root = isolated.GetScene().Create("VFX");
    std::vector<ParticleEmitter> emitters; std::vector<Trail> trails;
    ConfigureVfx(asset, params, emitters, trails);
    for (size_t i = 0; i < emitters.size(); ++i) {
        EntityId id = isolated.GetScene().Create(asset.emitters[i].name, root); isolated.GetScene().Add<ParticleEmitter>(id) = emitters[i];
    }
    for (size_t i = 0; i < trails.size(); ++i) {
        EntityId id = isolated.GetScene().Create(asset.trails[i].name, root); isolated.GetScene().Add<Trail>(id) = trails[i];
    }
    Json result = CheckVfx(isolated, root); result["path"] = path; result["budget"] = asset.maxParticles;
    return result;
}

Json VfxAssetCall(Engine& engine, const std::string& operation, const Json& args) {
    if (operation == "list") {
        Json result = Json::MakeArray();
        for (const auto& path : ListFiles(engine.ProjectDir(), ".vfx.json", true)) result.push(RelativePath(path, engine.ProjectDir()));
        return result;
    }
    const std::string path = AssetPath(engine, args);
    if (operation == "create") {
        if (engine.VfxDocument(path)) throw ApiError("vfx_editing", "asset is open in the Effects document", "Use vfx.set and vfx.save, or close the document first.");
        return WriteAsset(engine, path, args["vfx"]);
    }
    if (operation == "from_entity") {
        const EntityId root = args["id"].isString() ? engine.GetScene().FindByName(args["id"].asString()) : static_cast<EntityId>(args["id"].asInt());
        Scene& scene = engine.GetScene();
        if (!scene.Exists(root)) throw ApiError("entity_not_found", "effect root does not exist");
        const Mat4 rootWorld = scene.WorldMatrix(root), inverse = rootWorld.Inverse();
        const Mat4 identity = inverse * rootWorld;
        for (int row = 0; row < 4; ++row) for (int column = 0; column < 4; ++column)
            if (!std::isfinite(identity.at(row, column)) || std::abs(identity.at(row, column) - (row == column ? 1.0f : 0.0f)) > 0.001f)
                throw ApiError("invalid_vfx", "effect root transform is singular", "Use a nonzero scale before packing an effect.");
        Json json = Json::MakeObject(); json["format"] = "oe.vfx"; json["version"] = 1;
        json["name"] = scene.Record(root)->name; json["emitters"] = Json::MakeArray(); json["trails"] = Json::MakeArray();
        std::set<EntityId> selected{root}; std::vector<EntityId> pending{root}; std::set<std::string> names;
        while (!pending.empty()) {
            EntityId id = pending.back(); pending.pop_back();
            for (EntityId child : scene.Children(id)) if (selected.insert(child).second) pending.push_back(child);
            if (selected.size() > 256) throw ApiError("invalid_range", "effect subtree exceeds 256 entities");
        }
        for (EntityId id : selected) for (const char* typeName : {"ParticleEmitter", "Trail"}) {
            const auto& type = *TypeRegistry::Find(typeName); const void* value = scene.GetComponent(id, type);
            if (!value) continue;
            std::string name = scene.Record(id)->name;
            for (char& character : name) if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                (character >= '0' && character <= '9') || character == '_' || character == '-')) character = '_';
            name = name.substr(0, 48); if (name.empty()) name = "effect";
            if (names.count(name)) name += "_" + std::to_string(id) + (std::string(typeName) == "Trail" ? "_trail" : "_particles");
            names.insert(name);
            Json block = Json::MakeObject(); block["name"] = name;
            const bool trail = std::string(typeName) == "Trail";
            block[trail ? "trail" : "emitter"] = ComponentToJson(type, value);
            block["matrix"] = Json::MakeArray(); const Mat4 local = inverse * scene.WorldMatrix(id);
            for (float number : local.m) block["matrix"].push(number);
            json[trail ? "trails" : "emitters"].push(block);
        }
        if (engine.VfxDocument(path)) throw ApiError("vfx_editing", "export would overwrite the open Effects document", "Choose another path or close the document first.");
        return WriteAsset(engine, path, json);
    }
    const VfxAsset asset = ReadAsset(engine, path);
    if (operation == "get") return asset.json;
    if (operation == "set") {
        Json json = asset.json;
        const int modes = (args.has("emitter") ? 1 : 0) + (args.has("trail") ? 1 : 0) + (args.has("params") ? 1 : 0);
        if (modes != 1) throw ApiError("invalid_vfx", "select exactly one emitter, trail or params", "Use {path,emitter:'flames',values:{rate:60}}.");
        if (args.has("params")) {
            if (args.has("values")) throw ApiError("invalid_vfx", "params edit cannot also have values");
            json["params"] = args["params"];
        } else {
            const bool trail = args.has("trail"); const char* key = trail ? "trail" : "emitter";
            bool found = false;
            if (!args["values"].isObject()) throw ApiError("invalid_vfx", "values must be an object");
            for (Json& block : json[trail ? "trails" : "emitters"].items()) if (block["name"] == args[key]) {
                found = true; for (const auto& pair : args["values"].members()) block[key][pair.first] = pair.second;
            }
            if (!found) throw ApiError("no_emitter", "asset block does not exist", "Call vfx.get to list block names.");
        }
        if (engine.VfxDocument(path)) {
            Json edit = Json::MakeObject(); edit["vfx"] = json;
            if (args.has("merge")) edit["merge"] = args["merge"];
            return engine.VfxDocumentCall("replace", edit)["document"];
        }
        return WriteAsset(engine, path, json);
    }
    if (operation == "diff") {
        Json before;
        if (args.has("against")) before = Semantic(ReadAsset(engine, args["against"].asString()));
        else {
            VfxAsset defaults = asset;
            for (auto& block : defaults.emitters) { block.emitter = ParticleEmitter{}; block.local = Mat4{}; }
            for (auto& block : defaults.trails) { block.trail = Trail{}; block.local = Mat4{}; }
            defaults.json["params"] = Json::MakeObject(); defaults.maxParticles = 10000; before = Semantic(defaults);
        }
        Json result = Json::MakeArray(); Diff(before, Semantic(asset), "", result);
        for (Json& change : result.items()) {
            const std::string field = change["field"].asString();
            if (!field.empty() && field[0] == '$') change["field"] = field.substr(1);
        }
        return result;
    }
    throw ApiError("invalid_vfx", "unknown VFX asset operation");
}

Json VfxRuntimeCall(Engine& engine, const std::string& operation, EntityId id, const Json& args) {
    Scene& scene = engine.GetScene();
    if (operation == "spawn") {
        const std::string path = AssetPath(engine, args);
        std::string error;
        const auto cached = engine.Assets().GetVfx(path, &error);
        if (!cached) throw ApiError("invalid_vfx", error, "Validate the file with vfx.check.");
        const VfxAsset& asset = *cached;
        const Json params = args.has("params") ? args["params"] : Json::MakeObject(); TexturePaths(engine, asset, params);
        Transform transform;
        if (args.has("position")) {
            Json fields = Json::MakeObject(); fields["position"] = args["position"];
            if (!args["position"].isArray() || args["position"].size() != 3) throw ApiError("invalid_vfx", "position must be [x,y,z]");
            for (const auto& number : args["position"].items()) if (!number.isNumber() || !std::isfinite(number.asFloat()) || std::abs(number.asFloat()) > 1e6f)
                throw ApiError("invalid_range", "position must be finite and within +/-1000000");
            ApplyComponentJson(*TypeRegistry::Find("Transform"), &transform, fields, &error);
        }
        const EntityId parent = args["parent"].isString() ? scene.FindByName(args["parent"].asString()) : static_cast<EntityId>(args["parent"].asInt());
        if (args.has("parent") && parent != 0 && !scene.Exists(parent)) throw ApiError("entity_not_found", "parent does not exist");
        if (args["parent"].isString() && parent == 0) throw ApiError("entity_not_found", "parent does not exist");
        VFXSystem system; system.asset = path; system.params = params; system.autoDestroy = args["autoDestroy"].asBool(true);
        if (!RefreshVfx(system, engine.Assets(), &error)) throw ApiError("invalid_vfx", error);
        id = scene.Create(args["name"].asString(asset.json["name"].asString("VFX")), parent);
        scene.Add<Transform>(id) = transform; scene.Add<VFXSystem>(id) = std::move(system);
        Json out = Json::MakeObject(); out["id"] = id; out["asset"] = path; return out;
    }
    VFXSystem* system = scene.Get<VFXSystem>(id);
    if (!system) throw ApiError("no_vfx", "entity has no VFXSystem", "Use vfx.spawn or add a VFXSystem linked to an asset.");
    VFXSystem query;
    std::string error;
    if (operation == "state" && !system->definition) {
        query = *system; system = &query;  // initialize a query copy before the first simulation step
        if (!RefreshVfx(query, engine.Assets(), &error)) throw ApiError("invalid_vfx", error);
    } else if (operation != "state" && !RefreshVfx(*system, engine.Assets(), &error)) throw ApiError("invalid_vfx", error);
    if (operation == "set_param") {
        Json params = EffectiveVfxParams(*system); params[args["name"].asString()] = args["value"];
        TexturePaths(engine, *system->definition, params); system->runtimeParams[args["name"].asString()] = args["value"];
        RefreshVfx(*system, engine.Assets(), &error);
    } else if (operation == "play") {
        if (args["restart"].asBool(true)) { system->definition.reset(); RefreshVfx(*system, engine.Assets(), &error); }
        system->playbackOverride = true; system->runtimePlaying = true; system->suppressDestroy = false;
    } else if (operation == "stop" || operation == "clear") {
        if (operation == "stop") { system->playbackOverride = true; system->runtimePlaying = false; system->started = true; system->suppressDestroy = false; }
        if (operation == "clear" || args["kill"].asBool()) {
            for (auto& emitter : system->emitters) emitter.particles.clear();
            for (auto& trail : system->trails) trail.points.clear();
            if (operation == "clear") system->suppressDestroy = true;
        }
    } else if (operation == "burst") {
        if (args.has("count") && (args["count"].asNumber() < 0 || args["count"].asNumber() > 10000 ||
            std::floor(args["count"].asNumber()) != args["count"].asNumber())) throw ApiError("invalid_range", "burst count must be 0..10000");
        bool found = !args.has("emitter"); int accepted = 0;
        for (const auto& block : system->definition->emitters) if (block.name == args["emitter"].asString()) found = true;
        if (!found) throw ApiError("no_emitter", "unknown emitter", "Call vfx.state to list names.");
        size_t live = 0; for (const auto& emitter : system->emitters) live += emitter.particles.size();
        for (size_t i = 0; i < system->emitters.size(); ++i) {
            if (args.has("emitter") && system->definition->emitters[i].name != args["emitter"].asString()) continue;
            auto& emitter = system->emitters[i];
            const int count = std::min(args["count"].asInt(emitter.burst), std::max(0, system->definition->maxParticles - static_cast<int>(live)));
            const int births = BurstEmitter(emitter, id, scene.WorldMatrix(id) * system->definition->emitters[i].local, VfxSeedMix(i), count);
            accepted += births; live += static_cast<size_t>(births);
        }
        system->suppressDestroy = false; engine.Touch(); Json result = Json::MakeObject(); result["accepted"] = accepted; return result;
    } else if (operation != "state") throw ApiError("invalid_vfx", "unknown VFX runtime operation");
    Json result = Json::MakeObject(); result["id"] = id; result["asset"] = system->asset;
    if (operation != "state") engine.Touch();
    result["playing"] = VfxPlaying(*system); result["finished"] = VfxFinished(*system); result["error"] = system->error;
    result["params"] = Json::MakeObject();
    const Json params = EffectiveVfxParams(*system);
    if (system->definition) for (const auto& param : system->definition->json["params"].members())
        result["params"][param.first] = params.has(param.first) ? params[param.first] : param.second["default"];
    result["emitters"] = Json::MakeArray(); result["trails"] = Json::MakeArray(); uint64_t count = 0, pointCount = 0;
    if (system->definition) {
        result["budget"] = system->definition->maxParticles;
        for (size_t i = 0; i < system->emitters.size(); ++i) {
            const auto& emitter = system->emitters[i]; Json block = Json::MakeObject(); block["name"] = system->definition->emitters[i].name;
            block["count"] = static_cast<uint64_t>(emitter.particles.size()); block["emitted"] = emitter.emitted;
            block["maxParticles"] = emitter.maxParticles; block["finished"] = ParticlesFinished(emitter);
            result["emitters"].push(block); count += emitter.particles.size();
        }
        for (size_t i = 0; i < system->trails.size(); ++i) {
            Json block = Json::MakeObject(); block["name"] = system->definition->trails[i].name;
            block["pointCount"] = static_cast<uint64_t>(system->trails[i].points.size()); result["trails"].push(block);
            pointCount += system->trails[i].points.size();
        }
    }
    result["count"] = count; result["pointCount"] = pointCount; return result;
}
}  // namespace oe
