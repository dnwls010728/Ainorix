#include "team/TeamStore.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

#include "api/Commands.h"
#include "assets/Assets.h"
#include "core/FileSystem.h"
#include "core/Log.h"
#include "team/Backends.h"

namespace oe {
namespace {

constexpr size_t kMaxNameCharacters = 40;
constexpr size_t kMaxDescriptionBytes = 400;
constexpr size_t kMaxInstructionsBytes = 16 * 1024;
constexpr size_t kMaxTeamFileBytes = 1024 * 1024;
constexpr uintmax_t kMaxAvatarSourceBytes = 16 * 1024 * 1024;
constexpr int kMaxAvatarSourcePixels = 8192;
constexpr int kTeamFileVersion = 1;
const char* const kAgentKeys[] = {"name", "avatar", "description", "instructions", "backend", "model", "access"};

std::string Lower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

std::string Trim(const std::string& text) {
    size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return std::string();
    return text.substr(start, text.find_last_not_of(" \t\r\n") - start + 1);
}

// Counts the characters of well-formed UTF-8 (no overlong forms, surrogates or values above
// U+10FFFF); false for anything else.
bool Utf8Characters(const std::string& text, size_t& count) {
    count = 0;
    for (size_t i = 0; i < text.size(); ++count) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 0;
        if (!length || i + length > text.size()) return false;
        uint32_t code = length == 1 ? lead : lead & (0xFFu >> (length + 1));
        for (size_t k = 1; k < length; ++k) {
            const unsigned char next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0) != 0x80) return false;
            code = (code << 6) | (next & 0x3Fu);
        }
        const uint32_t minimum[] = {0, 0, 0x80, 0x800, 0x10000};
        if (code < minimum[length] || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return false;
        i += length;
    }
    return true;
}

bool HasControl(const std::string& text, bool allowLineBreaks) {
    for (char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u == 0x7F || (u < 0x20 && !(allowLineBreaks && (c == '\n' || c == '\r' || c == '\t')))) return true;
    }
    return false;
}

