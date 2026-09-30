#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/Json.h"
#include "render/Font.h"
#include "render/Mesh.h"
#include "scene/TileGrid.h"

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
    // Material file (*.mat.json). nullptr + error on failure.
    std::shared_ptr<const Material> GetMaterial(const std::string& path, std::string* error = nullptr);
    // UI font: "default" (built-in Roboto) or a .ttf/.otf/.ttc path. nullptr + error on failure.
    std::shared_ptr<FontFace> GetFont(const std::string& path, std::string* error = nullptr);
    // Tileset file (*.tileset.json: image, grid, tile rules). nullptr + error on failure.
    std::shared_ptr<const Tileset> GetTileset(const std::string& path, std::string* error = nullptr);
    // GetTileset as a TilesetLookup (for BuildTileRules, physics, overlays).
    TilesetLookup Tilesets();

    // Reloads assets whose files changed; returns their paths.
    std::vector<std::string> PollChanges();
    void Clear();
    // Drops one cached asset (after a tool rewrote the file).
    void Forget(const std::string& path);

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
    Entry<Material> LoadMaterial(const std::string& path);
    Entry<Tileset> LoadTileset(const std::string& path);
    struct FontEntry {
        std::shared_ptr<FontFace> asset;
        std::string error;
        int64_t mtime = 0;
    };
    FontEntry LoadFont(const std::string& path);

    Engine& engine_;
    std::map<std::string, Entry<Mesh>> meshes_;
    std::map<std::string, Entry<Texture>> textures_;
    std::map<std::string, Entry<Material>> materials_;
    std::map<std::string, Entry<Tileset>> tilesets_;
    std::map<std::string, FontEntry> fonts_;
};

}  // namespace oe
