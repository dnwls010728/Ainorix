#pragma once
#include <string>

namespace oe {

// The team a new project starts with (docs/TEAM.md §3.3): Lead (Sonnet, routes and delegates),
// Core (Opus, hard gameplay code), Content (Sonnet, scenes/prefabs/UI/texts), Art (Codex, images and
// sound) and QA (Sonnet, play-tests, edits nothing). Written to `<projectDir>/.oe/team/team.json`
// through TeamStore, so it is validated like any other change. An agent whose CLI is not installed
// simply shows as offline.
//
// False with `error` when the project already has agents or its team file cannot be read; nothing
// is changed then.
bool SeedDefaultTeam(const std::string& projectDir, std::string* error);

}  // namespace oe
