#include "common/Wire.h"

namespace rpg {

bool ValidName(const std::string& name) {
    size_t characters = 0;
    for (size_t i = 0; i < name.size(); ++characters) {
        const uint8_t lead = static_cast<uint8_t>(name[i]);
        uint32_t code;
        size_t length;
        if (lead < 0x80) { code = lead; length = 1; }
        else if ((lead & 0xe0) == 0xc0) { code = lead & 0x1f; length = 2; }
        else if ((lead & 0xf0) == 0xe0) { code = lead & 0x0f; length = 3; }
        else if ((lead & 0xf8) == 0xf0) { code = lead & 0x07; length = 4; }
        else return false;
        if (i + length > name.size()) return false;
        for (size_t k = 1; k < length; ++k) {
            const uint8_t next = static_cast<uint8_t>(name[i + k]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3f);
        }
        // Shortest form only, no surrogates, inside Unicode.
        if ((length == 2 && code < 0x80) || (length == 3 && code < 0x800) || (length == 4 && code < 0x10000) || code > 0x10ffff ||
            (code >= 0xd800 && code <= 0xdfff)) return false;
        const bool control = code < 0x20 || (code >= 0x7f && code < 0xa0);
        const bool space = code == 0x20 || code == 0xa0 || code == 0x1680 || (code >= 0x2000 && code <= 0x200f) || (code >= 0x2028 && code <= 0x202f) ||
                           code == 0x205f || code == 0x2060 || code == 0x3000 || code == 0xfeff;
        if (control || space) return false;
        i += length;
    }
    return characters >= 2 && characters <= 16;
}

}  // namespace rpg
