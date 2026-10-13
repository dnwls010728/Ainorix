#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/Image.h"
#include "core/Math.h"
#include "scene/Scene.h"
#include "scene/Components.h"
#include "render/Decals.h"

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
    float shaderTime = 0;     // fixed simulation seconds supplied by the engine/tools
    // The game is being played (Engine::InPlaySession: running or paused), not its scene edited.
    // Graphs read it as the reserved uniform `playing`, for effects that would be in the way while
    // editing (a world that bends away from the camera).
    bool playing = false;
    // Editor-only fill light (0 = off). A scene lit by its gameplay scripts (a night level, a
    // day/night cycle) is near-black while it is edited because the scripts are not running; this
    // raises the ambient floor so geometry stays visible to edit. It never affects the shipped game
    // or the Game view, only the editor's Scene view when the user turns the Light overlay on.
    float editorFill = 0.0f;
    // Editor overlays.
    bool drawGrid = false;
    EntityId highlight = kNullEntity;
    std::vector<DebugLine> lines;  // drawn after geometry (depth tested)
    bool drawUI = true;            // screen-space UIText/UIPanel/UIButton
    // Reflection probe pictures (software renderer only): when set, the frame is the scenery alone
    // (see ProbeShows), lit without probes, and the lit colors before any post effect are written
    // here (width * height, values above 1 kept); the target's color is not meaningful.
    std::vector<Color>* sceneColor = nullptr;
    bool reflectionCapture = false;  // Internal planar capture: prevents recursion, retains dynamic scenery.
    EntityId excludeEntity = kNullEntity;  // Internal capture receiver excluded from all draw/shadow passes.
};

struct RenderStats {
    int drawnEntities = 0;
    int triangles = 0;
    double milliseconds = 0.0;
    int drawCalls = 0;  // GPU renderer: mesh draws of the scene pass after culling (0 for the software renderer)
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
    DecalAtlas decalAtlas_;
};

// Upper bound for the software renderer's worker threads (default 16). The
// output is identical for any value; tests use it to prove that.
void SetMaxRenderThreads(int threads);

// Builds the same render matrices for an explicit camera; false if it lacks Camera.
bool MakeCameraView(const Scene& scene, EntityId camera, float aspect, RenderView& out);

// Builds the view from the scene's first active camera. Returns false (and a
// sensible default view) when the scene has no camera.
bool MakeSceneView(const Scene& scene, float aspect, RenderView& out);
// Orbit-style view used by the editor and by agents that want an arbitrary viewpoint.
RenderView MakeLookAtView(const Vec3& eye, const Vec3& target, float fovDeg, float aspect);

}  // namespace oe
