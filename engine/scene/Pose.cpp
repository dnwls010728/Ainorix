#include "scene/Pose.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

#include "assets/Assets.h"
#include "scene/Components.h"
#include "scene/Systems.h"

namespace oe {

namespace {

constexpr int kMaxChain = 8;
constexpr int kDescentPasses = 16;

Vec3 PositionOf(const Mat4& m) { return Vec3(m.at(0, 3), m.at(1, 3), m.at(2, 3)); }

// The rotation of a node matrix: its axes without scale or translation.
Mat4 RotationOf(const Mat4& m) {
    Mat4 out;
    for (int col = 0; col < 3; ++col) {
        Vec3 axis(m.at(0, col), m.at(1, col), m.at(2, col));
        float length = Length(axis);
        if (length > 1e-12f) axis = axis * (1.0f / length);
        out.at(0, col) = axis.x; out.at(1, col) = axis.y; out.at(2, col) = axis.z;
    }
    return out;
}

// Rodrigues' rotation about a unit axis.
Mat4 AxisAngle(const Vec3& axis, float angle) {
    const float c = std::cos(angle), s = std::sin(angle), t = 1 - c;
    Mat4 r;
    r.at(0, 0) = t * axis.x * axis.x + c;          r.at(0, 1) = t * axis.x * axis.y - s * axis.z; r.at(0, 2) = t * axis.x * axis.z + s * axis.y;
    r.at(1, 0) = t * axis.x * axis.y + s * axis.z; r.at(1, 1) = t * axis.y * axis.y + c;          r.at(1, 2) = t * axis.y * axis.z - s * axis.x;
    r.at(2, 0) = t * axis.x * axis.z - s * axis.y; r.at(2, 1) = t * axis.y * axis.z + s * axis.x; r.at(2, 2) = t * axis.z * axis.z + c;
    return r;
}

// Any unit vector at a right angle to `v`.
Vec3 Perpendicular(const Vec3& v) {
    Vec3 side = Cross(v, std::abs(v.y) < 0.9f * Length(v) ? Vec3(0, 1, 0) : Vec3(1, 0, 0));
    return Normalize(side);
}

// Shortest rotation that turns the direction `from` into `to`.
Mat4 Arc(const Vec3& from, const Vec3& to) {
    const float lengths = Length(from) * Length(to);
    if (lengths < 1e-12f) return Mat4();
    const Vec3 cross = Cross(from, to);
    const float sine = Length(cross), cosine = Dot(from, to);
    if (sine < 1e-7f * lengths) return cosine >= 0 ? Mat4() : AxisAngle(Perpendicular(from), kPi);
    return AxisAngle(cross * (1.0f / sine), std::atan2(sine, cosine));
}

// Applies a model-space rotation about `pivot` to `root` and every node below it.
void TurnSubtree(const Mesh& mesh, std::vector<Mat4>& nodes, int root, const Vec3& pivot, const Mat4& rotation) {
    const Mat4 turn = Mat4::Translation(pivot) * rotation * Mat4::Translation(pivot * -1.0f);
    for (size_t i = 0; i < nodes.size(); ++i) {
        int node = static_cast<int>(i);
        for (size_t steps = 0; node >= 0 && node != root && steps <= nodes.size(); ++steps) node = mesh.nodes[static_cast<size_t>(node)].parent;
        if (node == root) nodes[i] = turn * nodes[i];
    }
}

// The side a limb bends to in the model's default pose, as an axis fixed to the middle bone:
// carried into the current pose it tells an elbow from a hyperextended one. Zero when the default
// pose is straight (a T-pose arm), which leaves the choice to the pole or the posed limb.
Vec3 RestHinge(const std::vector<Mat4>& rest, const std::vector<Mat4>& nodes, int upper, int middle, int end) {
    const Vec3 b = PositionOf(rest[static_cast<size_t>(middle)]);
    const Vec3 toUpper = PositionOf(rest[static_cast<size_t>(upper)]) - b, toEnd = PositionOf(rest[static_cast<size_t>(end)]) - b;
    const Vec3 hinge = Cross(toUpper, toEnd);
    if (Length(hinge) < 0.02f * Length(toUpper) * Length(toEnd)) return Vec3();
    const Vec3 local = RotationOf(rest[static_cast<size_t>(middle)]).Transposed().TransformDir(Normalize(hinge));
    return RotationOf(nodes[static_cast<size_t>(middle)]).TransformDir(local);
}

// Upper joint a, middle joint b, end c: sets the angle at b from the reach, swings a onto the
// target, then rolls the middle joint towards the pole. `restHinge` (may be zero) keeps the bend
// on the side the model bends by itself while the clip passes through a straight limb.
void SolveTwoBone(const Mesh& mesh, std::vector<Mat4>& nodes, int upper, int middle, int end, const Vec3& target, const Vec3& pole,
                  const Vec3& restHinge, float soften) {
    const Vec3 a = PositionOf(nodes[static_cast<size_t>(upper)]);
    const Vec3 b = PositionOf(nodes[static_cast<size_t>(middle)]);
    Vec3 c = PositionOf(nodes[static_cast<size_t>(end)]);
    const float upperLength = Length(b - a), lowerLength = Length(c - b);
    if (upperLength < 1e-6f || lowerLength < 1e-6f) return;
    const float margin = 1e-4f * (upperLength + lowerLength);
    const float nearest = std::abs(upperLength - lowerLength) + margin, farthest = upperLength + lowerLength - margin;
    if (nearest >= farthest) return;
    float reach = Length(target - a);
    // Near full extension a small change of the reach moves the middle joint a lot, and a goal that
    // leaves the reach snaps the limb straight. Inside the soft zone the reach approaches the full
    // length exponentially instead (same value and slope where the zone begins).
    const float zone = Clamp(std::isfinite(soften) ? soften : 0.0f, 0.0f, 0.5f) * (upperLength + lowerLength);
    if (zone > 0 && reach > upperLength + lowerLength - zone)
        reach = upperLength + lowerLength - zone * std::exp(-(reach - (upperLength + lowerLength - zone)) / zone);
    reach = Clamp(reach, nearest, farthest);
    const Vec3 toUpper = (a - b) * (1.0f / upperLength), toEnd = (c - b) * (1.0f / lowerLength);
    const float wanted = std::acos(Clamp((upperLength * upperLength + lowerLength * lowerLength - reach * reach) / (2 * upperLength * lowerLength), -1.0f, 1.0f));
    float current = std::acos(Clamp(Dot(toUpper, toEnd), -1.0f, 1.0f));
    Vec3 hinge = Cross(toUpper, toEnd);
    // Which way is "bent": the pole's side, else the model's own. Without one of them the sign of the
    // posed bend decides, and a clip that straightens the limb would flip the joint from frame to frame.
    const Vec3 side = Length(pole) > 1e-6f ? Cross(toEnd, pole) : restHinge;
    if (Length(hinge) < 1e-5f) {  // a straight limb has no bend plane yet
        hinge = Length(side) > 1e-5f ? side - toEnd * Dot(side, toEnd) : Vec3();
        if (Length(hinge) < 1e-5f) hinge = Perpendicular(toEnd);
        current = kPi;
    } else if (Dot(hinge, side) < 0) {  // bent the other way: measured about the wanted axis that is a negative angle
        hinge = hinge * -1.0f;
        current = -current;
    }
    TurnSubtree(mesh, nodes, middle, b, AxisAngle(Normalize(hinge), wanted - current));
    c = PositionOf(nodes[static_cast<size_t>(end)]);
    TurnSubtree(mesh, nodes, upper, a, Arc(c - a, target - a));
    if (Length(pole) < 1e-6f) return;
    c = PositionOf(nodes[static_cast<size_t>(end)]);
    if (Length(c - a) < 1e-6f) return;
    const Vec3 axis = Normalize(c - a);
    const Vec3 elbow = PositionOf(nodes[static_cast<size_t>(middle)]) - a;
    const Vec3 elbowSide = elbow - axis * Dot(elbow, axis), poleSide = pole - axis * Dot(pole, axis);
    if (Length(elbowSide) < 1e-6f || Length(poleSide) < 1e-6f) return;
    const float roll = std::atan2(Dot(Cross(elbowSide, poleSide), axis), Dot(elbowSide, poleSide));
    TurnSubtree(mesh, nodes, upper, a, AxisAngle(axis, roll));
}

bool Finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// A bone that swings and the bone below it whose position is the swinging end.
struct SpringJoint {
    int node;
    int end;
};

// The bones a SpringBone turns: `root` and every node below it that has a node below itself,
// parents first. A bone with several children swings by its first one; the others ride along.
std::vector<SpringJoint> SpringChain(const Mesh& mesh, int root) {
    std::vector<SpringJoint> joints;
    if (root < 0) return joints;
    std::vector<int> order{root};
    for (size_t at = 0; at < order.size() && order.size() <= mesh.nodes.size(); ++at) {
        int first = -1;
        for (size_t i = 0; i < mesh.nodes.size(); ++i) {
            if (mesh.nodes[i].parent != order[at]) continue;
            if (first < 0) first = static_cast<int>(i);
            order.push_back(static_cast<int>(i));
        }
        if (first >= 0) joints.push_back({order[at], first});
    }
    return joints;
}

// A jump of the animated end further than this in one step (a respawn, a scene cut) restarts the
// bone at rest instead of whipping it across the level. Meters, on top of what a swing can cover.
constexpr float kSpringTeleport = 2.0f;

// Turns the bones of one SpringBone inside `nodes` (model space) towards their simulated ends.
// With `step` (the same component, mutable) the ends first advance by `dt`: Verlet integration in
// world space, so a model that starts, stops or turns leaves them behind; a pull towards where the
// pose puts the end under its already swung parent, damped by the speed relative to that place (a
// model moving steadily drags nothing); then the bone length is restored. Without it the stored ends are
// replayed, which is what drawing does. Both walk the chain the same way and so give the same pose.
void SwingSpring(const Mesh& mesh, std::vector<Mat4>& nodes, const Mat4& world, const SpringBone& spring, SpringBone* step, float dt) {
    const std::vector<SpringJoint> joints = SpringChain(mesh, FindModelNode(mesh, spring.bone));
    if (joints.empty()) return;
    const bool stored = spring.simulated == spring.bone && spring.tails.size() == joints.size() && spring.previous.size() == joints.size() &&
                        spring.rests.size() == joints.size();
    if (!step && !stored) return;
    const Mat4 inverse = world.Inverse();
    const float weight = std::isfinite(spring.weight) ? Clamp(spring.weight, 0.0f, 1.0f) : 0.0f;
    const float pull = Clamp(std::isfinite(spring.stiffness) ? spring.stiffness * dt * dt : 0.0f, 0.0f, 1.0f);
    const float keep = std::exp(-Clamp(std::isfinite(spring.damping) ? spring.damping : 0.0f, 0.0f, 60.0f) * dt);
    const Vec3 fall = Finite(spring.gravity) ? spring.gravity * (dt * dt) : Vec3();
    if (step && !stored) {
        step->tails.assign(joints.size(), Vec3());
        step->previous.assign(joints.size(), Vec3());
        step->rests.assign(joints.size(), Vec3());
        step->simulated = spring.bone;
    }
    for (size_t i = 0; i < joints.size(); ++i) {
        const Vec3 head = PositionOf(nodes[static_cast<size_t>(joints[i].node)]);
        const Vec3 rest = PositionOf(nodes[static_cast<size_t>(joints[i].end)]);
        const Vec3 worldHead = world.TransformPoint(head), worldRest = world.TransformPoint(rest);
        const float length = Length(worldRest - worldHead);
        if (!Finite(worldHead) || !Finite(worldRest)) continue;
        Vec3 tail = stored ? spring.tails[i] : worldRest;
        if (step) {
            Vec3 before = stored ? spring.previous[i] : worldRest, carried = stored ? spring.rests[i] : worldRest;
            if (!Finite(tail) || !Finite(before) || !Finite(carried) || Length(worldRest - tail) > 2 * length + kSpringTeleport)
                tail = before = carried = worldRest;
            // The pull is measured against where the pose had the end when `tail` was taken: pulling
            // towards this step's place as well would add the pose's own speed to the swing.
            Vec3 next = tail + (tail - before) * keep + (worldRest - carried) * (1 - keep) + (carried - tail) * pull + fall;
            const float reach = Length(next - worldHead);
            next = reach > 1e-9f && Finite(next) ? worldHead + (next - worldHead) * (length / reach) : worldRest;
            step->previous[i] = tail;
            step->rests[i] = worldRest;
            step->tails[i] = tail = next;
        }
        if (length < 1e-9f || weight <= 0) continue;
        const Vec3 to = inverse.TransformPoint(worldRest + (tail - worldRest) * weight) - head;
        if (!Finite(to)) continue;
        TurnSubtree(mesh, nodes, joints[i].node, head, Arc(rest - head, to));
    }
}

}  // namespace

int FindModelNode(const Mesh& mesh, const std::string& name) {
    if (name.empty()) return -1;
    for (size_t i = 0; i < mesh.nodes.size(); ++i) if (mesh.nodes[i].name == name) return static_cast<int>(i);
    return -1;
}

void SolveIk(const Mesh& mesh, std::vector<Mat4>& nodes, const IkGoal& goal, const std::vector<Mat4>* rest) {
    if (goal.node < 0 || static_cast<size_t>(goal.node) >= nodes.size() || nodes.size() != mesh.nodes.size()) return;
    const float weight = std::isfinite(goal.weight) ? Clamp(goal.weight, 0.0f, 1.0f) : 0.0f;
    if (weight <= 0) return;
    const int chain = std::max(1, std::min(goal.chain, kMaxChain));
    int joints[kMaxChain];  // from the end's parent upwards
    int count = 0;
    for (int node = mesh.nodes[static_cast<size_t>(goal.node)].parent; node >= 0 && count < chain; node = mesh.nodes[static_cast<size_t>(node)].parent)
        joints[count++] = node;
    if (count < chain) return;
    const size_t end = static_cast<size_t>(goal.node);
    const Vec3 start = PositionOf(nodes[end]);
    const Vec3 target = start + (goal.target - start) * weight;
    if (!std::isfinite(target.x) || !std::isfinite(target.y) || !std::isfinite(target.z)) return;
    if (count == 2) {
        const Vec3 restHinge = rest && rest->size() == nodes.size() ? RestHinge(*rest, nodes, joints[1], joints[0], goal.node) : Vec3();
        SolveTwoBone(mesh, nodes, joints[1], joints[0], goal.node, target, goal.pole, restHinge, goal.soften);
    } else {
        for (int pass = 0; pass < kDescentPasses; ++pass) {
            for (int i = 0; i < count; ++i) {
                const Vec3 pivot = PositionOf(nodes[static_cast<size_t>(joints[i])]);
                TurnSubtree(mesh, nodes, joints[i], pivot, Arc(PositionOf(nodes[end]) - pivot, target - pivot));
            }
            if (Length(PositionOf(nodes[end]) - target) < 1e-4f) break;
        }
    }
    if (!goal.matchRotation) return;
    // Rotation that takes the node's orientation to the wanted one, scaled by the weight as an axis and angle.
    const Mat4 delta = RotationOf(goal.rotation) * RotationOf(nodes[end]).Transposed();
    const float angle = std::acos(Clamp((delta.at(0, 0) + delta.at(1, 1) + delta.at(2, 2) - 1) * 0.5f, -1.0f, 1.0f));
    const Vec3 axis(delta.at(2, 1) - delta.at(1, 2), delta.at(0, 2) - delta.at(2, 0), delta.at(1, 0) - delta.at(0, 1));
    if (angle < 1e-5f) return;
    const Vec3 pivot = PositionOf(nodes[end]);
    if (weight >= 1 || Length(axis) < 1e-4f) {  // a half turn has no stable axis: it is not blended
        if (weight >= 0.5f) TurnSubtree(mesh, nodes, goal.node, pivot, delta);
    } else {
        TurnSubtree(mesh, nodes, goal.node, pivot, AxisAngle(Normalize(axis), angle * weight));
    }
}

namespace {

// The BoneIK goals among the direct children of `id`, in entity id order, applied to `nodes`.
void ApplyGoals(const Scene& scene, EntityId id, const Mesh& mesh, std::vector<Mat4>& nodes) {
    std::vector<Mat4> rest;  // default pose, evaluated once when a goal asks for it
    for (const auto& kv : scene.Pool<BoneIK>()) {
        const EntityRecord* record = scene.Record(kv.first);
        const Transform* transform = scene.Get<Transform>(kv.first);
        if (!record || record->parent != id || !transform) continue;
        IkGoal goal;
        goal.node = FindModelNode(mesh, kv.second.bone);
        goal.chain = kv.second.chain;
        goal.target = transform->position;  // a direct child's Transform is in model space
        goal.weight = kv.second.weight;
        goal.pole = kv.second.pole;
        goal.soften = kv.second.soften;
        goal.matchRotation = kv.second.matchRotation;
        goal.rotation = Mat4::RotationEuler(transform->rotation);
        if (rest.empty()) rest = EvaluateNodePose(mesh, nullptr, 0);
        SolveIk(mesh, nodes, goal, &rest);
    }
}

}  // namespace

std::vector<Mat4> EvaluateEntityPose(const Scene& scene, EntityId id, const Mesh& mesh, bool ik) {
    const Animator* animator = scene.Get<Animator>(id);
    const AnimationClip* clip = animator ? FindAnimationClip(mesh, animator->clip) : nullptr;
    std::vector<Mat4> nodes = EvaluateNodePose(mesh, clip, animator ? animator->time : 0);
    if (!ik) return nodes;
    ApplyGoals(scene, id, mesh, nodes);
    for (const auto& kv : scene.Pool<SpringBone>()) {  // id order, as they were stepped
        const EntityRecord* record = scene.Record(kv.first);
        if (!record || record->parent != id || kv.second.tails.empty()) continue;
        SwingSpring(mesh, nodes, scene.WorldMatrix(id), kv.second, nullptr, 0);
    }
    return nodes;
}

int SpringBoneCount(const Mesh& mesh, const std::string& bone) {
    return static_cast<int>(SpringChain(mesh, FindModelNode(mesh, bone)).size());
}

void UpdateSpringBones(Scene& scene, AssetManager& assets, float dt) {
    if (!(dt > 0)) return;
    std::map<EntityId, std::pair<std::shared_ptr<const Mesh>, std::vector<Mat4>>> poses;  // clip + IK, then the springs stepped so far
    for (auto& kv : scene.Pool<SpringBone>()) {
        const EntityRecord* record = scene.Record(kv.first);
        const MeshRenderer* renderer = record ? scene.Get<MeshRenderer>(record->parent) : nullptr;
        if (!renderer) continue;
        auto found = poses.find(record->parent);
        if (found == poses.end()) {
            std::shared_ptr<const Mesh> mesh = assets.GetMesh(renderer->mesh);
            std::vector<Mat4> nodes;
            if (mesh) nodes = EvaluateEntityPose(scene, record->parent, *mesh, false);
            found = poses.emplace(record->parent, std::make_pair(std::move(mesh), std::move(nodes))).first;
            // The goals, without the springs: those are added below, one step further each.
            if (found->second.first) ApplyGoals(scene, record->parent, *found->second.first, found->second.second);
        }
        if (!found->second.first) continue;
        SwingSpring(*found->second.first, found->second.second, scene.WorldMatrix(record->parent), kv.second, &kv.second, dt);
    }
}

void UpdateBoneAttachments(Scene& scene, AssetManager& assets) {
    std::map<EntityId, std::pair<std::shared_ptr<const Mesh>, std::vector<Mat4>>> poses;  // one evaluation per model entity
    for (auto& kv : scene.Pool<BoneAttachment>()) {
        const EntityRecord* record = scene.Record(kv.first);
        Transform* transform = scene.Get<Transform>(kv.first);
        const MeshRenderer* renderer = record ? scene.Get<MeshRenderer>(record->parent) : nullptr;
        if (!transform || !renderer) continue;
        auto found = poses.find(record->parent);
        if (found == poses.end()) {
            std::shared_ptr<const Mesh> mesh = assets.GetMesh(renderer->mesh);
            std::vector<Mat4> nodes;
            if (mesh) nodes = EvaluateEntityPose(scene, record->parent, *mesh);
            found = poses.emplace(record->parent, std::make_pair(std::move(mesh), std::move(nodes))).first;
        }
        const Mesh* mesh = found->second.first.get();
        const int node = mesh ? FindModelNode(*mesh, kv.second.bone) : -1;
        if (node < 0) continue;
        const Mat4& bone = found->second.second[static_cast<size_t>(node)];
        const Mat4 local = Mat4::Translation(PositionOf(bone)) * RotationOf(bone) * Mat4::Translation(kv.second.position) *
                           Mat4::RotationEuler(kv.second.rotation);
        Vec3 position, rotation, scale;
        DecomposeTRS(local, position, rotation, scale);
        if (!std::isfinite(position.x + position.y + position.z + rotation.x + rotation.y + rotation.z)) continue;
        transform->position = position;
        transform->rotation = rotation;
    }
}

}  // namespace oe
