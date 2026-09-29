// Headless GPU device for Linux: an OpenGL ES 3 context over EGL without a
// window (Mesa's surfaceless platform, or a 1x1 pbuffer). Used for the
// editor viewport and GPU screenshots on servers and CI; with Mesa's
// llvmpipe it works without any GPU.
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <string>

#include "platform/Platform.h"
#include "platform/gl/SokolGL.h"
#include "render/GpuDevice.h"

namespace oe {

namespace {

class EglDevice final : public GpuDevice {
public:
    bool Init(std::string* error) {
        auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
        if (getPlatformDisplay) display_ = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        EGLint major = 0, minor = 0;
        if (display_ == EGL_NO_DISPLAY || !eglInitialize(display_, &major, &minor)) {
            display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
            if (display_ == EGL_NO_DISPLAY || !eglInitialize(display_, &major, &minor)) return Fail(error, "no EGL display");
        }
        if (!eglBindAPI(EGL_OPENGL_ES_API)) return Fail(error, "EGL has no OpenGL ES");
        const EGLint configAttrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
        EGLConfig config = nullptr;
        EGLint count = 0;
        eglChooseConfig(display_, configAttrs, &config, 1, &count);
        const EGLint contextAttrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
        if (count > 0) {
            const EGLint pbufferAttrs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
            surface_ = eglCreatePbufferSurface(display_, config, pbufferAttrs);
            context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, contextAttrs);
        } else {
            // Surfaceless platforms may expose no pbuffer configs.
            context_ = eglCreateContext(display_, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, contextAttrs);
        }
        if (context_ == EGL_NO_CONTEXT) return Fail(error, "cannot create an OpenGL ES 3 context");
        if (!eglMakeCurrent(display_, surface_, surface_, context_)) return Fail(error, "eglMakeCurrent failed");
        return true;
    }

    ~EglDevice() override {
        if (display_ == EGL_NO_DISPLAY) return;
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
        if (surface_ != EGL_NO_SURFACE) eglDestroySurface(display_, surface_);
        eglTerminate(display_);
    }

    const char* Name() const override { return "gles3-egl"; }

    sg_environment Environment() const override {
        sg_environment env{};
        env.defaults.color_format = SG_PIXELFORMAT_RGBA8;
        env.defaults.depth_format = SG_PIXELFORMAT_NONE;
        env.defaults.sample_count = 1;
        return env;
    }

    sg_swapchain Swapchain() override {
        sg_swapchain sc{};
        sc.invalid = true;
        return sc;
    }

    void Present() override {}

    bool ReadPixels(sg_image image, int width, int height, uint32_t* out) override { return GLReadPixels(image, width, height, out); }

private:
    bool Fail(std::string* error, const char* message) {
        if (error) *error = std::string("EGL: ") + message;
        return false;
    }

    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLSurface surface_ = EGL_NO_SURFACE;
    EGLContext context_ = EGL_NO_CONTEXT;
};

}  // namespace

std::unique_ptr<GpuDevice> CreateGpuDevice(Window* window, std::string* error) {
    if (window) {
        if (error) *error = "the headless platform has no windows to present to";
        return nullptr;
    }
    auto device = std::make_unique<EglDevice>();
    if (!device->Init(error)) return nullptr;
    return device;
}

}  // namespace oe
