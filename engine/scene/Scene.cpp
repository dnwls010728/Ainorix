#include "scene/Scene.h"

#include <algorithm>

#include "scene/Components.h"

namespace oe {

Scene::Scene() {
    RegisterBuiltinComponents();
    for (const ComponentType& t : TypeRegistry::All()) pools_.push_back(t.makePool());
}

void Scene::Clear() {
    entities_.clear();
    for (auto& p : pools_) p->Clear();
    nextId_ = 1;
    name = "Untitled";
}

EntityId Scene::Create(const std::string& entityName, EntityId parent, EntityId forcedId) {
    EntityId id = forcedId != kNullEntity ? forcedId : nextId_;
    if (entities_.count(id)) id = nextId_;
    nextId_ = std::max(nextId_, id + 1);
    EntityRecord rec;
    rec.id = id;
    rec.name = entityName.empty() ? "Entity " + std::to_string(id) : entityName;
    rec.parent = entities_.count(parent) ? parent : kNullEntity;
    entities_[id] = rec;
    return id;
}

int Scene::Destroy(EntityId id) {
    if (!Exists(id)) return 0;
    int count = 0;
    for (EntityId child : Children(id)) count += Destroy(child);
    for (auto& p : pools_) p->Remove(id);
    entities_.erase(id);
    return count + 1;
}

EntityRecord* Scene::Record(EntityId id) {
    auto it = entities_.find(id);
    return it == entities_.end() ? nullptr : &it->second;
}

const EntityRecord* Scene::Record(EntityId id) const {
    auto it = entities_.find(id);
    return it == entities_.end() ? nullptr : &it->second;
}

std::vector<EntityId> Scene::Children(EntityId id) const {
    std::vector<EntityId> out;
    for (const auto& kv : entities_) {
        if (kv.second.parent == id && kv.first != id) out.push_back(kv.first);
    }
    return out;
}

EntityId Scene::FindByName(const std::string& entityName) const {
    for (const auto& kv : entities_) {
        if (kv.second.name == entityName) return kv.first;
    }
    return kNullEntity;
}

bool Scene::SetParent(EntityId id, EntityId parent, std::string* error) {
    EntityRecord* rec = Record(id);
    if (!rec) {
        if (error) *error = "entity " + std::to_string(id) + " does not exist";
        return false;
    }
    if (parent != kNullEntity) {
        if (!Exists(parent)) {
            if (error) *error = "parent entity " + std::to_string(parent) + " does not exist";
            return false;
        }
        for (EntityId p = parent; p != kNullEntity; p = Record(p)->parent) {
            if (p == id) {
                if (error) *error = "cannot parent entity " + std::to_string(id) + " under its own descendant";
                return false;
            }
        }
    }
    rec->parent = parent;
    return true;
}

void* Scene::AddComponent(EntityId id, const ComponentType& type) {
    if (!Exists(id)) return nullptr;
    while (pools_.size() <= static_cast<size_t>(type.index)) pools_.push_back(TypeRegistry::All()[pools_.size()].makePool());
    if (void* existing = pools_[static_cast<size_t>(type.index)]->Get(id)) return existing;
    return pools_[static_cast<size_t>(type.index)]->Add(id);
}

void* Scene::GetComponent(EntityId id, const ComponentType& type) {
    if (static_cast<size_t>(type.index) >= pools_.size()) return nullptr;
    return pools_[static_cast<size_t>(type.index)]->Get(id);
}

const void* Scene::GetComponent(EntityId id, const ComponentType& type) const {
    if (static_cast<size_t>(type.index) >= pools_.size()) return nullptr;
    return static_cast<const IComponentPool&>(*pools_[static_cast<size_t>(type.index)]).Get(id);
}

bool Scene::RemoveComponent(EntityId id, const ComponentType& type) {
    if (static_cast<size_t>(type.index) >= pools_.size()) return false;
    return pools_[static_cast<size_t>(type.index)]->Remove(id);
}

std::vector<const ComponentType*> Scene::ComponentsOf(EntityId id) const {
    std::vector<const ComponentType*> out;
    for (const ComponentType& t : TypeRegistry::All()) {
        if (GetComponent(id, t)) out.push_back(&t);
    }
    return out;
}

Mat4 Scene::WorldMatrix(EntityId id) const {
    Mat4 m;
    int guard = 0;
    for (EntityId cur = id; cur != kNullEntity && guard < 256; ++guard) {
        const EntityRecord* rec = Record(cur);
        if (!rec) break;
        if (const Transform* t = Get<Transform>(cur)) m = Mat4::TRS(t->position, t->rotation, t->scale) * m;
        cur = rec->parent;
    }
    return m;
}

Json Scene::EntityToJson(EntityId id) const {
    const EntityRecord* rec = Record(id);
    if (!rec) return Json();
    Json e = Json::MakeObject();
    e["id"] = rec->id;
    e["name"] = rec->name;
    if (rec->parent != kNullEntity) e["parent"] = rec->parent;
    Json comps = Json::MakeObject();
    for (const ComponentType* t : ComponentsOf(id)) comps[t->name] = ComponentToJson(*t, GetComponent(id, *t));
    e["components"] = comps;
    return e;
}

Json Scene::ToJson() const {
    Json root = Json::MakeObject();
    root["format"] = kFormat;
    root["version"] = kVersion;
    root["name"] = name;
    Json list = Json::MakeArray();
    for (const auto& kv : entities_) list.push(EntityToJson(kv.first));
    root["entities"] = list;
    return root;
}

EntityId Scene::CreateFromJson(const Json& e, bool keepId, std::string* error) {
    if (!e.isObject()) {
        if (error) *error = "entity must be an object";
        return kNullEntity;
    }
    EntityId forced = keepId ? static_cast<EntityId>(e["id"].asNumber(0)) : kNullEntity;
    EntityId parent = static_cast<EntityId>(e["parent"].asNumber(0));
    EntityId id = Create(e["name"].asString(""), parent, forced);
    const Json& comps = e["components"];
    if (!comps.isNull() && !comps.isObject()) {
        if (error) *error = "entity 'components' must be an object keyed by component type";
        Destroy(id);
        return kNullEntity;
    }
    for (const auto& kv : comps.members()) {
        const ComponentType* type = TypeRegistry::Find(kv.first);
        if (!type) {
            std::string names;
            for (const ComponentType& t : TypeRegistry::All()) names += (names.empty() ? "" : ", ") + t.name;
            if (error) *error = "unknown component type '" + kv.first + "'. Known types: " + names;
            Destroy(id);
            return kNullEntity;
        }
        void* c = AddComponent(id, *type);
        if (!ApplyComponentJson(*type, c, kv.second, error)) {
            Destroy(id);
            return kNullEntity;
        }
    }
    return id;
}

bool Scene::FromJson(const Json& json, std::string* error) {
    if (!json.isObject() || json["format"].asString("") != kFormat) {
        if (error) *error = std::string("not a scene file (expected \"format\": \"") + kFormat + "\")";
        return false;
    }
    Clear();
    name = json["name"].asString("Untitled");
    const Json& list = json["entities"];
    // Pass 1: create all entities without parents so forward references work.
    for (const Json& e : list.items()) {
        Json copy = e;
        copy.erase("parent");
        if (CreateFromJson(copy, true, error) == kNullEntity) return false;
    }
    for (const Json& e : list.items()) {
        EntityId id = static_cast<EntityId>(e["id"].asNumber(0));
        EntityId parent = static_cast<EntityId>(e["parent"].asNumber(0));
        if (parent != kNullEntity && !SetParent(id, parent, error)) return false;
    }
    return true;
}

}  // namespace oe
