#include "scene/Components.h"

namespace oe {

void RegisterBuiltinComponents() {
    TypeRegistry::Register<Transform>();
    TypeRegistry::Register<MeshRenderer>();
    TypeRegistry::Register<Camera>();
    TypeRegistry::Register<DirectionalLight>();
    TypeRegistry::Register<Rotator>();
    TypeRegistry::Register<Velocity>();
    TypeRegistry::Register<PlayerController>();
    TypeRegistry::Register<Tag>();
}

}  // namespace oe
