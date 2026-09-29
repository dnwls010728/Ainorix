#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace oe {

// 8-bit RGBA image, rows top to bottom.
struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
};

// PNG encoder (RGB, deflate with fixed Huffman + LZ77). No dependencies.
std::vector<uint8_t> EncodePng(const Image& image);
bool WritePng(const std::string& path, const Image& image);

// Baseline JPEG (stb_image_write), quality 1..100. Lossy and much faster
// than PNG; used to stream the editor viewport.
std::vector<uint8_t> EncodeJpeg(const Image& image, int quality);

std::string Base64Encode(const uint8_t* data, size_t size);
inline std::string Base64Encode(const std::vector<uint8_t>& data) { return Base64Encode(data.data(), data.size()); }

// FNV-1a 64-bit hash, used for deterministic frame fingerprints.
uint64_t Fnv1a64(const uint8_t* data, size_t size);

}  // namespace oe
