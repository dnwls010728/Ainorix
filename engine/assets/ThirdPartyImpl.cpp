// Implementation units of the vendored single-header libraries, compiled
// once here with their warnings silenced (third-party code, unmodified).
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO_WRITE
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#include "stb_image.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