bool ValidSlug(const std::string& id) {
    if (id.empty() || id.size() > 32) return false;
    for (char c : id) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

// "Mina Kim" -> "mina-kim"; a name without ASCII letters or digits gives "agent".
std::string Slug(const std::string& name) {
    std::string slug;
    for (char c : Lower(name)) {
        const bool keep = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (keep) slug += c;
        else if (!slug.empty() && slug.back() != '-') slug += '-';
        if (slug.size() == 28) break;  // leaves room for a "-NN" suffix inside 32 bytes
    }
    while (!slug.empty() && slug.back() == '-') slug.pop_back();
    return slug.empty() ? std::string("agent") : slug;
}

bool ValidAccess(const std::string& access) { return access == "read" || access == "edit" || access == "full"; }

std::string PresetOf(const std::string& avatar) { return avatar.rfind("preset:", 0) == 0 ? avatar.substr(7) : std::string(); }

bool KnownPreset(const std::string& preset) {
    const std::vector<std::string>& presets = TeamAvatarPresets();
    return std::find(presets.begin(), presets.end(), preset) != presets.end();
}

ApiError Invalid(const std::string& message, const std::string& hint = "") { return ApiError("invalid_argument", message, hint); }

// Copies the string fields present in `values` into `agent`. From the API an unknown key is a
// mistake; in a file it is skipped so files of newer versions keep loading.
void ApplyValues(AgentProfile& agent, const Json& values, bool fromFile) {
    if (!values.isObject()) throw Invalid("agent values must be an object");
    for (const auto& member : values.members()) {
        const std::string& key = member.first;
        std::string* field = key == "name" ? &agent.name : key == "avatar" ? &agent.avatar : key == "description" ? &agent.description :
                             key == "instructions" ? &agent.instructions : key == "backend" ? &agent.backend : key == "model" ? &agent.model :
                             key == "access" ? &agent.access : (fromFile && key == "id") ? &agent.id : nullptr;
        if (!field) {
            if (fromFile) continue;
            std::string known;
            for (const char* k : kAgentKeys) known += std::string(known.empty() ? "" : ", ") + k;
            throw Invalid("unknown agent field '" + key + "'", "Fields: " + known + ".");
        }
        if (!member.second.isString()) throw Invalid("agent field '" + key + "' must be a string");
        *field = member.second.asString();
    }
}

// Rules every stored profile satisfies, whatever wrote it.
void ValidateFields(AgentProfile& agent) {
    agent.name = Trim(agent.name);
    size_t characters = 0;
    if (!Utf8Characters(agent.name, characters) || characters < 1 || characters > kMaxNameCharacters || HasControl(agent.name, false)) {
        throw Invalid("agent name must be 1..40 characters of UTF-8 text on one line");
    }
    const std::string lowered = Lower(agent.name);
    if (lowered == "all" || lowered == "user") throw Invalid("'" + agent.name + "' is reserved (@all mentions everyone, user is the person)", "Choose another name.");
    if (!Utf8Characters(agent.description, characters) || agent.description.size() > kMaxDescriptionBytes || HasControl(agent.description, true)) {
        throw Invalid("agent description must be UTF-8 text of at most 400 bytes");
    }
    if (!Utf8Characters(agent.instructions, characters) || agent.instructions.size() > kMaxInstructionsBytes || HasControl(agent.instructions, true)) {
        throw Invalid("agent instructions must be UTF-8 text of at most 16384 bytes");
    }
    if (!agent.model.empty() && !ValidModelId(agent.model)) {
        throw ApiError("invalid_model", "'" + agent.model + "' is not a model id", "Use an id from team.backends, or 1..64 characters of A-Z a-z 0-9 . _ : [ ] -; empty = the CLI's default.");
    }
    if (!ValidAccess(agent.access)) throw Invalid("agent access must be read, edit or full");
}

void RequireUniqueName(const std::vector<AgentProfile>& agents, const AgentProfile& agent, size_t self) {
    for (size_t i = 0; i < agents.size(); ++i) {
        if (i != self && Lower(agents[i].name) == Lower(agent.name)) {
            throw ApiError("name_taken", "an agent named '" + agents[i].name + "' already exists", "Agent names are unique without regard to case.");
        }
    }
}

Json FileJson(const std::vector<AgentProfile>& agents, const TeamSettings& settings) {
    const TeamSettings defaults;
    Json root = Json::MakeObject();
    root["version"] = kTeamFileVersion;
    if (!settings.lead.empty()) root["lead"] = settings.lead;
    if (settings.maxConcurrent != defaults.maxConcurrent) root["maxConcurrent"] = settings.maxConcurrent;
    if (settings.maxHops != defaults.maxHops) root["maxHops"] = settings.maxHops;
    Json list = Json::MakeArray();
    for (const AgentProfile& agent : agents) {
        Json j = Json::MakeObject();
        j["id"] = agent.id;
        j["name"] = agent.name;
        j["avatar"] = agent.avatar;
        if (!agent.description.empty()) j["description"] = agent.description;
        if (!agent.instructions.empty()) j["instructions"] = agent.instructions;
        j["backend"] = agent.backend;
        if (!agent.model.empty()) j["model"] = agent.model;
        if (agent.access != "edit") j["access"] = agent.access;
        list.push(std::move(j));
    }
    root["agents"] = std::move(list);
    return root;
}

int IntegerSetting(const Json& values, const char* key, int minimum, int maximum, int current) {
    const Json* value = values.find(key);
    if (!value) return current;
    const double number = value->asNumber(-1.0);
    if (!value->isNumber() || number != std::floor(number) || number < minimum || number > maximum) {
        throw Invalid(std::string(key) + " must be an integer from " + std::to_string(minimum) + " to " + std::to_string(maximum));
    }
    return static_cast<int>(number);
}

}  // namespace

bool ValidTeamText(const std::string& text, size_t maxBytes) {
    size_t characters = 0;
    return text.size() <= maxBytes && Utf8Characters(text, characters) && !HasControl(text, true);
}

