#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "sokol_gfx.h"

namespace oe {

// A graphics device provided by the platform layer (D3D11 on Windows, WebGL2
// on the web, GLES3 over EGL on Linux). GpuRenderer only talks to sokol_gfx
// plus this interface; everything API specific lives in engine/platform/.
// Create one with CreateGpuDevice() (platform/Platform.h). There is at most
// one device per process because sokol_gfx is a global.
class GpuDevice {
public:
    virtual ~GpuDevice() = default;
    // Backend name, e.g. "d3d11", "d3d11-warp", "webgl2", "gles3-egl".
    virtual const char* Name() const = 0;
    virtual sg_environment Environment() const = 0;
    // Surface of the window the device presents to, sized to the window's
    // client area. `invalid` is set for headless devices and minimized windows.
    virtual sg_swapchain Swapchain() = 0;
    // Shows the frame drawn into Swapchain() (no-op on the web).
    virtual void Present() = 0;
    // Hands the commands recorded so far to the GPU so it starts on them while the CPU goes on
    // (an offscreen frame has no Present to do that). No-op where the API flushes by itself.
    virtual void Flush() {}
    // Copies a single-sampled RGBA8 color-attachment image into `out`
    // (width*height pixels, top row first, 0xAABBGGRR like RenderTarget).
    virtual bool ReadPixels(sg_image image, int width, int height, uint32_t* out) = 0;
    // Compiles one shader stage from source into bytecode the backend accepts (D3D11: HLSL to
    // DXBC) so GpuRenderer can cache programs on disk and compile them off the main thread. Safe to
    // call from any thread. False where the backend only takes source (GL) or on a compile error
    // (`error` gets the compiler output).
    virtual bool CompileShader(const std::string& source, const char* target, const char* entry,
                               std::vector<uint8_t>* bytecode, std::string* error) const {
        (void)source; (void)target; (void)entry; (void)bytecode; (void)error;
        return false;
    }
    // Names the compiler and its options for shader cache keys; empty = no CompileShader.
    virtual std::string ShaderCompilerId() const { return ""; }
};

}  // namespace oe
