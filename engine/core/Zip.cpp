#include "core/Zip.h"

namespace oe {

namespace {

uint16_t U16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t U32(const unsigned char* p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24); }

void Put16(std::vector<unsigned char>& out, uint32_t v) {
    out.push_back(static_cast<unsigned char>(v & 0xFF));
    out.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
}
void Put32(std::vector<unsigned char>& out, uint32_t v) {
    Put16(out, v & 0xFFFF);
    Put16(out, v >> 16);
}

bool Fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

}  // namespace

uint32_t Crc32(const unsigned char* data, size_t size, uint32_t crc) {
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

bool ReadZip(const std::vector<unsigned char>& bytes, std::vector<ZipEntry>& out, std::string* error) {
    out.clear();
    const size_t n = bytes.size();
    if (n < 22) return Fail(error, "not a zip archive (too small)");
    // End of central directory: last record with the signature (comment up to 64 KiB).
    size_t eocd = std::string::npos;
    for (size_t i = n - 22 + 1; i-- > 0 && n - i <= 22 + 0xFFFF;) {
        if (U32(&bytes[i]) == 0x06054b50) {
            eocd = i;
            break;
        }
    }
    if (eocd == std::string::npos) return Fail(error, "not a zip archive (no end of central directory)");
    const uint32_t count = U16(&bytes[eocd + 10]);
    const uint32_t cdSize = U32(&bytes[eocd + 12]);
    size_t p = U32(&bytes[eocd + 16]);
    if (count == 0xFFFF || p == 0xFFFFFFFFu || p + cdSize > n) return Fail(error, "unsupported zip archive (zip64 or damaged)");
    for (uint32_t i = 0; i < count; ++i) {
        if (p + 46 > n || U32(&bytes[p]) != 0x02014b50) return Fail(error, "damaged zip central directory");
        const unsigned char* c = &bytes[p];
        ZipEntry e;
        const uint16_t flags = U16(c + 8);
        e.method = U16(c + 10);
        e.time = U16(c + 12);
        e.date = U16(c + 14);
        e.crc = U32(c + 16);
        const uint32_t csize = U32(c + 20);
        e.size = U32(c + 24);
        const size_t nameLen = U16(c + 28), extraLen = U16(c + 30), commentLen = U16(c + 32);
        const size_t local = U32(c + 42);
        if (flags & 1) return Fail(error, "encrypted zip entries are not supported");
        if (p + 46 + nameLen > n) return Fail(error, "damaged zip central directory");
        e.name.assign(reinterpret_cast<const char*>(c + 46), nameLen);
        if (local + 30 > n || U32(&bytes[local]) != 0x04034b50) return Fail(error, "damaged zip entry " + e.name);
        const size_t data = local + 30 + U16(&bytes[local + 26]) + U16(&bytes[local + 28]);
        if (data + csize > n) return Fail(error, "damaged zip entry " + e.name);
        e.data.assign(bytes.begin() + static_cast<std::ptrdiff_t>(data), bytes.begin() + static_cast<std::ptrdiff_t>(data + csize));
        out.push_back(std::move(e));
        p += 46 + nameLen + extraLen + commentLen;
    }
    return true;
}

void ZipWriter::AddStored(const std::string& name, const unsigned char* data, size_t size, uint32_t align) {
    ZipEntry e;
    e.name = name;
    e.method = 0;
    e.size = static_cast<uint32_t>(size);
    e.crc = Crc32(data, size);
    Add(e, data, size, align);
}

void ZipWriter::AddRaw(const ZipEntry& entry, uint32_t align) {
    Add(entry, entry.data.data(), entry.data.size(), entry.method == 0 ? align : 1);
}

bool ZipWriter::Has(const std::string& name) const {
    for (const Central& c : central_) {
        if (c.header.name == name) return true;
    }
    return false;
}

void ZipWriter::Add(const ZipEntry& header, const unsigned char* data, size_t size, uint32_t align) {
    Central c;
    c.header.name = header.name;
    c.header.method = header.method;
    c.header.time = header.time;
    c.header.date = header.date;
    c.header.crc = header.crc;
    c.header.size = header.size;
    c.header.data.resize(size);  // only its size is used (compressed size)
    c.offset = static_cast<uint32_t>(out_.size());
    // Alignment extra field (id 0xD935: uint16 alignment + zero padding), as zipalign writes it.
    std::vector<unsigned char> extra;
    if (align > 1) {
        size_t dataStart = out_.size() + 30 + header.name.size() + 6;
        size_t pad = (align - dataStart % align) % align;
        Put16(extra, 0xD935);
        Put16(extra, static_cast<uint32_t>(2 + pad));
        Put16(extra, align);
        extra.insert(extra.end(), pad, 0);
    }
    Put32(out_, 0x04034b50);
    Put16(out_, header.method == 0 ? 10 : 20);
    Put16(out_, 0x0800);  // UTF-8 names; sizes in the header, no data descriptor
    Put16(out_, header.method);
    Put16(out_, header.time);
    Put16(out_, header.date);
    Put32(out_, header.crc);
    Put32(out_, static_cast<uint32_t>(size));
    Put32(out_, header.size);
    Put16(out_, static_cast<uint32_t>(header.name.size()));
    Put16(out_, static_cast<uint32_t>(extra.size()));
    out_.insert(out_.end(), header.name.begin(), header.name.end());
    out_.insert(out_.end(), extra.begin(), extra.end());
    out_.insert(out_.end(), data, data + size);
    central_.push_back(std::move(c));
}

const std::vector<unsigned char>& ZipWriter::Finish() {
    if (finished_) return out_;
    finished_ = true;
    const uint32_t cdOffset = static_cast<uint32_t>(out_.size());
    for (const Central& c : central_) {
        const ZipEntry& h = c.header;
        Put32(out_, 0x02014b50);
        Put16(out_, 20);  // made by: MS-DOS, zip 2.0
        Put16(out_, h.method == 0 ? 10 : 20);
        Put16(out_, 0x0800);
        Put16(out_, h.method);
        Put16(out_, h.time);
        Put16(out_, h.date);
        Put32(out_, h.crc);
        Put32(out_, static_cast<uint32_t>(h.data.size()));
        Put32(out_, h.size);
        Put16(out_, static_cast<uint32_t>(h.name.size()));
        Put16(out_, 0);  // extra
        Put16(out_, 0);  // comment
        Put16(out_, 0);  // disk
        Put16(out_, 0);  // internal attributes
        Put32(out_, 0);  // external attributes
        Put32(out_, c.offset);
        out_.insert(out_.end(), h.name.begin(), h.name.end());
    }
    const uint32_t cdSize = static_cast<uint32_t>(out_.size()) - cdOffset;
    Put32(out_, 0x06054b50);
    Put16(out_, 0);
    Put16(out_, 0);
    Put16(out_, static_cast<uint32_t>(central_.size()));
    Put16(out_, static_cast<uint32_t>(central_.size()));
    Put32(out_, cdSize);
    Put32(out_, cdOffset);
    Put16(out_, 0);
    return out_;
}

}  // namespace oe
