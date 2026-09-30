#include "scene/Reflect.h"

namespace oe {

const char* ToString(FieldType type) {
    switch (type) {
        case FieldType::Float: return "float";
        case FieldType::Int: return "int";
        case FieldType::Bool: return "bool";
        case FieldType::String: return "string";
        case FieldType::Vec3: return "vec3";
        case FieldType::Color: return "color";
        case FieldType::Entity: return "entity";
        case FieldType::Json: return "json";
    }
    return "float";
}

const FieldInfo* ComponentType::FindField(const std::string& fieldName) const {
    for (const FieldInfo& f : fields) {
        if (f.name == fieldName) return &f;
    }
    return nullptr;
}

std::vector<ComponentType>& TypeRegistry::Types() {
    static std::vector<ComponentType> types;
    return types;
}

const ComponentType* TypeRegistry::Find(const std::string& name) {
    for (const ComponentType& t : Types()) {
        if (t.name == name) return &t;
    }
    return nullptr;
}

namespace {
template <class T>
T& At(void* c, size_t offset) {
    return *reinterpret_cast<T*>(static_cast<char*>(c) + offset);
}
template <class T>
const T& At(const void* c, size_t offset) {
    return *reinterpret_cast<const T*>(static_cast<const char*>(c) + offset);
}

bool ReadTriple(const Json& v, float out[3], const char* keys, std::string* error, const std::string& fieldName) {
    if (v.isArray() && v.size() == 3 && v[0].isNumber() && v[1].isNumber() && v[2].isNumber()) {
        for (int i = 0; i < 3; ++i) out[i] = v[static_cast<size_t>(i)].asFloat();
        return true;
    }
    if (v.isObject()) {
        const char k[3] = {keys[0], keys[1], keys[2]};
        bool any = false;
        for (int i = 0; i < 3; ++i) {
            const Json* c = v.find(std::string(1, k[i]));
            if (c && c->isNumber()) {
                out[i] = c->asFloat();
                any = true;
            }
        }
        if (any) return true;
    }
    if (error) *error = "field '" + fieldName + "' expects an array of 3 numbers, got " + v.dump();
    return false;
}

bool ParseHexColor(const std::string& s, Color& c) {
    std::string h = s[0] == '#' ? s.substr(1) : s;
    if (h.size() != 6) return false;
    unsigned v = 0;
    for (char ch : h) {
        v <<= 4;
        if (ch >= '0' && ch <= '9') v |= static_cast<unsigned>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') v |= static_cast<unsigned>(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') v |= static_cast<unsigned>(ch - 'A' + 10);
        else return false;
    }
    c = Color(((v >> 16) & 0xFF) / 255.0f, ((v >> 8) & 0xFF) / 255.0f, (v & 0xFF) / 255.0f);
    return true;
}
}  // namespace

Json FieldToJson(const FieldInfo& field, const void* c) {
    switch (field.type) {
        case FieldType::Float: return Json(At<float>(c, field.offset));
        case FieldType::Int: return Json(At<int>(c, field.offset));
        case FieldType::Bool: return Json(At<bool>(c, field.offset));
        case FieldType::String: return Json(At<std::string>(c, field.offset));
        case FieldType::Entity: return Json(At<EntityId>(c, field.offset));
        case FieldType::Json: return At<Json>(c, field.offset);
        case FieldType::Vec3: {
            const Vec3& v = At<Vec3>(c, field.offset);
            return Json(Json::Array{v.x, v.y, v.z});
        }
        case FieldType::Color: {
            const Color& v = At<Color>(c, field.offset);
            return Json(Json::Array{v.r, v.g, v.b});
        }
    }
    return Json();
}