const std::vector<std::string>& TeamAvatarPresets() {
    // One name per picture in engine/editor/avatars/ (the EditorAvatarPresets test keeps them in step).
    static const std::vector<std::string> presets = {"fox"};
    return presets;
}

Image MakeAvatarImage(const Image& source) {
    Image out;
    if (source.width <= 0 || source.height <= 0 || source.rgba.size() < static_cast<size_t>(source.width) * static_cast<size_t>(source.height) * 4) return out;
    const int side = std::min(source.width, source.height);
    const int left = (source.width - side) / 2, top = (source.height - side) / 2;
    const double scale = static_cast<double>(side) / kAvatarSize;
    out.width = out.height = kAvatarSize;
    out.rgba.resize(static_cast<size_t>(kAvatarSize) * kAvatarSize * 4);
    for (int y = 0; y < kAvatarSize; ++y) {
        const double y0 = y * scale, y1 = (y + 1) * scale;
        for (int x = 0; x < kAvatarSize; ++x) {
            const double x0 = x * scale, x1 = (x + 1) * scale;
            // Area average of the covered source pixels, colors weighted by alpha so that
            // transparent pixels do not darken edges.
            double sum[4] = {0, 0, 0, 0}, area = 0;
            for (int sy = static_cast<int>(y0); sy < side && sy < y1; ++sy) {
                const double wy = std::min(y1, sy + 1.0) - std::max(y0, static_cast<double>(sy));
                for (int sx = static_cast<int>(x0); sx < side && sx < x1; ++sx) {
                    const double weight = wy * (std::min(x1, sx + 1.0) - std::max(x0, static_cast<double>(sx)));
                    const uint8_t* p = &source.rgba[(static_cast<size_t>(top + sy) * static_cast<size_t>(source.width) + static_cast<size_t>(left + sx)) * 4];
                    for (int c = 0; c < 3; ++c) sum[c] += weight * p[c] * p[3];
                    sum[3] += weight * p[3];
                    area += weight;
                }
            }
            uint8_t* q = &out.rgba[(static_cast<size_t>(y) * kAvatarSize + static_cast<size_t>(x)) * 4];
            for (int c = 0; c < 3; ++c) q[c] = sum[3] > 0 ? static_cast<uint8_t>(std::lround(sum[c] / sum[3])) : 0;
            q[3] = area > 0 ? static_cast<uint8_t>(std::lround(sum[3] / area)) : 0;
        }
    }
    return out;
}

std::string TeamStore::Directory() const { return JoinPath(JoinPath(projectDir_, ".oe"), "team"); }

std::string TeamStore::FilePath() const { return JoinPath(Directory(), "team.json"); }

bool TeamStore::Open(const std::string& projectDir, std::string* error) {
    projectDir_ = projectDir;
    agents_.clear();
    settings_ = TeamSettings();
    loadError_.clear();
    ++revision_;
    fileTime_ = FileModifiedTime(FilePath());
    if (!FileExists(FilePath())) return true;
    try {
        std::string text, parseError;
        if (!ReadTextFile(FilePath(), text)) throw Invalid("cannot read the file");
        if (text.size() > kMaxTeamFileBytes) throw Invalid("larger than 1 MiB");
        const Json root = Json::parse(text, &parseError);
        if (!parseError.empty()) throw Invalid(parseError);
        if (!root.isObject() || !root["agents"].isArray()) throw Invalid("expected an object with an \"agents\" array");
        if (root["version"].asInt(kTeamFileVersion) > kTeamFileVersion) throw Invalid("written by a newer version of the engine");
        if (root["agents"].size() > kMaxTeamAgents) throw Invalid("more than 16 agents");
        std::vector<AgentProfile> agents;
        for (const Json& item : root["agents"].items()) {
            AgentProfile agent;
            ApplyValues(agent, item, true);
            if (!ValidSlug(agent.id) || agent.id == "all" || agent.id == "user") throw Invalid("invalid agent id \"" + agent.id + "\"");
            for (const AgentProfile& other : agents) {
                if (other.id == agent.id) throw Invalid("duplicate agent id \"" + agent.id + "\"");
            }
            ValidateFields(agent);
            RequireUniqueName(agents, agent, agents.size());
            if (agent.avatar != "file" && !ValidSlug(PresetOf(agent.avatar))) throw Invalid(agent.id + ": avatar must be \"preset:<name>\" or \"file\"");
            if (agent.backend.size() > 32 || HasControl(agent.backend, false)) throw Invalid(agent.id + ": invalid backend id");
            agents.push_back(std::move(agent));
        }
        TeamSettings settings;
        settings.maxConcurrent = IntegerSetting(root, "maxConcurrent", 1, 8, settings.maxConcurrent);
        settings.maxHops = IntegerSetting(root, "maxHops", 0, 16, settings.maxHops);
        settings.lead = root["lead"].asString("");
        const bool leadExists = std::any_of(agents.begin(), agents.end(), [&](const AgentProfile& a) { return a.id == settings.lead; });
        if (!settings.lead.empty() && !leadExists) throw Invalid("lead \"" + settings.lead + "\" is not an agent of the team");
        agents_ = std::move(agents);
        settings_ = settings;
        return true;
    } catch (const ApiError& e) {
        loadError_ = FilePath() + ": " + e.what();
        if (error) *error = loadError_;
        return false;
    }
}

