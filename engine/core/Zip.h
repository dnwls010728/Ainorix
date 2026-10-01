#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace oe {

uint32_t Crc32(const unsigned char* data, size_t size, uint32_t crc = 0);

// One file of a zip archive. `data` holds the bytes as stored in the archive
// (still compressed when method != 0); nothing here inflates.
struct ZipEntry {
    std::string name;
    uint16_t method = 0;  // 0 stored, 8 deflate
    uint16_t time = 0, date = 0x21;  // MS-DOS time/date (default 1980-01-01)
    uint32_t crc = 0;
    uint32_t size = 0;  // uncompressed size
    std::vector<unsigned char> data;
};

// Reads the entries of a zip archive (no zip64, no encryption, no multi-disk).
bool ReadZip(const std::vector<unsigned char>& bytes, std::vector<ZipEntry>& out, std::string* error);

// Writes a zip archive in memory. Stored entries can be aligned (APKs need
// 4-byte aligned uncompressed files and page-aligned native libraries), using
// the Android alignment extra field (0xD935) like zipalign.
class ZipWriter {
public:
    // Adds uncompressed data whose first byte lands on a multiple of `align`.
    void AddStored(const std::string& name, const unsigned char* data, size_t size, uint32_t align = 4);
    // Adds an entry from ReadZip unchanged (stored entries are aligned to `align`).
    void AddRaw(const ZipEntry& entry, uint32_t align = 4);
    bool Has(const std::string& name) const;
    // The finished archive (central directory appended).
    const std::vector<unsigned char>& Finish();

private:
    struct Central {
        ZipEntry header;  // data not kept
        uint32_t offset = 0;
    };
    void Add(const ZipEntry& header, const unsigned char* data, size_t size, uint32_t align);

    std::vector<unsigned char> out_;
    std::vector<Central> central_;
    bool finished_ = false;
};

}  // namespace oe
