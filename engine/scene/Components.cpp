#include "scene/Components.h"

#include <algorithm>

#include "scene/SpriteAsset.h"

namespace oe {

void RegisterBuiltinComponents() {
    TypeRegistry::Register<Transform>();
    TypeRegistry::Register<MeshRenderer>();
    TypeRegistry::Register<Animator>();
    TypeRegistry::Register<BoneAttachment>();
    TypeRegistry::Register<BoneIK>();
    TypeRegistry::Register<SpringBone>();
    TypeRegistry::Register<SpringCollider>();
    TypeRegistry::Register<Camera>();
    TypeRegistry::Register<PostProcess>();
    TypeRegistry::Register<Light2D>();
    TypeRegistry::Register<Darkness2D>();
    TypeRegistry::Register<DirectionalLight>();
    TypeRegistry::Register<Tag>();
    TypeRegistry::Register<Script>();
    TypeRegistry::Register<Collider>();
    TypeRegistry::Register<RigidBody>();
    TypeRegistry::Register<CharacterBody>();
    TypeRegistry::Register<Collider2D>();
    TypeRegistry::Register<RigidBody2D>();
    TypeRegistry::Register<CharacterBody2D>();
    TypeRegistry::Register<Joint>();
    TypeRegistry::Register<Joint2D>();
    TypeRegistry::Register<Prefab>();
    TypeRegistry::Register<UIText>();
    TypeRegistry::Register<UIPanel>();
    TypeRegistry::Register<UIButton>();
    TypeRegistry::Register<UIImage>();
    TypeRegistry::Register<UIInput>();
    TypeRegistry::Register<UIDropdown>();
    TypeRegistry::Register<UIJoystick>();
    TypeRegistry::Register<UISlider>();
    TypeRegistry::Register<UILayout>();
    TypeRegistry::Register<UIScroll>();
    TypeRegistry::Register<UIMotion>();
    TypeRegistry::Register<UICanvas>();
    TypeRegistry::Register<AudioSource>();
    TypeRegistry::Register<PointLight>();
    TypeRegistry::Register<Skybox>();
    TypeRegistry::Register<ReflectionProbe>();
    TypeRegistry::Register<PlanarReflection>();
    TypeRegistry::Register<Decal>();
    TypeRegistry::Register<CameraFollow>();
    TypeRegistry::Register<Sprite>();
    TypeRegistry::Register<SpriteAnimation>();
    TypeRegistry::Register<Tilemap>();
    TypeRegistry::Register<NetSync>();
    TypeRegistry::Register<NetPlayer>();
    TypeRegistry::Register<Trail>();
    TypeRegistry::Register<VFXSystem>();
    TypeRegistry::Register<NavMesh>();
    TypeRegistry::Register<NavModifier>();
    TypeRegistry::Register<SpotLight>();
    TypeRegistry::Register<Data>();  // last: the earlier pools keep their order
}

bool Sprite::UpgradeFrom(Json& values, const Sprite& current) {
    static const char* const kOld[] = {"texture", "atlas", "columns", "rows", "pixelsPerUnit", "pivotX", "pivotY", "pixelArt", "normalTexture"};
    bool any = false;
    for (const char* key : kOld) any = any || values.has(key);
    if (!any) return false;
    // Start from the settings the sprite has (or the asset given with them); the old fields change them.
    SpriteSource s;
    ParseSpriteSource(values.has("asset") ? values["asset"].asString() : current.asset, s);
    const bool isAtlas = s.path.size() >= 11 && s.path.compare(s.path.size() - 11, 11, ".atlas.json") == 0;
    std::string texture = isAtlas ? std::string() : s.path, atlas = isAtlas ? s.path : std::string();
    if (values.has("texture")) texture = values["texture"].asString();
    if (values.has("atlas")) atlas = values["atlas"].asString();
    if (values.has("columns")) s.columns = std::max(1, std::min(4096, values["columns"].asInt(1)));
    if (values.has("rows")) s.rows = std::max(1, std::min(4096, values["rows"].asInt(1)));
    if (values.has("pixelsPerUnit")) s.ppu = values["pixelsPerUnit"].asNumber(16);
    if (values.has("pivotX")) s.pivotX = values["pivotX"].asNumber(0.5);
    if (values.has("pivotY")) s.pivotY = values["pivotY"].asNumber(0.5);
    if (values.has("pixelArt")) s.pixelArt = values["pixelArt"].asBool(true);
    if (values.has("normalTexture")) s.normal = values["normalTexture"].asString();
    s.path = atlas.empty() ? texture : atlas;  // an atlas always took precedence over the texture and its grid
    if (!atlas.empty()) s.columns = s.rows = 1;
    for (const char* key : kOld) values.erase(key);
    values["asset"] = FormatSpriteSource(s);
    return true;
}

std::string SpriteFrameKey(const SpriteFrame& frame) { return frame.name.empty() ? std::to_string(frame.index) : frame.name; }

const Json& Collider2DActivePoints(const Collider2D& collider, const Sprite* sprite) {
    if (!sprite || collider.shape != "polygon" || !collider.framePoints.isObject() || collider.framePoints.size() == 0) return collider.points;
    const std::string key = SpriteFrameKey(sprite->frame);
    if (!collider.framePoints.has(key)) return collider.points;
    const Json& outline = collider.framePoints[key];
    return outline.isArray() && outline.size() >= 3 ? outline : collider.points;
}

}  // namespace oe
