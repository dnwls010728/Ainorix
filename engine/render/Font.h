#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "render/Mesh.h"

namespace oe {

// A TrueType/OpenType font (stb_truetype) with glyph bitmaps rasterized on
// demand at whole pixel sizes and packed into atlas pages (white texels,
// coverage in alpha). Rasterization is plain deterministic C, so both
// renderers draw the same pixels.
class FontFace {
public:
    struct Glyph {
        int page = -1;             // atlas page, -1 = nothing to draw (space)
        int x0 = 0, y0 = 0;        // bitmap offset from the pen position on the baseline (pixels, +y down)
        int w = 0, h = 0;          // bitmap size
        int s = 0, t = 0;          // bitmap position in the page (texels)
        float advance = 0.0f;      // pen advance in pixels
    };

    // Takes a .ttf/.otf (or the first font of a .ttc). nullptr + error on failure.
    static std::shared_ptr<FontFace> Load(std::vector<unsigned char> bytes, std::string* error);
    // The embedded default font (Roboto Regular).
    static std::shared_ptr<FontFace> Default();

    ~FontFace();
    FontFace(const FontFace&) = delete;
    FontFace& operator=(const FontFace&) = delete;

    bool HasGlyph(uint32_t codepoint) const;
    // Vertical metrics in pixels for a font size (em height) of `px`.
    float Ascent(float px) const;
    float Descent(float px) const;  // negative
    float LineGap(float px) const;
    float Advance(uint32_t codepoint, float px) const;
    float Kern(uint32_t a, uint32_t b, float px) const;

    // Rasterized glyph at a whole pixel size (cached). `bold` thickens it.
    Glyph GetGlyph(uint32_t codepoint, int px, bool bold);
    // Atlas page for (px, bold), as used by Glyph::page.
    std::shared_ptr<const Texture> Page(int px, bool bold, int page);

    std::string familyName;
    size_t atlasBytes() const;

private:
    FontFace() = default;
    struct Atlas {
        std::vector<std::shared_ptr<Texture>> pages;
        std::map<uint32_t, Glyph> glyphs;
        int penX = 0, penY = 0, rowH = 0;
    };
    Atlas& AtlasFor(int px, bool bold);

    std::vector<unsigned char> data_;
    struct Info;
    std::unique_ptr<Info> info_;
    mutable std::mutex mutex_;
    std::map<int, Atlas> atlases_;  // key: px * 2 + bold
};

// The 5x7 pixel font built into the engine (font name "pixel"): ASCII 32..126,
// columns of 7 bits (bit 0 = top row). Other characters draw as '?'.
const uint8_t* PixelFontGlyph(uint32_t codepoint);

// Decodes UTF-8 (invalid bytes become U+FFFD).
std::vector<uint32_t> DecodeUtf8(const std::string& text);

}  // namespace oe
