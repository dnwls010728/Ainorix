// Builds without a GPU backend (no EGL found): sokol_gfx is compiled with its
// dummy backend so GpuRenderer links, and no device can be created, so the
// engine always uses the software renderer.
#include <string>

#include "platform/Platform.h"
#include "render/GpuDevice.h"

#define SOKOL_IMPL
#define SOKOL_DUMMY_BACKEND
#include "sokol_gfx.h"

namespace oe {

std::unique_ptr<GpuDevice> CreateGpuDevice(Window*, std::string* error) {
    if (error) *error = "this build has no GPU backend (Linux builds need EGL + OpenGL ES 3 development files)";
    return nullptr;
}

}  // namespace oe
