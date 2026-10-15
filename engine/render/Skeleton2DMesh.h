#pragma once
#include <memory>
#include <string>
#include <vector>

#include "render/Mesh.h"
#include "scene/Skeleton2D.h"

namespace oe {

class AssetManager;

// Depth step between slots (world units toward +Z), so cut-out parts sort by depth in draw order.
constexpr float kSkeleton2DSlotStep = 0.0002f;

// What a Skeleton2D draws in one pose: the attachments its slots show merged into one skinned mesh
// (positions in the setup pose, one joint per bone, a material per image and blend) and the bone
// palette (posed bone * inverse setup bone) both renderers skin it with.
struct Skeleton2DDrawable {
    std::shared_ptr<const Mesh> mesh;  // null when nothing is shown
    std::vector<Mat4> palette;
    std::vector<std::string> missing;  // images that did not load (drawn magenta)
};
// The mesh is cached by the skeleton, skin, shown attachments, their images, colors and draw order,
// so a clip that only moves bones reuses it every frame.
Skeleton2DDrawable BuildSkeleton2DDrawable(const std::shared_ptr<const Skeleton2DDef>& asset, const std::string& skin, const Skeleton2DPose& pose,
                                           AssetManager* assets);

}  // namespace oe
