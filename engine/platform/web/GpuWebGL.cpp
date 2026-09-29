// WebGL2 device for GpuRenderer on the web platform: a context on the page's
// <canvas id="canvas"> (sokol_gfx GLES3 backend, see gl/SokolGL.cpp). The
// browser presents the canvas after each animation frame.
#include <emscripten/html5.h>

#include <string>

#include "platform/Platform.h"
#include "platform/gl/SokolGL.h"
#include "render/GpuDevice.h"

namespace oe {

namespace {

class WebGLDevice final : public GpuDevice {
public:
    bool Init(std::string* error) {
        EmscriptenWebGLContextAttributes attrs;
        emscripten_webgl_init_context_attributes(&attrs);
        attrs.majorVersion = 2;
        attrs.minorVersion = 0;
        attrs.alpha = false;
        attrs.depth = false;      // depth lives in GpuRenderer's offscreen targets
        attrs.stencil = false;
        attrs.antialias = false;  // MSAA happens in the scene pass
        attrs.premultipliedAlpha = false;
        attrs.preserveDrawingBuffer = false;
        attrs.powerPreference = EM_WEBGL_POWER_PREFERENCE_HIGH_PERFORMANCE;
        context_ = emscripten_webgl_create_context("#canvas", &attrs);
        if (context_ <= 0) {
            if (error) *error = "this browser has no WebGL2";
            return false;
        }
        emscripten_webgl_make_context_current(context_);
        return true;
    }

    ~WebGLDevice() override {
        if (context_ > 0) emscripten_webgl_destroy_context(context_);
    }

    const char* Name() const override { return "webgl2"; }

    sg_environment Environment() const override {
        sg_environment env{};
        env.defaults.color_format = SG_PIXELFORMAT_RGBA8;
        env.defaults.depth_format = SG_PIXELFORMAT_NONE;
        env.defaults.sample_count = 1;
        return env;
    }

    sg_swapchain Swapchain() override {
        sg_swapchain sc{};
        int w = 0, h = 0;
        emscripten_webgl_get_drawing_buffer_size(context_, &w, &h);
        if (w <= 0 || h <= 0) {
            sc.invalid = true;
            return sc;
        }
        sc.width = w;
        sc.height = h;
        sc.sample_count = 1;
        sc.color_format = SG_PIXELFORMAT_RGBA8;
        sc.depth_format = SG_PIXELFORMAT_NONE;
        sc.gl.framebuffer = 0;
        return sc;
    }

    void Present() override {}

    bool ReadPixels(sg_image image, int width, int height, uint32_t* out) override { return GLReadPixels(image, width, height, out); }

private:
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE context_ = 0;
};

}  // namespace

std::unique_ptr<GpuDevice> CreateGpuDevice(Window* window, std::string* error) {
    if (!window) {
        if (error) *error = "the web platform renders into the page canvas only (create the window first)";
        return nullptr;
    }
    auto device = std::make_unique<WebGLDevice>();
    if (!device->Init(error)) return nullptr;
    return device;
}

}  // namespace oe
