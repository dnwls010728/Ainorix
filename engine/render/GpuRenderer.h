#pragma once
#include <memory>
#include <string>

#include "render/Renderer.h"

namespace oe {

class AssetManager;
class GpuDevice;

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
