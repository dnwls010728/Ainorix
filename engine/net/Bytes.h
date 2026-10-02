#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace oe {

// Network values use little-endian integers. Failures are sticky and do not advance
// the cursor or modify output. Length-prefixed blobs are checked before allocating.
class ByteReader {
public:
    // The caller owns data for the reader's lifetime; null is valid only for size 0.
    ByteReader(const uint8_t* data, size_t size);
    bool ReadU8(uint8_t& value);
    bool ReadU16(uint16_t& value);
    bool ReadU32(uint32_t& value);
    bool ReadU64(uint64_t& value);
    // Reads a uint32 length followed by bytes, with an explicit application limit.
    bool ReadBlob(std::vector<uint8_t>& value, size_t maxSize);
    size_t Remaining() const { return size_ - offset_; }
    bool Ok() const { return ok_; }
private:
    bool ReadInteger(uint64_t& value, size_t count);
    const uint8_t* data_;
    size_t size_;
    size_t offset_ = 0;
    bool ok_;
};

// Bounded encoder; an entire write either succeeds or leaves the buffer unchanged.
class ByteWriter {
public:
    explicit ByteWriter(size_t maxSize) : maxSize_(maxSize) {}
    bool WriteU8(uint8_t value);
    bool WriteU16(uint16_t value);
    bool WriteU32(uint32_t value);
    bool WriteU64(uint64_t value);
    // Writes a uint32 length and bytes; null is valid only for size 0.
    bool WriteBlob(const uint8_t* data, size_t size);
    const std::vector<uint8_t>& Data() const { return data_; }
    bool Ok() const { return ok_; }
private:
    bool WriteInteger(uint64_t value, size_t count);
    std::vector<uint8_t> data_;
    size_t maxSize_;
    bool ok_ = true;
};

}  // namespace oe
