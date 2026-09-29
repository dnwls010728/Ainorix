#include "scene/Prefab.h"

#include <functional>
#include <map>

#include "scene/Components.h"

namespace oe {

Json MakePrefab(const Scene& scene, EntityId root) {
    Json prefab = Json::MakeObject();
    prefab["format"] = kPrefabFormat;
    prefab["version"] = 1;
    const EntityRecord* rec = scene.Record(root);
    prefab["name"] = rec ? rec->name : "";
    Json list = Json::MakeArray();
    std::map<EntityId, EntityId> local;  // scene id -> prefab id
    std::function<void(EntityId)> visit = [&](EntityId id) {
        EntityId localId = static_cast<EntityId>(local.size() + 1);
        local[id] = localId;
        Json e = scene.EntityToJson(id);
        e["id"] = localId;
        if (id == root) {
            e.erase("parent");
            e["components"].erase("Prefab");  // an instance's marker is not part of the prefab
        } else {
            e["parent"] = local[scene.Record(id)->parent];
        }
        list.push(e);
        for (EntityId child : scene.Children(id)) visit(child);
    };
    if (rec) visit(root);
    prefab["entities"] = list;
    return prefab;
}

EntityId InstantiatePrefab(Scene& scene, const Json& prefab, const std::string& prefabPath, EntityId parent, std::string* error) {
    if (!prefab.isObject() || prefab["format"].asString("") != kPrefabFormat || prefab["entities"].size() == 0) {
        if (error) *error = "'" + prefabPath + "' is not a prefab file (expected \"format\": \"" + kPrefabFormat + "\" with entities)";
        return kNullEntity;
    }
    std::map<EntityId, EntityId> created;  // prefab id -> scene id
    EntityId root = kNullEntity;
    for (const Json& e : prefab["entities"].items()) {
        Json copy = e;
        EntityId localId = static_cast<EntityId>(e["id"].asNumber(0));
        EntityId localParent = static_cast<EntityId>(e["parent"].asNumber(0));
        copy.erase("id");
        copy.erase("parent");
        if (root == kNullEntity) {
            if (parent != kNullEntity) copy["parent"] = parent;
        } else {
            auto it = created.find(localParent);
            copy["parent"] = it != created.end() ? it->second : root;
        }
        EntityId id = scene.CreateFromJson(copy, false, error);
        if (id == kNullEntity) {
            if (root != kNullEntity) scene.Destroy(root);
            return kNullEntity;
        }
        created[localId] = id;
        if (root == kNullEntity) root = id;
    }
    scene.Add<Prefab>(root).path = prefabPath;
    return root;
}

}  // namespace oe