void TeamStore::Refresh() {
    if (projectDir_.empty() || FileModifiedTime(FilePath()) == fileTime_) return;
    std::string error;
    if (!Open(std::string(projectDir_), &error)) OE_LOG_WARN("team", "%s", error.c_str());
}

const AgentProfile* TeamStore::Find(const std::string& idOrName) const {
    for (const AgentProfile& agent : agents_) {
        if (agent.id == idOrName) return &agent;
    }
    const std::string lowered = Lower(idOrName);
    for (const AgentProfile& agent : agents_) {
        if (Lower(agent.name) == lowered) return &agent;
    }
    return nullptr;
}

std::string TeamStore::AvatarFile(const AgentProfile& agent) const {
    return agent.avatar == "file" ? JoinPath(JoinPath(Directory(), "avatars"), agent.id + ".png") : std::string();
}

size_t TeamStore::RequireIndex(const std::string& idOrName) const {
    const AgentProfile* agent = Find(idOrName);
    if (!agent) throw ApiError("unknown_agent", "no agent '" + idOrName + "' in the team", "team.list shows the agents; use an id or a name.");
    return static_cast<size_t>(agent - agents_.data());
}

void TeamStore::RequireWritable() const {
    if (projectDir_.empty()) throw ApiError("no_project", "no project is open", "Open a project directory first.");
    if (!loadError_.empty()) throw ApiError("team_file_invalid", loadError_, "Repair or delete .oe/team/team.json; the team is not changed while it is invalid.");
}

void TeamStore::Commit(std::vector<AgentProfile> agents, TeamSettings settings) {
    const std::string text = FileJson(agents, settings).dump(2) + "\n";
    std::string error;
    if (text.size() > kMaxTeamFileBytes || !WriteTextFileAtomic(FilePath(), text, &error)) {
        throw ApiError("write_failed", "cannot write " + FilePath() + (error.empty() ? "" : ": " + error), "Check that the project folder is writable.");
    }
    agents_ = std::move(agents);
    settings_ = std::move(settings);
    fileTime_ = FileModifiedTime(FilePath());
    ++revision_;
}

