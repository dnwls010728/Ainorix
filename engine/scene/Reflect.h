#pragma once
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/Json.h"
#include "core/Math.h"

namespace oe {

using EntityId = uint32_t;
constexpr EntityId kNullEntity = 0;

// ---------------------------------------------------------------------------
// Reflection. Every component registers its fields once; serialization, the
// command API, the JSON schema handed to AI agents and the editor inspector
// are all generated from this single description.
// ---------------------------------------------------------------------------

enum class FieldType { Float, Int, Bool, String, Vec3, Color, Entity, Json };
const char* ToString(FieldType type);

struct FieldInfo {
    std::string name;
    std::string doc;
    FieldType type = FieldType::Float;
    size_t offset = 0;
    bool hasRange = false;
    float min = 0, max = 0;
    std::vector<std::string> options;  // allowed values for String fields (empty = free text)
};

class FieldList {
public:
    template <class C, class M>
    FieldInfo& Add(const char* name, M C::*member, const char* doc) {
        static const C probe{};
        FieldInfo info;
        info.name = name;
        info.doc = doc;
        info.type = Deduce(static_cast<const M*>(nullptr));
        info.offset = static_cast<size_t>(reinterpret_cast<const char*>(&(probe.*member)) - reinterpret_cast<const char*>(&probe));
        fields.push_back(info);
        return fields.back();
    }
    std::vector<FieldInfo> fields;

private:
    static FieldType Deduce(const float*) { return FieldType::Float; }
    static FieldType Deduce(const int*) { return FieldType::Int; }
    static FieldType Deduce(const bool*) { return FieldType::Bool; }
    static FieldType Deduce(const std::string*) { return FieldType::String; }
    static FieldType Deduce(const Vec3*) { return FieldType::Vec3; }
    static FieldType Deduce(const Color*) { return FieldType::Color; }
    static FieldType Deduce(const EntityId*) { return FieldType::Entity; }
    static FieldType Deduce(const oe::Json*) { return FieldType::Json; }
};

// Type-erased storage for one component type. std::map keeps iteration in
// entity-id order so simulation and rendering are deterministic.
class IComponentPool {
public:
    virtual ~IComponentPool() = default;
    virtual void* Add(EntityId id) = 0;
    virtual void* Get(EntityId id) = 0;
    virtual const void* Get(EntityId id) const = 0;
    virtual bool Remove(EntityId id) = 0;
    virtual void Clear() = 0;
    virtual size_t Size() const = 0;
};

template <class T>
class ComponentPool final : public IComponentPool {
public:
    void* Add(EntityId id) override { return &data[id]; }
    void* Get(EntityId id) override {
        auto it = data.find(id);
        return it == data.end() ? nullptr : &it->second;
    }
    const void* Get(EntityId id) const override {
        auto it = data.find(id);
        return it == data.end() ? nullptr : &it->second;
    }
    bool Remove(EntityId id) override { return data.erase(id) > 0; }
    void Clear() override { data.clear(); }
    size_t Size() const override { return data.size(); }
    std::map<EntityId, T> data;
};

struct ComponentType {
    int index = -1;
    std::string name;
    std::string doc;
    std::vector<FieldInfo> fields;
    std::function<std::unique_ptr<IComponentPool>()> makePool;

    const FieldInfo* FindField(const std::string& fieldName) const;
};

template <class T>
struct ComponentTypeIndex {
    static int value;
};
template <class T>
int ComponentTypeIndex<T>::value = -1;

class TypeRegistry {
public:
    template <class T>
    static void Register() {
        if (ComponentTypeIndex<T>::value >= 0) return;
        ComponentType type;
        type.name = T::kTypeName;
        type.doc = T::kDoc;
        FieldList list;
        T::Reflect(list);
        type.fields = std::move(list.fields);
        type.makePool = [] { return std::make_unique<ComponentPool<T>>(); };
        type.index = static_cast<int>(Types().size());
        ComponentTypeIndex<T>::value = type.index;
        Types().push_back(std::move(type));
    }
    static const std::vector<ComponentType>& All() { return Types(); }
    static const ComponentType* Find(const std::string& name);

private:
    static std::vector<ComponentType>& Types();
};

// Field <-> JSON conversion. ApplyJson only touches the fields present in
// `values` (partial update) and reports unknown fields / bad types.
Json FieldToJson(const FieldInfo& field, const void* component);
bool FieldFromJson(const FieldInfo& field, void* component, const Json& value, std::string* error);
Json ComponentToJson(const ComponentType& type, const void* component);
bool ApplyComponentJson(const ComponentType& type, void* component, const Json& values, std::string* error);
// JSON schema (draft 2020-12 subset) describing a component's fields.
Json ComponentSchema(const ComponentType& type);

}  // namespace oe
