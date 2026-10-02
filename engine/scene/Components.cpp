#include "scene/Components.h"

namespace oe {

void RegisterBuiltinComponents() {
    TypeRegistry::Register<Transform>();
    TypeRegistry::Register<MeshRenderer>();
    TypeRegistry::Register<Animator>();
    TypeRegistry::Register<ParticleEmitter>();
    TypeRegistry::Register<Camera>();
    TypeRegistry::Register<PostProcess>();
    TypeRegistry::Register<DirectionalLight>();
    TypeRegistry::Register<Rotator>();
    TypeRegistry::Register<Velocity>();
    TypeRegistry::Register<PlayerController>();
    TypeRegistry::Register<Tag>();
    TypeRegistry::Register<Script>();
    TypeRegistry::Register<Collider>();
    TypeRegistry::Register<RigidBody>();
    TypeRegistry::Register<CharacterBody>();
    TypeRegistry::Register<Collider2D>();
    TypeRegistry::Register<RigidBody2D>();
    TypeRegistry::Register<CharacterBody2D>();
    TypeRegistry::Register<Prefab>();
    TypeRegistry::Register<UIText>();
    TypeRegistry::Register<UIPanel>();
    TypeRegistry::Register<UIButton>();
    TypeRegistry::Register<UIImage>();
    TypeRegistry::Register<UISlider>();
    TypeRegistry::Register<UILayout>();
    TypeRegistry::Register<UICanvas>();
    TypeRegistry::Register<AudioSource>();
    TypeRegistry::Register<PointLight>();
    TypeRegistry::Register<CameraFollow>();
    TypeRegistry::Register<Sprite>();
    TypeRegistry::Register<SpriteAnimation>();
    TypeRegistry::Register<Tilemap>();
    TypeRegistry::Register<NetSync>();
    TypeRegistry::Register<NetPlayer>();
}

}  // namespace oe
