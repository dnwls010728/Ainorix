#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace rpg {

// Bounds-checked reading of a payload. A failed read makes every later read fail and leaves
// its output untouched, so a handler checks Ok() (and usually End()) once at the end.
class Reader {
public:
    Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    bool U8(uint8_t& value) { uint32_t v; if (!Integer(v, 1)) return false; value = static_cast<uint8_t>(v); return true; }
    bool U16(uint16_t& value) { uint32_t v; if (!Integer(v, 2)) return false; value = static_cast<uint16_t>(v); return true; }
    bool U32(uint32_t& value) { return Integer(value, 4); }
    // u8 length + bytes; fails when longer than `maxBytes`.
    bool Str8(std::string& value, size_t maxBytes) {
        uint8_t length = 0;
        if (!U8(length)) return false;
        if (length > maxBytes || length > size_ - at_) { ok_ = false; return false; }
        value.assign(reinterpret_cast<const char*>(data_ + at_), length);
        at_ += length;
        return true;
    }
    bool Ok() const { return ok_; }
    bool End() const { return ok_ && at_ == size_; }
    const uint8_t* Rest() const { return data_ + at_; }
    size_t Remaining() const { return size_ - at_; }

private:
    bool Integer(uint32_t& value, size_t count) {
        if (!ok_ || count > size_ - at_) { ok_ = false; return false; }
        uint32_t v = 0;
        for (size_t i = 0; i < count; ++i) v |= static_cast<uint32_t>(data_[at_ + i]) << (8 * i);
        at_ += count;
        value = v;
        return true;
    }
    const uint8_t* data_;
    size_t size_, at_ = 0;
    bool ok_ = true;
};

// Builds a payload in a buffer the caller reuses (Clear keeps the capacity).
class Writer {
public:
    void Clear() { bytes_.clear(); }
    Writer& U8(uint8_t v) { bytes_.push_back(v); return *this; }
    Writer& U16(uint16_t v) { bytes_.push_back(static_cast<uint8_t>(v)); bytes_.push_back(static_cast<uint8_t>(v >> 8)); return *this; }
    Writer& U32(uint32_t v) { for (int i = 0; i < 4; ++i) bytes_.push_back(static_cast<uint8_t>(v >> (8 * i))); return *this; }
    // Strings longer than 255 bytes are cut: callers validate lengths before they get here.
    Writer& Str8(const std::string& text) {
        const size_t length = text.size() > 255 ? 255 : text.size();
        bytes_.push_back(static_cast<uint8_t>(length));
        bytes_.insert(bytes_.end(), text.begin(), text.begin() + static_cast<ptrdiff_t>(length));
        return *this;
    }
    Writer& Bytes(const void* data, size_t size) {
        bytes_.insert(bytes_.end(), static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
        return *this;
    }
    const uint8_t* Data() const { return bytes_.data(); }
    size_t Size() const { return bytes_.size(); }

private:
    std::vector<uint8_t> bytes_;
};

// Player-chosen names: valid UTF-8, 2..16 characters, no control characters or white space.
bool ValidName(const std::string& name);

}  // namespace rpg
