#include "team/Backends.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "core/FileSystem.h"
#include "platform/Platform.h"
#include "platform/Process.h"

namespace oe {
namespace {

constexpr size_t kMaxCapturedBytes = 64 * 1024;  // of a `--version` run, per stream
constexpr size_t kMaxModels = 64;
constexpr size_t kMaxConfigBytes = 8 * 1024 * 1024;  // Codex's model cache is about 0.5 MiB

struct Captured {
    std::string out, err;
    int exitCode = -1;
    bool timedOut = false;
};

// Runs a short command to its end without input: until its output streams ended AND it exited
// (a CLI may close its output and stay a moment, or exit and leave a helper holding the pipes).
// The process tree is killed at the timeout (timedOut), and one second after the program exited
// while something it started still holds the output pipes.
bool Capture(const ProcessOptions& options, double timeoutSeconds, Captured& captured, std::string* error) {
    std::unique_ptr<Process> process = PlatformStartProcess(options, error);
    if (!process) return false;
    std::atomic<bool> done{false};
    std::thread reader([&] {
        std::string line;
        bool isStderr = false;
        while (process->ReadLine(line, isStderr)) {
            std::string& target = isStderr ? captured.err : captured.out;
            if (target.size() + line.size() < kMaxCapturedBytes) target += line + "\n";
        }
        done = true;
    });
    const auto start = std::chrono::steady_clock::now();
    double exitedAt = -1.0;
    bool killed = false;
    for (;;) {
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const bool running = process->Running(nullptr);
        if (done && !running) break;
        if (!running && exitedAt < 0.0) exitedAt = elapsed;
        if (!killed && (running ? elapsed > timeoutSeconds : elapsed - exitedAt > 1.0)) {
            captured.timedOut = running;
            process->Kill();
            killed = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    reader.join();
    process->Running(&captured.exitCode);
    return true;
}

std::string FirstLine(const std::string& text) {
    size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return std::string();
    std::string line = text.substr(start, text.find('\n', start) - start);
    if (line.size() > 200) line.resize(200);
    return line;
}

std::string EnvironmentValue(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

// Codex keeps the model list its server sent in $CODEX_HOME/models_cache.json (default ~/.codex).
std::vector<ModelInfo> CodexModels() {
    std::string home = EnvironmentValue("CODEX_HOME");
    if (home.empty()) {
        std::string user = EnvironmentValue("USERPROFILE");
        if (user.empty()) user = EnvironmentValue("HOME");
        if (user.empty()) return {};
        home = JoinPath(user, ".codex");
    }
    std::string text;
    if (!ReadTextFile(JoinPath(home, "models_cache.json"), text) || text.size() > kMaxConfigBytes) return {};
    return ParseCodexModelCache(text);
}

Json ModelsToJson(const std::vector<ModelInfo>& models) {
    Json list = Json::MakeArray();
    for (const ModelInfo& model : models) {
        Json j = Json::MakeObject();
        j["id"] = model.id;
        j["label"] = model.label;
        if (model.isDefault) j["default"] = true;
        if (model.contextWindow > 0) j["contextWindow"] = model.contextWindow;
        list.push(std::move(j));
    }
    return list;
}

}  // namespace

const char* BackendStatusName(BackendStatus status) {
    switch (status) {
        case BackendStatus::Installed: return "installed";
        case BackendStatus::NotInstalled: return "notInstalled";
        case BackendStatus::Error: return "error";
        case BackendStatus::Unavailable: return "unavailable";
        case BackendStatus::Detecting: return "detecting";
    }
    return "error";
}

std::vector<Backend> BuiltinBackends() {
    std::vector<Backend> backends(3);
    Backend& claude = backends[0];
    claude.id = "claude";
    claude.displayName = "Claude Code";
    claude.executable = "claude";
    claude.installHint = "npm install -g @anthropic-ai/claude-code";
    // The CLI's own aliases: they follow new model releases without an engine update. The
    // labels carry no version: Claude Code offers no list to read one from, and a version is
    // shown only where it was detected on this PC, never guessed.
    claude.models = {{"fable", "Fable", false, 0}, {"opus", "Opus", false, 0}, {"sonnet", "Sonnet", true, 0}, {"haiku", "Haiku", false, 0}};
    claude.systemPromptFlag = true;
    claude.buildTurn = &ClaudeBuildTurn;
    claude.parseLine = &ParseClaudeLine;

    Backend& codex = backends[1];
    codex.id = "codex";
    codex.displayName = "Codex";
    codex.executable = "codex";
    codex.installHint = "npm install -g @openai/codex";
    // No built-in catalog: the models are the ones the installed Codex reports (its model cache).
    // When that cannot be read, only the CLI's default is offered, never a guessed list.
    codex.supportsImages = true;
    codex.discoverModels = &CodexModels;
    codex.buildTurn = &CodexBuildTurn;
    codex.parseLine = &ParseCodexLine;

    Backend& gemini = backends[2];
    gemini.id = "gemini";
    gemini.displayName = "Gemini CLI";
    gemini.executable = "gemini";
    gemini.installHint = "npm install -g @google/gemini-cli";
    // No catalog yet: the CLI's default model, or a custom id (the turn adapter is listed under Later).
    return backends;
}

std::string BackendOverridesPath() {
    const std::string directory = PlatformSaveStorage("OwnEngine").directory;
    return directory.empty() ? std::string() : JoinPath(directory, "backends.json");
}

bool ApplyBackendOverrides(const std::string& path, std::vector<Backend>& backends, std::string* error) {
    auto fail = [&](const std::string& message) {
        if (error) *error = path + ": " + message;
        return false;
    };
    std::string text;
    if (path.empty() || !FileExists(path)) return true;
    if (!ReadTextFile(path, text)) return fail("cannot read the file");
    if (text.size() > 1024 * 1024) return fail("larger than 1 MiB");
    std::string parseError;
    const Json root = Json::parse(text, &parseError);
    if (!parseError.empty()) return fail(parseError);
    if (!root.isObject()) return fail("expected an object keyed by backend id");
    std::vector<Backend> result = backends;
    for (const auto& member : root.members()) {
        auto backend = std::find_if(result.begin(), result.end(), [&](const Backend& b) { return b.id == member.first; });
        if (backend == result.end()) return fail("unknown backend \"" + member.first + "\"");
        const Json& entry = member.second;
        if (!entry.isObject()) return fail(member.first + ": expected an object");
        if (const Json* executable = entry.find("executable")) {
            const std::string value = executable->asString("");
            const bool control = std::any_of(value.begin(), value.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20; });
            if (value.empty() || value.size() > 1024 || control) return fail(member.first + ".executable: expected a program name or path");
            backend->executable = value;
        }
        if (const Json* images = entry.find("supportsImages")) {
            if (!images->isBool()) return fail(member.first + ".supportsImages: expected true or false");
            backend->supportsImages = images->asBool();
        }
        if (const Json* models = entry.find("models")) {
            if (!models->isArray() || models->size() > kMaxModels) return fail(member.first + ".models: expected an array of at most 64 models");
            std::vector<ModelInfo> list;
            int defaults = 0;
            for (const Json& item : models->items()) {
                ModelInfo model;
                model.id = item["id"].asString("");
                model.label = item["label"].asString(model.id);
                model.isDefault = item["default"].asBool(false);
                model.contextWindow = std::max(0, item["contextWindow"].asInt(0));
                if (!ValidModelId(model.id)) return fail(member.first + ".models: invalid model id \"" + model.id + "\"");
                if (model.label.empty() || model.label.size() > 64) return fail(member.first + ".models: label of \"" + model.id + "\" must be 1..64 bytes");
                defaults += model.isDefault ? 1 : 0;
                list.push_back(std::move(model));
            }
            if (defaults > 1) return fail(member.first + ".models: more than one default");
            backend->models = std::move(list);
            backend->discoverModels = nullptr;
        }
    }
    backends = std::move(result);
    return true;
}

BackendState DetectBackend(const Backend& backend, double timeoutSeconds) {
    BackendState state;
    state.models = backend.models;
    if (!PlatformProcessSupported()) {
        state.status = BackendStatus::Unavailable;
        state.message = "This platform cannot start agent CLIs yet.";
        return state;
    }
    state.path = PlatformFindExecutable(backend.executable);
    if (state.path.empty()) {
        state.message = backend.executable + " was not found in PATH.";
        return state;
    }
    ProcessOptions options;
    options.executable = state.path;
    options.arguments = {"--version"};
    options.pipeStdin = false;
    const std::string command = "`" + backend.executable + " --version`";
    Captured captured;
    state.status = BackendStatus::Error;
    if (!Capture(options, timeoutSeconds, captured, &state.message)) return state;
    state.version = ParseCliVersion(captured.out);
    if (captured.timedOut) {
        // A CLI that printed its version and then stays (an update check, a background helper)
        // is installed; one that printed nothing is not usable.
        if (state.version.empty()) {
            state.message = command + " did not finish in time.";
            return state;
        }
    } else if (captured.exitCode != 0) {
        state.version.clear();
        const std::string detail = FirstLine(captured.err.empty() ? captured.out : captured.err);
        state.message = command + " exited with code " + std::to_string(captured.exitCode) + (detail.empty() ? "." : ": " + detail);
        return state;
    }
    state.status = BackendStatus::Installed;
    if (state.version.empty()) state.version = ParseCliVersion(captured.err);
    if (backend.discoverModels) {
        std::vector<ModelInfo> discovered = backend.discoverModels();
        if (!discovered.empty()) {
            state.models = std::move(discovered);
            state.modelsDiscovered = true;
        }
    }
    return state;
}

std::vector<BackendState> DetectBackends(const std::vector<Backend>& backends, double timeoutSeconds) {
    std::vector<BackendState> states(backends.size());
    std::vector<std::thread> workers;
    for (size_t i = 0; i < backends.size(); ++i) {
        workers.emplace_back([&, i] { states[i] = DetectBackend(backends[i], timeoutSeconds); });
    }
    for (std::thread& worker : workers) worker.join();
    return states;
}

Json BackendToJson(const Backend& backend, const BackendState& state) {
    Json j = Json::MakeObject();
    j["id"] = backend.id;
    j["name"] = backend.displayName;
    j["status"] = BackendStatusName(state.status);
    if (!state.version.empty()) j["version"] = state.version;
    if (!state.path.empty()) j["path"] = state.path;
    if (!state.message.empty()) j["message"] = state.message;
    j["models"] = ModelsToJson(state.models);
    j["modelSource"] = state.modelsDiscovered ? "discovered" : "catalog";
    j["supportsImages"] = backend.supportsImages;
    j["installHint"] = backend.installHint;
    return j;
}

std::string ParseCliVersion(const std::string& output) {
    auto digit = [](char c) { return c >= '0' && c <= '9'; };
    for (size_t i = 0; i < output.size(); ++i) {
        if (!digit(output[i])) continue;
        size_t end = i;
        bool dotted = false;
        while (end < output.size() && (digit(output[end]) || (output[end] == '.' && end + 1 < output.size() && digit(output[end + 1])))) {
            dotted = dotted || output[end] == '.';
            ++end;
        }
        if (dotted) return output.substr(i, end - i);
        i = end;
    }
    return std::string();
}

bool ValidModelId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (char c : id) {
        const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!letter && c != '.' && c != '_' && c != ':' && c != '[' && c != ']' && c != '-') return false;
    }
    return true;
}

std::vector<ModelInfo> ParseCodexModelCache(const std::string& text) {
    std::string error;
    const Json root = Json::parse(text, &error);
    if (!error.empty() || !root["models"].isArray()) return {};
    std::vector<std::pair<double, ModelInfo>> listed;
    for (const Json& item : root["models"].items()) {
        ModelInfo model;
        model.id = item["slug"].asString("");
        if (!ValidModelId(model.id) || item["visibility"].asString("") != "list") continue;
        model.label = item["display_name"].asString(model.id);
        if (model.label.empty() || model.label.size() > 64) model.label = model.id;
        model.contextWindow = std::max(0, item["context_window"].asInt(0));
        listed.emplace_back(item["priority"].asNumber(1e9), std::move(model));
    }
    std::stable_sort(listed.begin(), listed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    if (listed.size() > kMaxModels) listed.resize(kMaxModels);
    std::vector<ModelInfo> models;
    for (auto& entry : listed) models.push_back(std::move(entry.second));
    if (!models.empty()) models.front().isDefault = true;
    return models;
}

}  // namespace oe
