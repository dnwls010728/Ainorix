#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/Json.h"
#include "core/Math.h"
#include "scene/AnimationEvents.h"

namespace oe {

// A 2D skeleton asset (*.skel2d.json, docs/SKELETON2D.md): bones, slots in draw order, attachments by
// skin and clips. Parsed and validated once (ParseSkeleton2D); everything below is immutable and shared.
// Units: world units, +Y up, rotations in degrees counter-clockwise; image pixels have +Y down.

struct Skeleton2DBone {
    std::string name;
    int parent = -1;  // index of an earlier bone; -1 = the root
    float x = 0, y = 0, rotation = 0, scaleX = 1, scaleY = 1;  // setup pose, relative to the parent
    float length = 0;  // along local +X; drawing and checks only
};

// An image placed relative to its slot's bone in the setup pose. Without `vertices` it is the image's
// rectangle (rigid); with them a mesh whose vertices may be weighted to several bones.
struct Skeleton2DAttachment {
    std::string name;
    std::string image;            // project-relative image
    bool hasRect = false;         // rect: a part of the image, in pixels (x, y from the top-left, w, h)
    float rect[4] = {0, 0, 0, 0};
    float pivot[2] = {0.5f, 0.5f};  // in the rectangle: 0,0 = bottom-left, 1,1 = top-right
    float x = 0, y = 0, rotation = 0, scaleX = 1, scaleY = 1;  // where the pivot sits in the bone's space
    float width = 0, height = 0;  // world size of the rectangle; 0 = pixels / pixelsPerUnit
    // Mesh: vertices in rectangle pixels (x right, y down), triangles as index triples, and per
    // vertex up to four (bone index, weight) pairs; empty weights = rigid to the slot's bone.
    std::vector<float> vertices;  // x0, y0, x1, y1, ...
    std::vector<uint32_t> triangles;
    std::vector<std::vector<std::pair<int, float>>> weights;
    bool IsMesh() const { return !vertices.empty(); }
};

struct Skeleton2DSlot {
    std::string name;
    int bone = 0;
    std::string attachment;  // shown in the setup pose; empty = nothing
    Vec4 color{1, 1, 1, 1};  // multiplies the attachment (rgb + alpha)
    bool additive = false;   // blend: add
};

// One key of a timeline. `curve` shapes the way to the next key.
enum class Skeleton2DCurve { Linear, Step, Smooth, EaseIn, EaseOut };
struct Skeleton2DKey {
    float time = 0;
    float v[4] = {0, 0, 0, 0};  // rotate: angle; translate/scale: x, y; color: r, g, b, a
    std::string text;           // attachment name ("" = hide)
    std::vector<int> order;     // drawOrder: slot indices back to front
    Skeleton2DCurve curve = Skeleton2DCurve::Linear;
};
struct Skeleton2DBoneTimeline {
    int bone = 0;
    std::vector<Skeleton2DKey> rotate, translate, scale;
};
struct Skeleton2DSlotTimeline {
    int slot = 0;
    std::vector<Skeleton2DKey> attachment, color;
};
struct Skeleton2DClip {
    std::string name;
    float duration = 0;  // seconds; default = the last key
    bool loop = true;
    std::vector<Skeleton2DBoneTimeline> bones;
    std::vector<Skeleton2DSlotTimeline> slots;
    std::vector<Skeleton2DKey> drawOrder;
    std::vector<AnimationMarker> events;
};

struct Skeleton2DDef {
    static constexpr size_t kMaxBones = 256;
    static constexpr size_t kMaxSlots = 256;
    static constexpr size_t kMaxAttachments = 1024;
    static constexpr size_t kMaxMeshVertices = 4096;   // per attachment
    static constexpr size_t kMaxClips = 256;
    static constexpr size_t kMaxKeys = 4096;           // per timeline
    float pixelsPerUnit = 100;
    bool pixelArt = false;
    std::vector<Skeleton2DBone> bones;  // parents before children
    std::vector<Skeleton2DSlot> slots;  // setup draw order, back to front
    // skin -> slot index -> attachment name -> attachment. "default" always exists.
    std::map<std::string, std::map<int, std::map<std::string, Skeleton2DAttachment>>> skins;
    std::vector<Skeleton2DClip> clips;

    int BoneIndex(const std::string& name) const;
    int SlotIndex(const std::string& name) const;
    const Skeleton2DClip* FindClip(const std::string& name) const;
    // The attachment a slot shows under that name: the skin's own, else the default skin's.
    const Skeleton2DAttachment* FindAttachment(const std::string& skin, int slot, const std::string& name) const;
};

// Parses and validates an asset. Problems name the place ("clips.walk.bones.arm.rotate[2]") so an agent can fix them.
std::shared_ptr<const Skeleton2DDef> ParseSkeleton2D(const Json& data, std::string* error);

// A pose: local bone values plus the slot state a clip keys.
struct Skeleton2DPose {
    struct Local { float x = 0, y = 0, rotation = 0, scaleX = 1, scaleY = 1; };
    std::vector<Local> bones;
    std::vector<std::string> attachments;  // per slot
    std::vector<Vec4> colors;              // per slot
    std::vector<int> drawOrder;            // slot indices back to front
    std::vector<Mat4> world;               // per bone, skeleton space (filled by Skeleton2DWorld)
};
Skeleton2DPose Skeleton2DSetupPose(const Skeleton2DDef& def);
// Applies `clip` at `time` (seconds, wrapped when it loops) over the setup pose. With `from` and a
// weight below 1, bone values are mixed from that clip's pose (crossfade); slot keys come from `clip`.
Skeleton2DPose Skeleton2DSample(const Skeleton2DDef& def, const Skeleton2DClip* clip, float time,
                                const Skeleton2DClip* from = nullptr, float fromTime = 0, float weight = 1);
struct Skeleton2D;
// The pose a Skeleton2D component shows now: its clip at its time, mixed from the previous clip while it fades.
Skeleton2DPose Skeleton2DComponentPose(const Skeleton2DDef& def, const Skeleton2D& component);
// Bone matrices in skeleton space from the local values (parents first).
void Skeleton2DWorld(const Skeleton2DDef& def, Skeleton2DPose& pose);
// Matrix of one bone's setup or posed local transform.
Mat4 Skeleton2DLocalMatrix(float x, float y, float rotation, float scaleX, float scaleY);
// Clip time after `dt` (loops wrap); clamps a non-looping clip at its end.
float Skeleton2DClipTime(const Skeleton2DClip& clip, float time);

}  // namespace oe
