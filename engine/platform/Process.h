#pragma once
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace oe {

// Child processes for tools (the agent CLIs of docs/TEAM.md). Games never start processes;
// platforms without an implementation report PlatformProcessSupported() == false.

// A longer output line is cut at this many bytes; the rest of that line is dropped.
constexpr size_t kMaxProcessLineBytes = 1024 * 1024;

struct ProcessOptions {
    std::string executable;              // path from PlatformFindExecutable (or any existing file)
    std::vector<std::string> arguments;  // argv entries, never a shell string
    std::string workingDirectory;        // empty = the parent's
    std::vector<std::pair<std::string, std::string>> environment;  // added to (or replacing) the parent's
    bool pipeStdin = true;               // false: the child reads end-of-file at once
};

// One running child. Destroying it kills the whole process tree and joins the readers, so a
// closed tool can never leave agents running.
class Process {
public:
    virtual ~Process() = default;
    // Blocks while the child's stdin pipe is full; false once the input is closed or the child left.
    virtual bool Write(const std::string& bytes, std::string* error) = 0;
    virtual void CloseInput() = 0;
    // Blocks for the next complete line of stdout or stderr (without the line break); false at the
    // end of both streams. Call it from one thread. Unread lines apply back-pressure to the child.
    virtual bool ReadLine(std::string& line, bool& isStderr) = 0;
    // False once the child exited; exitCode (optional) is then set.
    virtual bool Running(int* exitCode) = 0;
    // Ends the child and every process it started; ReadLine then reaches the end of the streams.
    virtual void Kill() = 0;
};

bool PlatformProcessSupported();
// Searches PATH (and PATHEXT on Windows) for a program name; a name with a directory is checked
// as given. Returns an absolute path with forward slashes, empty when not found. The current
// directory is not searched.
std::string PlatformFindExecutable(const std::string& name);
// Null with `error` set when the process cannot be started. Windows command scripts (.cmd/.bat,
// the shims npm installs) run through cmd.exe, which re-parses its command line: arguments with
// a quote, percent sign, exclamation mark or line break are refused for them, so text a user
// typed must travel through stdin or a file instead.
std::unique_ptr<Process> PlatformStartProcess(const ProcessOptions& options, std::string* error);

}  // namespace oe
