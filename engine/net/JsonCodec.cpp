#include "net/JsonCodec.h"

#include <cmath>
#include <cstring>
#include <map>
#include <string>

namespace oe {
namespace {
enum Tag : uint8_t { Null, False, True, Unsigned, Negative, Float32, Float64, String, StringRef, Array, Object };
constexpr size_t kInternBytes = 64;
constexpr double kExactInteger = 9007199254740992.0;  // 2^53

struct Writer {
    std::vector<uint8_t> out;
    size_t limit;
    std::map<std::string, uint32_t> table;
    bool ok = true;
    void Byte(uint8_t value) { if (out.size() >= limit) ok = false; else out.push_back(value); }
    void Varint(uint64_t value) {
        while (value >= 0x80) { Byte(static_cast<uint8_t>(value | 0x80)); value >>= 7; }
        Byte(static_cast<uint8_t>(value));
    }
    void Fixed(uint64_t value, size_t count) { for (size_t i = 0; i < count; ++i) Byte(static_cast<uint8_t>(value >> (i * 8))); }
    void Text(const std::string& text) {
        if (text.size() > kJsonCodecStringBytes) { ok = false; return; }
        auto found = table.find(text);
        if (found != table.end()) { Byte(StringRef); Varint(found->second); return; }
        Byte(String); Varint(text.size());
        if (text.size() > limit - out.size()) { ok = false; return; }
        out.insert(out.end(), text.begin(), text.end());
        // The reader applies the same rule, so indices agree without being transmitted.
        if (text.size() <= kInternBytes && table.size() < kJsonCodecTableStrings)
            table.emplace(text, static_cast<uint32_t>(table.size()));
    }
    void Value(const Json& value, unsigned depth) {
        if (!ok || depth > kJsonCodecDepth) { ok = false; return; }
        switch (value.type()) {
            case Json::Type::Null: Byte(Null); break;
            case Json::Type::Bool: Byte(value.asBool() ? True : False); break;
            case Json::Type::Number: {
                const double n = value.asNumber();
                if (!std::isfinite(n)) { ok = false; break; }
                if (std::floor(n) == n && std::fabs(n) < kExactInteger) {
                    Byte(n < 0 ? Negative : Unsigned); Varint(static_cast<uint64_t>(std::fabs(n)));
                } else if (static_cast<double>(static_cast<float>(n)) == n) {
                    const float single = static_cast<float>(n); uint32_t bits = 0;
                    std::memcpy(&bits, &single, sizeof(bits)); Byte(Float32); Fixed(bits, 4);
                } else {
                    uint64_t bits = 0; std::memcpy(&bits, &n, sizeof(bits)); Byte(Float64); Fixed(bits, 8);
                }
                break;
            }
            case Json::Type::String: Text(value.asString()); break;
            case Json::Type::Array:
                Byte(Array); Varint(value.items().size());
                for (const Json& item : value.items()) Value(item, depth + 1);
                break;
            case Json::Type::Object:
                Byte(Object); Varint(value.members().size());
                for (const auto& member : value.members()) { Text(member.first); Value(member.second, depth + 1); }
                break;
        }
    }
};

struct Reader {
    const uint8_t* data;
    size_t size, offset = 0;
    std::vector<std::string> table;
    bool Byte(uint8_t& value) { if (offset >= size) return false; value = data[offset++]; return true; }
    bool Varint(uint64_t& value) {
        value = 0;
        for (unsigned shift = 0; shift < 64; shift += 7) {
            uint8_t byte = 0;
            if (!Byte(byte)) return false;
            if (shift == 63 && byte > 1) return false;
            value |= static_cast<uint64_t>(byte & 0x7f) << shift;
            if (!(byte & 0x80)) return true;
        }
        return false;
    }
    bool Fixed(uint64_t& value, size_t count) {
        if (count > size - offset) return false;
        value = 0;
        for (size_t i = 0; i < count; ++i) value |= static_cast<uint64_t>(data[offset + i]) << (i * 8);
        offset += count; return true;
    }
    bool Text(uint8_t tag, std::string& text) {
        uint64_t n = 0;
        if (!Varint(n)) return false;
        if (tag == StringRef) { if (n >= table.size()) return false; text = table[static_cast<size_t>(n)]; return true; }
        if (tag != String || n > kJsonCodecStringBytes || n > size - offset) return false;
        text.assign(reinterpret_cast<const char*>(data + offset), static_cast<size_t>(n)); offset += static_cast<size_t>(n);
        if (text.size() <= kInternBytes && table.size() < kJsonCodecTableStrings) table.push_back(text);
        return true;
    }
    bool Value(Json& value, unsigned depth) {
        uint8_t tag = 0; uint64_t n = 0;
        if (depth > kJsonCodecDepth || !Byte(tag)) return false;
        switch (tag) {
            case Null: value = Json(); return true;
            case False: value = Json(false); return true;
            case True: value = Json(true); return true;
            case Unsigned: case Negative: {
                if (!Varint(n) || static_cast<double>(n) >= kExactInteger) return false;
                value = Json(tag == Negative ? -static_cast<double>(n) : static_cast<double>(n)); return true;
            }
            case Float32: {
                if (!Fixed(n, 4)) return false;
                const uint32_t bits = static_cast<uint32_t>(n); float single = 0;
                std::memcpy(&single, &bits, sizeof(single));
                if (!std::isfinite(single)) return false;
                value = Json(static_cast<double>(single)); return true;
            }
            case Float64: {
                if (!Fixed(n, 8)) return false;
                double number = 0; std::memcpy(&number, &n, sizeof(number));
                if (!std::isfinite(number)) return false;
                value = Json(number); return true;
            }
            case String: case StringRef: {
                std::string text;
                if (!Text(tag, text)) return false;
                value = Json(std::move(text)); return true;
            }
            case Array: {
                // Every element needs at least one byte: the count cannot over-allocate.
                if (!Varint(n) || n > size - offset) return false;
                Json::Array items; items.reserve(static_cast<size_t>(n));
                for (uint64_t i = 0; i < n; ++i) { Json item; if (!Value(item, depth + 1)) return false; items.push_back(std::move(item)); }
                value = Json(std::move(items)); return true;
            }
            case Object: {
                if (!Varint(n) || n > (size - offset) / 2) return false;
                Json::Object members; members.reserve(static_cast<size_t>(n));
                for (uint64_t i = 0; i < n; ++i) {
                    uint8_t keyTag = 0; std::string key; Json member;
                    if (!Byte(keyTag) || !Text(keyTag, key) || !Value(member, depth + 1)) return false;
                    members.emplace_back(std::move(key), std::move(member));
                }
                value = Json(std::move(members)); return true;
            }
            default: return false;
        }
    }
};
}  // namespace

bool EncodeJson(const Json& value, std::vector<uint8_t>& bytes, size_t maxBytes) {
    Writer writer; writer.limit = maxBytes;
    writer.Value(value, 0);
    if (!writer.ok) return false;
    bytes = std::move(writer.out);
    return true;
}

bool DecodeJson(const uint8_t* bytes, size_t size, Json& value) {
    if (!bytes && size) return false;
    Reader reader{bytes, size};
    Json result;
    if (!reader.Value(result, 0) || reader.offset != size) return false;
    value = std::move(result);
    return true;
}

}  // namespace oe
