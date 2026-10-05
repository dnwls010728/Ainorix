#pragma once
#include <string>
#include <vector>

#include "team/TeamStore.h"

namespace oe {
// Stable role prefix shared by turn entry points. Connection/history/task data follows it.
// Callable targets are sorted by id and carry no runtime states, times or counts.
std::string BuildTeamStandingContext(const AgentProfile& agent, std::vector<AgentProfile> callable,
                                     const std::string& projectDirectory);
}  // namespace oe
