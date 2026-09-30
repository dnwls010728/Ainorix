#include "core/Image.h"

#include <cstdio>
#include <cstring>

namespace oe {

namespace {

uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

uint32_t Adler32(const uint8_t* data, size_t size) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < size; ++i) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

class BitWriter {
public:
    explicit BitWriter(std::vector<uint8_t>& out) : out_(out) {}
    void Put(uint32_t value, int bits) {
        buffer_ |= static_cast<uint64_t>(value) << count_;
        count_ += bits;
        while (count_ >= 8) {
            out_.push_back(static_cast<uint8_t>(buffer_ & 0xFF));
            buffer_ >>= 8;
            count_ -= 8;
        }
    }
    // Huffman codes are defined MSB first; the stream is LSB first.
    void PutCode(uint32_t code, int bits) {
        uint32_t rev = 0;
        for (int i = 0; i < bits; ++i) rev |= ((code >> i) & 1u) << (bits - 1 - i);
        Put(rev, bits);
    }
    void Flush() {
        if (count_ > 0) out_.push_back(static_cast<uint8_t>(buffer_ & 0xFF));
        buffer_ = 0;
        count_ = 0;
    }

private:
    std::vector<uint8_t>& out_;
    uint64_t buffer_ = 0;
    int count_ = 0;
};

const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                               35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void PutLiteralOrLength(BitWriter& bw, int sym) {
    if (sym <= 143) bw.PutCode(0x30 + sym, 8);
    else if (sym <= 255) bw.PutCode(0x190 + (sym - 144), 9);
    else if (sym <= 279) bw.PutCode(sym - 256, 7);
    else bw.PutCode(0xC0 + (sym - 280), 8);
}

void PutMatch(BitWriter& bw, int length, int distance) {
    int li = 28;
    for (int i = 0; i < 29; ++i) {
        if (i == 28 || kLenBase[i + 1] > length) { li = i; break; }
    }
    PutLiteralOrLength(bw, 257 + li);
    if (kLenExtra[li]) bw.Put(static_cast<uint32_t>(length - kLenBase[li]), kLenExtra[li]);
    int di = 29;
    for (int i = 0; i < 30; ++i) {
        if (i == 29 || kDistBase[i + 1] > distance) { di = i; break; }
    }
    bw.PutCode(static_cast<uint32_t>(di), 5);
    if (kDistExtra[di]) bw.Put(static_cast<uint32_t>(distance - kDistBase[di]), kDistExtra[di]);
}

// zlib stream: single fixed-Huffman deflate block with greedy hash-chain LZ77.
std::vector<uint8_t> ZlibCompress(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out;
    out.reserve(data.size() / 4 + 64);
    out.push_back(0x78);
    out.push_back(0x01);
    BitWriter bw(out);
    bw.Put(1, 1);  // BFINAL
    bw.Put(1, 2);  // BTYPE = fixed Huffman

    constexpr int kHashBits = 15;
    constexpr int kWindow = 32768;
    constexpr int kMaxChain = 32;
    std::vector<int> head(1 << kHashBits, -1);
    std::vector<int> prev(data.size(), -1);
    const size_t n = data.size();
    auto hashAt = [&](size_t i) {
        uint32_t h = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8) | data[i + 2];
        return (h * 2654435761u) >> (32 - kHashBits);
    };
    auto insert = [&](size_t i) {
        if (i + 2 >= n) return;
        uint32_t h = hashAt(i);
        prev[i] = head[h];
        head[h] = static_cast<int>(i);
    };

    size_t i = 0;
    while (i < n) {
        int bestLen = 0, bestDist = 0;
        if (i + 2 < n) {
            int cand = head[hashAt(i)];
            int chain = 0;
            while (cand >= 0 && static_cast<int>(i) - cand <= kWindow && chain++ < kMaxChain) {
                size_t maxLen = n - i < 258 ? n - i : 258;
                size_t len = 0;
                while (len < maxLen && data[static_cast<size_t>(cand) + len] == data[i + len]) ++len;
                if (static_cast<int>(len) > bestLen) {
                    bestLen = static_cast<int>(len);
                    bestDist = static_cast<int>(i) - cand;
                    if (len == maxLen) break;
                }
                cand = prev[static_cast<size_t>(cand)];
            }
        }
        if (bestLen >= 3) {
            PutMatch(bw, bestLen, bestDist);
            for (int k = 0; k < bestLen; ++k) insert(i + static_cast<size_t>(k));
            i += static_cast<size_t>(bestLen);
        } else {
            PutLiteralOrLength(bw, data[i]);
            insert(i);
            ++i;
        }
    }
    PutLiteralOrLength(bw, 256);  // end of block
    bw.Flush();
    uint32_t adler = Adler32(data.data(), data.size());
    out.push_back(static_cast<uint8_t>(adler >> 24));
    out.push_back(static_cast<uint8_t>(adler >> 16));
    out.push_back(static_cast<uint8_t>(adler >> 8));
    out.push_back(static_cast<uint8_t>(adler));
    return out;
}

void PutU32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void PutChunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& payload) {
    PutU32(out, static_cast<uint32_t>(payload.size()));
    size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), payload.begin(), payload.end());
    PutU32(out, Crc32(out.data() + start, out.size() - start));
}

}  // namespace

std::vector<uint8_t> EncodePng(const Image& image, bool alpha) {
    const int w = image.width, h = image.height;
    const size_t channels = alpha ? 4 : 3;
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(h) * (static_cast<size_t>(w) * channels + 1));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);  // filter: none
        const uint8_t* row = image.rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 4;
        for (int x = 0; x < w; ++x) {
            raw.push_back(row[x * 4 + 0]);
            raw.push_back(row[x * 4 + 1]);
            raw.push_back(row[x * 4 + 2]);
            if (alpha) raw.push_back(row[x * 4 + 3]);
        }
    }
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> ihdr;
    PutU32(ihdr, static_cast<uint32_t>(w));
    PutU32(ihdr, static_cast<uint32_t>(h));
    ihdr.push_back(8);  // bit depth
    ihdr.push_back(alpha ? 6 : 2);  // color type RGBA / RGB
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    PutChunk(png, "IHDR", ihdr);
    PutChunk(png, "IDAT", ZlibCompress(raw));
    PutChunk(png, "IEND", {});
    return png;
}

bool WritePng(const std::string& path, const Image& image, bool alpha) {
    std::vector<uint8_t> png = EncodePng(image, alpha);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t written = std::fwrite(png.data(), 1, png.size(), f);
    std::fclose(f);
    return written == png.size();
}

std::string Base64Encode(const uint8_t* data, size_t size) {
    static const char* kTable = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    size_t i = 0;
    while (i + 2 < size) {
        uint32_t v = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8) | data[i + 2];
        out += kTable[(v >> 18) & 63];
        out += kTable[(v >> 12) & 63];
        out += kTable[(v >> 6) & 63];
        out += kTable[v & 63];
        i += 3;
    }
    if (i < size) {
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < size) v |= static_cast<uint32_t>(data[i + 1]) << 8;
        out += kTable[(v >> 18) & 63];
        out += kTable[(v >> 12) & 63];
        out += i + 1 < size ? kTable[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

uint64_t Fnv1a64(const uint8_t* data, size_t size) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) {
        h ^= data[i];
        h *= 1099511628211ull;
    }
    return h;
}

}  // namespace oe
