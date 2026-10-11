#pragma once
#include <memory>
#include <string>

#include "render/Renderer.h"
#include "sokol_gfx.h"

namespace oe {

class AssetManager;
class GpuDevice;
struct Material;
struct Mesh;
struct Texture;

// Hardware renderer on top of sokol_gfx (engine/render/shaders/Shaders.glsl).
// Draws the same scene description as SoftwareRenderer (RenderScene.h) with
// 4x MSAA, mipmapped textures, filtered shadow maps and full-resolution UI.
// Frame layout: shadow pass -> scene pass (MSAA) -> selection mask (editor)
// -> composite pass (post effects go here) + UI.
//
// Output is not bit-exact across GPUs, so frame hashes, tests and picking use
// the software renderer; this one is for what people look at (game window,
// editor viewport, `render.screenshot {renderer:"gpu"}`).
class GpuRenderer final : public IRenderer {
public:
    struct Settings {
        int msaa = 4;               // scene pass sample count (1 = off)
        int shadowMapSize = 2048;
        // Where devices that compile shader source themselves (D3D11) keep compiled programs,
        // keyed by a hash of the source; empty = no disk cache. The editor uses <project>/.oe/shader-cache.
        std::string shaderCacheDir;
        // Shader graph programs compile on worker threads; until one is ready its materials draw
        // with the interpreter (same image, slower shader). Off: compiled on first use (tests, screenshots).
        bool asyncShaders = false;
    };

    // Calls sg_setup() with the device's environment; the device must outlive the renderer.
    GpuRenderer(GpuDevice& device, AssetManager* assets, const Settings& settings);
    GpuRenderer(GpuDevice& device, AssetManager* assets) : GpuRenderer(device, assets, Settings()) {}
    ~GpuRenderer() override;

    const char* Name() const override { return name_.c_str(); }
    // Renders offscreen and reads the image back into target.color. Depth and
    // entity ids are not produced (use the software renderer for picking).
    RenderStats Render(const Scene& scene, const RenderView& view, RenderTarget& target) override;
    // Renders into the device's window swapchain and presents. The 3D image is
    // drawn at `renderScale` (0.25..1) of the window size and upscaled; UI is
    // always drawn at full resolution. Returns false if the window is minimized.
    bool RenderToWindow(const Scene& scene, const RenderView& view, float renderScale, RenderStats* stats = nullptr);
    // Renders into an offscreen texture kept per `slot` (resized as needed) and
    // returns its view for sampling, e.g. as an ImGui image in the native
    // editor. The frame is committed; nothing is read back or presented.
    sg_view RenderToTexture(const Scene& scene, const RenderView& view, int width, int height, int slot, RenderStats* stats = nullptr);
    // Reads the last RenderToTexture image of `slot` back to the CPU (tests, screenshots).
    bool ReadTexture(int slot, RenderTarget& target);
    // A texture as a sampleable view (e.g. tileset thumbnails in the editor), kept while `texture` lives.
    sg_view ImageView(const std::shared_ptr<const Texture>& texture);
    // Builds now what the first draw of these would build (asset.preload): texture and mesh uploads, and
    // for a material with a shader graph its program and scene pipelines (with asyncShaders the compile
    // is only started). Kept as long as the assets live, like everything drawn.
    void Preload(const std::vector<std::shared_ptr<const Material>>& materials,
                 const std::vector<std::shared_ptr<const Texture>>& textures,
                 const std::vector<std::shared_ptr<const Mesh>>& meshes);
    // Release cached copies whose CPU owners expired. Call between frames, before creating UI image views.
    void CollectGarbage();
    // Size of the window swapchain (0 when headless or minimized).
    void WindowSize(int* width, int* height);

    GpuDevice& Device() { return device_; }

private:
    struct Impl;
    GpuDevice& device_;
    std::unique_ptr<Impl> impl_;
    std::string name_;
};

}  // namespace oe
