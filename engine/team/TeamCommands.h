#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "api/Commands.h"
#include "team/Backends.h"
#include "team/TeamSession.h"

namespace oe {

// Adds the `team.*` commands (docs/TEAM.md §8). Called by tools that host an agent team (the
// `oe` front-end); the player runtime never registers them. The first form uses the built-in
// backends with the user's backends.json overrides; the second takes the table (tests).
//
// The returned handle is how the hosting tool talks to the team: it sets `host` (the API port
// agents attach to) and calls `update` once per frame so agent output is handled while no
// command arrives. Every team command also updates, so polling `team.state` is enough.
struct TeamHandle {
    TeamHost host;
    std::function<void()> update;
};
std::shared_ptr<TeamHandle> RegisterTeamCommands(CommandRegistry& registry);
std::shared_ptr<TeamHandle> RegisterTeamCommands(CommandRegistry& registry, std::vector<Backend> backends);

// Builds the single-recipient chat text used by team.handoff. The target is resolved by id/name;
// task text may not contain another team mention because team.send routes every mention it sees.
// Kept public so routing semantics have a small unit test independent of a real agent CLI.
std::string BuildTeamHandoffText(const TeamStore& store, const std::string& to, const std::string& task);

}  // namespace oe