const AgentProfile& TeamStore::Add(const Json& values, const BackendLookup& backends) {
    RequireWritable();
    if (agents_.size() >= kMaxTeamAgents) throw ApiError("team_full", "a team has at most 16 agents", "Remove an agent with team.remove first.");
    AgentProfile agent;
    ApplyValues(agent, values, false);
    if (!values.has("name")) throw ApiError("missing_argument", "an agent needs a name");
    ValidateFields(agent);
    RequireUniqueName(agents_, agent, agents_.size());
    std::string defaultModel;
    if (!backends || !backends(agent.backend, defaultModel)) {
        throw ApiError("unknown_backend", "no agent backend '" + agent.backend + "'", "team.backends lists the backend ids.");
    }
    if (!values.has("model")) agent.model = defaultModel;
    if (!values.has("avatar")) {
        // The first picture no teammate uses yet.
        agent.avatar = "preset:" + TeamAvatarPresets().front();
        for (const std::string& preset : TeamAvatarPresets()) {
            if (std::none_of(agents_.begin(), agents_.end(), [&](const AgentProfile& a) { return a.avatar == "preset:" + preset; })) {
                agent.avatar = "preset:" + preset;
                break;
            }
        }
    } else if (!KnownPreset(PresetOf(agent.avatar))) {
        throw ApiError("unknown_preset", "avatar must be \"preset:<name>\" with a name from team.presets", "Upload a picture with team.avatar {id, source} after adding the agent.");
    }
    const std::string base = Slug(agent.name);
    agent.id = base;
    for (int n = 2; agent.id == "all" || agent.id == "user" || Find(agent.id) != nullptr; ++n) agent.id = base + "-" + std::to_string(n);
    std::vector<AgentProfile> agents = agents_;
    TeamSettings settings = settings_;
    if (agents.empty()) settings.lead = agent.id;  // messages without a mention reach the first agent
    agents.push_back(std::move(agent));
    Commit(std::move(agents), std::move(settings));
    return agents_.back();
}

const AgentProfile& TeamStore::Update(const std::string& idOrName, const Json& values, const BackendLookup& backends) {
    RequireWritable();
    const size_t index = RequireIndex(idOrName);
    const AgentProfile& current = agents_[index];
    AgentProfile agent = current;
    ApplyValues(agent, values, false);
    ValidateFields(agent);
    RequireUniqueName(agents_, agent, index);
    if (agent.backend != current.backend) {
        std::string defaultModel;
        if (!backends || !backends(agent.backend, defaultModel)) {
            throw ApiError("unknown_backend", "no agent backend '" + agent.backend + "'", "team.backends lists the backend ids.");
        }
        if (!values.has("model")) agent.model = defaultModel;
    }
    if (agent.avatar != current.avatar) {
        if (agent.avatar == "file") throw Invalid("avatar \"file\" is set by uploading a picture", "Use team.avatar {id, source}.");
        if (!KnownPreset(PresetOf(agent.avatar))) throw ApiError("unknown_preset", "avatar must be \"preset:<name>\" with a name from team.presets");
    }
    const std::string uploaded = AvatarFile(current);
    const bool dropUpload = !uploaded.empty() && agent.avatar != "file";
    std::vector<AgentProfile> agents = agents_;
    agents[index] = std::move(agent);
    Commit(std::move(agents), settings_);
    if (dropUpload) RemoveAll(uploaded);
    return agents_[index];
}

std::string TeamStore::Remove(const std::string& idOrName) {
    RequireWritable();
    const size_t index = RequireIndex(idOrName);
    const std::string id = agents_[index].id, uploaded = AvatarFile(agents_[index]);
    std::vector<AgentProfile> agents = agents_;
    agents.erase(agents.begin() + static_cast<std::ptrdiff_t>(index));
    TeamSettings settings = settings_;
    if (settings.lead == id) settings.lead.clear();
    Commit(std::move(agents), std::move(settings));
    if (!uploaded.empty()) RemoveAll(uploaded);
    return id;
}

void TeamStore::Move(const std::string& idOrName, size_t index) {
    RequireWritable();
    const size_t from = RequireIndex(idOrName);
    std::vector<AgentProfile> agents = agents_;
    const AgentProfile agent = agents[from];
    agents.erase(agents.begin() + static_cast<std::ptrdiff_t>(from));
    agents.insert(agents.begin() + static_cast<std::ptrdiff_t>(std::min(index, agents.size())), agent);
    Commit(std::move(agents), settings_);
}

const AgentProfile& TeamStore::SetAvatarPreset(const std::string& idOrName, const std::string& preset) {
    if (!KnownPreset(preset)) throw ApiError("unknown_preset", "no avatar preset '" + preset + "'", "team.presets lists the preset names.");
    Json values = Json::MakeObject();
    values["avatar"] = "preset:" + preset;
    return Update(idOrName, values, BackendLookup());
}

