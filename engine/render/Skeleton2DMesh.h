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
// One shown attachment in the setup pose: positions in skeleton space (z given), texture coordinates, triangles and
// per vertex up to four bone influences. The geometry the mesh builder merges and the review tools rasterize.
struct Skeleton2DPiece {
    int slot = 0;
    const Skeleton2DAttachment* attachment = nullptr;
    std::shared_ptr<const Texture> texture;  // Skeleton2DPieces only
    std::vector<Vec3> positions;
    std::vector<float> uvs;  // two per position, 0..1 over the image
    std::vector<uint32_t> triangles;
    std::vector<SkinVertex> skin;  // joints are bone indices
};
// `setup` is the setup pose with its world matrices (Skeleton2DWorld).
Skeleton2DPiece Skeleton2DPieceGeometry(const Skeleton2DDef& def, int slot, const Skeleton2DAttachment& attachment, const Texture* texture,
                                        const Skeleton2DPose& setup, float z);
// What a pose shows, back to front (its draw order, attachments and skin).
std::vector<Skeleton2DPiece> Skeleton2DPieces(const Skeleton2DDef& def, const std::string& skin, const Skeleton2DPose& pose, AssetManager* assets);
// Per bone: posed world * inverse setup world, so a setup position moves with Skeleton2DSkinPoint.
std::vector<Mat4> Skeleton2DPalette(const Skeleton2DDef& def, const Skeleton2DPose& pose);
Vec3 Skeleton2DSkinPoint(const std::vector<Mat4>& palette, const SkinVertex& skin, const Vec3& setup);

// The mesh is cached by the skeleton, skin, shown attachments, their images, colors and draw order,
// so a clip that only moves bones reuses it every frame.
Skeleton2DDrawable BuildSkeleton2DDrawable(const std::shared_ptr<const Skeleton2DDef>& asset, const std::string& skin, const Skeleton2DPose& pose,
                                           AssetManager* assets);

}  // namespace oe
