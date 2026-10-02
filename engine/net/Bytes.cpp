#include "net/Bytes.h"

#include <limits>

namespace oe {

ByteReader::ByteReader(const uint8_t* data, size_t size)
    : data_(data), size_(data || size == 0 ? size : 0), ok_(data || size == 0) {}

bool ByteReader::ReadInteger(uint64_t& value, size_t count) {
    if (!ok_ || count > Remaining()) { ok_ = false; return false; }
    uint64_t result = 0;
    for (size_t i = 0; i < count; ++i) result |= static_cast<uint64_t>(data_[offset_ + i]) << (i * 8);
    offset_ += count;
    value = result;
    return true;
}

bool ByteReader::ReadU8(uint8_t& value) {
    uint64_t result = 0;
    if (!ReadInteger(result, 1)) return false;
    value = static_cast<uint8_t>(result);
    return true;
}
bool ByteReader::ReadU16(uint16_t& value) {
    uint64_t result = 0;
    if (!ReadInteger(result, 2)) return false;
    value = static_cast<uint16_t>(result);
    return true;
}
bool ByteReader::ReadU32(uint32_t& value) {
    uint64_t result = 0;
    if (!ReadInteger(result, 4)) return false;
    value = static_cast<uint32_t>(result);
    return true;
}
bool ByteReader::ReadU64(uint64_t& value) { return ReadInteger(value, 8); }

bool ByteReader::ReadBlob(std::vector<uint8_t>& value, size_t maxSize) {
    const size_t start = offset_;
    uint32_t size = 0;
    if (!ReadU32(size)) return false;
    if (size > maxSize || size > Remaining()) { offset_ = start; ok_ = false; return false; }
    value.assign(data_ + offset_, data_ + offset_ + size);
    offset_ += size;
    return true;
}

bool ByteWriter::WriteInteger(uint64_t value, size_t count) {
    if (!ok_ || count > maxSize_ - data_.size()) { ok_ = false; return false; }
    for (size_t i = 0; i < count; ++i) data_.push_back(static_cast<uint8_t>(value >> (i * 8)));
    return true;
}
bool ByteWriter::WriteU8(uint8_t value) { return WriteInteger(value, 1); }
bool ByteWriter::WriteU16(uint16_t value) { return WriteInteger(value, 2); }
bool ByteWriter::WriteU32(uint32_t value) { return WriteInteger(value, 4); }
bool ByteWriter::WriteU64(uint64_t value) { return WriteInteger(value, 8); }

bool ByteWriter::WriteBlob(const uint8_t* data, size_t size) {
    const size_t remaining = maxSize_ - data_.size();
    if (!ok_ || (!data && size != 0) || size > std::numeric_limits<uint32_t>::max() ||
        remaining < 4 || size > remaining - 4) { ok_ = false; return false; }
    WriteU32(static_cast<uint32_t>(size));
    if (size != 0) data_.insert(data_.end(), data, data + size);
    return true;
}

}  // namespace oe
