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
#include "stb_image.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "core/Image.h"

namespace oe {

std::vector<uint8_t> EncodeJpeg(const Image& image, int quality) {
    std::vector<uint8_t> out;
    if (image.width <= 0 || image.height <= 0) return out;
    out.reserve(static_cast<size_t>(image.width) * static_cast<size_t>(image.height) / 4);
    auto write = [](void* context, void* data, int size) {
        auto* v = static_cast<std::vector<uint8_t>*>(context);
        v->insert(v->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
    };
    stbi_write_jpg_to_func(write, &out, image.width, image.height, 4, image.rgba.data(), quality < 1 ? 1 : (quality > 100 ? 100 : quality));
    return out;
}

}  // namespace oe
