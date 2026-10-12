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

// PNG encoder (RGB, or RGBA with `alpha`; deflate with fixed Huffman + LZ77). No dependencies.
std::vector<uint8_t> EncodePng(const Image& image, bool alpha = false);
bool WritePng(const std::string& path, const Image& image, bool alpha = false);

// zlib streams (RFC 1950/1951). ZlibPack is the PNG encoder's compressor: one fixed-Huffman block
// with greedy LZ77, deterministic. ZlibUnpack reads any zlib stream (stored, fixed and dynamic
// blocks) whose plain size is known; false when the stream is damaged or the size does not match.
std::vector<uint8_t> ZlibPack(const std::vector<uint8_t>& data);
bool ZlibUnpack(const uint8_t* data, size_t size, size_t plainSize, std::vector<uint8_t>& out);


std::string Base64Encode(const uint8_t* data, size_t size);
inline std::string Base64Encode(const std::vector<uint8_t>& data) { return Base64Encode(data.data(), data.size()); }

// FNV-1a 64-bit hash, used for deterministic frame fingerprints.
uint64_t Fnv1a64(const uint8_t* data, size_t size);

}  // namespace oe
