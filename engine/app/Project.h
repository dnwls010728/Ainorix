#pragma once
#include <string>

#include "core/Json.h"

namespace oe {

// New projects are copies of templates/<name>/ (shipped next to the
// executable, falling back to the source tree):
//   project.json            {"format":"ownengine.project","name":..,"startScene":..}
//   scenes/*.scene.json     scenes (the sample is a two-level coin game)
//   prefabs/*.prefab.json   prefabs
//   scripts/*.lua           gameplay scripts
//   AGENTS.md               how AI agents should work with the project
// Sound effects (sounds/*.wav) are synthesized at creation time.
std::string FindTemplateDir(const std::string& templateName = "default");
bool CreateProject(const std::string& dir, const std::string& name, std::string* error);

// The template's start scene, normalized (all component fields present).
Json MakeSampleScene(const std::string& name);

}  // namespace oe
