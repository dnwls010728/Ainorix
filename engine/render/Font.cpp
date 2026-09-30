#include "render/Font.h"

#include <algorithm>
#include <cmath>

#include "stb_truetype.h"

namespace oe {

extern const unsigned char kDefaultFontData[];
extern const size_t kDefaultFontSize;

namespace {

constexpr int kPageSize = 512;

// Classic 5x7 pixel font for ASCII 32..126. Each glyph is 5 columns; bit 0
// of a column is the top row. Glyph cell is 6x8 units (1 unit spacing).
const uint8_t kPixelFont[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00}, {0x14, 0x7F, 0x14, 0x7F, 0x14},
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62}, {0x36, 0x49, 0x55, 0x22, 0x50}, {0x00, 0x05, 0x03, 0x00, 0x00},
    {0x00, 0x1C, 0x22, 0x41, 0x00}, {0x00, 0x41, 0x22, 0x1C, 0x00}, {0x08, 0x2A, 0x1C, 0x2A, 0x08}, {0x08, 0x08, 0x3E, 0x08, 0x08},
    {0x00, 0x50, 0x30, 0x00, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x60, 0x60, 0x00, 0x00}, {0x20, 0x10, 0x08, 0x04, 0x02},
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00}, {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31},
    {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E}, {0x00, 0x36, 0x36, 0x00, 0x00}, {0x00, 0x56, 0x36, 0x00, 0x00},
    {0x08, 0x14, 0x22, 0x41, 0x00}, {0x14, 0x14, 0x14, 0x14, 0x14}, {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x51, 0x09, 0x06},
    {0x32, 0x49, 0x79, 0x41, 0x3E}, {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x01, 0x01}, {0x3E, 0x41, 0x41, 0x51, 0x32},
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41},
    {0x7F, 0x40, 0x40, 0x40, 0x40}, {0x7F, 0x02, 0x04, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46}, {0x46, 0x49, 0x49, 0x49, 0x31},
    {0x01, 0x01, 0x7F, 0x01, 0x01}, {0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x7F, 0x20, 0x18, 0x20, 0x7F},
    {0x63, 0x14, 0x08, 0x14, 0x63}, {0x03, 0x04, 0x78, 0x04, 0x03}, {0x61, 0x51, 0x49, 0x45, 0x43}, {0x00, 0x7F, 0x41, 0x41, 0x00},
    {0x02, 0x04, 0x08, 0x10, 0x20}, {0x00, 0x41, 0x41, 0x7F, 0x00}, {0x04, 0x02, 0x01, 0x02, 0x04}, {0x40, 0x40, 0x40, 0x40, 0x40},
    {0x00, 0x01, 0x02, 0x04, 0x00}, {0x20, 0x54, 0x54, 0x54, 0x78}, {0x7F, 0x48, 0x44, 0x44, 0x38}, {0x38, 0x44, 0x44, 0x44, 0x20},
    {0x38, 0x44, 0x44, 0x48, 0x7F}, {0x38, 0x54, 0x54, 0x54, 0x18}, {0x08, 0x7E, 0x09, 0x01, 0x02}, {0x08, 0x14, 0x54, 0x54, 0x3C},
    {0x7F, 0x08, 0x04, 0x04, 0x78}, {0x00, 0x44, 0x7D, 0x40, 0x00}, {0x20, 0x40, 0x44, 0x3D, 0x00}, {0x00, 0x7F, 0x10, 0x28, 0x44},
    {0x00, 0x41, 0x7F, 0x40, 0x00}, {0x7C, 0x04, 0x18, 0x04, 0x78}, {0x7C, 0x08, 0x04, 0x04, 0x78}, {0x38, 0x44, 0x44, 0x44, 0x38},
    {0x7C, 0x14, 0x14, 0x14, 0x08}, {0x08, 0x14, 0x14, 0x18, 0x7C}, {0x7C, 0x08, 0x04, 0x04, 0x08}, {0x48, 0x54, 0x54, 0x54, 0x20},
    {0x04, 0x3F, 0x44, 0x40, 0x20}, {0x3C, 0x40, 0x40, 0x20, 0x7C}, {0x1C, 0x20, 0x40, 0x20, 0x1C}, {0x3C, 0x40, 0x30, 0x40, 0x3C},
    {0x44, 0x28, 0x10, 0x28, 0x44}, {0x0C, 0x50, 0x50, 0x50, 0x3C}, {0x44, 0x64, 0x54, 0x4C, 0x44}, {0x00, 0x08, 0x36, 0x41, 0x00},
    {0x00, 0x00, 0x7F, 0x00, 0x00}, {0x00, 0x41, 0x36, 0x08, 0x00}, {0x08, 0x04, 0x08, 0x10, 0x08},
};

}  // namespace

