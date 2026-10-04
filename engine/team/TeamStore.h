#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/Image.h"
#include "core/Json.h"

namespace oe {

// Agent profiles and team settings of one project (docs/TEAM.md §3): `<project>/.oe/team/team.json`
// plus uploaded pictures in `avatars/`. Local to the user's checkout (`.oe/` is git-ignored and
// never packaged).

struct AgentProfile {
    std::string id;            // stable slug [a-z0-9_-]{1,32}, generated from the first name; file names and @mentions
    std::string name;          // display name, 1..40 characters, unique without regard to ASCII case
    std::string avatar;        // "preset:<name>" or "file" (then avatars/<id>.png)
    std::string description;   // shown in the roster and given to teammates, <= 400 bytes
    std::string instructions;  // standing instructions (system prompt addition), <= 16 KiB
    std::string backend;       // backend id (team/Backends.h); an unknown one keeps loading
    std::string model;         // model id of that backend, empty = the CLI's default
    std::string access = "edit";  // read | edit | full (docs/TEAM.md §6)
};

struct TeamSettings {
    std::string lead;       // agent id that receives messages without a mention; empty = nobody
    int maxConcurrent = 3;  // turns running at once, 1..8
    int maxHops = 4;        // chained agent-to-agent turns per user message, 0..16
};

constexpr size_t kMaxTeamAgents = 16;
constexpr int kAvatarSize = 256;  // uploaded pictures are stored as kAvatarSize x kAvatarSize PNG

// Well-formed UTF-8 of at most maxBytes without control characters other than line breaks and tabs.
bool ValidTeamText(const std::string& text, size_t maxBytes);
// Ids of the built-in pictures ("fox", ...). The art is embedded by the editor.
const std::vector<std::string>& TeamAvatarPresets();
// Center-crops to a square and resamples (area average) to kAvatarSize; empty image when the
// input is empty.
Image MakeAvatarImage(const Image& source);

class TeamStore {
public:
    // Tells whether a backend id exists and what its default model is (empty = the CLI's own).
    using BackendLookup = std::function<bool(const std::string& backend, std::string& defaultModel)>;

    // Reads the team of a project; a missing file is an empty team. False with `error` when the
    // file is invalid: the store then refuses every change (LoadError) so nothing overwrites a
    // file the user may want to repair.
    bool Open(const std::string& projectDir, std::string* error);
    // Reads the file again when another process changed it since the last load or save.
    void Refresh();
    const std::string& ProjectDir() const { return projectDir_; }
    std::string Directory() const;  // <project>/.oe/team
    const std::string& LoadError() const { return loadError_; }

    const std::vector<AgentProfile>& Agents() const { return agents_; }
    const TeamSettings& Settings() const { return settings_; }
    // Bumped by every change (the editor redraws when it differs).
    uint64_t Revision() const { return revision_; }
    // By id, else by name (ASCII case-insensitive); nullptr when there is no such agent.
    const AgentProfile* Find(const std::string& idOrName) const;
    // Absolute path of an uploaded picture; empty for a preset avatar.
    std::string AvatarFile(const AgentProfile& agent) const;

    // Changes validate everything first, write team.json atomically and only then take effect.
    // They throw ApiError (code, message, hint) for caller mistakes and failed writes.
    // values: {name, description?, instructions?, backend?, model?, access?, avatar?: "preset:<name>"}.
    const AgentProfile& Add(const Json& values, const BackendLookup& backends);
    // Partial update with the same keys; a new backend without a model resets the model to that
    // backend's default. The id never changes.
    const AgentProfile& Update(const std::string& idOrName, const Json& values, const BackendLookup& backends);
    // Also deletes the uploaded picture and clears `lead` when it was this agent. Returns the id.
    std::string Remove(const std::string& idOrName);
    // Roster order (who is listed and scheduled first): moves the agent to `index`, clamped to the end.
    void Move(const std::string& idOrName, size_t index);
    const AgentProfile& SetAvatarPreset(const std::string& idOrName, const std::string& preset);
    // Decodes a png/jpg (any location, it is only read; <= 16 MiB, <= 8192 px per side) and
    // stores it as avatars/<id>.png.
    const AgentProfile& SetAvatarFile(const std::string& idOrName, const std::string& sourcePath);
    // values: {lead?, maxConcurrent?, maxHops?}.
    void SetSettings(const Json& values);

    // API form: every field present (defaults included), `avatarPath` for uploaded pictures.
    Json AgentToJson(const AgentProfile& agent) const;
    Json ToJson() const;

private:
    size_t RequireIndex(const std::string& idOrName) const;
    void RequireWritable() const;
    void Commit(std::vector<AgentProfile> agents, TeamSettings settings);
    std::string FilePath() const;

    std::string projectDir_, loadError_;
    std::vector<AgentProfile> agents_;
    TeamSettings settings_;
    uint64_t revision_ = 1;
    int64_t fileTime_ = 0;
};

}  // namespace oe
