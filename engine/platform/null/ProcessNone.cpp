// Platforms without child processes (web, Android, the headless null platform until it is
// ported): tools then report every agent CLI as unavailable (docs/TEAM.md).
#include "platform/Process.h"

namespace oe {

bool PlatformProcessSupported() { return false; }

std::string PlatformFindExecutable(const std::string&) { return std::string(); }

std::unique_ptr<Process> PlatformStartProcess(const ProcessOptions&, std::string* error) {
    if (error) *error = "child processes are not implemented on this platform";
    return nullptr;
}

}  // namespace oe
