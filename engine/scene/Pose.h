#pragma once
#include <string>
#include <vector>

#include "scene/Scene.h"

namespace oe {

class AssetManager;
struct Mesh;

// One inverse-kinematics request on node matrices in model space (see the BoneIK component).
struct IkGoal {
    int node = -1;       // the node that should reach `target`
    int chain = 2;       // ancestors that turn, 1..8
    Vec3 target;         // model space
    float weight = 1;    // 0 keeps the pose, 1 reaches the target
    Vec3 pole;           // chain 2: model-space direction of the middle joint; zero = keep the posed bend
    float soften = 0;    // chain 2: share of the limb length over which it eases into full extension (0 = none)
    bool matchRotation = false;
    Mat4 rotation;       // model-space orientation for matchRotation (rotation part only)
};

// First node with this exact name, -1 when there is none (or the name is empty).
int FindModelNode(const Mesh& mesh, const std::string& name);

// Turns the chain above goal.node inside `nodes` (model-space matrices from EvaluateNodePose) so
// the node reaches the target; everything below a turned node follows. Bone lengths and scales
// are kept, so a target out of reach leaves the chain stretched towards it. Chain 2 is the
// analytic two-bone solve, other lengths use a fixed number of cyclic-coordinate-descent passes
// (deterministic). Goals that name a node without enough ancestors do nothing.
// Bend side of a two-bone chain: the pole when given; else the side the limb bends to in the
// model's default pose (`rest` = EvaluateNodePose(mesh, nullptr, 0)), so a clip that straightens or
// slightly hyperextends the limb cannot flip the joint; else (no rest, or a straight default pose)
// the side it is bent to in `nodes`.
void SolveIk(const Mesh& mesh, std::vector<Mat4>& nodes, const IkGoal& goal, const std::vector<Mat4>* rest = nullptr);

// Node matrices of the model on entity `id` as it is shown: its Animator's clip and time, then
// the BoneIK goals of its direct children in entity id order, then the swing its SpringBone
// children have simulated so far. Both renderers, animation.state, animation.bones and
// BoneAttachment use this, so they agree. `ik` false stops after the clip: the pose a script adds
// its own goals to (foot placement reads where the clip puts a foot).
std::vector<Mat4> EvaluateEntityPose(const Scene& scene, EntityId id, const Mesh& mesh, bool ik = true);

// Advances every SpringBone by one step: the ends of its bones (world space) follow the model with
// inertia. Deterministic; the state lives in the component and is copied with the scene.
void UpdateSpringBones(Scene& scene, AssetManager& assets, float dt);

// How many bones SpringBone.bone = `bone` swings in this model (0 = unknown name or a bone with nothing below it).
int SpringBoneCount(const Mesh& mesh, const std::string& bone);

// Writes the Transform (position, rotation) of every BoneAttachment entity from its parent's pose.
void UpdateBoneAttachments(Scene& scene, AssetManager& assets);

}  // namespace oe