struct FontFace::Info {
    stbtt_fontinfo font{};
    int ascent = 0, descent = 0, lineGap = 0;
};

const uint8_t* PixelFontGlyph(uint32_t cp) {
    return kPixelFont[(cp >= 32 && cp <= 126) ? cp - 32 : '?' - 32];
}

std::vector<uint32_t> DecodeUtf8(const std::string& text) {
    std::vector<uint32_t> out;
    out.reserve(text.size());
    const size_t n = text.size();
    size_t i = 0;
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        size_t extra = 0;
        uint32_t cp = c;
        if (c >= 0x80) {
            if ((c >> 5) == 6) extra = 1, cp = c & 0x1F;
            else if ((c >> 4) == 14) extra = 2, cp = c & 0x0F;
            else if ((c >> 3) == 30) extra = 3, cp = c & 0x07;
            else extra = 99;
        }
        bool ok = extra != 99 && i + extra < n;
        for (size_t k = 1; ok && k <= extra; ++k) {
            unsigned char cc = static_cast<unsigned char>(text[i + k]);
            ok = (cc & 0xC0) == 0x80;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        out.push_back(cp);
        i += extra + 1;
    }
    return out;
}

std::shared_ptr<FontFace> FontFace::Load(std::vector<unsigned char> bytes, std::string* error) {
    std::shared_ptr<FontFace> f(new FontFace());
    f->data_ = std::move(bytes);
    f->info_ = std::make_unique<Info>();
    int offset = f->data_.size() >= 12 ? stbtt_GetFontOffsetForIndex(f->data_.data(), 0) : -1;
    if (offset < 0 || !stbtt_InitFont(&f->info_->font, f->data_.data(), offset)) {
        if (error) *error = "not a TrueType/OpenType font";
        return nullptr;
    }
    stbtt_GetFontVMetrics(&f->info_->font, &f->info_->ascent, &f->info_->descent, &f->info_->lineGap);
    int len = 0;
    const char* name = stbtt_GetFontNameString(&f->info_->font, &len, STBTT_PLATFORM_ID_MICROSOFT, STBTT_MS_EID_UNICODE_BMP, STBTT_MS_LANG_ENGLISH, 1);
    for (int i = 1; name && i < len; i += 2) f->familyName += name[i] ? name[i] : '?';  // UTF-16BE, ASCII part
    return f;
}

std::shared_ptr<FontFace> FontFace::Default() {
    static std::shared_ptr<FontFace> face = Load(std::vector<unsigned char>(kDefaultFontData, kDefaultFontData + kDefaultFontSize), nullptr);
    return face;
}

FontFace::~FontFace() = default;

bool FontFace::HasGlyph(uint32_t cp) const { return stbtt_FindGlyphIndex(&info_->font, static_cast<int>(cp)) != 0; }

float FontFace::Ascent(float px) const { return static_cast<float>(info_->ascent) * stbtt_ScaleForMappingEmToPixels(&info_->font, px); }
float FontFace::Descent(float px) const { return static_cast<float>(info_->descent) * stbtt_ScaleForMappingEmToPixels(&info_->font, px); }
float FontFace::LineGap(float px) const { return static_cast<float>(info_->lineGap) * stbtt_ScaleForMappingEmToPixels(&info_->font, px); }

float FontFace::Advance(uint32_t cp, float px) const {
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&info_->font, static_cast<int>(cp), &adv, &lsb);
    return static_cast<float>(adv) * stbtt_ScaleForMappingEmToPixels(&info_->font, px);
}

float FontFace::Kern(uint32_t a, uint32_t b, float px) const {
    return static_cast<float>(stbtt_GetCodepointKernAdvance(&info_->font, static_cast<int>(a), static_cast<int>(b))) *
           stbtt_ScaleForMappingEmToPixels(&info_->font, px);
}

