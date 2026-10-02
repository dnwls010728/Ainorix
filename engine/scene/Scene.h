#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/Json.h"
#include "core/Math.h"
#include "scene/Reflect.h"

namespace oe {

struct EntityRecord {
    EntityId id = kNullEntity;
    std::string name;
    EntityId parent = kNullEntity;
};

// A scene is a flat set of entities (with optional parent links) plus one
// component pool per registered component type.
class Scene {
public:
    static constexpr const char* kFormat = "ownengine.scene";
    static constexpr int kVersion = 1;

    Scene();
    Scene(const Scene&);
    Scene& operator=(const Scene&);

    void Clear();
    // Replay must preserve allocation gaps from entities deleted before frame zero.
    EntityId AllocationCursor() const { return nextId_; }
    void RestoreAllocationCursor(EntityId cursor) { if (cursor >= nextId_) nextId_ = cursor; }

    // `forcedId` != 0 creates the entity with that id (used when loading).
    EntityId Create(const std::string& name, EntityId parent = kNullEntity, EntityId forcedId = kNullEntity);
    // Destroys the entity and all of its descendants. Returns number destroyed.
    int Destroy(EntityId id);
    bool Exists(EntityId id) const { return entities_.count(id) > 0; }
    EntityRecord* Record(EntityId id);
    const EntityRecord* Record(EntityId id) const;
    const std::map<EntityId, EntityRecord>& Entities() const { return entities_; }
    std::vector<EntityId> Children(EntityId id) const;
    EntityId FindByName(const std::string& name) const;
    bool SetParent(EntityId id, EntityId parent, std::string* error);

    void* AddComponent(EntityId id, const ComponentType& type);
    void* GetComponent(EntityId id, const ComponentType& type);
    const void* GetComponent(EntityId id, const ComponentType& type) const;
    bool RemoveComponent(EntityId id, const ComponentType& type);
    std::vector<const ComponentType*> ComponentsOf(EntityId id) const;

    template <class T>
    T& Add(EntityId id) { return *static_cast<T*>(pools_[Index<T>()]->Add(id)); }
    template <class T>
    T* Get(EntityId id) { return static_cast<T*>(pools_[Index<T>()]->Get(id)); }
    template <class T>
    const T* Get(EntityId id) const { return static_cast<const T*>(pools_[Index<T>()]->Get(id)); }
    template <class T>
    std::map<EntityId, T>& Pool() { return static_cast<ComponentPool<T>*>(pools_[Index<T>()].get())->data; }
    template <class T>
    const std::map<EntityId, T>& Pool() const { return static_cast<const ComponentPool<T>*>(pools_[Index<T>()].get())->data; }

    // World transform of an entity (parent chain applied).
    Mat4 WorldMatrix(EntityId id) const;

    Json EntityToJson(EntityId id) const;
    Json ToJson() const;
    bool FromJson(const Json& json, std::string* error);
    // Creates an entity (and components) from the same shape EntityToJson emits.
    EntityId CreateFromJson(const Json& entity, bool keepId, std::string* error);

    std::string name = "Untitled";

private:
    template <class T>
    static size_t Index() { return static_cast<size_t>(ComponentTypeIndex<T>::value); }

    std::map<EntityId, EntityRecord> entities_;
    std::vector<std::unique_ptr<IComponentPool>> pools_;
    EntityId nextId_ = 1;
};

}  // namespace oe
