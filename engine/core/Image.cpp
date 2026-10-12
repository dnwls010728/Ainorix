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

// LSB-first bit reader over a deflate stream; `failed` once it ran past the end.
struct BitReader {
    const uint8_t* data;
    size_t size, pos = 0;
    uint64_t buffer = 0;
    int count = 0;
    bool failed = false;
    void Fill() {
        while (count <= 56 && pos < size) {
            buffer |= static_cast<uint64_t>(data[pos++]) << count;
            count += 8;
        }
    }
    uint32_t Bits(int need) {
        if (count < need) Fill();
        if (count < need) {
            failed = true;
            return 0;
        }
        const uint32_t value = static_cast<uint32_t>(buffer & ((1ull << need) - 1));
        buffer >>= need;
        count -= need;
        return value;
    }
};

// A canonical Huffman code: codes of up to kFastBits are looked up, longer ones walked bit by bit.
struct Huffman {
    static constexpr int kFastBits = 10;
    uint16_t fast[1 << kFastBits];  // (length << 9) | symbol; 0 = longer than kFastBits
    uint16_t counts[16];
    uint16_t symbols[288];

    bool Build(const uint8_t* lengths, int n) {
        std::memset(fast, 0, sizeof(fast));
        std::memset(counts, 0, sizeof(counts));
        for (int i = 0; i < n; ++i) ++counts[lengths[i]];
        counts[0] = 0;
        int left = 1;  // codes still free at this length: negative = over-subscribed
        uint16_t offsets[16], next[16];
        offsets[1] = 0;
        uint32_t code = 0;
        for (int len = 1; len < 16; ++len) {
            left = (left << 1) - counts[len];
            if (left < 0) return false;
            if (len < 15) offsets[len + 1] = static_cast<uint16_t>(offsets[len] + counts[len]);
            next[len] = static_cast<uint16_t>(code);
            code = (code + counts[len]) << 1;
        }
        for (int i = 0; i < n; ++i) {
            const int len = lengths[i];
            if (!len) continue;
            symbols[offsets[len]++] = static_cast<uint16_t>(i);
            const uint32_t canonical = next[len]++;
            if (len > kFastBits) continue;
            uint32_t reversed = 0;
            for (int b = 0; b < len; ++b) reversed |= ((canonical >> b) & 1u) << (len - 1 - b);
            for (uint32_t at = reversed; at < (1u << kFastBits); at += 1u << len) fast[at] = static_cast<uint16_t>((len << 9) | i);
        }
        return true;
    }

    int Decode(BitReader& in) const {
        if (in.count < 15) in.Fill();
        const uint16_t entry = fast[in.buffer & ((1u << kFastBits) - 1)];
        if (entry) {
            const int len = entry >> 9;
            if (in.count < len) {
                in.failed = true;
                return -1;
            }
            in.buffer >>= len;
            in.count -= len;
            return entry & 511;
        }
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= static_cast<int>(in.Bits(1));
            if (in.failed) return -1;
            const int count = counts[len];
            if (code - count < first) return symbols[index + (code - first)];
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        return -1;
    }
};

bool InflateBlock(BitReader& in, const Huffman& lengths, const Huffman& distances, std::vector<uint8_t>& out, size_t& at) {
    for (;;) {
        const int symbol = lengths.Decode(in);
        if (symbol < 0) return false;
        if (symbol < 256) {
            if (at >= out.size()) return false;
            out[at++] = static_cast<uint8_t>(symbol);
            continue;
        }
        if (symbol == 256) return true;
        if (symbol > 285) return false;
        const size_t length = kLenBase[symbol - 257] + in.Bits(kLenExtra[symbol - 257]);
        const int code = distances.Decode(in);
        if (code < 0 || code > 29) return false;
        const size_t distance = kDistBase[code] + in.Bits(kDistExtra[code]);
        if (in.failed || distance > at || length > out.size() - at) return false;
        for (size_t i = 0; i < length; ++i, ++at) out[at] = out[at - distance];  // may overlap itself
    }
}

}  // namespace

std::vector<uint8_t> ZlibPack(const std::vector<uint8_t>& data) { return ZlibCompress(data); }

bool ZlibUnpack(const uint8_t* data, size_t size, size_t plainSize, std::vector<uint8_t>& out) {
    out.clear();
    // CMF/FLG: deflate with a window of at most 32 KB, no preset dictionary, check bits.
    if (size < 6 || (data[0] & 0x0f) != 8 || (data[0] >> 4) > 7 || (data[1] & 0x20) || ((data[0] << 8) | data[1]) % 31 != 0) return false;
    out.resize(plainSize);
    BitReader in{data + 2, size - 6};
    size_t at = 0;
    for (bool last = false; !last;) {
        last = in.Bits(1) != 0;
        const uint32_t type = in.Bits(2);
        if (in.failed) return false;
        if (type == 0) {  // stored: byte aligned, length and its complement
            in.Bits(in.count & 7);
            const uint32_t length = in.Bits(16), inverse = in.Bits(16);
            if (in.failed || (length ^ 0xffffu) != inverse || length > plainSize - at) return false;
            for (uint32_t i = 0; i < length; ++i) out[at++] = static_cast<uint8_t>(in.Bits(8));
            if (in.failed) return false;
        } else if (type == 1) {
            static const struct Fixed {
                Huffman lengths, distances;
                Fixed() {
                    uint8_t bits[288];
                    for (int i = 0; i < 288; ++i) bits[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
                    lengths.Build(bits, 288);
                    for (int i = 0; i < 30; ++i) bits[i] = 5;
                    distances.Build(bits, 30);
                }
            } fixed;
            if (!InflateBlock(in, fixed.lengths, fixed.distances, out, at)) return false;
        } else if (type == 2) {
            const int literals = static_cast<int>(in.Bits(5)) + 257, dists = static_cast<int>(in.Bits(5)) + 1, codes = static_cast<int>(in.Bits(4)) + 4;
            if (in.failed || literals > 286 || dists > 30) return false;
            static const uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t bits[320] = {};
            for (int i = 0; i < codes; ++i) bits[kOrder[i]] = static_cast<uint8_t>(in.Bits(3));
            Huffman meta, lengths, distances;
            if (in.failed || !meta.Build(bits, 19)) return false;
            std::memset(bits, 0, sizeof(bits));
            for (int i = 0; i < literals + dists;) {
                const int symbol = meta.Decode(in);
                if (symbol < 0) return false;
                if (symbol < 16) {
                    bits[i++] = static_cast<uint8_t>(symbol);
                    continue;
                }
                uint8_t value = 0;
                int repeat = 0;
                if (symbol == 16) {
                    if (!i) return false;
                    value = bits[i - 1];
                    repeat = 3 + static_cast<int>(in.Bits(2));
                } else if (symbol == 17) repeat = 3 + static_cast<int>(in.Bits(3));
                else repeat = 11 + static_cast<int>(in.Bits(7));
                if (in.failed || i + repeat > literals + dists) return false;
                while (repeat--) bits[i++] = value;
            }
            if (!bits[256] || !lengths.Build(bits, literals) || !distances.Build(bits + literals, dists)) return false;
            if (!InflateBlock(in, lengths, distances, out, at)) return false;
        } else {
            return false;
        }
    }
    if (at != plainSize) return false;
    const uint8_t* tail = data + size - 4;
    const uint32_t adler = (static_cast<uint32_t>(tail[0]) << 24) | (static_cast<uint32_t>(tail[1]) << 16) | (static_cast<uint32_t>(tail[2]) << 8) | tail[3];
    return adler == Adler32(out.data(), out.size());
}

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
