// sokol_gfx implementation for the GLES3 / WebGL2 backend (Linux EGL and the
// web platform), plus the GL readback used by their GpuDevice::ReadPixels.
#include "platform/gl/SokolGL.h"

#include <cstring>
#include <vector>

// The implementation goes last: sokol_gfx.h has no include guard around it.
#define SOKOL_IMPL
#define SOKOL_GLES3
#include "sokol_gfx.h"

namespace oe {

bool GLReadPixels(sg_image image, int width, int height, uint32_t* out) {
    sg_gl_image_info info = sg_gl_query_image_info(image);
    GLuint tex = info.tex[info.active_slot];
    if (!tex || width <= 0 || height <= 0) return false;
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok) {
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, out);
        ok = glGetError() == GL_NO_ERROR;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    sg_reset_state_cache();
    if (!ok) return false;
    // GL rows start at the bottom; RenderTarget rows start at the top.
    std::vector<uint32_t> row(static_cast<size_t>(width));
    const size_t rowBytes = static_cast<size_t>(width) * sizeof(uint32_t);
    for (int y = 0; y < height / 2; ++y) {
        uint32_t* a = out + static_cast<size_t>(y) * static_cast<size_t>(width);
        uint32_t* b = out + static_cast<size_t>(height - 1 - y) * static_cast<size_t>(width);
        std::memcpy(row.data(), a, rowBytes);
        std::memcpy(a, b, rowBytes);
        std::memcpy(b, row.data(), rowBytes);
    }
    return true;
}

}  // namespace oe
