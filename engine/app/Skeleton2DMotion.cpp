#include "app/Skeleton2DMotion.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "api/Commands.h"
#include "app/Engine.h"
#include "assets/Assets.h"
#include "core/FileSystem.h"
#include "scene/Skeleton2D.h"

namespace oe {
namespace {

constexpr const char* kRecipes[] = {"idle", "walk", "run", "jump", "attack", "hit", "death"};

float Round3(float v) { return std::round(v * 1000.0f) / 1000.0f; }

// Writes clip keys for the recipe's bones, skipping roles the rig does not have.
class Writer {
public:
    Writer(const Skeleton2DDef& def, const Json& names, bool side) : def_(def), names_(names), side_(side) {
        setup_ = Skeleton2DSetupPose(def);
        Skeleton2DWorld(def, setup_);
        float lo = 1e30f, hi = -1e30f;
        for (size_t b = 0; b < def.bones.size(); ++b)
            for (float x : {0.0f, def.bones[b].length}) {
                const float y = setup_.world[b].TransformPoint(Vec3(x, 0, 0)).y;
                lo = std::min(lo, y), hi = std::max(hi, y);
            }
        height_ = std::max(0.01f, hi - lo);
    }
    // The rig's bone for a role, or "" when it has none.
    std::string Bone(const std::string& role) {
        const std::string name = names_.has(role) ? names_[role].asString() : role;
        if (def_.BoneIndex(name) > 0) return name;
        missing_.insert(role);
        return "";
    }
    // +1 or -1: the sign of a rotation that moves the bone's tip toward the world direction (dx, dy).
    float Toward(const std::string& bone, float dx, float dy) const {
        const Vec3 axis = setup_.world[static_cast<size_t>(def_.BoneIndex(bone))].TransformDir(Vec3(1, 0, 0));
        return (-axis.y * dx + axis.x * dy) >= 0 ? 1.0f : -1.0f;
    }
    // Rotation keys [t, degrees] times the direction sign; times and values in step.
    void Rotate(const std::string& role, float dx, float dy, const std::vector<float>& times, const std::vector<float>& degrees) {
        const std::string bone = Bone(role);
        if (bone.empty()) return;
        const float sign = Toward(bone, dx, dy);
        Json keys = Json::MakeArray();
        for (size_t i = 0; i < times.size(); ++i) {
            Json k = Json::MakeArray();
            k.push(Round3(times[i]));
            k.push(Round3(degrees[i] * sign));
            if (i + 1 < times.size()) k.push("smooth");
            keys.push(k);
        }
        clip_["bones"][bone]["rotate"] = keys;
    }
    // Rotation keys already signed (a turn that cancels another bone's).
    void RotateSigned(const std::string& role, const std::vector<float>& times, const std::vector<float>& degrees) {
        const std::string bone = Bone(role);
        if (bone.empty()) return;
        Json keys = Json::MakeArray();
        for (size_t i = 0; i < times.size(); ++i) {
            Json k = Json::MakeArray();
            k.push(Round3(times[i]));
            k.push(Round3(degrees[i]));
            if (i + 1 < times.size()) k.push("smooth");
            keys.push(k);
        }
        clip_["bones"][bone]["rotate"] = keys;
    }
    // Translation keys [t, x, y] in the bone's parent space (the hip hangs from the root: world axes).
    void Move(const std::string& role, const std::vector<float>& times, const std::vector<float>& xs, const std::vector<float>& ys) {
        const std::string bone = Bone(role);
        if (bone.empty()) return;
        Json keys = Json::MakeArray();
        for (size_t i = 0; i < times.size(); ++i) {
            Json k = Json::MakeArray();
            k.push(Round3(times[i]));
            k.push(Round3(xs[i]));
            k.push(Round3(ys[i]));
            if (i + 1 < times.size()) k.push("smooth");
            keys.push(k);
        }
        clip_["bones"][bone]["translate"] = keys;
    }
    void Event(float time, const std::string& name) {
        Json e = Json::MakeObject();
        e["time"] = Round3(time);
        e["name"] = name;
        if (!clip_.has("events")) clip_["events"] = Json::MakeArray();
        clip_["events"].push(e);
    }
    Json& Clip() { return clip_; }
    float Height() const { return height_; }
    // The length of a role's bone in world units (0 without one).
    float Length(const std::string& role) const {
        const std::string name = names_.has(role) ? names_[role].asString() : role;
        const int b = def_.BoneIndex(name);
        return b > 0 ? def_.bones[static_cast<size_t>(b)].length : 0;
    }
    bool Side() const { return side_; }
    const std::set<std::string>& Missing() const { return missing_; }

private:
    const Skeleton2DDef& def_;
    const Json& names_;
    bool side_;
    Skeleton2DPose setup_;
    float height_ = 1;
    Json clip_ = Json::MakeObject();
    std::set<std::string> missing_;
};

float Param(const Json& params, const char* name, float fallback, float lo, float hi) {
    const Json* v = params.isObject() ? params.find(name) : nullptr;
    if (!v) return fallback;
    if (!v->isNumber() || !std::isfinite(v->asNumber()) || v->asNumber() < lo || v->asNumber() > hi)
        throw ApiError("invalid_args", std::string("params.") + name + " must be a number in " + std::to_string(lo) + ".." + std::to_string(hi));
    return v->asFloat();
}

}  // namespace

Json Skeleton2DMotion(Engine& engine, const Json& args) {
    const std::string path = args["path"].asString();
    if (AssetManager::KindOf(path) != "skeleton2d") throw ApiError("invalid_path", "path must be a *.skel2d.json");
    std::string error;
    const auto def = engine.Assets().GetSkeleton2D(path, &error);
    if (!def) throw ApiError("invalid_skeleton", error, "skeleton2d.check {path} shows what is wrong.");
    const std::string recipe = args["recipe"].asString();
    if (std::find(std::begin(kRecipes), std::end(kRecipes), recipe) == std::end(kRecipes))
        throw ApiError("invalid_args", "unknown recipe '" + recipe + "'", "Recipes: idle, walk, run, jump, attack, hit, death.");
    const Json& params = args["params"];
    const Json& names = args["bones"].isObject() ? args["bones"] : Json::MakeObject();
    // A side view has front and back limbs; a front view left and right ones.
    bool side = def->BoneIndex(names.has("thigh_front") ? names["thigh_front"].asString() : "thigh_front") > 0;
    if (args.has("view")) {
        const std::string view = args["view"].asString();
        if (view != "front" && view != "side") throw ApiError("invalid_args", "view must be front or side");
        side = view == "side";
    }
    Writer w(*def, names, side);
    const std::string near = side ? "_front" : "_l", far = side ? "_back" : "_r";
    const float strength = Param(params, "strength", 1, 0, 3);
    const float H = w.Height();
    const float defaults[] = {2.4f, 1.0f, 0.65f, 1.0f, 0.6f, 0.4f, 1.2f};
    const float T = Param(params, "duration", defaults[std::find(std::begin(kRecipes), std::end(kRecipes), recipe) - std::begin(kRecipes)], 0.05f, 60);
    bool loop = true;
    // Directions: forward is +x (side views face right); outward is away from the body's middle (front views:
    // the character's left limbs are on the right of the picture).
    auto outward = [&](const std::string& suffix) { return suffix == "_l" ? 1.0f : -1.0f; };

    if (recipe == "idle") {
        const float b = Param(params, "breath", 0.006f, 0, 0.1f) * H * strength;
        w.Move("hip", {0, T * 0.5f, T}, {0, 0, 0}, {0, -b, 0});
        w.Rotate("chest", 1, 0, {0, T * 0.5f, T}, {0, 1.2f * strength, 0});
        w.Rotate("head", 1, 0, {0, T * 0.6f, T}, {0, -1.5f * strength, 0});
        for (const std::string& s : {near, far})
            w.Rotate("upper_arm" + s, side ? 1.0f : outward(s), 0, {0, T * 0.5f, T}, {0, 2.5f * strength, 0});
    } else if (recipe == "walk" || recipe == "run") {
        const bool run = recipe == "run";
        const float stride = Param(params, "stride", run ? 38.0f : 24.0f, 0, 90) * strength;
        const float knee = Param(params, "knee", run ? 80.0f : 40.0f, 0, 150) * strength;
        const float arm = Param(params, "arm", run ? 32.0f : 16.0f, 0, 120) * strength;
        const float b = Param(params, "bob", run ? 0.025f : 0.012f, 0, 0.2f) * H * strength;
        const float lean = Param(params, "lean", run ? 10.0f : 3.0f, -45, 45);
        if (side) {
            // Contact at 0 (front leg ahead) and T/2 (back leg ahead); the body is lowest at the contacts.
            // A leg swung by the stride reaches less far down: the body drops by that much at the contacts so the feet stay
            // on the ground (legs straight in the setup pose).
            const float leg = w.Length("thigh" + near) + w.Length("shin" + near);
            const float drop = run ? 0.0f : leg * (1 - std::cos(Radians(stride)));  // a run has a flight phase instead
            w.Move("hip", {0, T * 0.25f, T * 0.5f, T * 0.75f, T}, {0, 0, 0, 0, 0}, {-b - drop, b * 0.5f, -b - drop, b * 0.5f, -b - drop});
            // Four key poses per leg - contact (ahead), passing (under the body, straight), contact (behind), swing (passing
            // forward with the knee bent so the foot clears the ground) - the far leg half a cycle later. Each foot turns
            // back by its thigh and shin, so it stays flat.
            const std::vector<float> times = {0, T * 0.25f, T * 0.5f, T * 0.75f, T};
            for (const std::string& s : {near, far}) {
                const bool first = s == near;
                const std::vector<float> thigh = first ? std::vector<float>{stride, 0, -stride, knee * 0.5f, stride}
                                                       : std::vector<float>{-stride, knee * 0.5f, stride, 0, -stride};
                const std::vector<float> shin = first ? std::vector<float>{0, 0, 0, knee, 0} : std::vector<float>{0, knee, 0, 0, 0};
                w.Rotate("thigh" + s, 1, 0, times, thigh);
                w.Rotate("shin" + s, -1, 0, times, shin);  // the knee bends backward: the foot goes back
                const std::string thighBone = w.Bone("thigh" + s), shinBone = w.Bone("shin" + s);
                if (thighBone.empty() || shinBone.empty()) continue;
                std::vector<float> foot;
                for (size_t k = 0; k < times.size(); ++k) foot.push_back(-(w.Toward(thighBone, 1, 0) * thigh[k] + w.Toward(shinBone, -1, 0) * shin[k]));
                w.RotateSigned("foot" + s, times, foot);
            }
            w.Rotate("upper_arm" + near, 1, 0, {0, T * 0.5f, T}, {-arm, arm, -arm});
            w.Rotate("upper_arm" + far, 1, 0, {0, T * 0.5f, T}, {arm, -arm, arm});
            for (const std::string& s : {near, far}) w.Rotate("forearm" + s, 1, 0, {0, T}, {run ? 70.0f * strength : 12.0f * strength, run ? 70.0f * strength : 12.0f * strength});
            w.Rotate("chest", 1, 0, {0, T * 0.5f, T}, {-lean, -lean * 0.8f, -lean});
            w.Event(0, "step");
            w.Event(T * 0.5f, "step");
        } else {
            // Seen from the front: steps in place - each knee lifts in turn, the body bobs and sways, the arms swing a little.
            w.Move("hip", {0, T * 0.25f, T * 0.5f, T * 0.75f, T}, {0, 0, 0, 0, 0}, {0, -b, 0, -b, 0});
            w.Rotate("hip", 1, 0, {0, T * 0.25f, T * 0.75f, T}, {0, 1.5f * strength, -1.5f * strength, 0});
            const float lift = stride * 0.3f, bend = knee * 0.35f;
            w.Rotate("thigh" + near, outward(near), 0, {0, T * 0.25f, T * 0.5f, T}, {0, lift, 0, 0});
            w.Rotate("shin" + near, -outward(near), 0, {0, T * 0.25f, T * 0.5f, T}, {0, bend, 0, 0});
            w.Rotate("thigh" + far, outward(far), 0, {0, T * 0.5f, T * 0.75f, T}, {0, 0, lift, 0});
            w.Rotate("shin" + far, -outward(far), 0, {0, T * 0.5f, T * 0.75f, T}, {0, 0, bend, 0});
            w.Rotate("upper_arm" + near, outward(near), 0, {0, T * 0.5f, T}, {arm * 0.3f, -arm * 0.3f, arm * 0.3f});
            w.Rotate("upper_arm" + far, outward(far), 0, {0, T * 0.5f, T}, {-arm * 0.3f, arm * 0.3f, -arm * 0.3f});
            w.Event(T * 0.5f, "step");
            w.Event(T, "step");
        }
    } else if (recipe == "jump") {
        loop = false;
        const float crouch = 0.06f * H * strength, lift = Param(params, "lift", 0.25f, 0, 2) * H;
        const std::vector<float> t = {0, 0.2f * T, 0.35f * T, 0.6f * T, 0.85f * T, T};
        w.Move("hip", t, {0, 0, 0, 0, 0, 0}, {0, -crouch, lift * 0.45f, lift, -crouch * 0.8f, 0});
        for (const std::string& s : {near, far}) {
            const float out = side ? 1.0f : outward(s);
            w.Rotate("thigh" + s, out, 0, t, {0, 30 * strength, 0, 45 * strength, 25 * strength, 0});
            w.Rotate("shin" + s, -out, 0, t, {0, 55 * strength, 0, 70 * strength, 45 * strength, 0});
            // Arms swing back in the crouch and up on the way up.
            w.Rotate("upper_arm" + s, side ? 1.0f : out, 0, t, {0, -25 * strength, 110 * strength, 90 * strength, 20 * strength, 0});
        }
        w.Rotate("chest", 1, 0, t, {0, -8 * strength, 3 * strength, 0, -6 * strength, 0});
        w.Event(0.35f * T, "takeoff");
        w.Event(0.85f * T, "land");
    } else if (recipe == "attack") {
        loop = false;
        // params.hand: which arm strikes (l/r in a front view, front/back in a side view); default the near/right one.
        const std::string hand = params.isObject() && params.has("hand") ? "_" + params["hand"].asString() : (side ? near : far);
        if (w.Bone("upper_arm" + hand).empty()) throw ApiError("not_found", "no bone for upper_arm" + hand, side ? "params.hand is front or back." : "params.hand is l or r.");
        const float out = side ? 1.0f : outward(hand);
        const std::vector<float> t = {0, 0.3f * T, 0.5f * T, 0.65f * T, T};
        // Wind up (the arm goes back and up), strike through, hold, recover.
        w.Rotate("upper_arm" + hand, out, 0, t, {0, -70 * strength, 85 * strength, 80 * strength, 0});
        w.Rotate("forearm" + hand, 0, 1, t, {0, 45 * strength, 0, 0, 0});
        w.Rotate("chest", 1, 0, t, {0, 6 * strength, -10 * strength, -9 * strength, 0});
        w.Move("hip", t, {0, -0.01f * H * strength, 0.03f * H * strength, 0.03f * H * strength, 0}, {0, 0, 0, 0, 0});
        w.Event(0.5f * T, "hit");
    } else if (recipe == "hit") {
        loop = false;
        const std::vector<float> t = {0, 0.2f * T, T};
        w.Rotate("chest", -1, 0, t, {0, 12 * strength, 0});
        w.Rotate("head", -1, 0, t, {0, 10 * strength, 0});
        w.Move("hip", t, {0, -0.02f * H * strength, 0}, {0, 0, 0});
        for (const std::string& s : {near, far}) w.Rotate("upper_arm" + s, side ? -1.0f : outward(s), 0, t, {0, 10 * strength, 0});
    } else {  // death
        loop = false;
        const std::vector<float> t = {0, 0.25f * T, 0.7f * T, T};
        const float drop = 0.3f * H;
        w.Move("hip", t, {0, -0.02f * H, -0.05f * H, -0.06f * H}, {0, -0.02f * H, -drop, -drop});
        w.Rotate("hip", -1, 0, t, {0, 10, 70, 80});
        w.Rotate("chest", -1, 0, t, {0, 10, 15, 15});
        w.Rotate("head", -1, 0, t, {0, 15, 20, 25});
        for (const std::string& s : {near, far}) {
            const float out = side ? 1.0f : outward(s);
            w.Rotate("thigh" + s, out, 0, t, {0, 10, 60, 70});
            w.Rotate("shin" + s, -out, 0, t, {0, 20, 70, 80});
            w.Rotate("upper_arm" + s, side ? -1.0f : out, 0, t, {0, 30, 60, 70});
        }
        w.Event(T, "dead");
    }

    Json& clip = w.Clip();
    if (!clip.has("bones")) throw ApiError("not_found", "the skeleton has none of the bones the " + recipe + " recipe moves",
                                           "Map the recipe's roles to your bone names with bones: {hip: \"pelvis\", thigh_l: \"leg_upper_l\", ...}.");
    clip["duration"] = Round3(T);
    if (!loop) clip["loop"] = false;
    const std::string name = args["name"].asString(recipe);
    Json result = Json::MakeObject();
    result["clip"] = name;
    result["view"] = side ? "side" : "front";
    result["duration"] = Round3(T);
    result["loop"] = loop;
    result["bones"] = static_cast<int>(clip["bones"].size());
    Json missing = Json::MakeArray();
    for (const std::string& m : w.Missing()) missing.push(m);
    if (missing.size()) {
        result["missingRoles"] = missing;
        result["hint"] = "These roles have no bone of that name: map them with bones: {role: \"yourBone\"} if the rig has them.";
    }
    if (args["dryRun"].asBool()) {
        result["values"] = clip;
        return result;
    }
    // Into the asset, like skeleton2d.set {clip}: replaces a clip of that name.
    const std::string full = engine.ResolvePath(path);
    std::string text, parseError;
    if (!ReadTextFile(full, text)) throw ApiError("not_found", "cannot read " + path);
    Json data = Json::parse(text, &parseError);
    if (!parseError.empty()) throw ApiError("invalid_skeleton", parseError);
    if (!data["clips"].isObject()) data["clips"] = Json::MakeObject();
    data["clips"][name] = clip;
    if (!ParseSkeleton2D(data, &error)) throw ApiError("invalid_skeleton", error);
    if (!WriteTextFile(full, data.dump(2) + "\n")) throw ApiError("write_failed", "cannot write " + path);
    engine.Assets().Forget(path);
    result["next"] = "Look at it: skeleton2d.poses {path, clip: \"" + name + "\"}; check it: skeleton2d.analyze {path, clip: \"" + name +
                     "\"}; tune the numbers with params or the keys with skeleton2d.set.";
    return result;
}

}  // namespace oe
