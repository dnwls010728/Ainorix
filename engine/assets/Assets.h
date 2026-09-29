#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/Json.h"
#include "render/Mesh.h"

namespace oe {

class Engine;

// Decoders (stb_image / cgltf). `baseDir` resolves external files of .gltf.
bool DecodeImage(const unsigned char* data, size_t size, Texture& out, std::string* error);
bool LoadModelFile(const std::string& path, Mesh& out, std::string* error);

// Loads models and textures referenced by the scene, caches them, and
// reloads them when their files change (PollChanges). Failed loads are
// remembered (with the error) so they are reported once, not every frame.
class AssetManager {
public:
    explicit AssetManager(Engine& engine) : engine_(engine) {}

    // Built-in mesh name or model path (.glb/.gltf). nullptr + error on failure.
    std::shared_ptr<const Mesh> GetMesh(const std::string& name, std::string* error = nullptr);
    std::shared_ptr<const Texture> GetTexture(const std::string& path, std::string* error = nullptr);

    // Reloads assets whose files changed; returns their paths.
    std::vector<std::string> PollChanges();
    void Clear();

    // Description of any project file (model stats, image size, sound length...).
    Json Info(const std::string& path);
    static std::string KindOf(const std::string& path);

private:
    template <class T>
    struct Entry {
        std::shared_ptr<const T> asset;
        std::string error;
        int64_t mtime = 0;
    };
    Entry<Mesh> LoadMesh(const std::string& path);
    Entry<Texture> LoadTexture(const std::string& path);

    Engine& engine_;
    std::map<std::string, Entry<Mesh>> meshes_;
    std::map<std::string, Entry<Texture>> textures_;
};

}  // namespace oe
