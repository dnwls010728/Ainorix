// OpenGL ES 3 device for GpuRenderer on Android: one EGL context for the
// whole run, plus a window surface that follows the activity's window (it is
// destroyed when the app goes to the background and recreated on return, so
// GPU resources survive). sokol_gfx GLES3 backend, see gl/SokolGL.cpp.
#include <EGL/egl.h>
#include <android/native_window.h>

#include <string>

#include "platform/Platform.h"
#include "platform/android/AndroidApp.h"
#include "platform/gl/SokolGL.h"
#include "render/GpuDevice.h"

namespace oe {

namespace {

class AndroidGlesDevice final : public GpuDevice {
public:
    bool Init(std::string* error) {
        display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display_ == EGL_NO_DISPLAY || !eglInitialize(display_, nullptr, nullptr)) return Fail(error, "no EGL display");
        const EGLint configAttrs[] = {EGL_RENDERABLE_TYPE, 0x40 /* EGL_OPENGL_ES3_BIT_KHR */, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                                      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 0,
                                      EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0, EGL_NONE};
        EGLint count = 0;
        if (!eglChooseConfig(display_, configAttrs, &config_, 1, &count) || count < 1) return Fail(error, "no RGB8 OpenGL ES 3 config");
        const EGLint contextAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, contextAttrs);
        if (context_ == EGL_NO_CONTEXT) return Fail(error, "cannot create an OpenGL ES 3 context");
        // The surface goes away with the window: release it before the window dies.
        AndroidSetSurfaceLostHandler([this] { DestroySurface(); });
        if (!UpdateSurface()) return Fail(error, "cannot create a window surface");
        return true;
    }

    ~AndroidGlesDevice() override {
        AndroidSetSurfaceLostHandler(nullptr);
        if (display_ == EGL_NO_DISPLAY) return;
        DestroySurface();
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
        eglTerminate(display_);
    }

    const char* Name() const override { return "gles3-android"; }

    sg_environment Environment() const override {
        sg_environment env{};
        env.defaults.color_format = SG_PIXELFORMAT_RGBA8;
        env.defaults.depth_format = SG_PIXELFORMAT_NONE;
        env.defaults.sample_count = 1;
        return env;
    }

    sg_swapchain Swapchain() override {
        sg_swapchain sc{};
        EGLint w = 0, h = 0;
        if (!UpdateSurface() || surface_ == EGL_NO_SURFACE || !eglQuerySurface(display_, surface_, EGL_WIDTH, &w) ||
            !eglQuerySurface(display_, surface_, EGL_HEIGHT, &h) || w <= 0 || h <= 0) {
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

    void Present() override {
        if (surface_ == EGL_NO_SURFACE) return;
        if (!eglSwapBuffers(display_, surface_) && eglGetError() == EGL_BAD_SURFACE) DestroySurface();  // recreated next frame
    }

    bool ReadPixels(sg_image image, int width, int height, uint32_t* out) override { return GLReadPixels(image, width, height, out); }

private:
    bool Fail(std::string* error, const char* message) {
        if (error) *error = std::string("EGL: ") + message;
        return false;
    }

    // Makes the context current on the activity's current window (a new
    // window after rotation or returning from the background gets a new
    // surface). Without a window the context stays current surfaceless so
    // resources can still be created.
    bool UpdateSurface() {
        ANativeWindow* window = AndroidCurrentWindow();
        if (window != surfaceWindow_) {
            DestroySurface();
            if (window) {
                EGLint format = 0;
                eglGetConfigAttrib(display_, config_, EGL_NATIVE_VISUAL_ID, &format);
                ANativeWindow_setBuffersGeometry(window, 0, 0, format);
                surface_ = eglCreateWindowSurface(display_, config_, window, nullptr);
                if (surface_ == EGL_NO_SURFACE) return false;
                surfaceWindow_ = window;
                current_ = false;  // bind the new surface below
            }
        }
        if (!current_) {
            if (!eglMakeCurrent(display_, surface_, surface_, context_)) return false;
            current_ = true;
            if (surface_ != EGL_NO_SURFACE) eglSwapInterval(display_, 1);
        }
        return true;
    }

    void DestroySurface() {
        if (surface_ != EGL_NO_SURFACE) {
            // Keep the context current without a surface (EGL_KHR_surfaceless_context, every GLES3 device).
            eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, context_);
            eglDestroySurface(display_, surface_);
            surface_ = EGL_NO_SURFACE;
            current_ = false;
        }
        surfaceWindow_ = nullptr;
    }

    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLConfig config_ = nullptr;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;
    ANativeWindow* surfaceWindow_ = nullptr;
    bool current_ = false;
};

}  // namespace

std::unique_ptr<GpuDevice> CreateGpuDevice(Window* window, std::string* error) {
    if (!window) {
        if (error) *error = "the Android platform renders into the activity window only (create the window first)";
        return nullptr;
    }
    auto device = std::make_unique<AndroidGlesDevice>();
    if (!device->Init(error)) return nullptr;
    return device;
}

}  // namespace oe
