#include "app/Vfx.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <map>

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
    for (const auto& emitter : emitters) {
        if (!emitter.texture.empty()) engine.ResolvePath(emitter.texture);
        if (!emitter.material.empty()) engine.ResolvePath(emitter.material);
        if (!emitter.mesh.empty() && !GetBuiltinMesh(emitter.mesh)) engine.ResolvePath(emitter.mesh);
    }
    for (const auto& trail : trails) {
        if (!trail.texture.empty()) engine.ResolvePath(trail.texture);
        if (!trail.material.empty()) engine.ResolvePath(trail.material);
    }
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
        result[name] = ComponentToJson(VfxEmitterType(), &asset.emitters[i].emitter);
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
    std::vector<ParticleEmitter> emitters; std::vector<Trail> trails;
    ConfigureVfx(asset, params, emitters, trails);
    Json result = Json::MakeObject(); result["ok"] = true; result["warnings"] = Json::MakeArray();
    int instructions = 0; double work = 0;
    for (int kind = 0; kind < 2; ++kind) {
        const size_t size = kind ? trails.size() : emitters.size();
        for (size_t i = 0; i < size; ++i) {
            const Json checked = CheckVfxValues(engine, kind ? nullptr : &emitters[i], kind ? &trails[i] : nullptr);
            instructions += checked["expressionInstructions"].asInt(); work += checked["expressionStepInstructions"].asNumber();
            for (const auto& warning : checked["warnings"].items()) {
                Json item = warning; item["emitter"] = kind ? asset.trails[i].name : asset.emitters[i].name;
                result["warnings"].push(item);
            }
        }
    }
    result["expressionInstructions"] = instructions; result["expressionStepInstructions"] = work;
    result["path"] = path; result["budget"] = asset.maxParticles;
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
        if (args.has("vfx") == args.has("preset"))
            throw ApiError("invalid_vfx", "provide exactly one vfx document or preset name", "Use {path,preset:'ice_pillar'} or {path,vfx:{...}}.");
        if (args.has("preset")) {
            if (FileExists(engine.ResolvePath(path))) throw ApiError("already_exists", "asset already exists", "Choose another path or edit the existing asset with vfx.set.");
            return WriteAsset(engine, path, VfxPresetDocument(engine, args["preset"].asString()));
        }
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
        json["params"] = Json::MakeObject(); int budget = 0;
        auto matrix = [&](Json& block, const Mat4& local) {
            block.erase("transform"); block["matrix"] = Json::MakeArray();
            for (float number : local.m) block["matrix"].push(number);
        };
        for (EntityId id : selected) {
            if (const auto* system = scene.Get<VFXSystem>(id)) {
                const auto asset = ParseVfxAsset(engine.ReadProjectJson(system->asset));
                budget += asset.maxParticles;
                std::map<std::string, std::string> blocks, params;
                for (const char* key : {"emitters", "trails"}) for (const auto& block : asset.json[key].items())
                    blocks[block["name"].asString()] = "e" + std::to_string(id) + "_b" + std::to_string(blocks.size());
                for (const auto& param : asset.json["params"].members())
                    params[param.first] = "e" + std::to_string(id) + "_p" + std::to_string(params.size());
                for (const auto& param : asset.json["params"].members()) {
                    Json definition = param.second;
                    if (system->params.has(param.first)) definition["default"] = system->params[param.first];
                    for (auto& target : definition["targets"].items()) {
                        const std::string to = target["to"].asString(); const size_t dot = to.find('.');
                        target["to"] = blocks.at(to.substr(0, dot)) + to.substr(dot);
                    }
                    json["params"][params.at(param.first)] = definition;
                }
                for (int kind = 0; kind < 2; ++kind) {
                    const char* key = kind ? "trails" : "emitters";
                    for (size_t i = 0; i < asset.json[key].size(); ++i) {
                        Json block = asset.json[key][i]; block["name"] = blocks.at(block["name"].asString());
                        const Mat4 local = inverse * scene.WorldMatrix(id) * (kind ? asset.trails[i].local : asset.emitters[i].local);
                        matrix(block, local);
                        if (!kind) {
                            auto& settings = block["emitter"];
                            if (settings.has("onDeath") && !settings["onDeath"].asString().empty()) settings["onDeath"] = blocks.at(settings["onDeath"].asString());
                            if (settings.has("expressions")) for (auto& stage : settings["expressions"].members()) for (auto& channel : stage.second.members()) {
                                const std::string source = channel.second.asString(); std::string rewritten;
                                for (size_t at = 0; at < source.size();) {
                                    if (source.compare(at, 6, "param.") == 0) {
                                        size_t end = at + 6;
                                        while (end < source.size() && ((source[end] >= 'a' && source[end] <= 'z') ||
                                               (source[end] >= 'A' && source[end] <= 'Z') || (source[end] >= '0' && source[end] <= '9') || source[end] == '_')) ++end;
                                        const auto found = params.find(source.substr(at + 6, end - at - 6));
                                        if (found != params.end()) { rewritten += "param." + found->second; at = end; continue; }
                                    }
                                    rewritten += source[at++];
                                }
                                channel.second = rewritten;
                            }
                        }
                        json[key].push(block);
                    }
                }
            }
            if (const auto* trail = scene.Get<Trail>(id)) {
                std::string name = scene.Record(id)->name;
                for (char& c : name) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) c = '_';
                name = name.substr(0, 48); if (name.empty()) name = "effect";
                if (names.count(name)) name += "_" + std::to_string(id) + "_trail";
                names.insert(name); Json block = Json::MakeObject(); block["name"] = name;
                block["trail"] = ComponentToJson(*TypeRegistry::Find("Trail"), trail);
                matrix(block, inverse * scene.WorldMatrix(id)); json["trails"].push(block);
            }
        }
        json["budget"]["maxParticles"] = budget;
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
            for (auto& emitter : system->emitters) {
                emitter.particles.clear();
                if (operation == "clear" && args.has("restart")) {
                    emitter.randomState = 0; emitter.carry = 0; emitter.emitted = 0; emitter.elapsed = 0; emitter.cycle = 0;
                    emitter.initialBurstEmitted = !args["restart"].asBool();
                }
            }
            if (operation == "clear" && args.has("restart")) system->expressionTime = 0;
            for (auto& trail : system->trails) trail.points.clear();
            if (operation == "clear") system->suppressDestroy = true;
        }
    } else if (operation == "burst") {
        if (args.has("count") && (args["count"].asNumber() < 0 || args["count"].asNumber() > 10000 ||
            std::floor(args["count"].asNumber()) != args["count"].asNumber())) throw ApiError("invalid_range", "burst count must be 0..10000");
        bool found = !args.has("emitter"); int accepted = 0;
        for (const auto& block : system->definition->emitters) if (block.name == args["emitter"].asString()) found = true;
        if (!found) throw ApiError("no_emitter", "unknown emitter", "Call vfx.state to list names.");
        for (size_t i = 0; i < system->emitters.size(); ++i) {
            if (args.has("emitter") && system->definition->emitters[i].name != args["emitter"].asString()) continue;
            if (!ParticleSettingsValid(system->emitters[i])) {
                try { ValidateVfxSettings(VfxEmitterType(), &system->emitters[i], ComponentToJson(VfxEmitterType(), &system->emitters[i])); }
                catch (const ApiError& error) { throw ApiError("invalid_emitter", error.what(), "Correct the asset with vfx.set."); }
                throw ApiError("invalid_emitter", "emitter settings/curves must be finite and valid", "Correct the asset with vfx.set.");
            }
        }
        size_t live = 0; for (const auto& emitter : system->emitters) live += emitter.particles.size();
        for (size_t i = 0; i < system->emitters.size(); ++i) {
            if (args.has("emitter") && system->definition->emitters[i].name != args["emitter"].asString()) continue;
            auto& emitter = system->emitters[i];
            const int count = std::min(args["count"].asInt(emitter.burst), std::max(0, system->definition->maxParticles - static_cast<int>(live)));
            emitter.expressionSystemTime=system->expressionTime;
            const int births = BurstEmitter(emitter, id, scene.WorldMatrix(id) * system->definition->emitters[i].local, VfxSeedMix(i), count);
            accepted += births; live += static_cast<size_t>(births);
        }
        system->suppressDestroy = false; engine.Touch(); Json result = Json::MakeObject(); result["accepted"] = accepted; return result;
    } else if (operation != "state") throw ApiError("invalid_vfx", "unknown VFX runtime operation");
    Json result = Json::MakeObject(); result["id"] = id; result["asset"] = system->asset;
    if (operation != "state") engine.Touch();
    result["playing"] = VfxPlaying(*system); result["finished"] = VfxFinished(*system); result["error"] = system->error;
    result["systemTime"]=system->expressionTime;
    result["params"] = Json::MakeObject();
    const Json params = EffectiveVfxParams(*system);
    if (system->definition) for (const auto& param : system->definition->json["params"].members())
        result["params"][param.first] = params.has(param.first) ? params[param.first] : param.second["default"];
    result["emitters"] = Json::MakeArray(); result["trails"] = Json::MakeArray(); uint64_t count = 0, pointCount = 0;
    if (system->definition) {
        result["budget"] = system->definition->maxParticles;
        for (size_t i = 0; i < system->emitters.size(); ++i) {
            const auto& emitter = system->emitters[i]; Json block = DescribeVfxEmitter(emitter, args["particles"].asBool()); block["name"] = system->definition->emitters[i].name;
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
