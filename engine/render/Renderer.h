#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/Image.h"
#include "core/Math.h"
#include "scene/Scene.h"
#include "scene/Components.h"

namespace oe {

// CPU-side frame: color (RGBA8), depth and an entity-id buffer used for
// picking. GPU backends resolve into the same structure so screenshots and
// picking behave identically on every platform.
struct RenderTarget {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> color;  // 0xAABBGGRR (bytes R,G,B,A in memory)
    std::vector<float> depth;
    std::vector<EntityId> ids;

    void Resize(int w, int h);
    Image ToImage() const;
    EntityId IdAt(int x, int y) const;
    uint64_t Hash() const;
};

struct DebugLine {
    Vec3 a, b;
    Color color;
};

struct RenderView {
    Mat4 view;
    Mat4 proj;
    Vec3 eye;
    Color clearColor{0.12f, 0.14f, 0.18f};
    EntityId cameraEntity = kNullEntity;
    PostProcess postProcess;  // selected camera's effects; defaults leave scene color unchanged
    // Editor overlays.
    bool drawGrid = false;
    EntityId highlight = kNullEntity;
    std::vector<DebugLine> lines;  // drawn after geometry (depth tested)
    bool drawUI = true;            // screen-space UIText/UIPanel/UIButton
};

struct RenderStats {
    int drawnEntities = 0;
    int triangles = 0;
    double milliseconds = 0.0;
};

// Rendering backend interface. The software rasterizer is the reference
// implementation and works on every platform (and headless). Hardware
// backends (D3D12, Vulkan, Metal, WebGPU, console APIs) implement the same
// interface; see docs/PLATFORMS.md.
class IRenderer {
public:
    virtual ~IRenderer() = default;
    virtual const char* Name() const = 0;
    virtual RenderStats Render(const Scene& scene, const RenderView& view, RenderTarget& target) = 0;
};

class AssetManager;

class SoftwareRenderer final : public IRenderer {
public:
    // `assets` resolves model files and textures; null = built-in meshes only.
    explicit SoftwareRenderer(AssetManager* assets = nullptr) : assets_(assets) {}
    const char* Name() const override { return "software"; }
    RenderStats Render(const Scene& scene, const RenderView& view, RenderTarget& target) override;

private:
    AssetManager* assets_;
};

// Upper bound for the software renderer's worker threads (default 16). The
// output is identical for any value; tests use it to prove that.
void SetMaxRenderThreads(int threads);

// Builds the view from the scene's first active camera. Returns false (and a
// sensible default view) when the scene has no camera.
bool MakeSceneView(const Scene& scene, float aspect, RenderView& out);
// Orbit-style view used by the editor and by agents that want an arbitrary viewpoint.
RenderView MakeLookAtView(const Vec3& eye, const Vec3& target, float fovDeg, float aspect);

}  // namespace oe
