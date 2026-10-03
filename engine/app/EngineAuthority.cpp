#include "app/Engine.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <set>
#include "core/FileSystem.h"
#include "core/Image.h"
#include "core/Log.h"
#include "scene/Components.h"

namespace oe {
namespace {
FrameInput Sample(const InputState& input, const SyncConfig& config) {
    FrameInput sample;
    for (size_t i = 0; i < config.keys.size(); ++i) {
        if (input.IsDown(config.keys[i])) sample.down |= uint64_t{1} << i;
        if (input.pressedThisFrame.count(config.keys[i])) sample.pulse |= uint64_t{1} << i;
    }
    for (size_t i = 0; i < 6; ++i) if (config.axes[i]) {
        auto it = input.axes.find(InputState::kAxisNames[i]); float n = it == input.axes.end() ? 0 : it->second;
        if (!std::isfinite(n)) n = 0;
        sample.axes[i] = static_cast<int16_t>(std::round(std::max(i >= 4 ? 0.0f : -1.0f, std::min(1.0f, n)) * 32767));
    }
    return sample;
}
bool ReplicationValue(const Json& value, unsigned depth = 0) {
    if (depth > 8 || value.size() > 256) return false;
    if (value.isNumber()) return std::isfinite(value.asNumber());
    if (value.isString()) return value.asString().size() <= 8192;
    for (const auto& item : value.items()) if (!ReplicationValue(item, depth + 1)) return false;
    for (const auto& item : value.members()) if (item.first.size() > 128 || !ReplicationValue(item.second, depth + 1)) return false;
    return true;
}
Json Quantize(const Json& value, double step) {
    if (step <= 0) return value;
    if (value.isNumber()) return Json(std::round(value.asNumber() / step) * step);
    if (value.isArray()) { Json result = Json::MakeArray(); for (const auto& item : value.items()) result.push(Quantize(item, step)); return result; }
    return value;
}
Json Interpolate(const Json& a, const Json& b, double t) {
    if (a.isNumber() && b.isNumber()) return Json(a.asNumber() + (b.asNumber() - a.asNumber()) * t);
    if (a.isArray() && b.isArray() && a.size() == b.size()) {
        Json out = Json::MakeArray(); for (size_t i = 0; i < a.size(); ++i) out.push(Interpolate(a[i], b[i], t)); return out;
    }
    return b;
}
bool Id(const std::string& text, uint32_t& id) {
    if (text.empty() || text.size() > 10) return false;
    uint64_t value = 0; for (char c : text) { if (c < '0' || c > '9') return false; value = value * 10 + static_cast<unsigned>(c - '0'); }
    if (!value || value > UINT32_MAX - 2) return false; id = static_cast<uint32_t>(value); return true;
}
const FieldInfo* Field(const std::string& key, const ComponentType*& type) {
    auto dot = key.find('.'); if (dot == std::string::npos) return nullptr;
    type = TypeRegistry::Find(key.substr(0, dot)); return type ? type->FindField(key.substr(dot + 1)) : nullptr;
}
}
void Engine::ResetAuthority() {
    authority_.reset(); initialAuthorityEntities_.clear(); authorityIds_.clear(); replicatedEntities_.clear(); nextAuthorityId_ = 1;
    predictionStates_.clear(); authorityWorlds_.clear(); authorityEvents_.clear(); authorityInput_ = authorityRemoteFrame_ = authorityCorrections_ = 0;
}
bool Engine::PredictEntity(EntityId id) const {
    if (!authority_ || !authority_->Running() || !network_ || network_->IsHost()) return true;
    const auto* sync = scene_.Get<NetSync>(id);
    return !sync || (sync->predict && static_cast<uint32_t>(sync->owner) == network_->LocalPlayer());
}
void Engine::EnsureAuthority(bool refresh) {
    if (!network_ || networkConfig_.mode != "authoritative" || !network_->Connected()) return;
    if (refresh && authority_ && !authority_->Active()) authority_.reset();
    if (authority_) return; BeginSessionIfNeeded();
    std::string text = playSnapshot_->dump() + ":" + std::to_string(ContentHash()) + ":" + std::to_string(network_->Seed());
    uint64_t blueprint = Fnv1a64(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    authority_ = std::make_unique<Authority>(networkConfig_, network_->IsHost(), network_->LocalPlayer(), blueprint,
        [this](uint32_t player, const std::vector<uint8_t>& bytes) { return network_ && network_->SendSync(player, bytes); });
    authority_->SetUnreliableSend([this](uint32_t player, const std::vector<uint8_t>& bytes) { return network_ && network_->SendSync(player, bytes, false); });
    authority_->Tick(networkFrame_);
}
Json Engine::ReplicatedWorld(uint32_t player) {
    Json world = Json::MakeObject();
    if (scene_.Pool<NetSync>().size() > 1024) throw ApiError("network_limit", "at most 1024 replicated entities are supported");
    for (const auto& item : scene_.Pool<NetSync>()) if (!authorityIds_.count(item.first)) {
        if (nextAuthorityId_ >= UINT32_MAX - 2) throw ApiError("network_limit", "network entity ids exhausted");
        authorityIds_[item.first] = nextAuthorityId_++;
    }
    for (auto it = authorityIds_.begin(); it != authorityIds_.end();) {
        if (!scene_.Exists(it->first) || !scene_.Get<NetSync>(it->first)) it = authorityIds_.erase(it); else ++it;
    }
    EntityId observer = 0; int team = 0;
    for (const auto& item : scene_.Pool<NetPlayer>()) if (static_cast<uint32_t>(item.second.player) == player) { observer = item.first; team = item.second.team; break; }
    for (const auto& item : scene_.Pool<NetSync>()) {
        EntityId id = item.first; const auto& policy = item.second;
        if (policy.owner < 0 || (policy.owner && !std::any_of(authority_->Players().begin(), authority_->Players().end(), [&](uint32_t p) { return p == static_cast<uint32_t>(policy.owner); })))
            throw ApiError("network_owner", "NetSync.owner must belong to the ready roster");
        if (!std::isfinite(policy.distance) || policy.distance < 0 || !policy.fields.isObject() || policy.fields.size() > 64)
            throw ApiError("network_policy", "invalid NetSync distance/fields");
        if (static_cast<uint32_t>(policy.owner) != player) {
            if (policy.teamOnly && (!observer || policy.team != team)) continue;
            if (policy.distance > 0 && (!observer || Length(scene_.WorldMatrix(id).TransformPoint(Vec3{}) - scene_.WorldMatrix(observer).TransformPoint(Vec3{})) > policy.distance)) continue;
        }
        Json entity = Json::MakeObject(); entity["name"] = scene_.Record(id)->name;
        entity["source"] = id; entity["owner"] = policy.owner; entity["prefab"] = policy.prefab.empty() && scene_.Get<Prefab>(id) ? scene_.Get<Prefab>(id)->path : policy.prefab; entity["predict"] = policy.predict;
        EntityId parent = scene_.Record(id)->parent;
        if (parent && !authorityIds_.count(parent)) throw ApiError("network_policy", "replicated parents must also have NetSync");
        entity["parent"] = authorityIds_.count(parent) ? authorityIds_.at(parent) : 0;
        entity["always"] = Json::MakeArray();
        if (const auto* controller = scene_.Get<NetPlayer>(id)) entity["player"] = ComponentToJson(*TypeRegistry::Find("NetPlayer"), controller);
        for (const auto& selected : policy.fields.members()) {
            const ComponentType* type = nullptr; const FieldInfo* field = Field(selected.first, type);
            if (!field || type->name == "NetSync" || type->name == "NetPlayer" || !selected.second.isObject()) throw ApiError("network_policy", "unknown or reserved replication field");
            for (const auto& option : selected.second.members()) {
                if (option.first == "quantize") { if (!option.second.isNumber()) throw ApiError("network_policy", "quantize must be numeric"); }
                else if ((option.first != "always" && option.first != "onChange" && option.first != "ownerOnly") || !option.second.isBool()) throw ApiError("network_policy", "invalid replication field option");
            }
            const void* component = scene_.GetComponent(id, *type); if (!component) throw ApiError("network_policy", "replication field component is absent");
            if (selected.second["ownerOnly"].asBool() && static_cast<uint32_t>(policy.owner) != player) continue;
            double step = selected.second["quantize"].asNumber(); if (!std::isfinite(step) || step < 0 || step > 1000000) throw ApiError("network_policy", "invalid field quantization");
            Json value = FieldToJson(*field, component);
            if (!ReplicationValue(value)) throw ApiError("network_policy", "replication value exceeds finite/depth/size bounds");
            if (field->type == FieldType::Entity) {
                EntityId target = static_cast<EntityId>(value.asNumber()); value = authorityIds_.count(target) ? Json(authorityIds_.at(target)) : Json(0);
            } else value = Quantize(value, step);
            if (!ReplicationValue(value)) throw ApiError("network_policy", "quantized replication value is not finite");
            entity[selected.first] = value;
            if (selected.second["always"].asBool() || (selected.second.has("onChange") && !selected.second["onChange"].asBool())) entity["always"].push(selected.first);
        }
        world[std::to_string(authorityIds_.at(id))] = entity;
    }
    // Relevance cannot leave a replicated child without its local-space parent.
    bool removed = true;
    while (removed) {
        removed = false;
        for (auto it = world.members().begin(); it != world.members().end();) {
            uint32_t parent = static_cast<uint32_t>(it->second["parent"].asNumber());
            if (parent && !world.has(std::to_string(parent))) { it = world.members().erase(it); removed = true; } else ++it;
        }
    }
    return world;
}
void Engine::ValidateReplicatedWorld(const Json& world) const {
    // Validate a complete image before touching the live scene, including prefab paths.
    Scene probe; std::map<uint32_t, EntityId> probeIds;
    for (const auto& item : world.members()) {
        uint32_t netId; if (!Id(item.first, netId) || !item.second.isObject()) throw ApiError("network_snapshot", "invalid network entity id/record");
        const Json& record = item.second;
        auto integer = [](const Json& value, double low, double high) { return value.isNumber() && std::isfinite(value.asNumber()) && value.asNumber() >= low && value.asNumber() <= high && std::floor(value.asNumber()) == value.asNumber(); };
        if (!integer(record["source"], 1, UINT32_MAX) || !integer(record["parent"], 0, UINT32_MAX - 2) ||
            !record["name"].isString() || record["name"].asString().size() > 128 || !record["prefab"].isString() || record["prefab"].asString().size() > 256 ||
            !record["predict"].isBool() || !record["always"].isArray() || record["always"].size() > 64)
            throw ApiError("network_snapshot", "invalid snapshot metadata");
        double owner = record["owner"].asNumber(-1);
        if (!std::isfinite(owner) || owner < 0 || owner > INT32_MAX || std::floor(owner) != owner ||
            (owner && std::find(authority_->Players().begin(), authority_->Players().end(), static_cast<uint32_t>(owner)) == authority_->Players().end())) throw ApiError("network_snapshot", "invalid snapshot owner");
        std::string prefab = record["prefab"].asString(); if (!prefab.empty()) ResolvePath(prefab);
        EntityId id = probe.Create("Probe"); probeIds[netId] = id;
        if (record.has("player")) {
            std::string error;
            double player = record["player"]["player"].asNumber(-1);
            if (!record["player"].isObject() || !std::isfinite(player) || player < 0 || player > INT32_MAX || std::floor(player) != player) throw ApiError("network_snapshot", "invalid NetPlayer id");
            if (!ApplyComponentJson(*TypeRegistry::Find("NetPlayer"), &probe.Add<NetPlayer>(id), record["player"], &error)) throw ApiError("network_snapshot", "invalid NetPlayer metadata");
        }
        for (const auto& selected : record.members()) {
            if (selected.first.find('.') == std::string::npos) continue;
            const ComponentType* type = nullptr; const FieldInfo* field = Field(selected.first, type); std::string error;
            if (field && field->type == FieldType::Int && (!selected.second.isNumber() || selected.second.asNumber() < INT32_MIN || selected.second.asNumber() > INT32_MAX || !std::isfinite(selected.second.asNumber()))) throw ApiError("network_snapshot", "integer field exceeds its bounds");
            if (!field || type->name == "NetSync" || type->name == "NetPlayer" || !ReplicationValue(selected.second) || !FieldFromJson(*field, probe.AddComponent(id, *type), selected.second, &error))
                throw ApiError("network_snapshot", "invalid replicated field: " + selected.first);
        }
    }
    for (const auto& item : world.members()) {
        uint32_t id = 0; Id(item.first, id); uint32_t parent = static_cast<uint32_t>(item.second["parent"].asNumber());
        std::string error;
        if (!probe.SetParent(probeIds.at(id), probeIds.count(parent) ? probeIds.at(parent) : 0, &error)) throw ApiError("network_snapshot", "invalid network hierarchy");
    }
}
void Engine::ApplyReplicatedWorld(const Json& world, bool owned, bool remotes) {
    for (auto it = replicatedEntities_.begin(); it != replicatedEntities_.end();) {
        if (!world.has(std::to_string(it->first))) { scene_.Destroy(it->second); it = replicatedEntities_.erase(it); }
        else ++it;
    }
    for (const auto& item : world.members()) {
        uint32_t netId = 0; Id(item.first, netId); const Json& record = item.second;
        if (!replicatedEntities_.count(netId)) {
            EntityId id = 0, source = static_cast<EntityId>(record["source"].asNumber());
            // Initial scene entities are identified by server source id; runtime spawns use prefabs.
            if (scene_.Exists(source) && scene_.Get<NetSync>(source) && std::none_of(replicatedEntities_.begin(), replicatedEntities_.end(), [&](const auto& entry) { return entry.second == source; })) id = source;
            if (!id && !record["prefab"].asString().empty()) id = InstantiatePrefabFile(record["prefab"].asString(), 0);
            if (!id && initialAuthorityEntities_.count(source)) {
                Json original = initialAuthorityEntities_.at(source); original.erase("parent"); std::string error;
                id = scene_.CreateFromJson(original, false, &error);
                if (!id) throw ApiError("network_snapshot", error);
            }
            if (!id) id = scene_.Create(record["name"].asString());
            replicatedEntities_[netId] = id;
        }
        EntityId id = replicatedEntities_.at(netId);
        auto& policy = scene_.Add<NetSync>(id); policy.owner = static_cast<int>(record["owner"].asNumber()); policy.predict = record["predict"].asBool(true); policy.prefab = record["prefab"].asString();
        if (record.has("player")) { std::string error; ApplyComponentJson(*TypeRegistry::Find("NetPlayer"), &scene_.Add<NetPlayer>(id), record["player"], &error); }
    }
    for (const auto& item : world.members()) {
        uint32_t netId = 0; Id(item.first, netId); const Json& record = item.second;
        bool local = record["owner"].asNumber() == network_->LocalPlayer() && record["predict"].asBool(true);
        EntityId id = replicatedEntities_.at(netId);
        if ((local && !owned) || (!local && !remotes)) continue;
        for (const auto& selected : record.members()) {
            if (selected.first.find('.') == std::string::npos) continue;
            const ComponentType* type = nullptr; const FieldInfo* field = Field(selected.first, type); std::string error;
            Json value = selected.second;
            if (field->type == FieldType::Entity) { uint32_t target = static_cast<uint32_t>(value.asNumber()); value = replicatedEntities_.count(target) ? Json(replicatedEntities_.at(target)) : Json(0); }
            if (!FieldFromJson(*field, scene_.AddComponent(id, *type), value, &error)) throw ApiError("network_snapshot", error);
        }
    }
    for (const auto& item : world.members()) {
        uint32_t id = 0; Id(item.first, id); uint32_t parent = static_cast<uint32_t>(item.second["parent"].asNumber());
        std::string error; if (!scene_.SetParent(replicatedEntities_.at(id), replicatedEntities_.count(parent) ? replicatedEntities_.at(parent) : 0, &error)) throw ApiError("network_snapshot", error);
    }
}
void Engine::InterpolateAuthority(bool advance) {
    if (authorityWorlds_.empty()) return;
    const auto& latest = authorityWorlds_.back(); authorityRemoteFrame_ = std::max(authorityWorlds_.front().frame, std::min(authorityRemoteFrame_ + (advance ? 1 : 0), latest.frame));
    const Authority::Update* a = &authorityWorlds_.front(); const Authority::Update* b = &latest;
    for (const auto& world : authorityWorlds_) { if (world.frame <= authorityRemoteFrame_) a = &world; if (world.frame >= authorityRemoteFrame_) { b = &world; break; } }
    double t = a->frame == b->frame ? 1 : static_cast<double>(authorityRemoteFrame_ - a->frame) / static_cast<double>(b->frame - a->frame);
    Json world = latest.world;
    for (auto& entity : world.members()) {
        if (entity.second["owner"].asNumber() == network_->LocalPlayer() && entity.second["predict"].asBool(true)) continue;
        for (auto& field : entity.second.members()) if (field.first.find('.') != std::string::npos && a->world[entity.first].has(field.first) && b->world[entity.first].has(field.first))
            {
                const ComponentType* type = nullptr; const FieldInfo* info = Field(field.first, type);
                if (info && (info->type == FieldType::Float || info->type == FieldType::Vec3 || info->type == FieldType::Color))
                    field.second = Interpolate(a->world[entity.first][field.first], b->world[entity.first][field.first], t);
            }
    }
    ApplyReplicatedWorld(world, false, true);
}
bool Engine::PollAuthority() {
    if (!network_ || networkConfig_.mode != "authoritative") return true;
    EnsureAuthority(); if (!authority_) return true;
    try {
        for (const auto& message : network_->DrainSync()) {
            if (!message.second.empty() && message.second.front() == 32 && !network_->IsHost() && !authority_->Active()) EnsureAuthority(true);
            authority_->Receive(message.first, message.second);
        }
        authority_->Tick(networkFrame_);
        if (authority_->Active()) {
            // Clients follow the server: a shrinking roster only changes what it replicates.
            if (!network_->Connected()) authority_->Stop("match participant disconnected");
            else if (network_->IsHost()) DropAuthorityPlayers();
        }
        if (authority_->TakeStart()) {
            std::string error; scene_.FromJson(*playSnapshot_, &error); ResetRuntime(); audio_->ResetTimeline(); scripts_->ClearErrors(); scripts_->SetNetworkSeed(network_->Seed());
            frame_ = 0; simTime_ = 0; gameData_ = Json::MakeObject(); runtimeScene_ = scenePath_.empty() ? "" : RelativePath(scenePath_, projectDir_);
            journal_.reset(); snapshots_.clear(); checkpoints_.clear(); predictionStates_.clear(); replicatedEntities_.clear(); authorityIds_.clear(); nextAuthorityId_ = 1;
            authorityWorlds_.clear(); authorityEvents_.clear(); authorityInput_ = authorityRemoteFrame_ = authorityCorrections_ = outputConfirmed_ = 0;
            uiHovered_ = uiPressed_ = 0; heldKeys_.clear(); heldButtons_.clear();
            saves_.FreezeReads(true); saves_.DeferFlush(true);
            if (network_->IsHost()) {
                for (uint32_t player : authority_->Players()) {
                    if (dedicated_ && player == 1) continue;
                    EntityId id = 0; for (const auto& entry : scene_.Pool<NetPlayer>()) if (static_cast<uint32_t>(entry.second.player) == player) { id = entry.first; break; }
                    if (!id && !networkConfig_.playerPrefab.empty()) { id = InstantiatePrefabFile(networkConfig_.playerPrefab, 0); scene_.Add<NetSync>(id).prefab = networkConfig_.playerPrefab; }
                    if (id) { scene_.Add<NetPlayer>(id).player = static_cast<int>(player); scene_.Add<NetSync>(id).owner = static_cast<int>(player); }
                }
                ReplicatedWorld(1);
                std::map<uint32_t, Json> worlds; for (uint32_t player : authority_->Players()) if (player != 1) worlds[player] = ReplicatedWorld(player);
                authority_->Snapshot(0, worlds);
            } else {
                initialAuthorityEntities_.clear(); std::vector<EntityId> ids;
                for (const auto& entry : scene_.Pool<NetSync>()) { initialAuthorityEntities_[entry.first] = scene_.EntityToJson(entry.first); ids.push_back(entry.first); }
                for (EntityId id : ids) scene_.Destroy(id);
                RecordState(); predictionStates_[0] = CaptureNative(false);
            }
            network_->Seal();
        }
        if (!authority_->Running()) return authority_->State()["state"].asString() == "idle";
        auto updates = authority_->DrainUpdates();
        if (!updates.empty()) {
            for (const auto& update : updates) authorityWorlds_.push_back(update);
            while (authorityWorlds_.size() > 32) authorityWorlds_.pop_front();
            const auto& update = updates.back();
            auto checkpoint = predictionStates_.find(update.acknowledged);
            if (checkpoint == predictionStates_.end()) { authority_->Stop("prediction checkpoint missing"); return false; }
            ValidateReplicatedWorld(update.world);
            InputState raw = input_; auto output = audio_->TakeOutput(); auto start = std::chrono::steady_clock::now();
            RestoreNative(*checkpoint->second); audio_->DropPendingBefore(update.acknowledged);
            // Native entity ids/map return to the acknowledged branch, then rebuild the server map.
            for (auto it = replicatedEntities_.begin(); it != replicatedEntities_.end();) { if (!scene_.Exists(it->second)) it = replicatedEntities_.erase(it); else ++it; }
            ApplyReplicatedWorld(update.world, true, true); physics_->RebuildSolvers();
            authorityInput_ = update.acknowledged; frame_ = update.acknowledged; simTime_ = frame_ * kFixedDt;
            predictionStates_.erase(predictionStates_.lower_bound(update.acknowledged), predictionStates_.end());
            predictionStates_[update.acknowledged] = CaptureNative(false);
            replaying_ = true;
            for (const auto& input : authority_->PendingInputs()) {
                if (input.first <= update.acknowledged) continue;
                authorityInput_ = input.first; ApplyInputs({{network_->LocalPlayer(), input.second}});
                audio_->SetOutputMode(false, true); SimulateWorld(); predictionStates_[input.first] = CaptureNative(false);
            }
            replaying_ = false; input_ = raw; audio_->RestoreOutput(std::move(output));
            audio_->Confirm(update.acknowledged); outputConfirmed_ = update.acknowledged;
            predictionStates_.erase(predictionStates_.begin(), predictionStates_.lower_bound(update.acknowledged));
            authorityEvents_.erase(authorityEvents_.begin(), authorityEvents_.lower_bound(update.acknowledged));
            authorityRemoteFrame_ = std::max(authorityRemoteFrame_, update.frame > networkConfig_.interpolationFrames ? update.frame - networkConfig_.interpolationFrames : 0);
            replayMilliseconds_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); ++authorityCorrections_;
        }
        if (!network_->IsHost() && !authority_->CanPredict()) { InterpolateAuthority(); return false; }
        FrameInput sample = Sample(SampleNetworkInput(), networkConfig_.sync); deviceInput_ = input_; input_.pressedThisFrame.clear(); deviceInput_.pressedThisFrame.clear();
        if (network_->IsHost()) ApplyInputs(authority_->Consume(sample));
        else {
            authorityInput_ = authority_->Submit(sample); if (!authorityInput_) return false;
            ApplyInputs({{network_->LocalPlayer(), sample}}); InterpolateAuthority(); audio_->SetOutputMode(false, true);
        }
        return true;
    } catch (const std::exception& error) {
        replaying_ = false; inWorld_ = false;
        authority_->Stop(error.what()); return false;
    }
}
std::vector<PhysicsEvent> Engine::StepAuthorityPhysics(float dt) {
    std::map<EntityId, RigidBody> remoteBodies;
    std::map<EntityId, RigidBody2D> remoteBodies2D;
    std::map<EntityId, CharacterBody> remoteCharacters;
    std::map<EntityId, CharacterBody2D> remoteCharacters2D;
    std::set<EntityId> addedColliders, addedColliders2D;
    {
        for (const auto& item : scene_.Pool<NetSync>()) if (!PredictEntity(item.first)) {
            EntityId id = item.first;
            if (auto* body = scene_.Get<RigidBody>(id)) { remoteBodies[id] = *body; body->type = "kinematic"; body->velocity = {}; body->angularVelocity = {}; }
            if (auto* body = scene_.Get<RigidBody2D>(id)) { remoteBodies2D[id] = *body; body->type = "kinematic"; body->velocity = {}; body->angularVelocity = 0; }
            if (auto* character = scene_.Get<CharacterBody>(id)) {
                remoteCharacters[id] = *character;
                if (!scene_.Get<Collider>(id)) { auto& collider = scene_.Add<Collider>(id); collider.shape = character->shape; collider.radius = character->radius; collider.height = character->height; addedColliders.insert(id); }
            }
            if (auto* character = scene_.Get<CharacterBody2D>(id)) {
                remoteCharacters2D[id] = *character;
                if (!scene_.Get<Collider2D>(id)) { auto& collider = scene_.Add<Collider2D>(id); collider.shape = character->shape; collider.radius = character->radius; collider.height = character->height; collider.layer = character->layer; collider.ignoreLayers = character->ignoreLayers; addedColliders2D.insert(id); }
            }
        }
        for (const auto& item : remoteCharacters) scene_.Pool<CharacterBody>().erase(item.first);
        for (const auto& item : remoteCharacters2D) scene_.Pool<CharacterBody2D>().erase(item.first);
    }
    std::vector<PhysicsEvent> events = physics_->Step(scene_, dt);
    for (const auto& item : remoteBodies) scene_.Pool<RigidBody>()[item.first] = item.second;
    for (const auto& item : remoteBodies2D) scene_.Pool<RigidBody2D>()[item.first] = item.second;
    for (const auto& item : remoteCharacters) scene_.Pool<CharacterBody>()[item.first] = item.second;
    for (const auto& item : remoteCharacters2D) scene_.Pool<CharacterBody2D>()[item.first] = item.second;
    for (EntityId id : addedColliders) scene_.Pool<Collider>().erase(id);
    for (EntityId id : addedColliders2D) scene_.Pool<Collider2D>().erase(id);
    return events;
}
void Engine::DropAuthorityPlayers() {
    for (uint32_t player : authority_->TakeDropped()) { std::string error; network_->Kick(player, "send queue overflow", &error); }
    std::set<uint32_t> present;
    const Json roster = network_->Players();
    for (const Json& player : roster.items()) present.insert(static_cast<uint32_t>(player["id"].asNumber()));
    std::vector<uint32_t> gone;
    for (uint32_t player : authority_->Players()) if (player != 1 && !present.count(player)) gone.push_back(player);
    if (gone.empty()) return;
    // The ready barrier needs its whole roster; a match without remote participants is over.
    if (!authority_->Running() || gone.size() + 1 >= authority_->Players().size()) { authority_->Stop("match participant disconnected"); return; }
    for (uint32_t player : gone) {
        authority_->Drop(player);
        std::vector<EntityId> avatars;
        for (const auto& item : scene_.Pool<NetPlayer>()) if (static_cast<uint32_t>(item.second.player) == player) avatars.push_back(item.first);
        for (EntityId id : avatars) scene_.Destroy(id);
        for (auto& item : scene_.Pool<NetSync>()) if (static_cast<uint32_t>(item.second.owner) == player) item.second.owner = 0;
        OE_LOG_INFO("net", "player %u left the match", player);
    }
}
void Engine::AuthorityApplied() {
    if (!authority_ || !authority_->Running() || replaying_) return;
    try {
        if (network_->IsHost()) {
            uint64_t before = (frame_ - 1) * networkConfig_.snapshotRate / 60, after = frame_ * networkConfig_.snapshotRate / 60;
            if (before != after) {
                ReplicatedWorld(1);
                std::map<uint32_t, Json> worlds; for (uint32_t player : authority_->Players()) if (player != 1) worlds[player] = ReplicatedWorld(player);
                authority_->Snapshot(frame_, worlds);
            }
        } else { predictionStates_[authorityInput_] = CaptureNative(false); InterpolateAuthority(false); }
    } catch (const std::exception& error) { authority_->Stop(error.what()); }
}
}  // namespace oe
