// Implementation units of the vendored single-header libraries, compiled
// once here with their warnings silenced (third-party code, unmodified).
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO_WRITE
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_ONLY_HDR
#include "stb_image.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

// ufbx: FBX only, read from memory (no file access of its own); the unused evaluators are left out.
#define UFBX_NO_STDIO
#define UFBX_NO_FORMAT_OBJ
#define UFBX_NO_SUBDIVISION
#define UFBX_NO_TESSELLATION
#define UFBX_NO_GEOMETRY_CACHE
#define UFBX_NO_SCENE_EVALUATION
#define UFBX_NO_SKINNING_EVALUATION
#include "ufbx.c"

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
