#include "core/Pak.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "core/Image.h"
#include "core/Json.h"

namespace oe {

namespace {

constexpr char kMagic1[] = "OEPAK001", kMagic2[] = "OEPAK002";
constexpr size_t kSaltSize = 16;
constexpr uint32_t kIndexStream = 0xffffffffu;  // the index's cipher stream; files use their number
constexpr uint64_t kMaxIndex = 256ull << 20;
constexpr size_t kPackFrom = 64;                // smaller files are not worth a zlib header

// Mixed with a pak's salt into its key. Changing it makes older paks unreadable.
const unsigned char kSeed[32] = {0x4f, 0x77, 0x6e, 0x45, 0x9d, 0x21, 0xc3, 0x58, 0x07, 0xb6, 0x3a, 0xee, 0x71, 0x14, 0xd9, 0x82,
                                 0x2c, 0xf5, 0x60, 0x9b, 0xa8, 0x33, 0x1e, 0xc7, 0x55, 0x0d, 0x96, 0x4a, 0xe1, 0x7f, 0xb2, 0x68};

uint32_t Rotl(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
uint32_t Load32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// One 64-byte block of the ChaCha20 key stream (RFC 8439 section 2.3; the nonce is 12 bytes).
void ChaChaBlock(const unsigned char key[32], const unsigned char nonce[12], uint32_t counter, unsigned char out[64]) {
    uint32_t state[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
    for (int i = 0; i < 8; ++i) state[4 + i] = Load32(key + i * 4);
    state[12] = counter;
    for (int i = 0; i < 3; ++i) state[13 + i] = Load32(nonce + i * 4);
    uint32_t x[16];
    std::memcpy(x, state, sizeof(x));
    auto quarter = [&](int a, int b, int c, int d) {
        x[a] += x[b]; x[d] = Rotl(x[d] ^ x[a], 16);
        x[c] += x[d]; x[b] = Rotl(x[b] ^ x[c], 12);
        x[a] += x[b]; x[d] = Rotl(x[d] ^ x[a], 8);
        x[c] += x[d]; x[b] = Rotl(x[b] ^ x[c], 7);
    };
    for (int round = 0; round < 10; ++round) {
        quarter(0, 4, 8, 12); quarter(1, 5, 9, 13); quarter(2, 6, 10, 14); quarter(3, 7, 11, 15);
        quarter(0, 5, 10, 15); quarter(1, 6, 11, 12); quarter(2, 7, 8, 13); quarter(3, 4, 9, 14);
    }
    for (int i = 0; i < 16; ++i) {
        const uint32_t v = x[i] + state[i];
        out[i * 4] = static_cast<unsigned char>(v);
        out[i * 4 + 1] = static_cast<unsigned char>(v >> 8);
        out[i * 4 + 2] = static_cast<unsigned char>(v >> 16);
        out[i * 4 + 3] = static_cast<unsigned char>(v >> 24);
    }
}

// Enciphers or deciphers `size` bytes in place: stream `stream` of the key, from its start.
// The block counter and the high nonce word together count 2^64 blocks, so no file is too large.
void Cipher(const unsigned char key[32], uint32_t stream, unsigned char* data, uint64_t size) {
    unsigned char nonce[12] = {};
    nonce[0] = static_cast<unsigned char>(stream);
    nonce[1] = static_cast<unsigned char>(stream >> 8);
    nonce[2] = static_cast<unsigned char>(stream >> 16);
    nonce[3] = static_cast<unsigned char>(stream >> 24);
    unsigned char block[64];
    for (uint64_t at = 0, counter = 0; at < size; at += 64, ++counter) {
        const uint32_t high = static_cast<uint32_t>(counter >> 32);
        nonce[8] = static_cast<unsigned char>(high);
        nonce[9] = static_cast<unsigned char>(high >> 8);
        nonce[10] = static_cast<unsigned char>(high >> 16);
        nonce[11] = static_cast<unsigned char>(high >> 24);
        ChaChaBlock(key, nonce, static_cast<uint32_t>(counter), block);
        const uint64_t count = size - at < 64 ? size - at : 64;
        for (uint64_t i = 0; i < count; ++i) data[at + i] ^= block[i];
    }
}

void DeriveKey(const unsigned char* secret, const unsigned char* salt, unsigned char key[32]) {
    unsigned char nonce[12], block[64];
    std::memcpy(nonce, salt, 12);
    ChaChaBlock(secret, nonce, Load32(salt + 12), block);
    std::memcpy(key, block, 32);
}

std::string Lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// A relative path that stays inside the folder it is read below.
bool SafePath(const std::string& rel) {
    return !(rel.empty() || rel[0] == '/' || rel.find('\\') != std::string::npos || rel.find(':') != std::string::npos || rel == ".." ||
             rel.rfind("../", 0) == 0 || rel.find("/../") != std::string::npos || (rel.size() >= 3 && rel.compare(rel.size() - 3, 3, "/..") == 0));
}

}  // namespace

// The 16-byte mark that heads a player binary's key slot (the literal is in tools/player/main.cpp).
// It is kept here XOR 0x5a and rebuilt at runtime so no second contiguous copy of the real mark sits
// in the binary to be found before the slot. "OEPAK" and 11 bytes that do not spell anything.
void PakKeyMark(unsigned char out[kPakKeyMarkSize]) {
    static const unsigned char veiled[kPakKeyMarkSize] = {0x15, 0x1f, 0x0a, 0x1b, 0x11, 0xeb, 0x06, 0x7d,
                                                          0xb3, 0x1a, 0xd7, 0x6c, 0x90, 0x28, 0x45, 0xce};
    for (size_t i = 0; i < kPakKeyMarkSize; ++i) out[i] = veiled[i] ^ 0x5a;
}

bool EmbedPakKey(std::vector<unsigned char>& binary, const unsigned char* key) {
    unsigned char mark[kPakKeyMarkSize];
    PakKeyMark(mark);
    if (binary.size() < kPakKeyMarkSize + kPakKeySize) return false;
    for (size_t i = 0; i + kPakKeyMarkSize + kPakKeySize <= binary.size(); ++i) {
        if (std::memcmp(binary.data() + i, mark, kPakKeyMarkSize) != 0) continue;
        std::memcpy(binary.data() + i + kPakKeyMarkSize, key, kPakKeySize);
        return true;
    }
    return false;
}

const unsigned char* EmbeddedPakKey(const unsigned char* slot) {
    // An unkeyed slot holds the placeholder (0xee) that the player binary ships with; a keyed one holds
    // the game's key. The placeholder is non-zero so a wasm data segment keeps it (a run of zeros is
    // trimmed, which would leave the slot bytes landing on other data). All zero also means unkeyed.
    const unsigned char* key = slot + kPakKeyMarkSize;
    bool placeholder = true, zero = true;
    for (size_t i = 0; i < kPakKeySize; ++i) {
        if (key[i] != kPakKeyUnset) placeholder = false;
        if (key[i]) zero = false;
    }
    return (placeholder || zero) ? nullptr : key;
}

std::string BuildPak(const std::vector<std::pair<std::string, std::vector<unsigned char>>>& files, const unsigned char* key) {
    Json index = Json::MakeObject();
    index["files"] = Json::MakeArray();
    uint64_t offset = 0, content = 0;
    std::vector<std::vector<unsigned char>> stored(files.size());  // empty: the file as it is
    size_t number = 0;
    for (const auto& file : files) {
        const uint64_t hash = Fnv1a64(file.second.data(), file.second.size());
        Json entry = Json::MakeObject();
        entry["path"] = file.first;
        entry["offset"] = static_cast<double>(offset);
        entry["size"] = static_cast<double>(file.second.size());
        // Packed where that saves at least a twentieth; PNG, JPEG and the like gain nothing.
        std::vector<unsigned char>& packed = stored[number++];
        if (file.second.size() >= kPackFrom) packed = ZlibPack(file.second);
        if (packed.empty() || packed.size() > file.second.size() - file.second.size() / 20) packed.clear();
        else entry["packed"] = static_cast<double>(packed.size());
        const uint64_t storedSize = packed.empty() ? file.second.size() : packed.size();
        char text[17];
        std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
        entry["hash"] = std::string(text);
        index["files"].push(entry);
        offset += storedSize;
        // The salt follows from the content, so the same game packs to the same bytes.
        const uint64_t name = Fnv1a64(reinterpret_cast<const uint8_t*>(file.first.data()), file.first.size());
        content = (content ^ hash ^ (name * 0x9e3779b97f4a7c15ull)) * 0x100000001b3ull + file.second.size();
    }
    std::string header = index.dump();
    unsigned char salt[kSaltSize], pakKey[32];
    const uint64_t second = Fnv1a64(reinterpret_cast<const uint8_t*>(header.data()), header.size());
    std::memcpy(salt, &content, 8);
    std::memcpy(salt + 8, &second, 8);
    DeriveKey(key ? key : kSeed, salt, pakKey);

    std::string pak(kMagic2, 8);
    pak.append(reinterpret_cast<const char*>(salt), kSaltSize);
    const uint32_t n = static_cast<uint32_t>(header.size());
    for (int i = 0; i < 4; ++i) pak += static_cast<char>((n >> (8 * i)) & 0xff);
    Cipher(pakKey, kIndexStream, reinterpret_cast<unsigned char*>(&header[0]), header.size());
    pak += header;
    pak.reserve(pak.size() + static_cast<size_t>(offset));
    for (size_t i = 0; i < files.size(); ++i) {
        const std::vector<unsigned char>& bytes = stored[i].empty() ? files[i].second : stored[i];
        const size_t at = pak.size();
        pak.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        Cipher(pakKey, static_cast<uint32_t>(i), reinterpret_cast<unsigned char*>(&pak[0]) + at, bytes.size());
    }
    return pak;
}

bool PakReader::ReadAt(uint64_t offset, uint64_t size, std::vector<unsigned char>& out) const {
    if (offset > total_ || size > total_ - offset) return false;
    out.resize(static_cast<size_t>(size));
    if (!size) return true;
    if (file_.empty()) {
        std::memcpy(out.data(), memory_.data() + static_cast<size_t>(offset), static_cast<size_t>(size));
        return true;
    }
    std::ifstream f(std::filesystem::u8path(file_), std::ios::binary);  // its own stream: readers do not share a position
    if (!f) return false;
    f.seekg(static_cast<std::streamoff>(offset));
    f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    return static_cast<uint64_t>(f.gcount()) == size;
}

bool PakReader::ParseIndex(std::string* error) {
    auto fail = [&](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    std::vector<unsigned char> head;
    if (!ReadAt(0, 8, head)) return fail("not an OwnEngine game.pak");
    ciphered_ = std::memcmp(head.data(), kMagic2, 8) == 0;
    if (!ciphered_ && std::memcmp(head.data(), kMagic1, 8) != 0) return fail("not an OwnEngine game.pak");
    uint64_t at = 8;
    if (ciphered_) {
        std::vector<unsigned char> salt;
        if (!ReadAt(at, kSaltSize, salt)) return fail("damaged game.pak (header)");
        DeriveKey(secret_, salt.data(), key_);
        at += kSaltSize;
    }
    std::vector<unsigned char> sizeBytes, indexBytes;
    if (!ReadAt(at, 4, sizeBytes)) return fail("damaged game.pak (header)");
    const uint64_t indexSize = Load32(sizeBytes.data());
    at += 4;
    if (indexSize > kMaxIndex || !ReadAt(at, indexSize, indexBytes)) return fail("damaged game.pak (index)");
    if (ciphered_) Cipher(key_, kIndexStream, indexBytes.data(), indexBytes.size());
    dataStart_ = at + indexSize;
    std::string parseError;
    const Json index = Json::parse(std::string(indexBytes.begin(), indexBytes.end()), &parseError);
    if (!parseError.empty() || !index["files"].isArray()) return fail("damaged game.pak (index)");
    uint32_t number = 0;
    for (const Json& f : index["files"].items()) {
        const std::string rel = f["path"].asString();
        const double offset = f["offset"].asNumber(-1), size = f["size"].asNumber(-1);
        if (!SafePath(rel)) return fail("game.pak: invalid path '" + rel + "'");
        const double room = static_cast<double>(total_ - dataStart_);
        const double packed = f.has("packed") ? f["packed"].asNumber(-1) : size;
        // Deflate cannot expand a stream more than 1032 times: a larger claim is not a real file.
        if (offset < 0 || size < 0 || packed < 0 || offset > room || packed > room - offset || size > packed * 1032.0 + 1024.0)
            return fail("game.pak: damaged entry " + rel);
        PakEntry entry;
        entry.offset = static_cast<uint64_t>(offset);
        entry.size = static_cast<uint64_t>(size);
        entry.stored = static_cast<uint64_t>(packed);
        entry.hash = std::strtoull(f["hash"].asString("0").c_str(), nullptr, 16);
        entry.number = number++;
        if (!entries_.emplace(rel, entry).second) return fail("game.pak: '" + rel + "' is listed twice");
        paths_.push_back(rel);
        lower_.emplace(Lower(rel), rel);
    }
    return true;
}

bool PakReader::OpenFile(const std::string& path, std::string* error, const unsigned char* key) {
    *this = PakReader();
    std::memcpy(secret_, key ? key : kSeed, kPakKeySize);
    std::error_code ec;
    const auto size = std::filesystem::file_size(std::filesystem::u8path(path), ec);
    if (ec) {
        if (error) *error = "cannot read " + path;
        return false;
    }
    file_ = path;
    total_ = static_cast<uint64_t>(size);
    return ParseIndex(error);
}

bool PakReader::OpenMemory(std::vector<unsigned char> bytes, std::string* error, const unsigned char* key) {
    *this = PakReader();
    std::memcpy(secret_, key ? key : kSeed, kPakKeySize);
    memory_ = std::move(bytes);
    total_ = memory_.size();
    return ParseIndex(error);
}

const PakEntry* PakReader::Find(const std::string& path) const {
    auto it = entries_.find(path);
    if (it == entries_.end()) {
        const auto folded = lower_.find(Lower(path));
        if (folded != lower_.end()) it = entries_.find(folded->second);
    }
    return it == entries_.end() ? nullptr : &it->second;
}

bool PakReader::Read(const std::string& path, std::vector<unsigned char>& out, std::string* error) const {
    const PakEntry* entry = Find(path);
    if (!entry || !ReadAt(dataStart_ + entry->offset, entry->stored, out)) {
        if (error) *error = entry ? "game.pak: cannot read " + path : "game.pak has no " + path;
        return false;
    }
    if (!ciphered_) return true;
    Cipher(key_, entry->number, out.data(), out.size());
    if (entry->stored != entry->size) {
        std::vector<unsigned char> plain;
        if (!ZlibUnpack(out.data(), out.size(), static_cast<size_t>(entry->size), plain)) {
            if (error) *error = "game.pak: " + path + " is damaged";
            out.clear();
            return false;
        }
        out.swap(plain);
    }
    if (Fnv1a64(out.data(), out.size()) != entry->hash) {
        if (error) *error = "game.pak: " + path + " is damaged";
        out.clear();
        return false;
    }
    return true;
}

}  // namespace oe