FontFace::Atlas& FontFace::AtlasFor(int px, bool bold) { return atlases_[px * 2 + (bold ? 1 : 0)]; }

FontFace::Glyph FontFace::GetGlyph(uint32_t cp, int px, bool bold) {
    std::lock_guard<std::mutex> lock(mutex_);
    px = std::max(1, std::min(px, 1024));
    Atlas& atlas = AtlasFor(px, bold);
    auto found = atlas.glyphs.find(cp);
    if (found != atlas.glyphs.end()) return found->second;

    Glyph g;
    float scale = stbtt_ScaleForMappingEmToPixels(&info_->font, static_cast<float>(px));
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&info_->font, static_cast<int>(cp), &adv, &lsb);
    g.advance = static_cast<float>(adv) * scale;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetCodepointBitmapBox(&info_->font, static_cast<int>(cp), scale, scale, &x0, &y0, &x1, &y1);
    int w = x1 - x0, h = y1 - y0;
    if (w > 0 && h > 0) {
        std::vector<unsigned char> bits(static_cast<size_t>(w) * static_cast<size_t>(h));
        stbtt_MakeCodepointBitmap(&info_->font, bits.data(), w, h, w, scale, scale, static_cast<int>(cp));
        // Faux bold: smear the coverage to the right (like most engines' synthetic bold).
        int grow = bold ? std::max(1, (px + 10) / 20) : 0;
        if (grow > 0) {
            int nw = w + grow;
            std::vector<unsigned char> thick(static_cast<size_t>(nw) * static_cast<size_t>(h), 0);
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < nw; ++x) {
                    unsigned char m = 0;
                    for (int k = 0; k <= grow; ++k) {
                        int sx = x - k;
                        if (sx >= 0 && sx < w) m = std::max(m, bits[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(sx)]);
                    }
                    thick[static_cast<size_t>(y) * static_cast<size_t>(nw) + static_cast<size_t>(x)] = m;
                }
            }
            bits.swap(thick);
            w = nw;
            g.advance += static_cast<float>(grow);
        }
        // Shelf packing with a 1-texel gap; a new page when this one is full.
        auto newPage = [&](int minW, int minH) {
            auto page = std::make_shared<Texture>();
            page->width = std::max(kPageSize, minW + 2);
            page->height = std::max(kPageSize, minH + 2);
            page->texels.assign(static_cast<size_t>(page->width) * static_cast<size_t>(page->height), 0x00FFFFFFu);
            atlas.pages.push_back(page);
            atlas.penX = 1;
            atlas.penY = 1;
            atlas.rowH = 0;
        };
        if (atlas.pages.empty()) newPage(w, h);
        Texture* page = atlas.pages.back().get();
        if (atlas.penX + w + 1 > page->width) {
            atlas.penX = 1;
            atlas.penY += atlas.rowH + 1;
            atlas.rowH = 0;
        }
        if (atlas.penY + h + 1 > page->height) {
            newPage(w, h);
            page = atlas.pages.back().get();
        }
        g.page = static_cast<int>(atlas.pages.size()) - 1;
        g.s = atlas.penX;
        g.t = atlas.penY;
        g.w = w;
        g.h = h;
        g.x0 = x0;
        g.y0 = y0;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                uint32_t a = bits[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)];
                page->texels[static_cast<size_t>(g.t + y) * static_cast<size_t>(page->width) + static_cast<size_t>(g.s + x)] = 0x00FFFFFFu | (a << 24);
            }
        }
        ++page->version;
        atlas.penX += w + 1;
        atlas.rowH = std::max(atlas.rowH, h);
    }
    atlas.glyphs[cp] = g;
    return g;
}

std::shared_ptr<const Texture> FontFace::Page(int px, bool bold, int page) {
    std::lock_guard<std::mutex> lock(mutex_);
    Atlas& atlas = AtlasFor(std::max(1, std::min(px, 1024)), bold);
    if (page < 0 || page >= static_cast<int>(atlas.pages.size())) return nullptr;
    return atlas.pages[static_cast<size_t>(page)];
}

size_t FontFace::atlasBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t n = 0;
    for (const auto& kv : atlases_) {
        for (const auto& p : kv.second.pages) n += p->texels.size() * 4;
    }
    return n;
}

}  // namespace oe