bool FieldFromJson(const FieldInfo& field, void* c, const Json& v, std::string* error) {
    auto typeError = [&](const char* expected) {
        if (error) *error = "field '" + field.name + "' expects " + expected + ", got " + v.typeName() + " " + v.dump();
        return false;
    };
    switch (field.type) {
        case FieldType::Float: {
            if (!v.isNumber()) return typeError("a number");
            float f = v.asFloat();
            if (field.hasRange) f = Clamp(f, field.min, field.max);
            At<float>(c, field.offset) = f;
            return true;
        }
        case FieldType::Int:
            if (!v.isNumber()) return typeError("an integer");
            At<int>(c, field.offset) = field.hasRange ? static_cast<int>(Clamp(static_cast<float>(v.asNumber()), field.min, field.max)) : v.asInt();
            return true;
        case FieldType::Bool:
            if (!v.isBool()) return typeError("true or false");
            At<bool>(c, field.offset) = v.asBool();
            return true;
        case FieldType::Entity:
            if (!v.isNumber()) return typeError("an entity id (integer)");
            At<EntityId>(c, field.offset) = static_cast<EntityId>(v.asNumber());
            return true;
        case FieldType::Json:
            if (!v.isObject() && !v.isArray()) return typeError("a JSON object or array");
            At<Json>(c, field.offset) = v;
            return true;
        case FieldType::String: {
            if (!v.isString()) return typeError("a string");
            if (!field.options.empty()) {
                bool ok = false;
                for (const std::string& o : field.options) ok = ok || o == v.asString();
                if (!ok) {
                    std::string opts;
                    for (const std::string& o : field.options) opts += (opts.empty() ? "" : ", ") + o;
                    if (error) *error = "field '" + field.name + "' must be one of [" + opts + "], got \"" + v.asString() + "\"";
                    return false;
                }
            }
            At<std::string>(c, field.offset) = v.asString();
            return true;
        }
        case FieldType::Vec3: {
            float t[3];
            Vec3& dst = At<Vec3>(c, field.offset);
            t[0] = dst.x; t[1] = dst.y; t[2] = dst.z;
            if (!ReadTriple(v, t, "xyz", error, field.name)) return false;
            dst = Vec3(t[0], t[1], t[2]);
            return true;
        }
        case FieldType::Color: {
            Color& dst = At<Color>(c, field.offset);
            if (v.isString()) {
                if (!ParseHexColor(v.asString(), dst)) return typeError("[r,g,b] in 0..1 or \"#rrggbb\"");
                return true;
            }
            float t[3] = {dst.r, dst.g, dst.b};
            if (!ReadTriple(v, t, "rgb", error, field.name)) return false;
            dst = Color(t[0], t[1], t[2]);
            return true;
        }
    }
    return false;
}

Json ComponentToJson(const ComponentType& type, const void* c) {
    Json out = Json::MakeObject();
    for (const FieldInfo& f : type.fields) out[f.name] = FieldToJson(f, c);
    return out;
}

bool ApplyComponentJson(const ComponentType& type, void* c, const Json& values, std::string* error) {
    if (values.isNull()) return true;
    if (!values.isObject()) {
        if (error) *error = "component '" + type.name + "' values must be an object, got " + values.typeName();
        return false;
    }
    for (const auto& kv : values.members()) {
        const FieldInfo* f = type.FindField(kv.first);
        if (!f) {
            std::string names;
            for (const FieldInfo& fi : type.fields) names += (names.empty() ? "" : ", ") + fi.name;
            if (error) *error = "component '" + type.name + "' has no field '" + kv.first + "'. Valid fields: " + names;
            return false;
        }
        if (!FieldFromJson(*f, c, kv.second, error)) return false;
    }
    return true;
}

Json ComponentSchema(const ComponentType& type) {
    Json schema = Json::MakeObject();
    schema["type"] = "object";
    schema["description"] = type.doc;
    Json props = Json::MakeObject();
    for (const FieldInfo& f : type.fields) {
        Json p = Json::MakeObject();
        p["description"] = f.doc;
        p["x-oe-type"] = ToString(f.type);
        switch (f.type) {
            case FieldType::Float: p["type"] = "number"; break;
            case FieldType::Int:
            case FieldType::Entity: p["type"] = "integer"; break;
            case FieldType::Bool: p["type"] = "boolean"; break;
            case FieldType::Json: {
                Json types = Json::MakeArray();
                types.push("object");
                types.push("array");
                p["type"] = types;
                break;
            }
            case FieldType::String: p["type"] = "string"; break;
            case FieldType::Vec3:
            case FieldType::Color:
                p["type"] = "array";
                p["items"]["type"] = "number";
                p["minItems"] = 3;
                p["maxItems"] = 3;
                break;
        }
        if (f.hasRange) {
            p["minimum"] = f.min;
            p["maximum"] = f.max;
        }
        if (!f.options.empty()) {
            Json e = Json::MakeArray();
            for (const std::string& o : f.options) e.push(o);
            p["enum"] = e;
        }
        props[f.name] = p;
    }
    schema["properties"] = props;
    schema["additionalProperties"] = false;
    return schema;
}

}  // namespace oe
