#pragma once
#include <string>
#include <vector>

#include "core/Json.h"

namespace oe {

class Engine;

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

// Copies a file from anywhere on disk into the open project (oe import, files
// dropped on the native editor): models go to assets/models/, images to
// assets/textures/, sounds to sounds/, anything else to assets/, unless
// `destRel` names the project-relative target. Returns that relative path.
// Throws ApiError (not_found, invalid_path, write_failed).
std::string ImportAssetFile(Engine& engine, const std::string& sourcePath, const std::string& destRel = "");

// ----- Packaging (oe package) ----------------------------------------------------

// Files a packaged game ships: everything in the project except hidden files
// and agent notes (AGENTS.md, CLAUDE.md). Relative paths with '/', sorted.
std::vector<std::string> GameFiles(const std::string& projectDir);

// Writes the web player's game data archive:
//   "OEPAK001" | uint32 little-endian index size | JSON index
//   {"files":[{"path","offset","size"}]} | file bytes (offsets relative to their start)
// tools/player/web/index.html unpacks it into /game before the engine starts.
bool WriteGamePak(const std::string& projectDir, const std::vector<std::string>& files, const std::string& outPath,
                  std::string* error, double* dataBytes = nullptr);

}  // namespace oe
