#include "app/Project.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "api/Commands.h"
#include "app/Engine.h"
#include "assets/Assets.h"
#include "audio/Wav.h"
#include "core/FileSystem.h"
#include "core/Log.h"
#include "platform/Platform.h"
#include "scene/Scene.h"

namespace oe {

namespace {
// Sounds every new project gets: {file, generator preset}.
const char* kTemplateSounds[][2] = {{"sounds/coin.wav", "coin"}, {"sounds/success.wav", "success"}, {"sounds/jump.wav", "jump"}};
}  // namespace

std::string FindTemplateDir(const std::string& templateName) {
    std::string candidates[] = {JoinPath(JoinPath(ExecutableDirectory(), "templates"), templateName),
                                JoinPath(JoinPath(OE_SOURCE_DIR, "templates"), templateName)};
    for (const std::string& c : candidates) {
        if (FileExists(JoinPath(c, "project.json"))) return c;
    }
    return candidates[1];
}

bool CreateProject(const std::string& dir, const std::string& name, std::string* error) {
    if (FileExists(JoinPath(dir, "project.json"))) {
        if (error) *error = "a project already exists at " + dir;
        return false;
    }
    std::string templateDir = FindTemplateDir();
    std::vector<std::string> files = ListFiles(templateDir, "", true);
    if (files.empty()) {
        if (error) *error = "project template not found at " + templateDir;
        return false;
    }
    for (const std::string& src : files) {
        std::string rel = RelativePath(src, templateDir);
        std::string text;
        if (!ReadTextFile(src, text)) {
            if (error) *error = "cannot read template file " + src;
            return false;
        }
        if (rel == "project.json") {
            size_t at = text.find("{{name}}");
            if (at != std::string::npos) text.replace(at, 8, Json(name).dump().substr(1, Json(name).dump().size() - 2));
        }
        if (!WriteTextFile(JoinPath(dir, rel), text)) {
            if (error) *error = "cannot write " + JoinPath(dir, rel);
            return false;
        }
    }
    for (const auto& sound : kTemplateSounds) {
        std::vector<float> mono;
        GenerateSound(sound[1], 0, mono);
        if (!WriteWav(JoinPath(dir, sound[0]), mono, 1, kAudioSampleRate)) {
            if (error) *error = std::string("cannot write ") + sound[0];
            return false;
        }
    }
    return true;
}

Json MakeSampleScene(const std::string& name) {
    std::string text, err;
    std::string path = JoinPath(FindTemplateDir(), "scenes/main.scene.json");
    Scene scene;
    if (!ReadTextFile(path, text) || !scene.FromJson(Json::parse(text), &err)) {
        OE_LOG_ERROR("project", "cannot load template scene %s %s", path.c_str(), err.c_str());
    }
    scene.name = name;
    return scene.ToJson();
}

std::vector<std::string> GameFiles(const std::string& projectDir) {
    std::vector<std::string> files;
    for (const std::string& src : ListFiles(projectDir, "", true)) {
        std::string rel = RelativePath(src, projectDir);
        bool hidden = rel.empty() || rel[0] == '.' || rel.find("/.") != std::string::npos;
        // tools/ holds development helpers (e.g. art generators), not game data.
        if (hidden || rel == "AGENTS.md" || rel == "CLAUDE.md" || rel.rfind("tools/", 0) == 0) continue;
        files.push_back(rel);
    }
    std::sort(files.begin(), files.end());
    return files;
}

bool WriteGamePak(const std::string& projectDir, const std::vector<std::string>& files, const std::string& outPath, std::string* error,
                  double* dataBytes) {
    Json index = Json::MakeObject();
    index["files"] = Json::MakeArray();
    std::string data;
    for (const std::string& rel : files) {
        std::vector<unsigned char> bytes;
        if (!ReadBinaryFile(JoinPath(projectDir, rel), bytes)) {
            if (error) *error = "cannot read " + rel;
            return false;
        }
        Json entry = Json::MakeObject();
        entry["path"] = rel;
        entry["offset"] = static_cast<double>(data.size());
        entry["size"] = static_cast<double>(bytes.size());
        index["files"].push(entry);
        data.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    std::string header = index.dump();
    std::string pak = "OEPAK001";
    uint32_t n = static_cast<uint32_t>(header.size());
    for (int i = 0; i < 4; ++i) pak += static_cast<char>((n >> (8 * i)) & 0xFF);
    pak += header;
    pak += data;
    if (!WriteTextFile(outPath, pak)) {
        if (error) *error = "cannot write " + outPath;
        return false;
    }
    if (dataBytes) *dataBytes = static_cast<double>(data.size());
    return true;
}

std::string ImportAssetFile(Engine& engine, const std::string& sourcePath, const std::string& destRel) {
    std::vector<unsigned char> bytes;
    if (!ReadBinaryFile(sourcePath, bytes)) throw ApiError("not_found", "cannot read " + sourcePath, "Check the file path.");
    std::string name = sourcePath.substr(sourcePath.find_last_of("/\\") + 1);
    std::string kind = AssetManager::KindOf(name);
    std::string folder = kind == "model" ? "assets/models/" : kind == "texture" ? "assets/textures/" : kind == "audio" ? "sounds/" : "assets/";
    std::string rel = destRel.empty() ? folder + name : destRel;
    std::string dest = engine.ResolvePath(rel);  // throws for paths outside the project
    CreateDirectories(ParentPath(dest));
    FILE* f = std::fopen(dest.c_str(), "wb");
    bool ok = f && std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    if (f) ok = std::fclose(f) == 0 && ok;
    if (!ok) throw ApiError("write_failed", "cannot write " + dest, "Check that the project folder is writable.");
    return rel;
}

}  // namespace oe
