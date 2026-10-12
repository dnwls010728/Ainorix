#include "scene/Retarget.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include "core/Image.h"
#include "scene/Pose.h"
#include "scene/Systems.h"

namespace oe {

namespace {

Vec3 PositionOf(const Mat4& m) { return Vec3(m.at(0, 3), m.at(1, 3), m.at(2, 3)); }

// The rotation of a node matrix: its axes without scale or translation.
Mat4 RotationOf(const Mat4& m) {
    Mat4 out;
    for (int col = 0; col < 3; ++col) {
        Vec3 axis(m.at(0, col), m.at(1, col), m.at(2, col));
        const float length = Length(axis);
        if (length > 1e-12f) axis = axis * (1.0f / length);
        out.at(0, col) = axis.x; out.at(1, col) = axis.y; out.at(2, col) = axis.z;
    }
    return out;
}

Mat4 AxisAngle(const Vec3& axis, float angle) {
    const float c = std::cos(angle), s = std::sin(angle), t = 1 - c;
    Mat4 r;
    r.at(0, 0) = t * axis.x * axis.x + c;          r.at(0, 1) = t * axis.x * axis.y - s * axis.z; r.at(0, 2) = t * axis.x * axis.z + s * axis.y;
    r.at(1, 0) = t * axis.x * axis.y + s * axis.z; r.at(1, 1) = t * axis.y * axis.y + c;          r.at(1, 2) = t * axis.y * axis.z - s * axis.x;
    r.at(2, 0) = t * axis.x * axis.z - s * axis.y; r.at(2, 1) = t * axis.y * axis.z + s * axis.x; r.at(2, 2) = t * axis.z * axis.z + c;
    return r;
}

// Shortest rotation that turns the direction `from` into `to`.
Mat4 Arc(const Vec3& from, const Vec3& to) {
    const float lengths = Length(from) * Length(to);
    if (lengths < 1e-12f) return Mat4();
    const Vec3 cross = Cross(from, to);
    const float sine = Length(cross), cosine = Dot(from, to);
    if (sine < 1e-6f * lengths) return Mat4();  // parallel (or opposite: no single shortest turn)
    return AxisAngle(cross * (1.0f / sine), std::atan2(sine, cosine));
}

Vec4 QuaternionOf(const Mat4& m) {
    const float trace = m.at(0, 0) + m.at(1, 1) + m.at(2, 2);
    Vec4 q;
    if (trace > 0) {
        const float s = std::sqrt(trace + 1) * 2;
        q = Vec4((m.at(2, 1) - m.at(1, 2)) / s, (m.at(0, 2) - m.at(2, 0)) / s, (m.at(1, 0) - m.at(0, 1)) / s, 0.25f * s);
    } else if (m.at(0, 0) > m.at(1, 1) && m.at(0, 0) > m.at(2, 2)) {
        const float s = std::sqrt(1 + m.at(0, 0) - m.at(1, 1) - m.at(2, 2)) * 2;
        q = Vec4(0.25f * s, (m.at(0, 1) + m.at(1, 0)) / s, (m.at(0, 2) + m.at(2, 0)) / s, (m.at(2, 1) - m.at(1, 2)) / s);
    } else if (m.at(1, 1) > m.at(2, 2)) {
        const float s = std::sqrt(1 + m.at(1, 1) - m.at(0, 0) - m.at(2, 2)) * 2;
        q = Vec4((m.at(0, 1) + m.at(1, 0)) / s, 0.25f * s, (m.at(1, 2) + m.at(2, 1)) / s, (m.at(0, 2) - m.at(2, 0)) / s);
    } else {
        const float s = std::sqrt(1 + m.at(2, 2) - m.at(0, 0) - m.at(1, 1)) * 2;
        q = Vec4((m.at(0, 2) + m.at(2, 0)) / s, (m.at(1, 2) + m.at(2, 1)) / s, 0.25f * s, (m.at(1, 0) - m.at(0, 1)) / s);
    }
    const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return length > 1e-12f ? q * (1.0f / length) : Vec4(0, 0, 0, 1);
}

bool IsBelow(const Mesh& mesh, int node, int ancestor) {
    for (size_t steps = 0; node >= 0 && steps <= mesh.nodes.size(); ++steps, node = mesh.nodes[static_cast<size_t>(node)].parent)
        if (node == ancestor) return true;
    return false;
}

}  // namespace

namespace {

// "mixamorig:LeftUpLeg", "Bip001 L Thigh", "thigh_l", "J_Bip_L_UpperLeg" -> "leftupleg", "lthigh", "thighl", "lupperleg".
std::string PlainBoneName(const std::string& name) {
    std::string plain;
    const size_t colon = name.rfind(':');
    for (size_t i = colon == std::string::npos ? 0 : colon + 1; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (std::isalnum(c)) plain += static_cast<char>(std::tolower(c));
    }
    for (const char* prefix : {"mixamorig", "bip001", "bip01", "bip002", "jbipc", "jbip", "bone", "def", "jnt"}) {
        const size_t length = std::strlen(prefix);
        if (plain.size() > length && plain.compare(0, length, prefix) == 0) { plain = plain.substr(length); break; }
    }
    return plain;
}

struct BodyPart {
    const char* part;
    bool sided;
    bool end;  // keeps its rest orientation (RetargetOptions::ends)
    std::vector<const char*> names;  // {s} = l / r, {side} = left / right
};

const std::vector<BodyPart>& BodyParts() {
    static const std::vector<BodyPart> parts = {
        {"hips", false, false, {"hips", "pelvis", "hip"}},
        {"spine", false, false, {"spine", "spine01", "spine1x"}},
        {"spine1", false, false, {"spine1", "spine02", "chest"}},
        {"spine2", false, false, {"spine2", "spine03", "upperchest"}},
        {"neck", false, false, {"neck", "neck01", "neck1"}},
        {"head", false, false, {"head"}},
        {"shoulder", true, false, {"{side}shoulder", "{s}clavicle", "clavicle{s}", "{s}shoulder", "shoulder{s}"}},
        {"upperArm", true, false, {"{side}arm", "{s}upperarm", "upperarm{s}", "{side}upperarm"}},
        {"forearm", true, false, {"{side}forearm", "{s}forearm", "forearm{s}", "lowerarm{s}", "{side}lowerarm", "{s}lowerarm"}},
        {"hand", true, false, {"{side}hand", "{s}hand", "hand{s}"}},
        {"thigh", true, false, {"{side}upleg", "{s}thigh", "thigh{s}", "{side}upperleg", "{s}upperleg", "upperleg{s}"}},
        {"calf", true, false, {"{side}leg", "{s}calf", "calf{s}", "{side}lowerleg", "{s}lowerleg", "lowerleg{s}", "shin{s}"}},
        {"foot", true, true, {"{side}foot", "{s}foot", "foot{s}"}},
        {"toe", true, true, {"{side}toebase", "{s}toe0", "ball{s}", "{side}toes", "{s}toebase", "{s}toe", "toe{s}"}},
    };
    return parts;
}

int FindBodyPart(const Mesh& mesh, const std::vector<std::string>& plain, const BodyPart& part, bool right) {
    for (const char* pattern : part.names) {
        std::string wanted = pattern;
        for (const auto& item : {std::pair<const char*, const char*>{"{side}", right ? "right" : "left"}, {"{s}", right ? "r" : "l"}}) {
            const size_t at = wanted.find(item.first);
            if (at != std::string::npos) wanted.replace(at, std::strlen(item.first), item.second);
        }
        for (size_t i = 0; i < plain.size(); ++i) if (plain[i] == wanted && !mesh.nodes[i].name.empty()) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace

Json GuessRetargetMap(const Mesh& source, const Mesh& target) {
    std::vector<std::string> sourceNames, targetNames;
    for (const ModelNode& node : source.nodes) sourceNames.push_back(PlainBoneName(node.name));
    for (const ModelNode& node : target.nodes) targetNames.push_back(PlainBoneName(node.name));
    Json out = Json::MakeObject();
    out["bones"] = Json::MakeObject();
    out["hips"] = "";
    out["ends"] = Json::MakeArray();
    out["unmatched"] = Json::MakeArray();
    for (const BodyPart& part : BodyParts()) {
        for (int side = 0; side < (part.sided ? 2 : 1); ++side) {
            const int s = FindBodyPart(source, sourceNames, part, side == 1), t = FindBodyPart(target, targetNames, part, side == 1);
            const std::string label = part.sided ? std::string(side ? "right " : "left ") + part.part : std::string(part.part);
            if (s < 0 || t < 0) { out["unmatched"].push(label); continue; }
            const std::string& bone = target.nodes[static_cast<size_t>(t)].name;
            out["bones"][bone] = source.nodes[static_cast<size_t>(s)].name;
            if (std::string(part.part) == "hips") out["hips"] = bone;
            if (part.end) out["ends"].push(bone);
        }
    }
    return out;
}

bool RetargetClips(const Mesh& source, const Mesh& target, const RetargetOptions& options, Json& gltf, Json& report, std::string* error) {
    auto fail = [&](const std::string& message) { if (error) *error = message; return false; };
    report = Json::MakeObject();
    report["missing"] = Json::MakeArray();
    // Mapped pairs: target node -> source node.
    std::vector<int> sourceOf(target.nodes.size(), -1);
    std::vector<int> mapped;
    for (const auto& pair : options.bones) {
        const int t = FindModelNode(target, pair.first), s = FindModelNode(source, pair.second);
        if (t < 0 || s < 0) {
            report["missing"].push((t < 0 ? "target: " + pair.first : "source: " + pair.second));
            continue;
        }
        if (target.nodes[static_cast<size_t>(t)].usesMatrix) { report["missing"].push("target (matrix node): " + pair.first); continue; }
        sourceOf[static_cast<size_t>(t)] = s;
        mapped.push_back(t);
    }
    if (mapped.empty()) return fail("no bone of the map exists in both models");
    std::sort(mapped.begin(), mapped.end());
    const int hips = FindModelNode(target, options.hips);
    if (!options.hips.empty() && (hips < 0 || sourceOf[static_cast<size_t>(hips)] < 0)) return fail("hips must be a mapped target bone");

    const std::vector<Mat4> sourceRest = EvaluateNodePose(source, nullptr, 0), targetRest = EvaluateNodePose(target, nullptr, 0);
    // The turn around the vertical that lays the source's rest skeleton over the target's (models that
    // face different ways): the best fit of the mapped bones' positions seen from above.
    Vec3 sourceCenter, targetCenter;
    for (int t : mapped) {
        sourceCenter = sourceCenter + PositionOf(sourceRest[static_cast<size_t>(sourceOf[static_cast<size_t>(t)])]);
        targetCenter = targetCenter + PositionOf(targetRest[static_cast<size_t>(t)]);
    }
    sourceCenter = sourceCenter * (1.0f / mapped.size());
    targetCenter = targetCenter * (1.0f / mapped.size());
    double sine = 0, cosine = 0;
    for (int t : mapped) {
        const Vec3 a = PositionOf(sourceRest[static_cast<size_t>(sourceOf[static_cast<size_t>(t)])]) - sourceCenter;
        const Vec3 b = PositionOf(targetRest[static_cast<size_t>(t)]) - targetCenter;
        cosine += a.x * b.x + a.z * b.z;
        sine += a.z * b.x - a.x * b.z;
    }
    const float yaw = static_cast<float>(std::atan2(sine, cosine));
    const Mat4 align = Mat4::RotationY(yaw);
    report["yaw"] = yaw * 57.29577951f;

    // Rest pose correction: the turn that lays each target bone along its source bone (a bone's
    // direction = towards its only mapped bone below; ends such as hands take the bone above's turn).
    // Bones listed in `ends` are treated as ends too: a foot's bone points wherever the rig's author put
    // the toe bone (straight down in some rigs, forward in others; heels change it again), and laying it
    // along the source's would tip the shoe.
    std::vector<char> isEnd(target.nodes.size(), 0);
    for (const std::string& name : options.ends) {
        const int node = FindModelNode(target, name);
        if (node >= 0) isEnd[static_cast<size_t>(node)] = 1;
        else report["missing"].push("target: " + name);
    }
    std::vector<Mat4> correction(target.nodes.size());
    for (int t : mapped) {
        if (isEnd[static_cast<size_t>(t)]) continue;
        int below = -1, count = 0;
        for (int other : mapped) {
            if (other == t || !IsBelow(target, other, t)) continue;
            bool nearest = true;  // no mapped bone between the two
            for (int node = target.nodes[static_cast<size_t>(other)].parent; node >= 0 && node != t; node = target.nodes[static_cast<size_t>(node)].parent)
                nearest = nearest && sourceOf[static_cast<size_t>(node)] < 0;
            if (nearest) below = other, ++count;
        }
        if (count != 1) continue;
        const Vec3 targetAlong = PositionOf(targetRest[static_cast<size_t>(below)]) - PositionOf(targetRest[static_cast<size_t>(t)]);
        const Vec3 sourceAlong = PositionOf(sourceRest[static_cast<size_t>(sourceOf[static_cast<size_t>(below)])]) -
                                 PositionOf(sourceRest[static_cast<size_t>(sourceOf[static_cast<size_t>(t)])]);
        correction[static_cast<size_t>(t)] = Arc(targetAlong, align.TransformDir(sourceAlong));
    }
    std::vector<char> corrected(target.nodes.size(), 0);
    for (int t : mapped) {
        bool hasBelow = false;
        for (int other : mapped) hasBelow = hasBelow || (other != t && IsBelow(target, other, t));
        if (hasBelow && !isEnd[static_cast<size_t>(t)]) continue;
        for (int node = target.nodes[static_cast<size_t>(t)].parent; node >= 0; node = target.nodes[static_cast<size_t>(node)].parent) {
            if (sourceOf[static_cast<size_t>(node)] < 0 || isEnd[static_cast<size_t>(node)]) continue;
            correction[static_cast<size_t>(t)] = correction[static_cast<size_t>(node)];
            break;
        }
    }
    (void)corrected;

    // Sizes: how much further the target's hips move for the same step.
    float scale = 1;
    if (hips >= 0) {
        const float sourceHeight = PositionOf(sourceRest[static_cast<size_t>(sourceOf[static_cast<size_t>(hips)])]).y;
        const float targetHeight = PositionOf(targetRest[static_cast<size_t>(hips)]).y;
        if (std::abs(sourceHeight) > 1e-4f && std::abs(targetHeight) > 1e-4f) scale = targetHeight / sourceHeight;
    }
    report["scale"] = scale;
    report["bones"] = static_cast<uint64_t>(mapped.size());

    // Parents before children.
    std::vector<int> order;
    std::vector<char> placed(target.nodes.size(), 0);
    for (size_t i = 0; i < target.nodes.size(); ++i) {
        std::vector<int> chain;
        for (int node = static_cast<int>(i); node >= 0 && !placed[static_cast<size_t>(node)]; node = target.nodes[static_cast<size_t>(node)].parent) chain.push_back(node);
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) order.push_back(*it), placed[static_cast<size_t>(*it)] = 1;
    }
    std::vector<Mat4> restLocal(target.nodes.size());
    for (size_t i = 0; i < target.nodes.size(); ++i) {
        const int parent = target.nodes[i].parent;
        restLocal[i] = parent < 0 ? RotationOf(targetRest[i]) : RotationOf(targetRest[static_cast<size_t>(parent)]).Transposed() * RotationOf(targetRest[i]);
    }

    gltf = Json::parse(R"J({"asset":{"version":"2.0","generator":"OwnEngine animation.retarget"},"scene":0,"scenes":[{"nodes":[]}],
      "nodes":[],"animations":[],"bufferViews":[],"accessors":[],"buffers":[{}]})J");
    std::vector<uint8_t> bin;
    auto accessor = [&](const std::vector<float>& values, int components, const char* type) {
        Json view = Json::MakeObject();
        view["buffer"] = 0;
        view["byteOffset"] = static_cast<uint64_t>(bin.size());
        view["byteLength"] = static_cast<uint64_t>(values.size() * 4);
        Json a = Json::MakeObject();
        a["bufferView"] = static_cast<uint64_t>(gltf["bufferViews"].size());
        a["componentType"] = 5126;
        a["count"] = static_cast<uint64_t>(values.size() / static_cast<size_t>(components));
        a["type"] = type;
        if (components == 1 && !values.empty()) {
            a["min"] = Json(Json::Array{values.front()});
            a["max"] = Json(Json::Array{values.back()});
        }
        gltf["bufferViews"].push(view);
        gltf["accessors"].push(a);
        const size_t at = bin.size();
        bin.resize(at + values.size() * 4);
        if (!values.empty()) std::memcpy(bin.data() + at, values.data(), values.size() * 4);
        return static_cast<int>(gltf["accessors"].size()) - 1;
    };
    std::vector<int> fileNode(target.nodes.size(), -1);
    for (int t : mapped) {
        Json node = Json::MakeObject();
        node["name"] = target.nodes[static_cast<size_t>(t)].name;
        fileNode[static_cast<size_t>(t)] = static_cast<int>(gltf["nodes"].size());
        gltf["scenes"][0]["nodes"].push(static_cast<uint64_t>(gltf["nodes"].size()));
        gltf["nodes"].push(node);
    }

    std::vector<std::pair<std::string, const AnimationClip*>> clips;
    if (options.clips.empty()) {
        for (const AnimationClip& clip : source.clips) clips.emplace_back(clip.name, &clip);
    } else {
        for (const auto& pair : options.clips) {
            const AnimationClip* clip = FindAnimationClip(source, pair.second);
            if (!clip) report["missing"].push("clip: " + pair.second);
            else clips.emplace_back(pair.first, clip);
        }
    }
    if (clips.empty()) return fail("the source has none of the clips");
    const float rate = std::isfinite(options.rate) ? std::max(1.0f, std::min(options.rate, 120.0f)) : 30.0f;
    report["clips"] = Json::MakeArray();
    const Mat4 unalign = align.Transposed();
    for (const auto& item : clips) {
        const AnimationClip& clip = *item.second;
        const int keys = std::max(2, static_cast<int>(std::ceil(clip.duration * rate)) + 1);
        std::vector<float> times(static_cast<size_t>(keys));
        std::vector<std::vector<float>> rotations(mapped.size());
        std::vector<float> movement;
        std::vector<Vec4> last(mapped.size(), Vec4(0, 0, 0, 1));
        for (int k = 0; k < keys; ++k) {
            const float time = clip.duration * static_cast<float>(k) / static_cast<float>(keys - 1);
            times[static_cast<size_t>(k)] = time;
            const std::vector<Mat4> pose = EvaluateNodePose(source, &clip, time);
            std::vector<Mat4> world(target.nodes.size());
            for (int node : order) {
                const size_t n = static_cast<size_t>(node);
                const int parent = target.nodes[n].parent;
                if (sourceOf[n] >= 0) {
                    // How far the source bone has turned from its rest, in the target's axes, on top of the
                    // target's own rest orientation laid along the source's.
                    const size_t s = static_cast<size_t>(sourceOf[n]);
                    const Mat4 turned = align * RotationOf(pose[s]) * RotationOf(sourceRest[s]).Transposed() * unalign;
                    world[n] = turned * correction[n] * RotationOf(targetRest[n]);
                } else {
                    world[n] = parent < 0 ? restLocal[n] : world[static_cast<size_t>(parent)] * restLocal[n];
                }
            }
            for (size_t i = 0; i < mapped.size(); ++i) {
                const size_t n = static_cast<size_t>(mapped[i]);
                const int parent = target.nodes[n].parent;
                Vec4 q = QuaternionOf(parent < 0 ? world[n] : world[static_cast<size_t>(parent)].Transposed() * world[n]);
                if (k > 0 && q.x * last[i].x + q.y * last[i].y + q.z * last[i].z + q.w * last[i].w < 0) q = q * -1;  // keys stay on one side
                last[i] = q;
                rotations[i].insert(rotations[i].end(), {q.x, q.y, q.z, q.w});
            }
            if (hips >= 0) {
                const size_t s = static_cast<size_t>(sourceOf[static_cast<size_t>(hips)]);
                const Vec3 moved = align.TransformDir(PositionOf(pose[s]) - PositionOf(sourceRest[s])) * scale;
                const int parent = target.nodes[static_cast<size_t>(hips)].parent;
                const Vec3 local = parent < 0 ? moved : targetRest[static_cast<size_t>(parent)].Inverse().TransformDir(moved);
                const Vec3 at = target.nodes[static_cast<size_t>(hips)].translation + local;
                movement.insert(movement.end(), {at.x, at.y, at.z});
            }
        }
        Json animation = Json::MakeObject();
        animation["name"] = item.first;
        animation["samplers"] = Json::MakeArray();
        animation["channels"] = Json::MakeArray();
        const int input = accessor(times, 1, "SCALAR");
        auto channel = [&](int node, const char* path, int output) {
            Json sampler = Json::MakeObject();
            sampler["input"] = input;
            sampler["output"] = output;
            sampler["interpolation"] = "LINEAR";
            Json entry = Json::MakeObject(), to = Json::MakeObject();
            to["node"] = node;
            to["path"] = path;
            entry["sampler"] = static_cast<uint64_t>(animation["samplers"].size());
            entry["target"] = to;
            animation["samplers"].push(sampler);
            animation["channels"].push(entry);
        };
        for (size_t i = 0; i < mapped.size(); ++i) channel(fileNode[static_cast<size_t>(mapped[i])], "rotation", accessor(rotations[i], 4, "VEC4"));
        if (hips >= 0) channel(fileNode[static_cast<size_t>(hips)], "translation", accessor(movement, 3, "VEC3"));
        gltf["animations"].push(animation);
        Json entry = Json::MakeObject();
        entry["name"] = item.first;
        entry["duration"] = clip.duration;
        entry["keys"] = keys;
        report["clips"].push(entry);
    }
    gltf["buffers"][0]["byteLength"] = static_cast<uint64_t>(bin.size());
    gltf["buffers"][0]["uri"] = "data:application/octet-stream;base64," + Base64Encode(bin);
    return true;
}

}  // namespace oe
