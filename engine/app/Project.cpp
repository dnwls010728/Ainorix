#include "app/Project.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "api/Commands.h"
#include "app/Engine.h"
#include "assets/Assets.h"
#include "audio/Wav.h"
#include "core/FileSystem.h"
#include "core/Log.h"
#include "core/Pak.h"
#include "platform/Platform.h"
#include "scene/Scene.h"
#include "script/ScriptCheck.h"

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
        // tools/ holds development helpers (e.g. art generators) and web/ files for the web page
        // (`oe package --web` puts them next to index.html), not game data. server/ is the project's own
        // back end (sources, databases, keys of services the game talks to): it never ships to players.
        if (hidden || rel == "AGENTS.md" || rel == "CLAUDE.md" || rel.rfind("tools/", 0) == 0 || rel.rfind("web/", 0) == 0 || rel.rfind("server/", 0) == 0) continue;
        files.push_back(rel);
    }
    std::sort(files.begin(), files.end());
    return files;
}

bool WriteGamePak(const std::string& projectDir, const std::vector<std::string>& files, const std::string& outPath, std::string* error,
                  double* dataBytes, const unsigned char* key, bool stripScripts) {
    std::vector<std::pair<std::string, std::vector<unsigned char>>> entries;
    double total = 0;
    for (const std::string& rel : files) {
        std::vector<unsigned char> bytes;
        if (!ReadBinaryFile(JoinPath(projectDir, rel), bytes)) {
            if (error) *error = "cannot read " + rel;
            return false;
        }
        total += static_cast<double>(bytes.size());
        if (rel.size() > 4 && rel.compare(rel.size() - 4, 4, ".lua") == 0) {
            // The chunk name is the one ScriptHost gives the file, so messages read the same.
            std::string bytecode, compileError;
            if (CompileLuaBytecode(std::string(bytes.begin(), bytes.end()), "@" + rel, bytecode, &compileError, stripScripts)) bytes.assign(bytecode.begin(), bytecode.end());
            else OE_LOG_WARN("package", "%s is packaged as source: %s", rel.c_str(), compileError.c_str());
        }
        entries.emplace_back(rel, std::move(bytes));
    }
    if (!WriteTextFile(outPath, BuildPak(entries, key))) {
        if (error) *error = "cannot write " + outPath;
        return false;
    }
    if (dataBytes) *dataBytes = total;
    return true;
}

bool ExtractGamePak(const std::vector<unsigned char>& pak, const std::string& dir, std::string* error, const unsigned char* key) {
    auto fail = [&](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    PakReader reader;
    if (!reader.OpenMemory(pak, error, key)) return false;
    for (const std::string& rel : reader.Paths()) {
        std::vector<unsigned char> data;
        std::string readError;
        if (!reader.Read(rel, data, &readError)) return fail(readError);
        std::string path = JoinPath(dir, rel);
        CreateDirectories(ParentPath(path));
        FILE* out = std::fopen(path.c_str(), "wb");
        bool ok = out && (data.empty() || std::fwrite(data.data(), 1, data.size(), out) == data.size());
        if (out) ok = std::fclose(out) == 0 && ok;
        if (!ok) return fail("cannot write " + path);
    }
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
