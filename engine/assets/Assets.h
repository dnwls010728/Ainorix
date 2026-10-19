#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/Json.h"
#include "render/Environment.h"
#include "render/Font.h"
#include "render/Mesh.h"
#include "scene/SpriteAsset.h"
#include "scene/TileGrid.h"

namespace oe {

class Engine;
class Scene;
struct VfxAsset;
struct Skeleton2DDef;

// Decoders (stb_image / cgltf / ufbx). External files of a model resolve beside it.
bool DecodeImage(const unsigned char* data, size_t size, Texture& out, std::string* error);
// Size of an encoded image from its header, without decoding the pixels (callers bound the
// memory a decode may take). False when the data is not a supported image.
bool ImageDimensions(const unsigned char* data, size_t size, int& width, int& height);
// An image as floats (width * height * 3, display colors): Radiance .hdr keeps values above 1
// (converted from linear light like the 8-bit decode does), other formats give 0..1.
bool DecodeImageFloat(const unsigned char* data, size_t size, std::vector<float>& rgb, int& width, int& height, std::string* error);
// glTF 2.0 (.glb/.gltf) or FBX (.fbx, see assets/Fbx.h), chosen by the extension.
// A supplied sidecar replaces clips.json during validation, without writing any files.
bool LoadModelFile(const std::string& path, Mesh& out, std::string* error, const Json* sidecar = nullptr);

// Loads models and textures referenced by the scene, caches them, and
// reloads them when their files change (PollChanges). Failed loads are
// remembered (with the error) so they are reported once, not every frame.
class AssetManager {
public:
    explicit AssetManager(Engine& engine) : engine_(engine) {}

    // Built-in mesh name or model path (.glb/.gltf/.fbx). nullptr + error on failure.
    std::shared_ptr<const Mesh> GetMesh(const std::string& name, std::string* error = nullptr);
    std::shared_ptr<const Texture> GetTexture(const std::string& path, std::string* error = nullptr);
    // An equirectangular image (.hdr/.png/.jpg) prepared as an environment (Skybox.texture): blurred
    // levels for reflections and the irradiance terms. nullptr + error on failure.
    std::shared_ptr<const EnvironmentMap> GetEnvironment(const std::string& path, std::string* error = nullptr);
    // Material file (*.mat.json). nullptr + error on failure.
    std::shared_ptr<const Material> GetMaterial(const std::string& path, std::string* error = nullptr);
    // Portable surface graph (*.shader.json); validated and cached including failures.
    std::shared_ptr<const ShaderGraph> GetShader(const std::string& path, std::string* error = nullptr);
    // Linked effects (*.vfx.json), including cached failures and hot reload.
    std::shared_ptr<const VfxAsset> GetVfx(const std::string& path, std::string* error = nullptr);
    // UI font: "default" (built-in Roboto) or a .ttf/.otf/.ttc path. nullptr + error on failure.
    std::shared_ptr<FontFace> GetFont(const std::string& path, std::string* error = nullptr);
    // Tileset file (*.tileset.json: image, grid, tile rules). nullptr + error on failure.
    std::shared_ptr<const Tileset> GetTileset(const std::string& path, std::string* error = nullptr);
    // Reusable sprite animation (*.anim.json), strictly validated and hot reloaded.
    std::shared_ptr<const Json> GetAnimation(const std::string& path, std::string* error = nullptr);
    // 2D skeleton (*.skel2d.json, docs/SKELETON2D.md), validated by ParseSkeleton2D and hot reloaded.
    std::shared_ptr<const Skeleton2DDef> GetSkeleton2D(const std::string& path, std::string* error = nullptr);
    // Named .atlas.json or TexturePacker frames, normalized and hot reloaded. A sprite asset
    // (*.sprite.json) loads here too: an atlas with pixelsPerUnit, pixelArt, pivot and frame collision.
    std::shared_ptr<const Json> GetAtlas(const std::string& path, std::string* error = nullptr);
    // What Sprite.asset names, as a normalized sprite asset: a *.sprite.json file (NormalizeSpriteAsset), or an image or
    // atlas with settings (SpriteSource: "hero.png?columns=6", made from the image; "grid": [columns, rows] marks a
    // grid, "" = no image and no frames). Null + error otherwise.
    std::shared_ptr<const Json> GetSprite(const std::string& path, std::string* error = nullptr);
    // GetTileset as a TilesetLookup (for BuildTileRules, physics, overlays).
    TilesetLookup Tilesets();
    // GetSprite / GetAnimation for physics and overlays (Collider2D shape "sprite").
    SpriteAssets SpriteLookup();

