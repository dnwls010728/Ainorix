#pragma once
#include <string>
#include "scene/Scene.h"

namespace oe {

// One stopped-scene preview. The copy owns all pose edits; the authored scene stays untouched.
struct AnimationPreview {
    Scene scene;
    EntityId id = kNullEntity;
    std::string clip;
    double time = 0;
    float speed = 1;
    bool playing = false, loop = true;
    bool ik = false;  // script-driven targets are frozen during isolated preview; opt in explicitly
    uint64_t epoch = 0;
};

}  // namespace oe
