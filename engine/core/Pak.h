#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace oe {

// A game's files in one archive (game.pak), read without unpacking.
//
// "OEPAK002" | salt[16] | u32 index size | index | data
//   index: JSON {"files":[{path, offset, size, hash, packed?}]}; offsets are relative to the start of the data.
//   packed: the file is a zlib stream of that many bytes (kept only where it saves space: images and
//   compressed models stay as they are); size is always the plain size.
//   The index and every file are enciphered (ChaCha20) with a key made from the salt and a constant
//   compiled into the engine. That keeps the files from being read or swapped with an archive tool
//   or a text editor; it is not protection against someone who takes the player apart.
//   hash: FNV-1a 64 of the plain bytes, checked when a file is read. Order on reading: decipher, unpack, check.
// "OEPAK001" (older packages) has no salt and no cipher: u32 index size | JSON index | data.
struct PakEntry {
    uint64_t offset = 0;
    uint64_t size = 0;      // plain bytes
    uint64_t stored = 0;    // bytes in the pak: less than size when the file is packed
    uint64_t hash = 0;      // 0 in OEPAK001
    uint32_t number = 0;    // place in the index: the file's cipher stream
};

// How many bytes a pak key is. The master secret from which a pak's per-file keys are derived.
constexpr size_t kPakKeySize = 32;

// The bytes of an OEPAK002 archive of `files` (relative path with forward slashes, content). `key`
// is the master secret (kPakKeySize bytes); nullptr uses the engine's built-in seed, so a package
// tool that gives a game its own key makes a pak no other game's player can read.
std::string BuildPak(const std::vector<std::pair<std::string, std::vector<unsigned char>>>& files, const unsigned char* key = nullptr);

// A player binary carries a key slot: a 16-byte mark and then kPakKeySize zero bytes. `oe package`
// copies the player and writes the game's key over the zeros (EmbedPakKey); a slot still all zero
// means the engine seed, so the committed runtimes behave as an unkeyed build.
constexpr size_t kPakKeyMarkSize = 16;
// The byte an unkeyed slot's key bytes are filled with (not zero, so a wasm data segment keeps them).
constexpr unsigned char kPakKeyUnset = 0xee;
// Writes `key` into the first key slot of a player binary's bytes; false when no slot is found.
bool EmbedPakKey(std::vector<unsigned char>& binary, const unsigned char* key);
// The key a player binary carries, or nullptr when its slot is still zero (use the engine seed).
const unsigned char* EmbeddedPakKey(const unsigned char* slot);

// Index of a pak that stays in its file or in memory; files are read (and deciphered) one at a time.
// Reading is safe from several threads once Open returned.
class PakReader {
public:
    // `key` is the master secret the pak was built with (nullptr = the engine seed).
    bool OpenFile(const std::string& path, std::string* error, const unsigned char* key = nullptr);
    bool OpenMemory(std::vector<unsigned char> bytes, std::string* error, const unsigned char* key = nullptr);
    // Relative paths (forward slashes) in the pak's order.
    const std::vector<std::string>& Paths() const { return paths_; }
    // Exact path first, then without regard to ASCII case (as Windows folders behave).
    const PakEntry* Find(const std::string& path) const;
    bool Read(const std::string& path, std::vector<unsigned char>& out, std::string* error = nullptr) const;

private:
    bool ReadAt(uint64_t offset, uint64_t size, std::vector<unsigned char>& out) const;
    bool ParseIndex(std::string* error);
    unsigned char secret_[kPakKeySize] = {};  // the master secret; derives the per-pak key with the salt

    std::string file_;                   // read from here, or from memory_
    std::vector<unsigned char> memory_;
    uint64_t total_ = 0;                 // size of the whole pak
    uint64_t dataStart_ = 0;
    bool ciphered_ = false;
    unsigned char key_[32] = {};
    std::vector<std::string> paths_;
    std::map<std::string, PakEntry> entries_;
    std::map<std::string, std::string> lower_;  // lower-case path -> path
};

}  // namespace oe
