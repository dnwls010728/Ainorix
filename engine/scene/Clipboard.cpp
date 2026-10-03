#include "scene/Clipboard.h"

#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace oe {
namespace {

void RemapReferences(Json& entity, const std::map<EntityId, EntityId>& ids) {
    for (auto& component : entity["components"].members()) {
        const ComponentType* type = TypeRegistry::Find(component.first);
        if (!type) continue;
        for (const FieldInfo& field : type->fields) if (field.type == FieldType::Entity && component.second.has(field.name)) {
            EntityId old = static_cast<EntityId>(component.second[field.name].asNumber());
            auto found = ids.find(old);
            component.second[field.name] = found == ids.end() ? kNullEntity : found->second;
        }
    }
}

bool ReadId(const Json& value, EntityId& id) {
    double number = value.asNumber(-1);
    if (!value.isNumber() || !std::isfinite(number) || number < 0 || number > std::numeric_limits<EntityId>::max() || std::floor(number) != number) return false;
    id = static_cast<EntityId>(number);
    return true;
}

}  // namespace

Json CopyEntities(const Scene& scene, const std::vector<EntityId>& selection) {
    std::set<EntityId> selected(selection.begin(), selection.end());
    std::map<EntityId, EntityId> ids;
    for (const auto& entry : scene.Entities()) {
        for (EntityId ancestor = entry.first; ancestor != kNullEntity; ancestor = scene.Record(ancestor)->parent) {
            if (selected.count(ancestor)) {
                ids[entry.first] = static_cast<EntityId>(ids.size() + 1);
                break;
            }
        }
    }
    Json document = Json::MakeObject(), entities = Json::MakeArray();
    document["format"] = "ownengine.clipboard";
    document["version"] = 1;
    for (const auto& entry : ids) {
        Json entity = scene.EntityToJson(entry.first);
        entity["id"] = entry.second;
        const auto parent = ids.find(scene.Record(entry.first)->parent);
        if (parent == ids.end()) entity.erase("parent");
        else entity["parent"] = parent->second;
        RemapReferences(entity, ids);
        entities.push(entity);
    }
    document["entities"] = entities;
    return document;
}

bool PasteEntities(Scene& scene, const Json& document, EntityId parent,
                   std::vector<EntityId>& roots, std::string* error) {
    const auto fail = [&](const std::string& message) { if (error) *error = message; return false; };
    roots.clear();
    if (!document.isObject() || document["format"].asString() != "ownengine.clipboard" || document["version"].asNumber(-1) != 1 ||
        !document["entities"].isArray() || document["entities"].size() == 0 || document["entities"].size() > 10000 ||
        document.dump().size() > 8 * 1024 * 1024) return fail("expected an ownengine.clipboard v1 document with 1..10000 entities (at most 8 MiB)");
    if (parent && !scene.Exists(parent)) return fail("paste parent does not exist");
    std::set<EntityId> localIds;
    for (const Json& entity : document["entities"].items()) {
        EntityId id = 0;
        if (!entity.isObject() || !ReadId(entity["id"], id) || !id || !localIds.insert(id).second)
            return fail("clipboard entity ids must be unique positive integers");
    }
    for (const Json& entity : document["entities"].items()) {
        EntityId id = 0;
        if (entity.has("parent") && (!ReadId(entity["parent"], id) || (id && !localIds.count(id))))
            return fail("clipboard parent must reference an entity in the document");
        for (const auto& component : entity["components"].members()) {
            const ComponentType* type = TypeRegistry::Find(component.first);
            if (!type) return fail("unknown clipboard component " + component.first);
            for (const FieldInfo& field : type->fields) if (field.type == FieldType::Entity && component.second.has(field.name)) {
                if (!ReadId(component.second[field.name], id) || (id && !localIds.count(id)))
                    return fail("clipboard entity reference must be null or local to the document");
            }
        }
    }
    Json validation = document;
    validation["format"] = Scene::kFormat;
    Scene source;
    if (!source.FromJson(validation, error)) return false;
    Scene candidate = scene;
    std::map<EntityId, EntityId> ids;
    for (const auto& entry : source.Entities()) {
        const std::string base = entry.second.name + " Copy";
        std::string name = base;
        for (size_t suffix = 2; candidate.FindByName(name); ++suffix) name = base + " " + std::to_string(suffix);
        ids[entry.first] = candidate.Create(name);
    }
    std::vector<EntityId> createdRoots;
    for (const auto& entry : source.Entities()) {
        const EntityId id = ids.at(entry.first);
        const EntityId newParent = entry.second.parent ? ids.at(entry.second.parent) : parent;
        if (!candidate.SetParent(id, newParent, error)) return false;
        if (!entry.second.parent) createdRoots.push_back(id);
        Json entity = source.EntityToJson(entry.first);
        RemapReferences(entity, ids);
        for (const auto& component : entity["components"].members()) {
            const ComponentType& type = *TypeRegistry::Find(component.first);
            if (!ApplyComponentJson(type, candidate.AddComponent(id, type), component.second, error)) return false;
        }
    }
    scene = candidate;
    roots = std::move(createdRoots);
    return true;
}

}  // namespace oe
