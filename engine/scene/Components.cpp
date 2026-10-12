#include "scene/Components.h"

namespace oe {

void RegisterBuiltinComponents() {
    TypeRegistry::Register<Transform>();
    TypeRegistry::Register<MeshRenderer>();
    TypeRegistry::Register<Animator>();
    TypeRegistry::Register<BoneAttachment>();
    TypeRegistry::Register<BoneIK>();
    TypeRegistry::Register<SpringBone>();
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
    TypeRegistry::Register<CameraFollow>();
    TypeRegistry::Register<Sprite>();
    TypeRegistry::Register<SpriteAnimation>();
    TypeRegistry::Register<Tilemap>();
    TypeRegistry::Register<NetSync>();
    TypeRegistry::Register<NetPlayer>();
    TypeRegistry::Register<Trail>();
    TypeRegistry::Register<VFXSystem>();
    TypeRegistry::Register<Data>();  // last: the earlier pools keep their order
}

}  // namespace oe
