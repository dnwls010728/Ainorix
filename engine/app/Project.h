#pragma once
#include <string>

#include "core/Json.h"

namespace oe {

// Project layout:
//   <dir>/project.json              {"format":"ownengine.project","name":..,"startScene":..}
//   <dir>/scenes/main.scene.json    start scene
//   <dir>/AGENTS.md                 how AI agents should work with this project
bool CreateProject(const std::string& dir, const std::string& name, std::string* error);

// Demo scene: camera, sun, ground, spinning cube, player-controlled sphere.
Json MakeSampleScene(const std::string& name);

}  // namespace oe
