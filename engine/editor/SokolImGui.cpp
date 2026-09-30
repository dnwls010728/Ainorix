// sokol_imgui implementation (SOKOL_IMGUI_NO_SOKOL_APP is set by CMake): draws Dear ImGui through sokol_gfx (the same
// device the GpuRenderer uses). The graphics API define (SOKOL_D3D11 or
// SOKOL_GLES3) comes from CMakeLists.txt and only selects which embedded
// shader sokol_imgui uses; sokol_gfx itself is implemented in engine/platform/.
#include "imgui.h"
#include "sokol_gfx.h"

#define SOKOL_IMGUI_IMPL
#include "sokol_imgui.h"
