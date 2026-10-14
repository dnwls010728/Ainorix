#include "scene/PropertyTracks.h"

#include <cmath>
#include <set>

#include "scene/Scene.h"

namespace oe {

namespace {

bool StepType(FieldType type) { return type != FieldType::Float && type != FieldType::Vec3 && type != FieldType::Color; }

// Numbers and arrays of numbers of the same length blend; anything else takes the earlier key.
Json Blend(const Json& a, const Json& b, double t) {
    if (a.isNumber() && b.isNumber()) return Json(a.asNumber() + (b.asNumber() - a.asNumber()) * t);
    if (a.isArray() && b.isArray() && a.size() == b.size()) {
        Json out = Json::MakeArray();
        for (size_t i = 0; i < a.size(); ++i) {
            if (!a[i].isNumber() || !b[i].isNumber()) return a;
            out.push(a[i].asNumber() + (b[i].asNumber() - a[i].asNumber()) * t);
        }
        return out;
    }
    return a;
}

}  // namespace

bool AnimatableField(const FieldInfo& field) { return field.type != FieldType::Json && field.type != FieldType::Entity; }

bool ValidatePropertyTracks(const Json& tracks, std::string* error) {
    auto fail = [&](const std::string& message) {
        if (error) *error = "tracks: " + message;
        return false;
    };
    if (!tracks.isArray()) return fail("needs an array of {target, component, field, interpolation, keys}");
    if (tracks.size() > kMaxPropertyTracks) return fail("at most " + std::to_string(kMaxPropertyTracks) + " tracks per clip");
    std::set<std::string> seen;
    for (size_t i = 0; i < tracks.size(); ++i) {
        const Json& track = tracks[i];
        const std::string where = "track " + std::to_string(i) + ": ";
        if (!track.isObject()) return fail(where + "needs an object");
        for (const auto& member : track.members())
            if (member.first != "target" && member.first != "component" && member.first != "field" && member.first != "interpolation" &&
                member.first != "keys")
                return fail(where + "unknown field " + member.first);
        if (track.has("target") && (!track["target"].isString() || track["target"].asString().size() > 256))
            return fail(where + "target must be a child path of at most 256 bytes (\"\" = the animated entity)");
        const ComponentType* type = TypeRegistry::Find(track["component"].asString());
        if (!type) return fail(where + "unknown component '" + track["component"].asString() + "'");
        if (type->name == "SpriteAnimation" || type->name == "Animator") return fail(where + "a track cannot drive the animation components");
        const FieldInfo* field = type->FindField(track["field"].asString());
        if (!field) return fail(where + type->name + " has no field '" + track["field"].asString() + "'");
        if (!AnimatableField(*field)) return fail(where + type->name + "." + field->name + " cannot be animated (JSON and entity fields)");
        const std::string interpolation = track["interpolation"].asString("linear");
        if (track.has("interpolation") && !track["interpolation"].isString()) return fail(where + "interpolation must be a string");
        if (interpolation != "linear" && interpolation != "smooth" && interpolation != "step")
            return fail(where + "interpolation must be linear, smooth or step");
        if (!seen.insert(track["target"].asString() + "|" + type->name + "." + field->name).second)
            return fail(where + "the same property is animated twice");
        const Json& keys = track["keys"];
        if (!keys.isArray() || keys.size() == 0 || keys.size() > kMaxPropertyTrackKeys)
            return fail(where + "keys needs 1.." + std::to_string(kMaxPropertyTrackKeys) + " {time, value}");
        // A scratch component checks each value against the field's type.
        std::unique_ptr<IComponentPool> scratch = type->makePool();
        void* component = scratch->Add(1);
        double previous = -1;
        for (size_t k = 0; k < keys.size(); ++k) {
            const Json& key = keys[k];
            if (!key.isObject() || !key["time"].isNumber() || !key.has("value") || key.size() != 2)
                return fail(where + "key " + std::to_string(k) + " needs {time, value}");
            const double time = key["time"].asNumber();
            if (!std::isfinite(time) || time < 0 || time > 3600) return fail(where + "key times must be in 0..3600 seconds");
            if (time < previous) return fail(where + "keys must be in time order");
            previous = time;
            std::string message;
            if (!FieldFromJson(*field, component, key["value"], &message)) return fail(where + "key " + std::to_string(k) + ": " + message);
        }
    }
    return true;
}

EntityId PropertyTrackTarget(const Scene& scene, EntityId owner, const std::string& target) {
    EntityId at = owner;
    size_t start = 0;
    while (start < target.size() && at != kNullEntity) {
        size_t end = target.find('/', start);
        if (end == std::string::npos) end = target.size();
        const std::string name = target.substr(start, end - start);
        start = end + 1;
        if (name.empty()) continue;
        EntityId found = kNullEntity;
        for (EntityId child : scene.Children(at)) {  // the lowest id wins when names repeat
            const EntityRecord* record = scene.Record(child);
            if (record && record->name == name && (found == kNullEntity || child < found)) found = child;
        }
        at = found;
    }
    return at;
}

Json SamplePropertyTrack(const Json& track, double time, bool stepOnly) {
    const Json& keys = track["keys"];
    if (!keys.isArray() || keys.size() == 0) return Json();
    size_t next = 0;
    while (next < keys.size() && keys[next]["time"].asNumber() <= time) ++next;
    if (next == 0) return keys[0]["value"];
    if (next == keys.size()) return keys[keys.size() - 1]["value"];
    const Json& a = keys[next - 1];
    const Json& b = keys[next];
    const std::string interpolation = track["interpolation"].asString("linear");
    if (stepOnly || interpolation == "step") return a["value"];
    const double span = b["time"].asNumber() - a["time"].asNumber();
    double t = span > 0 ? (time - a["time"].asNumber()) / span : 1.0;
    if (interpolation == "smooth") t = t * t * (3 - 2 * t);
    return Blend(a["value"], b["value"], t);
}

void ApplyPropertyTracks(Scene& scene, EntityId owner, const Json& tracks, double time) {
    if (!tracks.isArray()) return;
    for (const Json& track : tracks.items()) {
        const ComponentType* type = TypeRegistry::Find(track["component"].asString());
        if (!type || type->name == "SpriteAnimation" || type->name == "Animator") continue;
        const FieldInfo* field = type->FindField(track["field"].asString());
        if (!field || !AnimatableField(*field)) continue;
        const EntityId target = PropertyTrackTarget(scene, owner, track["target"].asString());
        if (target == kNullEntity) continue;
        void* component = scene.GetComponent(target, *type);
        if (!component) continue;
        const Json value = SamplePropertyTrack(track, time, StepType(field->type));
        if (!value.isNull()) FieldFromJson(*field, component, value, nullptr);
    }
}

}  // namespace oe
