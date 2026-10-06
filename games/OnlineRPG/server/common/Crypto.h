#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rpg {

// SHA-256, HMAC-SHA256 and PBKDF2-HMAC-SHA256 (FIPS 180-4, RFC 2104, RFC 8018), written here so
// the servers need no crypto library. test/ checks them against the published vectors.
using Digest = std::array<uint8_t, 32>;

class Sha256 {
public:
    Sha256();
    void Update(const void* data, size_t size);
    Digest Finish();

private:
    friend class HmacKey;
    void Block(const uint8_t* block);
    uint32_t state_[8];
    uint8_t buffer_[64];
    uint64_t bytes_ = 0;
};

Digest Sha256Of(const void* data, size_t size);

// A key with its inner and outer pad already hashed: each Mac costs two more blocks, not four.
class HmacKey {
public:
    HmacKey(const void* key, size_t size);
    Digest Mac(const void* data, size_t size) const;
    // One PBKDF2 step: HMAC of a previous 32-byte output, the hot loop of a password hash.
    void MacDigest(const uint32_t in[8], uint32_t out[8]) const;

private:
    Sha256 inner_, outer_;
};

// `out` receives `size` derived bytes. iterations >= 1.
void Pbkdf2(const void* password, size_t passwordSize, const void* salt, size_t saltSize, uint32_t iterations, uint8_t* out, size_t size);

// Compares all bytes regardless of where they differ.
bool ConstantTimeEqual(const void* a, const void* b, size_t size);

// Operating system entropy. False only when the system source fails.
bool RandomBytes(void* out, size_t size);

// Unpadded base64url (RFC 4648 section 5). Decode fails on any other character or a bad length.
std::string Base64Url(const uint8_t* data, size_t size);
bool Base64UrlDecode(const std::string& text, std::vector<uint8_t>& out);
std::string Hex(const uint8_t* data, size_t size);
bool HexDecode(const std::string& text, std::vector<uint8_t>& out);

}  // namespace rpg
