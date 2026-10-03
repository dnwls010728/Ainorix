#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/Json.h"

namespace oe {

// Compact binary form of a Json value for network payloads (authoritative snapshots).
// One tag byte per value, LEB128 integers, float32 when exact, and a per-message string
// table so repeated keys such as "Transform.position" cost one or two bytes after their
// first use. Object member order is preserved.
//
// Encoding fails (false, output untouched) for non-finite numbers, nesting deeper than
// kJsonCodecDepth or more than maxBytes of output.
bool EncodeJson(const Json& value, std::vector<uint8_t>& bytes, size_t maxBytes);

// Decodes untrusted bytes. Every length and count is checked against the remaining input
// before allocating; unknown tags, bad string references, non-finite numbers, excessive
// depth and trailing bytes are rejected. Never throws.
bool DecodeJson(const uint8_t* bytes, size_t size, Json& value);

constexpr unsigned kJsonCodecDepth = 16;
constexpr size_t kJsonCodecStringBytes = 8192;   // longest single string
constexpr size_t kJsonCodecTableStrings = 1024;  // interned strings per message (<= 64 bytes each)

}  // namespace oe