const AgentProfile& TeamStore::SetAvatarFile(const std::string& idOrName, const std::string& sourcePath) {
    RequireWritable();
    const size_t index = RequireIndex(idOrName);
    const std::string extension = Lower(sourcePath.substr(std::min(sourcePath.size(), sourcePath.rfind('.'))));
    if (extension != ".png" && extension != ".jpg" && extension != ".jpeg") throw ApiError("invalid_image", "a profile picture must be a .png, .jpg or .jpeg file");
    std::error_code ec;
    const uintmax_t bytes = std::filesystem::file_size(std::filesystem::u8path(sourcePath), ec);
    std::vector<unsigned char> data;
    if (ec || !ReadBinaryFile(sourcePath, data)) throw ApiError("not_found", "cannot read '" + sourcePath + "'", "Pass the path of an image file on this PC.");
    if (bytes > kMaxAvatarSourceBytes) throw ApiError("invalid_image", "the picture is larger than 16 MiB");
    int width = 0, height = 0;
    Texture texture;
    std::string error;
    if (!ImageDimensions(data.data(), data.size(), width, height)) throw ApiError("invalid_image", "'" + sourcePath + "' is not an image the engine can decode");
    if (width > kMaxAvatarSourcePixels || height > kMaxAvatarSourcePixels) throw ApiError("invalid_image", "the picture is larger than 8192 pixels on a side");
    if (!DecodeImage(data.data(), data.size(), texture, &error)) throw ApiError("invalid_image", error);
    Image source;
    source.width = texture.width;
    source.height = texture.height;
    source.rgba.resize(texture.texels.size() * 4);
    std::memcpy(source.rgba.data(), texture.texels.data(), source.rgba.size());
    const std::vector<uint8_t> png = EncodePng(MakeAvatarImage(source), true);

    std::vector<AgentProfile> agents = agents_;
    agents[index].avatar = "file";
    const std::string target = AvatarFile(agents[index]);
    if (!WriteTextFileAtomic(target, std::string(png.begin(), png.end()), &error)) {
        throw ApiError("write_failed", "cannot write " + target + ": " + error, "Check that the project folder is writable.");
    }
    Commit(std::move(agents), settings_);
    return agents_[index];
}

void TeamStore::SetSettings(const Json& values) {
    RequireWritable();
    TeamSettings settings = settings_;
    settings.maxConcurrent = IntegerSetting(values, "maxConcurrent", 1, 8, settings.maxConcurrent);
    settings.maxHops = IntegerSetting(values, "maxHops", 0, 16, settings.maxHops);
    if (const Json* lead = values.find("lead")) {
        if (!lead->isString()) throw Invalid("lead must be an agent id or name, or \"\" for nobody");
        settings.lead = lead->asString().empty() ? std::string() : agents_[RequireIndex(lead->asString())].id;
    }
    Commit(agents_, std::move(settings));
}

Json TeamStore::AgentToJson(const AgentProfile& agent) const {
    Json j = Json::MakeObject();
    j["id"] = agent.id;
    j["name"] = agent.name;
    j["avatar"] = agent.avatar;
    if (agent.avatar == "file") j["avatarPath"] = RelativePath(AvatarFile(agent), projectDir_);
    j["description"] = agent.description;
    j["instructions"] = agent.instructions;
    j["backend"] = agent.backend;
    j["model"] = agent.model;
    j["access"] = agent.access;
    return j;
}

Json TeamStore::ToJson() const {
    Json root = Json::MakeObject();
    root["version"] = kTeamFileVersion;
    root["revision"] = revision_;
    root["lead"] = settings_.lead;
    root["maxConcurrent"] = settings_.maxConcurrent;
    root["maxHops"] = settings_.maxHops;
    Json list = Json::MakeArray();
    for (const AgentProfile& agent : agents_) list.push(AgentToJson(agent));
    root["agents"] = std::move(list);
    return root;
}

}  // namespace oe
