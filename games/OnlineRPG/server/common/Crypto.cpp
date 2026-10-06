#include "common/Crypto.h"

#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <cstdio>
#endif

namespace rpg {
namespace {

const uint32_t kRound[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t Rotr(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }

// The compression function on a message schedule whose first 16 words are already big-endian words.
void Compress(uint32_t state[8], uint32_t w[64]) {
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t t1 = h + (Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25)) + ((e & f) ^ (~e & g)) + kRound[i] + w[i];
        const uint32_t t2 = (Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

}  // namespace

Sha256::Sha256() {
    const uint32_t initial[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(state_, initial, sizeof(state_));
}

void Sha256::Block(const uint8_t* block) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) | (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 8) | block[i * 4 + 3];
    Compress(state_, w);
}

void Sha256::Update(const void* data, size_t size) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    size_t used = static_cast<size_t>(bytes_ % 64);
    bytes_ += size;
    if (used) {
        const size_t take = size < 64 - used ? size : 64 - used;
        std::memcpy(buffer_ + used, bytes, take);
        bytes += take; size -= take; used += take;
        if (used < 64) return;
        Block(buffer_);
    }
    for (; size >= 64; bytes += 64, size -= 64) Block(bytes);
    if (size) std::memcpy(buffer_, bytes, size);
}

Digest Sha256::Finish() {
    const uint64_t bits = bytes_ * 8;
    size_t used = static_cast<size_t>(bytes_ % 64);
    buffer_[used++] = 0x80;
    if (used > 56) {
        std::memset(buffer_ + used, 0, 64 - used);
        Block(buffer_);
        used = 0;
    }
    std::memset(buffer_ + used, 0, 56 - used);
    for (int i = 0; i < 8; ++i) buffer_[56 + i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    Block(buffer_);
    Digest out;
    for (int i = 0; i < 8; ++i) {
        out[i * 4] = static_cast<uint8_t>(state_[i] >> 24);
        out[i * 4 + 1] = static_cast<uint8_t>(state_[i] >> 16);
        out[i * 4 + 2] = static_cast<uint8_t>(state_[i] >> 8);
        out[i * 4 + 3] = static_cast<uint8_t>(state_[i]);
    }
    return out;
}

Digest Sha256Of(const void* data, size_t size) {
    Sha256 hash;
    hash.Update(data, size);
    return hash.Finish();
}

HmacKey::HmacKey(const void* key, size_t size) {
    uint8_t block[64] = {};
    if (size > 64) {
        const Digest digest = Sha256Of(key, size);
        std::memcpy(block, digest.data(), digest.size());
    } else if (size) {
        std::memcpy(block, key, size);
    }
    uint8_t pad[64];
    for (int i = 0; i < 64; ++i) pad[i] = block[i] ^ 0x36;
    inner_.Update(pad, 64);
    for (int i = 0; i < 64; ++i) pad[i] = block[i] ^ 0x5c;
    outer_.Update(pad, 64);
}

Digest HmacKey::Mac(const void* data, size_t size) const {
    Sha256 inner = inner_;
    inner.Update(data, size);
    const Digest first = inner.Finish();
    Sha256 outer = outer_;
    outer.Update(first.data(), first.size());
    return outer.Finish();
}

void HmacKey::MacDigest(const uint32_t in[8], uint32_t out[8]) const {
    // Both messages are one pad block (already hashed) plus 32 bytes: a single padded block each,
    // with the length 64 + 32 bytes = 768 bits.
    uint32_t w[64];
    uint32_t state[8];
    std::memcpy(w, in, 32);
    w[8] = 0x80000000;
    std::memset(w + 9, 0, 6 * sizeof(uint32_t));
    w[15] = 768;
    std::memcpy(state, inner_.state_, sizeof(state));
    Compress(state, w);
    std::memcpy(w, state, 32);
    w[8] = 0x80000000;
    std::memset(w + 9, 0, 6 * sizeof(uint32_t));
    w[15] = 768;
    std::memcpy(out, outer_.state_, 32);
    Compress(out, w);
}

void Pbkdf2(const void* password, size_t passwordSize, const void* salt, size_t saltSize, uint32_t iterations, uint8_t* out, size_t size) {
    const HmacKey key(password, passwordSize);
    std::vector<uint8_t> first(static_cast<const uint8_t*>(salt), static_cast<const uint8_t*>(salt) + saltSize);
    first.resize(saltSize + 4);
    for (uint32_t block = 1; size; ++block) {
        first[saltSize] = static_cast<uint8_t>(block >> 24);
        first[saltSize + 1] = static_cast<uint8_t>(block >> 16);
        first[saltSize + 2] = static_cast<uint8_t>(block >> 8);
        first[saltSize + 3] = static_cast<uint8_t>(block);
        const Digest u1 = key.Mac(first.data(), first.size());
        uint32_t u[8], sum[8];
        for (int i = 0; i < 8; ++i)
            u[i] = (static_cast<uint32_t>(u1[i * 4]) << 24) | (static_cast<uint32_t>(u1[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(u1[i * 4 + 2]) << 8) | u1[i * 4 + 3];
        std::memcpy(sum, u, sizeof(sum));
        for (uint32_t i = 1; i < iterations; ++i) {
            uint32_t next[8];
            key.MacDigest(u, next);
            for (int k = 0; k < 8; ++k) { u[k] = next[k]; sum[k] ^= next[k]; }
        }
        uint8_t bytes[32];
        for (int i = 0; i < 8; ++i) {
            bytes[i * 4] = static_cast<uint8_t>(sum[i] >> 24);
            bytes[i * 4 + 1] = static_cast<uint8_t>(sum[i] >> 16);
            bytes[i * 4 + 2] = static_cast<uint8_t>(sum[i] >> 8);
            bytes[i * 4 + 3] = static_cast<uint8_t>(sum[i]);
        }
        const size_t take = size < 32 ? size : 32;
        std::memcpy(out, bytes, take);
        out += take;
        size -= take;
    }
}

bool ConstantTimeEqual(const void* a, const void* b, size_t size) {
    const volatile uint8_t* x = static_cast<const volatile uint8_t*>(a);
    const volatile uint8_t* y = static_cast<const volatile uint8_t*>(b);
    uint8_t difference = 0;
    for (size_t i = 0; i < size; ++i) difference = static_cast<uint8_t>(difference | (x[i] ^ y[i]));
    return difference == 0;
}

bool RandomBytes(void* out, size_t size) {
#ifdef _WIN32
    return BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    std::FILE* file = std::fopen("/dev/urandom", "rb");
    if (!file) return false;
    const bool ok = std::fread(out, 1, size, file) == size;
    std::fclose(file);
    return ok;
#endif
}

std::string Base64Url(const uint8_t* data, size_t size) {
    std::string out;
    out.reserve((size * 4 + 2) / 3);
    size_t i = 0;
    for (; i + 2 < size; i += 3) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8) | data[i + 2];
        out += kBase64[v >> 18]; out += kBase64[(v >> 12) & 63]; out += kBase64[(v >> 6) & 63]; out += kBase64[v & 63];
    }
    if (size - i == 1) {
        const uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        out += kBase64[v >> 18]; out += kBase64[(v >> 12) & 63];
    } else if (size - i == 2) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8);
        out += kBase64[v >> 18]; out += kBase64[(v >> 12) & 63]; out += kBase64[(v >> 6) & 63];
    }
    return out;
}

