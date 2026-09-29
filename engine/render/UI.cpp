#include "render/UI.h"

#include <algorithm>
#include <cmath>

#include "scene/Components.h"

namespace oe {

namespace {

// Classic 5x7 pixel font for ASCII 32..126. Each glyph is 5 columns; bit 0
// of a column is the top row. Glyph cell is 6x8 units (1 unit spacing).
const uint8_t kFont[95][5] = {
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

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines(1);
    for (char c : text) {
        if (c == '\n') lines.emplace_back();
        else lines.back() += c;
    }
    return lines;
}

// Counts UTF-8 code points so non-ASCII characters take one '?' cell.
size_t Glyphs(const std::string& line) {
    size_t n = 0;
    for (unsigned char c : line) {
        if ((c & 0xC0) != 0x80) ++n;
    }
    return n;
}

void Anchor(const std::string& name, float* ax, float* ay) {
    *ax = 0.0f;
    *ay = 0.0f;
    if (name.find("right") != std::string::npos) *ax = 1.0f;
    else if (name == "top" || name == "bottom" || name == "center") *ax = 0.5f;
    if (name.find("bottom") != std::string::npos) *ay = 1.0f;
    else if (name == "left" || name == "right" || name == "center") *ay = 0.5f;
}

UIRect Place(EntityId id, UIRect::Kind kind, const std::string& anchor, float x, float y, float w, float h, int order, int width, int height) {
    float s = static_cast<float>(height) / kUIReferenceHeight;
    float ax, ay;
    Anchor(anchor, &ax, &ay);
    UIRect r;
    r.entity = id;
    r.kind = kind;
    r.w = w;
    r.h = h;
    r.x = ax * static_cast<float>(width) + x * s - ax * w;
    r.y = ay * static_cast<float>(height) + y * s - ay * h;
    r.order = order;
    return r;
}

uint32_t Pack(const Color& c) {
    auto ch = [](float v) { return static_cast<uint32_t>(Clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return ch(c.r) | (ch(c.g) << 8) | (ch(c.b) << 16) | 0xFF000000u;
}

void FillRect(RenderTarget& t, float x0, float y0, float x1, float y1, const Color& c, float alpha, EntityId id) {
    int ix0 = std::max(0, static_cast<int>(std::lround(x0)));
    int iy0 = std::max(0, static_cast<int>(std::lround(y0)));
    int ix1 = std::min(t.width, static_cast<int>(std::lround(x1)));
    int iy1 = std::min(t.height, static_cast<int>(std::lround(y1)));
    uint32_t solid = Pack(c);
    for (int y = iy0; y < iy1; ++y) {
        for (int x = ix0; x < ix1; ++x) {
            size_t i = static_cast<size_t>(y) * static_cast<size_t>(t.width) + static_cast<size_t>(x);
            if (alpha >= 0.999f) {
                t.color[i] = solid;
            } else {
                uint32_t d = t.color[i];
                Color dc((d & 0xFF) / 255.0f, ((d >> 8) & 0xFF) / 255.0f, ((d >> 16) & 0xFF) / 255.0f);
                t.color[i] = Pack(dc * (1.0f - alpha) + c * alpha);
            }
            t.ids[i] = id;
        }
    }
}

// Draws text with its block's top-left at (x, y); lines are aligned inside
// the block by `ax` (0 left, 0.5 center, 1 right).
void DrawText(RenderTarget& t, const std::string& text, float x, float y, float lineHeight, float ax, const Color& c, EntityId id) {
    float u = lineHeight / 8.0f;
    float blockW, blockH;
    MeasureText(text, lineHeight, &blockW, &blockH);
    std::vector<std::string> lines = SplitLines(text);
    for (size_t li = 0; li < lines.size(); ++li) {
        float lineW = std::max(0.0f, static_cast<float>(Glyphs(lines[li])) * 6.0f - 1.0f) * u;
        float lx = x + (blockW - lineW) * ax;
        float ly = y + static_cast<float>(li) * 8.0f * u;
        size_t col = 0;
        for (size_t i = 0; i < lines[li].size(); ++i) {
            unsigned char ch = static_cast<unsigned char>(lines[li][i]);
            if ((ch & 0xC0) == 0x80) continue;  // UTF-8 continuation byte
            int glyph = (ch >= 32 && ch <= 126) ? ch - 32 : '?' - 32;
            for (int gx = 0; gx < 5; ++gx) {
                uint8_t bits = kFont[glyph][gx];
                for (int gy = 0; gy < 7; ++gy) {
                    if (!(bits & (1u << gy))) continue;
                    float px = lx + (static_cast<float>(col) * 6.0f + static_cast<float>(gx)) * u;
                    float py = ly + static_cast<float>(gy) * u;
                    FillRect(t, px, py, px + u, py + u, c, 1.0f, id);
                }
            }
            ++col;
        }
    }
}

}  // namespace

void MeasureText(const std::string& text, float lineHeight, float* w, float* h) {
    float u = lineHeight / 8.0f;
    size_t widest = 0;
    std::vector<std::string> lines = SplitLines(text);
    for (const std::string& l : lines) widest = std::max(widest, Glyphs(l));
    *w = std::max(0.0f, static_cast<float>(widest) * 6.0f - 1.0f) * u;
    *h = std::max(0.0f, static_cast<float>(lines.size()) * 8.0f - 1.0f) * u;
}

std::vector<UIRect> LayoutUI(const Scene& scene, int width, int height) {
    std::vector<UIRect> out;
    float s = static_cast<float>(height) / kUIReferenceHeight;
    for (const auto& kv : scene.Pool<UIPanel>()) {
        const UIPanel& p = kv.second;
        if (p.visible) out.push_back(Place(kv.first, UIRect::Kind::Panel, p.anchor, p.x, p.y, p.width * s, p.height * s, p.order, width, height));
    }
    for (const auto& kv : scene.Pool<UIButton>()) {
        const UIButton& b = kv.second;
        if (b.visible) out.push_back(Place(kv.first, UIRect::Kind::Button, b.anchor, b.x, b.y, b.width * s, b.height * s, b.order, width, height));
    }
    for (const auto& kv : scene.Pool<UIText>()) {
        const UIText& tx = kv.second;
        if (!tx.visible) continue;
        float w, h;
        MeasureText(tx.text, tx.size * s, &w, &h);
        out.push_back(Place(kv.first, UIRect::Kind::Text, tx.anchor, tx.x, tx.y, w, h, tx.order, width, height));
    }
    std::stable_sort(out.begin(), out.end(), [](const UIRect& a, const UIRect& b) {
        return a.order != b.order ? a.order < b.order : a.entity < b.entity;
    });
    return out;
}

void DrawUI(const Scene& scene, RenderTarget& target) {
    float s = static_cast<float>(target.height) / kUIReferenceHeight;
    for (const UIRect& r : LayoutUI(scene, target.width, target.height)) {
        switch (r.kind) {
            case UIRect::Kind::Panel: {
                const UIPanel* p = scene.Get<UIPanel>(r.entity);
                FillRect(target, r.x, r.y, r.x + r.w, r.y + r.h, p->color, p->opacity, r.entity);
                break;
            }
            case UIRect::Kind::Button: {
                const UIButton* b = scene.Get<UIButton>(r.entity);
                FillRect(target, r.x, r.y, r.x + r.w, r.y + r.h, b->color, 1.0f, r.entity);
                float tw, th;
                MeasureText(b->text, b->size * s, &tw, &th);
                DrawText(target, b->text, r.x + (r.w - tw) * 0.5f, r.y + (r.h - th) * 0.5f, b->size * s, 0.5f, b->textColor, r.entity);
                break;
            }
            case UIRect::Kind::Text: {
                const UIText* tx = scene.Get<UIText>(r.entity);
                float ax, ay;
                Anchor(tx->anchor, &ax, &ay);
                DrawText(target, tx->text, r.x, r.y, tx->size * s, ax, tx->color, r.entity);
                break;
            }
        }
    }
}

EntityId HitTestButton(const Scene& scene, float px, float py, int width, int height) {
    EntityId hit = kNullEntity;
    for (const UIRect& r : LayoutUI(scene, width, height)) {
        if (r.kind == UIRect::Kind::Button && px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h) hit = r.entity;  // last = topmost
    }
    return hit;
}

}  // namespace oe
