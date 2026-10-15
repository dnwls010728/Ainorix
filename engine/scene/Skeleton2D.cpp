#include "scene/Skeleton2D.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

#include "scene/Components.h"

namespace oe {
namespace {

// A validation failure at a place in the file; caught by ParseSkeleton2D.
struct Invalid : std::runtime_error {
    explicit Invalid(const std::string& message) : std::runtime_error(message) {}
};

[[noreturn]] void Fail(const std::string& where, const std::string& what) { throw Invalid(where + ": " + what); }

void OnlyFields(const Json& object, const std::string& where, std::initializer_list<const char*> allowed) {
    if (!object.isObject()) Fail(where, "must be an object");
    for (const auto& member : object.members()) {
        bool known = false;
        for (const char* name : allowed) known = known || member.first == name;
        if (known) continue;
        std::string list;
        for (const char* name : allowed) list += (list.empty() ? "" : ", ") + std::string(name);
        Fail(where, "unknown field '" + member.first + "' (allowed: " + list + ")");
    }
}

float Number(const Json& value, const std::string& where, float fallback, float lo = -1e6f, float hi = 1e6f) {
    if (value.isNull()) return fallback;
    if (!value.isNumber() || !std::isfinite(value.asNumber())) Fail(where, "must be a finite number");
    const double v = value.asNumber();
    if (v < lo || v > hi) Fail(where, "must be in " + std::to_string(lo) + ".." + std::to_string(hi));
    return static_cast<float>(v);
}

std::string Name(const Json& value, const std::string& where) {
    if (!value.isString() || value.asString().empty() || value.asString().size() > 128) Fail(where, "must be a name (1..128 characters)");
    return value.asString();
}

bool HexColor(const std::string& s, Vec4& out) {
    const std::string h = !s.empty() && s[0] == '#' ? s.substr(1) : s;
    if (h.size() != 6 && h.size() != 8) return false;
    unsigned long v = 0;
    for (char ch : h) {
        v <<= 4;
        if (ch >= '0' && ch <= '9') v |= static_cast<unsigned long>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') v |= static_cast<unsigned long>(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') v |= static_cast<unsigned long>(ch - 'A' + 10);
        else return false;
    }
    if (h.size() == 6) v = (v << 8) | 0xFF;
    out = Vec4(static_cast<float>((v >> 24) & 0xFF) / 255.0f, static_cast<float>((v >> 16) & 0xFF) / 255.0f,
               static_cast<float>((v >> 8) & 0xFF) / 255.0f, static_cast<float>(v & 0xFF) / 255.0f);
    return true;
}

// "#rrggbb", "#rrggbbaa" or [r, g, b(, a)] in 0..1.
Vec4 ColorValue(const Json& value, const std::string& where) {
    Vec4 c(1, 1, 1, 1);
    if (value.isString()) {
        if (!HexColor(value.asString(), c)) Fail(where, "must be \"#rrggbb\", \"#rrggbbaa\" or [r, g, b, a]");
        return c;
    }
    if (!value.isArray() || (value.size() != 3 && value.size() != 4)) Fail(where, "must be \"#rrggbb\", \"#rrggbbaa\" or [r, g, b, a]");
    float* parts[4] = {&c.x, &c.y, &c.z, &c.w};
    for (size_t i = 0; i < value.size(); ++i) *parts[i] = Number(value[static_cast<int>(i)], where, 1, 0, 1);
    return c;
}

Skeleton2DCurve CurveName(const std::string& name, const std::string& where) {
    if (name == "linear") return Skeleton2DCurve::Linear;
    if (name == "step") return Skeleton2DCurve::Step;
    if (name == "smooth") return Skeleton2DCurve::Smooth;
    if (name == "easeIn") return Skeleton2DCurve::EaseIn;
    if (name == "easeOut") return Skeleton2DCurve::EaseOut;
    Fail(where, "unknown curve '" + name + "' (linear, step, smooth, easeIn, easeOut)");
}

enum class KeyKind { Rotate, Translate, Scale, Color, Attachment };

// Keys are compact arrays: [time, value..., curve?] (docs/SKELETON2D.md, "Clips").
std::vector<Skeleton2DKey> Keys(const Json& list, const std::string& where, KeyKind kind) {
    if (!list.isArray()) Fail(where, "must be an array of keys [time, value..., curve]");
    if (list.size() > Skeleton2DDef::kMaxKeys) Fail(where, "has more than 4096 keys");
    std::vector<Skeleton2DKey> keys;
    for (size_t i = 0; i < list.size(); ++i) {
        const std::string at = where + "[" + std::to_string(i) + "]";
        const Json& k = list[static_cast<int>(i)];
        if (!k.isArray() || k.size() < 2) Fail(at, "must be [time, value..., curve]");
        Skeleton2DKey key;
        key.time = Number(k[0], at + "[0] (time)", 0, 0, 3600);
        if (!keys.empty() && key.time <= keys.back().time) Fail(at, "times must increase");
        size_t next = 1;
        switch (kind) {
            case KeyKind::Rotate: key.v[0] = Number(k[1], at, 0); next = 2; break;
            case KeyKind::Translate:
            case KeyKind::Scale:
                if (k.size() < 3) Fail(at, "must be [time, x, y, curve]");
                key.v[0] = Number(k[1], at, 0);
                key.v[1] = Number(k[2], at, 0);
                next = 3;
                break;
            case KeyKind::Color: {
                const Vec4 c = ColorValue(k[1], at);
                key.v[0] = c.x; key.v[1] = c.y; key.v[2] = c.z; key.v[3] = c.w;
                next = 2;
                if (k.size() > 2 && k[2].isNumber()) { key.v[3] = Number(k[2], at + " (alpha)", 1, 0, 1); next = 3; }
                break;
            }
            case KeyKind::Attachment:
                if (!k[1].isString() && !k[1].isNull()) Fail(at, "value must be an attachment name or null (hide)");
                key.text = k[1].asString("");
                key.curve = Skeleton2DCurve::Step;
                next = 2;
                break;
        }
        if (next < k.size()) {
            if (kind == KeyKind::Attachment || !k[static_cast<int>(next)].isString() || next + 1 != k.size())
                Fail(at, "has extra values; the last one may only be a curve name");
            key.curve = CurveName(k[static_cast<int>(next)].asString(), at);
        }
        keys.push_back(std::move(key));
    }
    return keys;
}

Skeleton2DAttachment ParseAttachment(const Json& a, const std::string& where, const std::string& name, const Skeleton2DDef& def) {
    OnlyFields(a, where, {"image", "rect", "pivot", "x", "y", "rotation", "scaleX", "scaleY", "width", "height", "vertices", "triangles", "weights", "filled"});
    Skeleton2DAttachment out;
    out.name = name;
    if (!a["image"].isString() || a["image"].asString().empty()) Fail(where + ".image", "must be a project image path");
    out.image = a["image"].asString();
    if (a.has("rect")) {
        const Json& r = a["rect"];
        if (!r.isArray() || r.size() != 4) Fail(where + ".rect", "must be [x, y, width, height] in image pixels");
        for (int i = 0; i < 4; ++i) out.rect[i] = Number(r[i], where + ".rect", 0, 0, 16384);
        if (out.rect[2] < 1 || out.rect[3] < 1) Fail(where + ".rect", "width and height must be at least 1 pixel");
        out.hasRect = true;
    }
    if (a.has("pivot")) {
        const Json& p = a["pivot"];
        if (!p.isArray() || p.size() != 2) Fail(where + ".pivot", "must be [x, y], 0..1 of the rectangle (0,0 = bottom-left)");
        out.pivot[0] = Number(p[0], where + ".pivot", 0.5f, -4, 5);
        out.pivot[1] = Number(p[1], where + ".pivot", 0.5f, -4, 5);
    }
    out.x = Number(a["x"], where + ".x", 0);
    out.y = Number(a["y"], where + ".y", 0);
    out.rotation = Number(a["rotation"], where + ".rotation", 0);
    out.scaleX = Number(a["scaleX"], where + ".scaleX", 1);
    out.scaleY = Number(a["scaleY"], where + ".scaleY", 1);
    out.width = Number(a["width"], where + ".width", 0, 0, 10000);
    out.height = Number(a["height"], where + ".height", 0, 0, 10000);
    if (a.has("filled") && !a["filled"].isBool()) Fail(where + ".filled", "must be true or false");
    out.filled = a["filled"].asBool(false);
    const bool mesh = a.has("vertices") || a.has("triangles") || a.has("weights");
    if (!mesh) return out;
    const Json& v = a["vertices"];
    if (!v.isArray() || v.size() < 6 || v.size() % 2 != 0) Fail(where + ".vertices", "must be [x0, y0, x1, y1, ...] in rectangle pixels, at least 3 vertices");
    if (v.size() / 2 > Skeleton2DDef::kMaxMeshVertices) Fail(where + ".vertices", "has more than 4096 vertices");
    for (size_t i = 0; i < v.size(); ++i) out.vertices.push_back(Number(v[static_cast<int>(i)], where + ".vertices", 0, -16384, 32768));
    const size_t count = out.vertices.size() / 2;
    const Json& t = a["triangles"];
    if (!t.isArray() || t.size() < 3 || t.size() % 3 != 0) Fail(where + ".triangles", "must be vertex index triples");
    for (size_t i = 0; i < t.size(); ++i) {
        const double n = t[static_cast<int>(i)].asNumber(-1);
        if (!t[static_cast<int>(i)].isNumber() || n < 0 || n >= static_cast<double>(count) || std::floor(n) != n)
            Fail(where + ".triangles[" + std::to_string(i) + "]", "must be a vertex index 0.." + std::to_string(count - 1));
        out.triangles.push_back(static_cast<uint32_t>(n));
    }
    if (a.has("weights")) {
        const Json& w = a["weights"];
        if (!w.isArray() || w.size() != count) Fail(where + ".weights", "needs one entry per vertex (" + std::to_string(count) + "): [[bone, weight], ...]");
        for (size_t i = 0; i < count; ++i) {
            const std::string at = where + ".weights[" + std::to_string(i) + "]";
            const Json& list = w[static_cast<int>(i)];
            if (!list.isArray() || list.size() == 0 || list.size() > 4) Fail(at, "must list 1..4 [bone, weight] pairs");
            std::vector<std::pair<int, float>> influences;
            float total = 0;
            for (const Json& pair : list.items()) {
                if (!pair.isArray() || pair.size() != 2 || !pair[0].isString()) Fail(at, "each influence is [boneName, weight]");
                const int bone = def.BoneIndex(pair[0].asString());
                if (bone < 0) Fail(at, "unknown bone '" + pair[0].asString() + "'");
                const float weight = Number(pair[1], at, 0, 0, 1);
                total += weight;
                influences.emplace_back(bone, weight);
            }
            if (total <= 1e-6f) Fail(at, "weights add up to zero");
            for (auto& influence : influences) influence.second /= total;  // normalized, like glTF skins
            out.weights.push_back(std::move(influences));
        }
    }
    return out;
}

Skeleton2DClip ParseClip(const Json& c, const std::string& where, const std::string& name, const Skeleton2DDef& def) {
    OnlyFields(c, where, {"duration", "loop", "bones", "slots", "drawOrder", "events"});
    Skeleton2DClip clip;
    clip.name = name;
    clip.loop = c["loop"].asBool(true);
    if (c.has("loop") && !c["loop"].isBool()) Fail(where + ".loop", "must be true or false");
    float last = 0;
    if (c.has("bones")) {
        if (!c["bones"].isObject()) Fail(where + ".bones", "must map bone names to {rotate, translate, scale}");
        for (const auto& b : c["bones"].members()) {
            const std::string at = where + ".bones." + b.first;
            Skeleton2DBoneTimeline timeline;
            timeline.bone = def.BoneIndex(b.first);
            if (timeline.bone < 0) Fail(at, "unknown bone");
            OnlyFields(b.second, at, {"rotate", "translate", "scale"});
            if (b.second.has("rotate")) timeline.rotate = Keys(b.second["rotate"], at + ".rotate", KeyKind::Rotate);
            if (b.second.has("translate")) timeline.translate = Keys(b.second["translate"], at + ".translate", KeyKind::Translate);
            if (b.second.has("scale")) timeline.scale = Keys(b.second["scale"], at + ".scale", KeyKind::Scale);
            for (const auto* keys : {&timeline.rotate, &timeline.translate, &timeline.scale})
                if (!keys->empty()) last = std::max(last, keys->back().time);
            clip.bones.push_back(std::move(timeline));
        }
        std::sort(clip.bones.begin(), clip.bones.end(), [](const auto& a, const auto& b) { return a.bone < b.bone; });
    }
    if (c.has("slots")) {
        if (!c["slots"].isObject()) Fail(where + ".slots", "must map slot names to {attachment, color}");
        for (const auto& s : c["slots"].members()) {
            const std::string at = where + ".slots." + s.first;
            Skeleton2DSlotTimeline timeline;
            timeline.slot = def.SlotIndex(s.first);
            if (timeline.slot < 0) Fail(at, "unknown slot");
            OnlyFields(s.second, at, {"attachment", "color"});
            if (s.second.has("attachment")) timeline.attachment = Keys(s.second["attachment"], at + ".attachment", KeyKind::Attachment);
            if (s.second.has("color")) timeline.color = Keys(s.second["color"], at + ".color", KeyKind::Color);
            for (const Skeleton2DKey& key : timeline.attachment)
                if (!key.text.empty() && !def.FindAttachment("", timeline.slot, key.text)) {
                    bool inSkin = false;
                    for (const auto& skin : def.skins) inSkin = inSkin || def.FindAttachment(skin.first, timeline.slot, key.text);
                    if (!inSkin) Fail(at + ".attachment", "no skin has attachment '" + key.text + "' in this slot");
                }
            for (const auto* keys : {&timeline.attachment, &timeline.color})
                if (!keys->empty()) last = std::max(last, keys->back().time);
            clip.slots.push_back(std::move(timeline));
        }
        std::sort(clip.slots.begin(), clip.slots.end(), [](const auto& a, const auto& b) { return a.slot < b.slot; });
    }
    if (c.has("drawOrder")) {
        const Json& list = c["drawOrder"];
        if (!list.isArray() || list.size() > Skeleton2DDef::kMaxKeys) Fail(where + ".drawOrder", "must be keys [time, [slot names back to front]]");
        for (size_t i = 0; i < list.size(); ++i) {
            const std::string at = where + ".drawOrder[" + std::to_string(i) + "]";
            const Json& k = list[static_cast<int>(i)];
            if (!k.isArray() || k.size() != 2 || !k[1].isArray()) Fail(at, "must be [time, [slot names back to front]]");
            Skeleton2DKey key;
            key.time = Number(k[0], at, 0, 0, 3600);
            key.curve = Skeleton2DCurve::Step;
            if (!clip.drawOrder.empty() && key.time <= clip.drawOrder.back().time) Fail(at, "times must increase");
            std::set<int> seen;
            for (const Json& slot : k[1].items()) {
                const int index = slot.isString() ? def.SlotIndex(slot.asString()) : -1;
                if (index < 0) Fail(at, "unknown slot " + slot.dump());
                if (!seen.insert(index).second) Fail(at, "slot " + slot.dump() + " is listed twice");
                key.order.push_back(index);
            }
            if (key.order.size() != def.slots.size()) Fail(at, "must list every slot once (" + std::to_string(def.slots.size()) + ")");
            last = std::max(last, key.time);
            clip.drawOrder.push_back(std::move(key));
        }
    }
    clip.duration = c.has("duration") ? Number(c["duration"], where + ".duration", 0, 0, 3600) : last;
    if (c.has("events")) {
        const Json& list = c["events"];
        if (!list.isArray() || list.size() > kMaxAnimationMarkers) Fail(where + ".events", "must be up to 256 {time, name}");
        for (size_t i = 0; i < list.size(); ++i) {
            const std::string at = where + ".events[" + std::to_string(i) + "]";
            OnlyFields(list[static_cast<int>(i)], at, {"time", "name"});
            AnimationMarker marker;
            marker.time = Number(list[static_cast<int>(i)]["time"], at + ".time", 0, 0, clip.duration);
            marker.name = Name(list[static_cast<int>(i)]["name"], at + ".name");
            clip.events.push_back(marker);
        }
        std::stable_sort(clip.events.begin(), clip.events.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    }
    return clip;
}

float Shape(Skeleton2DCurve curve, float t) {
    switch (curve) {
        case Skeleton2DCurve::Step: return 0;
        case Skeleton2DCurve::Smooth: return t * t * (3 - 2 * t);
        case Skeleton2DCurve::EaseIn: return t * t;
        case Skeleton2DCurve::EaseOut: return 1 - (1 - t) * (1 - t);
        case Skeleton2DCurve::Linear: break;
    }
    return t;
}

// The key before `time` and the eased fraction toward the next one.
const Skeleton2DKey* Locate(const std::vector<Skeleton2DKey>& keys, float time, const Skeleton2DKey** next, float* t) {
    *next = nullptr;
    *t = 0;
    if (keys.empty()) return nullptr;
    if (time <= keys.front().time) return &keys.front();
    auto after = std::upper_bound(keys.begin(), keys.end(), time, [](float v, const Skeleton2DKey& k) { return v < k.time; });
    const Skeleton2DKey* key = &*(after - 1);
    if (after == keys.end()) return key;
    *next = &*after;
    *t = Shape(key->curve, (time - key->time) / (after->time - key->time));
    return key;
}

void SampleValues(const std::vector<Skeleton2DKey>& keys, float time, int count, float* out) {
    const Skeleton2DKey* next;
    float t;
    const Skeleton2DKey* key = Locate(keys, time, &next, &t);
    if (!key) return;
    for (int i = 0; i < count; ++i) out[i] = next ? Lerp(key->v[i], next->v[i], t) : key->v[i];
}

void ApplyBones(const Skeleton2DDef& def, const Skeleton2DClip& clip, float time, std::vector<Skeleton2DPose::Local>& bones) {
    for (const Skeleton2DBoneTimeline& timeline : clip.bones) {
        const Skeleton2DBone& setup = def.bones[static_cast<size_t>(timeline.bone)];
        Skeleton2DPose::Local& local = bones[static_cast<size_t>(timeline.bone)];
        float rotate = 0, translate[2] = {0, 0}, scale[2] = {1, 1};
        SampleValues(timeline.rotate, time, 1, &rotate);
        SampleValues(timeline.translate, time, 2, translate);
        SampleValues(timeline.scale, time, 2, scale);
        // Keys are relative to the setup pose: added angles and offsets, multiplied scales.
        local.rotation = setup.rotation + rotate;
        local.x = setup.x + translate[0];
        local.y = setup.y + translate[1];
        local.scaleX = setup.scaleX * scale[0];
        local.scaleY = setup.scaleY * scale[1];
    }
}

}  // namespace

int Skeleton2DDef::BoneIndex(const std::string& name) const {
    for (size_t i = 0; i < bones.size(); ++i) if (bones[i].name == name) return static_cast<int>(i);
    return -1;
}

int Skeleton2DDef::SlotIndex(const std::string& name) const {
    for (size_t i = 0; i < slots.size(); ++i) if (slots[i].name == name) return static_cast<int>(i);
    return -1;
}

const Skeleton2DClip* Skeleton2DDef::FindClip(const std::string& name) const {
    for (const Skeleton2DClip& clip : clips) if (clip.name == name) return &clip;
    return nullptr;
}

const Skeleton2DAttachment* Skeleton2DDef::FindAttachment(const std::string& skin, int slot, const std::string& name) const {
    for (const std::string& which : {skin, std::string("default")}) {
        auto s = skins.find(which);
        if (s == skins.end()) continue;
        auto byslot = s->second.find(slot);
        if (byslot == s->second.end()) continue;
        auto found = byslot->second.find(name);
        if (found != byslot->second.end()) return &found->second;
    }
    return nullptr;
}

std::shared_ptr<const Skeleton2DDef> ParseSkeleton2D(const Json& data, std::string* error) {
    auto def = std::make_shared<Skeleton2DDef>();
    try {
        OnlyFields(data, "skeleton", {"pixelsPerUnit", "pixelArt", "bones", "slots", "skins", "springs", "ik", "clips"});
        def->pixelsPerUnit = Number(data["pixelsPerUnit"], "pixelsPerUnit", 100, 0.01f, 100000);
        def->pixelArt = data["pixelArt"].asBool(false);
        const Json& bones = data["bones"];
        if (!bones.isArray() || bones.size() == 0) Fail("bones", "must list at least one bone (the root first)");
        if (bones.size() > Skeleton2DDef::kMaxBones) Fail("bones", "has more than 256 bones");
        for (size_t i = 0; i < bones.size(); ++i) {
            const std::string at = "bones[" + std::to_string(i) + "]";
            const Json& b = bones[static_cast<int>(i)];
            OnlyFields(b, at, {"name", "parent", "x", "y", "rotation", "scaleX", "scaleY", "length"});
            Skeleton2DBone bone;
            bone.name = Name(b["name"], at + ".name");
            if (def->BoneIndex(bone.name) >= 0) Fail(at + ".name", "bone '" + bone.name + "' is listed twice");
            if (b.has("parent")) {
                bone.parent = def->BoneIndex(Name(b["parent"], at + ".parent"));
                if (bone.parent < 0) Fail(at + ".parent", "'" + b["parent"].asString() + "' is not a bone listed before this one");
            } else if (i > 0) {
                Fail(at + ".parent", "only the first bone (the root) has no parent");
            }
            bone.x = Number(b["x"], at + ".x", 0);
            bone.y = Number(b["y"], at + ".y", 0);
            bone.rotation = Number(b["rotation"], at + ".rotation", 0);
            bone.scaleX = Number(b["scaleX"], at + ".scaleX", 1);
            bone.scaleY = Number(b["scaleY"], at + ".scaleY", 1);
            bone.length = Number(b["length"], at + ".length", 0, 0, 1e6f);
            def->bones.push_back(bone);
        }
        const Json& slots = data["slots"];
        if (!slots.isArray()) Fail("slots", "must list slots back to front");
        if (slots.size() > Skeleton2DDef::kMaxSlots) Fail("slots", "has more than 256 slots");
        for (size_t i = 0; i < slots.size(); ++i) {
            const std::string at = "slots[" + std::to_string(i) + "]";
            const Json& s = slots[static_cast<int>(i)];
            OnlyFields(s, at, {"name", "bone", "attachment", "color", "blend"});
            Skeleton2DSlot slot;
            slot.name = Name(s["name"], at + ".name");
            if (def->SlotIndex(slot.name) >= 0) Fail(at + ".name", "slot '" + slot.name + "' is listed twice");
            slot.bone = def->BoneIndex(Name(s["bone"], at + ".bone"));
            if (slot.bone < 0) Fail(at + ".bone", "unknown bone '" + s["bone"].asString() + "'");
            if (s.has("attachment") && !s["attachment"].isString() && !s["attachment"].isNull()) Fail(at + ".attachment", "must be an attachment name");
            slot.attachment = s["attachment"].asString("");
            if (s.has("color")) slot.color = ColorValue(s["color"], at + ".color");
            const std::string blend = s["blend"].asString("alpha");
            if (blend != "alpha" && blend != "add") Fail(at + ".blend", "must be alpha or add");
            slot.additive = blend == "add";
            def->slots.push_back(slot);
        }
        def->skins["default"];
        size_t attachments = 0;
        if (data.has("skins")) {
            if (!data["skins"].isObject()) Fail("skins", "must map skin names to {slot: {attachment: {...}}}");
            for (const auto& skin : data["skins"].members()) {
                const std::string at = "skins." + skin.first;
                Name(Json(skin.first), at);
                if (!skin.second.isObject()) Fail(at, "must map slot names to attachments");
                auto& out = def->skins[skin.first];
                for (const auto& slot : skin.second.members()) {
                    const int index = def->SlotIndex(slot.first);
                    if (index < 0) Fail(at + "." + slot.first, "unknown slot");
                    if (!slot.second.isObject()) Fail(at + "." + slot.first, "must map attachment names to attachments");
                    for (const auto& a : slot.second.members()) {
                        Name(Json(a.first), at + "." + slot.first);
                        if (++attachments > Skeleton2DDef::kMaxAttachments) Fail("skins", "has more than 1024 attachments");
                        out[index][a.first] = ParseAttachment(a.second, at + "." + slot.first + "." + a.first, a.first, *def);
                    }
                }
            }
        }
        for (size_t i = 0; i < def->slots.size(); ++i) {
            const std::string& name = def->slots[i].attachment;
            if (name.empty()) continue;
            bool found = false;
            for (const auto& skin : def->skins) found = found || def->FindAttachment(skin.first, static_cast<int>(i), name);
            if (!found) Fail("slots[" + std::to_string(i) + "].attachment", "no skin has attachment '" + name + "' in slot '" + def->slots[i].name + "'");
        }
        if (data.has("springs")) {
            const Json& list = data["springs"];
            if (!list.isArray() || list.size() > Skeleton2DDef::kMaxBones) Fail("springs", "must list {bone, stiffness, damping, inertia, gravity, limit}");
            for (size_t i = 0; i < list.size(); ++i) {
                const std::string at = "springs[" + std::to_string(i) + "]";
                const Json& s = list[static_cast<int>(i)];
                OnlyFields(s, at, {"bone", "stiffness", "damping", "inertia", "gravity", "limit"});
                Skeleton2DSpring spring;
                spring.bone = def->BoneIndex(Name(s["bone"], at + ".bone"));
                if (spring.bone <= 0) Fail(at + ".bone", "must be a bone other than the root");
                for (const Skeleton2DSpring& other : def->springs)
                    if (other.bone == spring.bone) Fail(at + ".bone", "already has a spring");
                spring.stiffness = Number(s["stiffness"], at + ".stiffness", 60, 0, 10000);
                spring.damping = Number(s["damping"], at + ".damping", 6, 0, 1000);
                spring.inertia = Number(s["inertia"], at + ".inertia", 1, 0, 100);
                spring.gravity = Number(s["gravity"], at + ".gravity", 0, -1000, 1000);
                spring.limit = Number(s["limit"], at + ".limit", 60, 0, 180);
                def->springs.push_back(spring);
            }
            std::sort(def->springs.begin(), def->springs.end(), [](const auto& a, const auto& b) { return a.bone < b.bone; });
        }
        if (data.has("ik")) {
            const Json& list = data["ik"];
            if (!list.isArray() || list.size() > Skeleton2DDef::kMaxBones) Fail("ik", "must list {name, bones, target, bend, mix}");
            for (size_t i = 0; i < list.size(); ++i) {
                const std::string at = "ik[" + std::to_string(i) + "]";
                const Json& c = list[static_cast<int>(i)];
                OnlyFields(c, at, {"name", "bones", "target", "bend", "mix"});
                Skeleton2DIk ik;
                ik.name = Name(c["name"], at + ".name");
                const Json& chain = c["bones"];
                if (!chain.isArray() || chain.size() < 1 || chain.size() > 2) Fail(at + ".bones", "must be one bone, or a parent and its child");
                for (const Json& b : chain.items()) {
                    const int index = b.isString() ? def->BoneIndex(b.asString()) : -1;
                    if (index <= 0) Fail(at + ".bones", "unknown bone " + b.dump() + " (the root cannot be solved)");
                    ik.bones.push_back(index);
                }
                if (ik.bones.size() == 2 && def->bones[static_cast<size_t>(ik.bones[1])].parent != ik.bones[0])
                    Fail(at + ".bones", "the second bone must be a child of the first");
                ik.target = def->BoneIndex(Name(c["target"], at + ".target"));
                if (ik.target < 0) Fail(at + ".target", "unknown bone '" + c["target"].asString() + "'");
                // The target must not move with the chain, or solving would chase itself.
                for (int b = ik.target; b >= 0; b = def->bones[static_cast<size_t>(b)].parent)
                    if (b == ik.bones[0]) Fail(at + ".target", "must not be the chain or below it (give it the root or the hip as parent)");
                const std::string bend = c["bend"].asString("positive");
                if (bend != "positive" && bend != "negative") Fail(at + ".bend", "must be positive or negative");
                ik.bendPositive = bend == "positive";
                ik.mix = Number(c["mix"], at + ".mix", 1, 0, 1);
                def->ik.push_back(ik);
            }
        }
        if (data.has("clips")) {
            if (!data["clips"].isObject()) Fail("clips", "must map clip names to clips");
            if (data["clips"].size() > Skeleton2DDef::kMaxClips) Fail("clips", "has more than 256 clips");
            for (const auto& c : data["clips"].members()) def->clips.push_back(ParseClip(c.second, "clips." + Name(Json(c.first), "clips"), c.first, *def));
        }
    } catch (const Invalid& ex) {
        if (error) *error = ex.what();
        return nullptr;
    }
    if (error) error->clear();
    return def;
}

Mat4 Skeleton2DLocalMatrix(float x, float y, float rotation, float scaleX, float scaleY) {
    return Mat4::Translation(Vec3(x, y, 0)) * Mat4::RotationZ(Radians(rotation)) * Mat4::Scale(Vec3(scaleX, scaleY, 1));
}

Skeleton2DPose Skeleton2DSetupPose(const Skeleton2DDef& def) {
    Skeleton2DPose pose;
    for (const Skeleton2DBone& b : def.bones) pose.bones.push_back({b.x, b.y, b.rotation, b.scaleX, b.scaleY});
    for (size_t i = 0; i < def.slots.size(); ++i) {
        pose.attachments.push_back(def.slots[i].attachment);
        pose.colors.push_back(def.slots[i].color);
        pose.drawOrder.push_back(static_cast<int>(i));
    }
    return pose;
}

float Skeleton2DClipTime(const Skeleton2DClip& clip, float time) {
    if (!std::isfinite(time)) return 0;
    if (clip.loop && clip.duration > 0) {
        float t = std::fmod(time, clip.duration);
        return t < 0 ? t + clip.duration : t;
    }
    return Clamp(time, 0, clip.duration);
}

Skeleton2DPose Skeleton2DSample(const Skeleton2DDef& def, const Skeleton2DClip* clip, float time,
                                const Skeleton2DClip* from, float fromTime, float weight) {
    Skeleton2DPose pose = Skeleton2DSetupPose(def);
    if (clip) {
        time = Skeleton2DClipTime(*clip, time);
        ApplyBones(def, *clip, time, pose.bones);
        if (from && weight < 1) {
            std::vector<Skeleton2DPose::Local> before = Skeleton2DSetupPose(def).bones;
            ApplyBones(def, *from, Skeleton2DClipTime(*from, fromTime), before);
            const float w = Clamp(weight, 0, 1);
            for (size_t i = 0; i < pose.bones.size(); ++i) {
                Skeleton2DPose::Local& b = pose.bones[i];
                const Skeleton2DPose::Local& a = before[i];
                b.x = Lerp(a.x, b.x, w);
                b.y = Lerp(a.y, b.y, w);
                b.rotation = Lerp(a.rotation, b.rotation, w);
                b.scaleX = Lerp(a.scaleX, b.scaleX, w);
                b.scaleY = Lerp(a.scaleY, b.scaleY, w);
            }
        }
        for (const Skeleton2DSlotTimeline& timeline : clip->slots) {
            const size_t s = static_cast<size_t>(timeline.slot);
            const Skeleton2DKey* next;
            float t;
            // Step timelines keep the setup value until their first key.
            if (const Skeleton2DKey* key = Locate(timeline.attachment, time, &next, &t))
                if (time >= key->time) pose.attachments[s] = key->text;
            if (!timeline.color.empty()) {
                float c[4] = {1, 1, 1, 1};
                SampleValues(timeline.color, time, 4, c);
                pose.colors[s] = Vec4(c[0], c[1], c[2], c[3]);
            }
        }
        const Skeleton2DKey* next;
        float t;
        if (const Skeleton2DKey* key = Locate(clip->drawOrder, time, &next, &t))
            if (time >= key->time) pose.drawOrder = key->order;
    }
    Skeleton2DWorld(def, pose);
    Skeleton2DSolveIk(def, pose);
    return pose;
}

Skeleton2DPose Skeleton2DComponentPose(const Skeleton2DDef& def, const Skeleton2D& component) {
    const Skeleton2DClip* from = component.from.empty() ? nullptr : def.FindClip(component.from);
    const float weight = from && component.mix > 0 ? Clamp(component.mixed / component.mix, 0, 1) : 1;
    // `time` belongs to the clip that played last; a clip set since then starts on the next step.
    const bool started = !component.current.empty() && component.currentAsset == component.asset;
    Skeleton2DPose pose = Skeleton2DSample(def, def.FindClip(started ? component.current : component.clip), component.time, from, component.fromTime, weight);
    // Springs swing on top of the clip (and the IK).
    bool swung = false;
    for (size_t s = 0; s < def.springs.size() && s < component.springAngle.size(); ++s)
        if (component.springAngle[s] != 0) {
            pose.bones[static_cast<size_t>(def.springs[s].bone)].rotation += component.springAngle[s];
            swung = true;
        }
    if (swung) Skeleton2DWorld(def, pose);
    return pose;
}

float Skeleton2DWorldAngle(const Mat4& world) { return Degrees(std::atan2(world.at(1, 0), world.at(0, 0))); }

void Skeleton2DSolveIk(const Skeleton2DDef& def, Skeleton2DPose& pose) {
    for (const Skeleton2DIk& ik : def.ik) {
        if (ik.mix <= 0) continue;
        const size_t a = static_cast<size_t>(ik.bones[0]);
        const Vec3 target = pose.world[static_cast<size_t>(ik.target)].TransformPoint(Vec3());
        const Vec3 pa = pose.world[a].TransformPoint(Vec3());
        const int parentA = def.bones[a].parent;
        const float parentAngle = parentA >= 0 ? Skeleton2DWorldAngle(pose.world[static_cast<size_t>(parentA)]) : 0;
        auto setLocal = [&](size_t bone, float worldAngle, float parentWorld) {
            // Blend from the clip's angle by the shorter way round.
            float& local = pose.bones[bone].rotation;
            float delta = worldAngle - parentWorld - local;
            delta = std::fmod(delta + 540.0f, 360.0f) - 180.0f;
            local += delta * ik.mix;
        };
        if (ik.bones.size() == 1) {
            setLocal(a, Degrees(std::atan2(target.y - pa.y, target.x - pa.x)), parentAngle);
            Skeleton2DWorld(def, pose);
            continue;
        }
        const size_t b = static_cast<size_t>(ik.bones[1]);
        const Vec3 pb = pose.world[b].TransformPoint(Vec3());
        const Vec3 tip = pose.world[b].TransformPoint(Vec3(std::max(1e-3f, def.bones[b].length), 0, 0));
        const float l1 = Length(pb - pa), l2 = Length(tip - pb);
        if (l1 < 1e-5f || l2 < 1e-5f) continue;
        // Law of cosines: the angle at the chain's root between the target line and the first bone.
        const float d = Clamp(Length(target - pa), std::fabs(l1 - l2) + 1e-4f, l1 + l2 - 1e-4f);
        const float cosine = Clamp((l1 * l1 + d * d - l2 * l2) / (2 * l1 * d), -1, 1);
        const float base = std::atan2(target.y - pa.y, target.x - pa.x);
        const float toB = base + (ik.bendPositive ? -1.0f : 1.0f) * std::acos(cosine);
        // The first bone's own axis is off the line to its child by this much.
        const float offset = std::atan2(pb.y - pa.y, pb.x - pa.x) - Radians(Skeleton2DWorldAngle(pose.world[a]));
        setLocal(a, Degrees(toB - offset), parentAngle);
        Skeleton2DWorld(def, pose);
        const Vec3 nb = pose.world[b].TransformPoint(Vec3());
        setLocal(b, Degrees(std::atan2(target.y - nb.y, target.x - nb.x)), Skeleton2DWorldAngle(pose.world[a]));
        Skeleton2DWorld(def, pose);
    }
}

void Skeleton2DWorld(const Skeleton2DDef& def, Skeleton2DPose& pose) {
    pose.world.assign(def.bones.size(), Mat4{});
    for (size_t i = 0; i < def.bones.size(); ++i) {
        const Skeleton2DPose::Local& b = pose.bones[i];
        const Mat4 local = Skeleton2DLocalMatrix(b.x, b.y, b.rotation, b.scaleX, b.scaleY);
        const int parent = def.bones[i].parent;
        pose.world[i] = parent >= 0 ? pose.world[static_cast<size_t>(parent)] * local : local;
    }
}

}  // namespace oe
