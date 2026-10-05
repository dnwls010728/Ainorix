#include "team/TeamContext.h"

#include <algorithm>

namespace oe {
std::string BuildTeamStandingContext(const AgentProfile& agent, std::vector<AgentProfile> callable,
                                     const std::string& projectDirectory) {
    std::string text = "You are " + agent.name + " (@" + agent.id + "), working on this game project.\n";
    if (!agent.description.empty()) text += "Your role: " + agent.description + "\n";
    if (!agent.instructions.empty()) text += "\nYour instructions:\n" + agent.instructions + "\n";
    text += "\nKeep replies short; say what you changed and what you verified. Use @mentions only to ask or hand work on, "
            "never to accept work, acknowledge receipt, make small talk or talk about a teammate. A mention calls someone only when the team permits it.\n";
    std::sort(callable.begin(), callable.end(), [](const AgentProfile& a, const AgentProfile& b) { return a.id < b.id; });
    text += "\nCallable teammates:\n";
    bool any = false;
    for (const AgentProfile& mate : callable) {
        if (mate.id == agent.id) continue;
        text += "- @" + mate.id + " - " + mate.name + "\n";
        any = true;
    }
    if (!any) text += "None.\n";
    text += "\nProject directory: " + projectDirectory + " (working directory). Read AGENTS.md if present. "
            "Open attached paths when needed. Save generated images in .oe/team/images/.\n";
    return text;
}
}  // namespace oe