bool Base64UrlDecode(const std::string& text, std::vector<uint8_t>& out) {
    if (text.size() % 4 == 1) return false;
    std::vector<uint8_t> bytes;
    bytes.reserve(text.size() * 3 / 4);
    uint32_t value = 0;
    int bits = 0;
    for (char c : text) {
        int digit;
        if (c >= 'A' && c <= 'Z') digit = c - 'A';
        else if (c >= 'a' && c <= 'z') digit = c - 'a' + 26;
        else if (c >= '0' && c <= '9') digit = c - '0' + 52;
        else if (c == '-') digit = 62;
        else if (c == '_') digit = 63;
        else return false;
        value = (value << 6) | static_cast<uint32_t>(digit);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(static_cast<uint8_t>(value >> bits));
        }
    }
    if (value & ((1u << bits) - 1)) return false;  // leftover bits must be zero: one text per value
    out = std::move(bytes);
    return true;
}

std::string Hex(const uint8_t* data, size_t size) {
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out += "0123456789abcdef"[data[i] >> 4];
        out += "0123456789abcdef"[data[i] & 15];
    }
    return out;
}

bool HexDecode(const std::string& text, std::vector<uint8_t>& out) {
    if (text.size() % 2) return false;
    std::vector<uint8_t> bytes;
    auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (size_t i = 0; i < text.size(); i += 2) {
        const int high = digit(text[i]), low = digit(text[i + 1]);
        if (high < 0 || low < 0) return false;
        bytes.push_back(static_cast<uint8_t>(high * 16 + low));
    }
    out = std::move(bytes);
    return true;
}

}  // namespace rpg