    // Reflection probe pictures by probe entity (taken and read by ActiveProbes / CaptureProbe in
    // RenderScene.h). InvalidateProbes drops them, so each is taken again when it is next drawn:
    // the engine calls it when the scenery may have changed (scene loaded, play started or stopped,
    // probe.bake, and - with `edit` - an edit while stopped).
    // An interactive editor defers the edits (DeferProbeEdits): while a gizmo is dragged every frame
    // is an edit, so the pictures are only marked stale and it drops them once the edits pause.
    std::map<EntityId, std::shared_ptr<const EnvironmentMap>>& ProbePictures() { return probes_; }
    void InvalidateProbes(bool edit = false) {
        if (edit && deferProbeEdits_) {
            probesStale_ = !probes_.empty();
            return;
        }
        probes_.clear();
        probesStale_ = false;
    }
    // The scene the pictures belong to (the engine's own): other scenes drawn with these assets
    // (asset thumbnails, effect previews) have entity ids of their own and use no probes.
    void SetProbeScene(const Scene* scene) { probeScene_ = scene; }
    const Scene* ProbeScene() const { return probeScene_; }
    void DeferProbeEdits(bool on) { deferProbeEdits_ = on; }
    bool ProbesStale() const { return probesStale_; }

    // Color space of the project (project.json "rendering": {"colorSpace"}, render/ColorSpace.h): with linear
    // lighting the renderers decode authored colors and color textures, and 8-bit environment images are
    // decoded when they load (an .hdr image is linear already). Changing it drops the environments.
    void SetLinearColor(bool linear) {
        if (linear != linearColor_) environments_.clear();
        linearColor_ = linear;
    }
    bool LinearColor() const { return linearColor_; }

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
        int64_t sidecarMtime = 0;  // <file>.events.json of a model, <file>.texture.json of an image (creation/deletion too)
        std::map<std::string, int64_t> dependencies;  // model clips sidecar and external sources
    };
    Entry<Mesh> LoadMesh(const std::string& path);
    Entry<Texture> LoadTexture(const std::string& path);
    Entry<EnvironmentMap> LoadEnvironment(const std::string& path);
    Entry<Material> LoadMaterial(const std::string& path);
    Entry<ShaderGraph> LoadShader(const std::string& path);
    Entry<Tileset> LoadTileset(const std::string& path);
    Entry<VfxAsset> LoadVfx(const std::string& path);
    Entry<Json> LoadAtlas(const std::string& path);
    Entry<Json> LoadGeneratedSprite(const std::string& source);
    Entry<Json> LoadAnimation(const std::string& path);
    Entry<Skeleton2DDef> LoadSkeleton2D(const std::string& path);
    struct FontEntry {
        std::shared_ptr<FontFace> asset;
        std::string error;
        int64_t mtime = 0;
    };
    FontEntry LoadFont(const std::string& path);

    Engine& engine_;
    std::map<std::string, Entry<Mesh>> meshes_;
    std::map<std::string, Entry<Texture>> textures_;
    std::map<std::string, Entry<EnvironmentMap>> environments_;
    bool linearColor_ = false;
    std::map<EntityId, std::shared_ptr<const EnvironmentMap>> probes_;
    bool deferProbeEdits_ = false, probesStale_ = false;
    const Scene* probeScene_ = nullptr;
    std::map<std::string, Entry<Material>> materials_;
    std::vector<std::string> materialsLoading_;  // the nextPass chain being loaded (cycle detection)
    std::map<std::string, Entry<ShaderGraph>> shaders_;
    std::map<std::string, Entry<Tileset>> tilesets_;
    std::map<std::string, Entry<Json>> animations_;
    std::map<std::string, Entry<Skeleton2DDef>> skeletons2D_;
    std::map<std::string, Entry<Json>> atlases_;
    std::map<std::string, Entry<Json>> generatedSprites_;  // Sprite.asset image forms ("hero.png?columns=6")
    std::map<std::string, Entry<VfxAsset>> vfx_;
    std::map<std::string, FontEntry> fonts_;
};

}  // namespace oe
