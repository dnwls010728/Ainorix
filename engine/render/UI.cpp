#include "render/UI.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "assets/Assets.h"
#include "render/Font.h"
#include "scene/Components.h"

namespace oe {

namespace {

// ----- Geometry --------------------------------------------------------------

struct Box {
    float x = 0, y = 0, w = 0, h = 0;
};

struct Clip {
    bool on = false;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    Clip Intersect(const Box& b) const {
        Clip c;
        c.on = true;
        c.x0 = b.x;
        c.y0 = b.y;
        c.x1 = b.x + b.w;
        c.y1 = b.y + b.h;
        if (on) {
            c.x0 = std::max(c.x0, x0);
            c.y0 = std::max(c.y0, y0);
            c.x1 = std::min(c.x1, x1);
            c.y1 = std::min(c.y1, y1);
        }
        return c;
    }
    bool Contains(float px, float py) const { return !on || (px >= x0 && px < x1 && py >= y0 && py < y1); }
};

// Anchor presets as Unity-style anchorMin/anchorMax/pivot (0 = left/top, 1 = right/bottom).
struct AnchorSpec {
    float minX = 0, maxX = 0, pivotX = 0;
    float minY = 0, maxY = 0, pivotY = 0;
};

AnchorSpec ParseAnchor(const std::string& name) {
    AnchorSpec a;
    auto pinX = [&](float v) { a.minX = a.maxX = a.pivotX = v; };
    auto pinY = [&](float v) { a.minY = a.maxY = a.pivotY = v; };
    auto stretchX = [&] { a.minX = 0.0f, a.maxX = 1.0f, a.pivotX = 0.5f; };
    auto stretchY = [&] { a.minY = 0.0f, a.maxY = 1.0f, a.pivotY = 0.5f; };
    if (name.rfind("stretch", 0) == 0) {
        std::string rest = name.size() > 8 ? name.substr(8) : "";
        if (rest.empty()) {
            stretchX();
            stretchY();
        } else if (rest == "top" || rest == "middle" || rest == "bottom") {
            stretchX();
            pinY(rest == "top" ? 0.0f : rest == "middle" ? 0.5f : 1.0f);
        } else {
            stretchY();
            pinX(rest == "left" ? 0.0f : rest == "center" ? 0.5f : 1.0f);
        }
        return a;
    }
    float ax = 0.0f, ay = 0.0f;
    if (name.find("right") != std::string::npos) ax = 1.0f;
    else if (name == "top" || name == "bottom" || name == "center") ax = 0.5f;
    if (name.find("bottom") != std::string::npos) ay = 1.0f;
    else if (name == "left" || name == "right" || name == "center") ay = 0.5f;
    pinX(ax);
    pinY(ay);
    return a;
}

struct Placement {
    std::string anchor;
    float x = 0, y = 0, w = 0, h = 0;  // reference pixels
};

// Rectangle of an element in its parent. `natW/natH` (pixels) replace the
// size on pinned axes when >= 0; on stretched axes the size field is added
// to the parent's size.
Box Place(const Placement& p, const Box& parent, float s, float natW, float natH) {
    AnchorSpec a = ParseAnchor(p.anchor);
    Box b;
    bool sx = a.maxX > a.minX, sy = a.maxY > a.minY;
    b.w = sx ? (a.maxX - a.minX) * parent.w + p.w * s : (natW >= 0.0f ? natW : p.w * s);
    b.h = sy ? (a.maxY - a.minY) * parent.h + p.h * s : (natH >= 0.0f ? natH : p.h * s);
    b.w = std::max(0.0f, b.w);
    b.h = std::max(0.0f, b.h);
    float px = parent.x + (a.minX + a.pivotX * (a.maxX - a.minX)) * parent.w + p.x * s;
    float py = parent.y + (a.minY + a.pivotY * (a.maxY - a.minY)) * parent.h + p.y * s;
    b.x = px - a.pivotX * b.w;
    b.y = py - a.pivotY * b.h;
    return b;
}

// ----- Fonts and text ------------------------------------------------------------

struct FontRef {
    std::shared_ptr<FontFace> face;  // null = built-in pixel font
};

FontRef ResolveFont(const std::string& name, AssetManager* assets) {
    if (name == "pixel") return {};
    if (name.empty() || name == "default" || !assets) return {FontFace::Default()};
    std::shared_ptr<FontFace> f = assets->GetFont(name);
    return {f ? f : FontFace::Default()};
}

struct RichChar {
    uint32_t cp;
    Color color;
    bool bold;
};

bool ParseColorValue(const std::string& v, Color* out) {
    static const std::map<std::string, Color> kNames = {
        {"white", {1, 1, 1}},        {"black", {0, 0, 0}},          {"red", {1, 0.2f, 0.2f}},    {"green", {0.3f, 0.9f, 0.3f}},
        {"blue", {0.3f, 0.5f, 1}},   {"yellow", {1, 0.9f, 0.2f}},   {"orange", {1, 0.6f, 0.1f}}, {"gray", {0.6f, 0.6f, 0.6f}},
        {"grey", {0.6f, 0.6f, 0.6f}}, {"cyan", {0.2f, 0.9f, 1}},   {"magenta", {1, 0.3f, 1}},   {"purple", {0.6f, 0.3f, 1}},
    };
    auto it = kNames.find(v);
    if (it != kNames.end()) {
        *out = it->second;
        return true;
    }
    if (v.empty() || v[0] != '#' || (v.size() != 4 && v.size() != 7)) return false;
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int d[6];
    for (size_t i = 1; i < v.size(); ++i) {
        if ((d[i - 1] = hex(v[i])) < 0) return false;
    }
    if (v.size() == 4) *out = Color(d[0] / 15.0f, d[1] / 15.0f, d[2] / 15.0f);
    else *out = Color((d[0] * 16 + d[1]) / 255.0f, (d[2] * 16 + d[3]) / 255.0f, (d[4] * 16 + d[5]) / 255.0f);
    return true;
}

// Text -> characters with color/bold. Tags: <color=#rgb|#rrggbb|name>, </color>, <b>, </b>.
// Anything else (and everything when rich is false) is literal text.
std::vector<RichChar> ParseRich(const std::string& text, bool rich, const Color& base, bool bold) {
    std::vector<RichChar> out;
    std::vector<uint32_t> cps = DecodeUtf8(text);
    std::vector<Color> colors{base};
    int boldDepth = bold ? 1 : 0;
    for (size_t i = 0; i < cps.size(); ++i) {
        if (rich && cps[i] == '<') {
            size_t end = i + 1;
            while (end < cps.size() && cps[end] != '>' && end - i < 32) ++end;
            if (end < cps.size() && cps[end] == '>') {
                std::string tag;
                for (size_t k = i + 1; k < end; ++k) tag += cps[k] < 128 ? static_cast<char>(cps[k]) : '?';
                Color c;
                bool handled = true;
                if (tag == "b") ++boldDepth;
                else if (tag == "/b") boldDepth = std::max(bold ? 1 : 0, boldDepth - 1);
                else if (tag == "/color") {
                    if (colors.size() > 1) colors.pop_back();
                } else if (tag.rfind("color=", 0) == 0 && ParseColorValue(tag.substr(6), &c)) colors.push_back(c);
                else handled = false;
                if (handled) {
                    i = end;
                    continue;
                }
            }
        }
        if (cps[i] == '\r') continue;
        out.push_back({cps[i], colors.back(), boldDepth > 0});
    }
    return out;
}

bool IsCjk(uint32_t cp) { return (cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x3400 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF); }

struct ShapedGlyph {
    uint32_t cp = 0;
    float x = 0;    // pen position from the line start
    float ink = 0;  // width this glyph adds to the line if it is the last one
    Color color;
    bool bold = false;
    FontFace* face = nullptr;
    int index = 0;  // character index (visibleCharacters)
};

struct ShapedText {
    FontRef font;
    int px = 0;       // TrueType: whole pixel size used for metrics and bitmaps
    float unit = 0;   // pixel font: size of one font pixel
    float ascent = 0;
    float lineHeight = 0;
    float width = 0, height = 0;
    std::vector<std::vector<ShapedGlyph>> lines;
    std::vector<float> lineWidths;
};

ShapedText Shape(const std::vector<RichChar>& chars, const FontRef& font, float sizePx, float letterPx, float lineSpacing, float wrapWidth) {
    ShapedText t;
    t.font = font;
    FontFace* primary = font.face.get();
    FontFace* fallback = FontFace::Default().get();
    float inkHeight;
    if (primary) {
        t.px = std::max(1, static_cast<int>(std::lround(sizePx)));
        float px = static_cast<float>(t.px);
        t.ascent = primary->Ascent(px);
        inkHeight = t.ascent - primary->Descent(px);
        t.lineHeight = (inkHeight + primary->LineGap(px)) * lineSpacing;
    } else {
        t.unit = sizePx / 8.0f;
        t.ascent = 7.0f * t.unit;
        inkHeight = 7.0f * t.unit;
        t.lineHeight = 8.0f * t.unit * lineSpacing;
    }
    t.lines.emplace_back();
    bool wrapped = false;  // the current line started because of wrapping
    float pen = 0.0f;
    uint32_t prev = 0;
    FontFace* prevFace = nullptr;
    auto lineWidth = [](const std::vector<ShapedGlyph>& line) { return line.empty() ? 0.0f : line.back().x + line.back().ink; };
    for (size_t i = 0; i < chars.size(); ++i) {
        const RichChar& c = chars[i];
        if (c.cp == '\n') {
            t.lines.emplace_back();
            wrapped = false;
            pen = 0.0f;
            prev = 0;
            continue;
        }
        std::vector<ShapedGlyph>* line = &t.lines.back();
        if (wrapped && line->empty() && c.cp == ' ') continue;
        ShapedGlyph g;
        g.cp = c.cp;
        g.color = c.color;
        g.bold = c.bold;
        g.index = static_cast<int>(i);
        float advance;
        if (primary) {
            g.face = primary->HasGlyph(c.cp) || !fallback->HasGlyph(c.cp) ? primary : fallback;
            advance = g.face->GetGlyph(c.cp, t.px, c.bold).advance;
            if (prev && prevFace == g.face) pen += g.face->Kern(prev, c.cp, static_cast<float>(t.px));
            g.ink = advance;
        } else {
            advance = 6.0f * t.unit;
            g.ink = (c.bold ? 6.0f : 5.0f) * t.unit;
        }
        g.x = pen;
        if (wrapWidth > 0.0f && !line->empty() && g.x + g.ink > wrapWidth + 0.5f) {
            if (c.cp == ' ') {  // a space overflows: break here and drop it
                t.lines.emplace_back();
                wrapped = true;
                pen = 0.0f;
                prev = 0;
                continue;
            }
            // Break at the last space, or next to a CJK character; a word longer than the line breaks anywhere.
            size_t cut = line->size();
            bool dropSpace = false;
            if (!IsCjk(c.cp) && !IsCjk(line->back().cp)) {
                for (size_t k = line->size() - 1; k > 0; --k) {
                    uint32_t cp = (*line)[k].cp;
                    if (cp == ' ') {
                        cut = k;
                        dropSpace = true;
                        break;
                    }
                    if (IsCjk(cp) || IsCjk((*line)[k - 1].cp)) {
                        cut = k;
                        break;
                    }
                }
            }
            std::vector<ShapedGlyph> moved(line->begin() + static_cast<std::ptrdiff_t>(cut + (dropSpace ? 1 : 0)), line->end());
            line->erase(line->begin() + static_cast<std::ptrdiff_t>(cut), line->end());
            t.lines.emplace_back();
            line = &t.lines.back();
            wrapped = true;
            float shift = moved.empty() ? 0.0f : moved.front().x;
            for (ShapedGlyph& m : moved) {
                m.x -= shift;
                line->push_back(m);
            }
            g.x = moved.empty() ? 0.0f : g.x - shift;
        }
        line->push_back(g);
        pen = g.x + advance + letterPx;
        prev = c.cp;
        prevFace = g.face;
    }
    for (const auto& line : t.lines) {
        float w = lineWidth(line);
        t.lineWidths.push_back(w);
        t.width = std::max(t.width, w);
    }
    t.height = static_cast<float>(t.lines.size() - 1) * t.lineHeight + inkHeight;
    return t;
}

// ----- Quads ----------------------------------------------------------------------

void AddQuad(std::vector<UIQuad>& out, UIQuad q, const Clip& clip) {
    if (clip.on) {
        int cx0 = static_cast<int>(std::lround(clip.x0)), cy0 = static_cast<int>(std::lround(clip.y0));
        int cx1 = static_cast<int>(std::lround(clip.x1)), cy1 = static_cast<int>(std::lround(clip.y1));
        int x0 = std::max(q.x0, cx0), y0 = std::max(q.y0, cy0), x1 = std::min(q.x1, cx1), y1 = std::min(q.y1, cy1);
        if (x1 <= x0 || y1 <= y0) return;
        if (q.texture && (x0 != q.x0 || y0 != q.y0 || x1 != q.x1 || y1 != q.y1)) {
            float fw = static_cast<float>(q.x1 - q.x0), fh = static_cast<float>(q.y1 - q.y0);
            float ds = (q.s1 - q.s0) / fw, dt = (q.t1 - q.t0) / fh;
            float s0 = q.s0 + static_cast<float>(x0 - q.x0) * ds, s1 = q.s0 + static_cast<float>(x1 - q.x0) * ds;
            float t0 = q.t0 + static_cast<float>(y0 - q.y0) * dt, t1 = q.t0 + static_cast<float>(y1 - q.y0) * dt;
            q.s0 = s0, q.s1 = s1, q.t0 = t0, q.t1 = t1;
        }
        q.x0 = x0, q.y0 = y0, q.x1 = x1, q.y1 = y1;
    }
    if (q.x1 <= q.x0 || q.y1 <= q.y0 || q.alpha <= 0.0f) return;
    out.push_back(std::move(q));
}

UIQuad RectQuad(float x0, float y0, float x1, float y1, const Color& c, float alpha, EntityId id) {
    UIQuad q;
    q.x0 = static_cast<int>(std::lround(x0));
    q.y0 = static_cast<int>(std::lround(y0));
    q.x1 = static_cast<int>(std::lround(x1));
    q.y1 = static_cast<int>(std::lround(y1));
    q.color = c;
    q.alpha = alpha;
    q.entity = id;
    return q;
}

void Shape(UIQuad& q, const Box& b, float radius, float border, const Color& borderColor, float borderAlpha) {
    radius = std::max(0.0f, std::min(radius, std::min(b.w, b.h) * 0.5f));
    if (radius < 0.01f && border < 0.01f) return;
    q.shaped = true;
    q.shape[0] = std::round(b.x);
    q.shape[1] = std::round(b.y);
    q.shape[2] = std::round(b.x + b.w);
    q.shape[3] = std::round(b.y + b.h);
    q.radius = radius;
    q.border = std::max(0.0f, border);
    q.borderColor = borderColor;
    q.borderAlpha = borderAlpha;
}

void AddBox(std::vector<UIQuad>& out, const Box& b, const Color& c, float alpha, float radius, float border, const Color& borderColor, EntityId id,
            const Clip& clip) {
    UIQuad q = RectQuad(b.x, b.y, b.x + b.w, b.y + b.h, c, alpha, id);
    Shape(q, b, radius, border, borderColor, alpha);
    AddQuad(out, std::move(q), clip);
}

Color Brighten(const Color& c, float k) { return Color(Clamp(c.r * k, 0.0f, 1.0f), Clamp(c.g * k, 0.0f, 1.0f), Clamp(c.b * k, 0.0f, 1.0f)); }

struct TextDraw {
    float alpha = 1.0f;
    float alignX = 0.0f, alignY = 0.0f;
    int maxChars = -1;
    float outline = 0.0f;  // pixels
    Color outlineColor;
    float shadow = 0.0f;  // pixels
    Color shadowColor;
};

// Emits shaped text inside box `b`.
void EmitText(std::vector<UIQuad>& out, const ShapedText& t, const Box& b, const TextDraw& d, EntityId id, const Clip& clip) {
    float top = b.y + (b.h - t.height) * d.alignY;
    struct Pass {
        float ox, oy;
        const Color* color;  // null = glyph color
        float alpha;
    };
    std::vector<Pass> passes;
    if (d.shadow > 0.0f) {
        float o = std::max(1.0f, std::round(d.shadow));
        passes.push_back({o, o, &d.shadowColor, d.alpha * 0.5f});
    }
    if (d.outline > 0.0f) {
        float o = std::max(1.0f, std::round(d.outline));
        int dirs = o > 2.0f ? 16 : 8;
        for (int k = 0; k < dirs; ++k) {
            float a = static_cast<float>(k) * 6.2831853f / static_cast<float>(dirs);
            passes.push_back({std::round(std::cos(a) * o), std::round(std::sin(a) * o), &d.outlineColor, d.alpha});
        }
    }
    passes.push_back({0, 0, nullptr, d.alpha});
    for (const Pass& pass : passes) {
        for (size_t li = 0; li < t.lines.size(); ++li) {
            float lineX = b.x + (b.w - t.lineWidths[li]) * d.alignX;
            float lineTop = top + static_cast<float>(li) * t.lineHeight;
            for (const ShapedGlyph& g : t.lines[li]) {
                if (d.maxChars >= 0 && g.index >= d.maxChars) continue;
                if (g.cp == ' ') continue;
                const Color& c = pass.color ? *pass.color : g.color;
                if (!t.font.face) {
                    const uint8_t* bits = PixelFontGlyph(g.cp);
                    float u = t.unit;
                    for (int bold = 0; bold <= (g.bold ? 1 : 0); ++bold) {
                        for (int gx = 0; gx < 5; ++gx) {
                            for (int gy = 0; gy < 7; ++gy) {
                                if (!(bits[gx] & (1u << gy))) continue;
                                float px = lineX + g.x + static_cast<float>(gx + bold) * u + pass.ox;
                                float py = lineTop + static_cast<float>(gy) * u + pass.oy;
                                AddQuad(out, RectQuad(px, py, px + u, py + u, c, pass.alpha, id), clip);
                            }
                        }
                    }
                    continue;
                }
                FontFace::Glyph gl = g.face->GetGlyph(g.cp, t.px, g.bold);
                if (gl.page < 0) continue;
                UIQuad q;
                float baseline = std::round(lineTop + t.ascent);
                q.x0 = static_cast<int>(std::lround(lineX + g.x + pass.ox)) + gl.x0;
                q.y0 = static_cast<int>(baseline + pass.oy) + gl.y0;
                q.x1 = q.x0 + gl.w;
                q.y1 = q.y0 + gl.h;
                q.color = c;
                q.alpha = pass.alpha;
                q.texture = g.face->Page(t.px, g.bold, gl.page);
                q.s0 = static_cast<float>(gl.s);
                q.t0 = static_cast<float>(gl.t);
                q.s1 = static_cast<float>(gl.s + gl.w);
                q.t1 = static_cast<float>(gl.t + gl.h);
                q.nearest = true;
                q.entity = id;
                AddQuad(out, std::move(q), clip);
            }
        }
    }
}

// ----- The element tree -------------------------------------------------------------

// Kinds in the order they draw on one entity (background first).
int KindRank(UIRect::Kind k) {
    switch (k) {
        case UIRect::Kind::Panel: return 0;
        case UIRect::Kind::Image: return 1;
        case UIRect::Kind::Slider: return 2;
        case UIRect::Kind::Button: return 3;
        case UIRect::Kind::Text: return 4;
    }
    return 5;
}

struct Elem {
    UIRect::Kind kind;
    Placement place;
    float opacity = 1.0f;
    bool visible = true;
    int order = 0;
};

struct Node {
    EntityId id = kNullEntity;
    EntityId parent = kNullEntity;
    std::vector<Elem> elems;  // draw order
    std::vector<EntityId> kids;
    const Elem* primary = nullptr;  // defines the rectangle children live in
};

class Builder {
public:
    Builder(const Scene& scene, int width, int height, AssetManager* assets) : scene_(scene), assets_(assets) {
        screen_.w = static_cast<float>(width);
        screen_.h = static_cast<float>(height);
        s_ = UIScale(scene, width, height);
        parentBox_ = screen_;
        Collect();
    }

    // Lays out every visible element; `emit` also produces quads.
    void Run(std::vector<UIRect>* rects, std::vector<UIQuad>* quads) {
        rects_ = rects;
        quads_ = quads;
        std::vector<EntityId> roots;
        for (const auto& kv : nodes_) {
            if (kv.second.parent == kNullEntity) roots.push_back(kv.first);
        }
        SortSiblings(roots);
        for (EntityId r : roots) {
            const Node& n = nodes_.at(r);
            Visit(n, Place(n, screen_), Clip{}, 1.0f);
        }
    }

private:
    template <class T>
    void AddElems(UIRect::Kind kind) {
        for (const auto& kv : scene_.Pool<T>()) {
            const T& c = kv.second;
            Elem e;
            e.kind = kind;
            e.place = {c.anchor, c.x, c.y, c.width, c.height};
            e.opacity = c.opacity;
            e.visible = c.visible;
            e.order = c.order;
            nodes_[kv.first].elems.push_back(e);
        }
    }

    void Collect() {
        AddElems<UIPanel>(UIRect::Kind::Panel);
        AddElems<UIImage>(UIRect::Kind::Image);
        AddElems<UISlider>(UIRect::Kind::Slider);
        AddElems<UIButton>(UIRect::Kind::Button);
        AddElems<UIText>(UIRect::Kind::Text);
        for (auto& kv : nodes_) {
            Node& n = kv.second;
            n.id = kv.first;
            std::stable_sort(n.elems.begin(), n.elems.end(), [](const Elem& a, const Elem& b) {
                return a.order != b.order ? a.order < b.order : KindRank(a.kind) < KindRank(b.kind);
            });
            // The rectangle for children: the panel, else the first element in rank order.
            for (const Elem& e : n.elems) {
                if (!n.primary || KindRank(e.kind) < KindRank(n.primary->kind)) n.primary = &e;
            }
            const EntityRecord* rec = scene_.Record(kv.first);
            EntityId p = rec ? rec->parent : kNullEntity;
            for (int guard = 0; p != kNullEntity && guard < 1000; ++guard) {
                if (nodes_.count(p)) break;
                const EntityRecord* pr = scene_.Record(p);
                p = pr ? pr->parent : kNullEntity;
            }
            n.parent = p;
        }
        for (auto& kv : nodes_) {
            if (kv.second.parent != kNullEntity) nodes_[kv.second.parent].kids.push_back(kv.first);
        }
        for (auto& kv : nodes_) SortSiblings(kv.second.kids);
    }

    void SortSiblings(std::vector<EntityId>& ids) const {
        std::stable_sort(ids.begin(), ids.end(), [&](EntityId a, EntityId b) {
            int oa = nodes_.at(a).primary->order, ob = nodes_.at(b).primary->order;
            return oa != ob ? oa < ob : a < b;
        });
    }

    const UILayout* LayoutOf(const Node& n) const { return scene_.Get<UILayout>(n.id); }

    // ----- sizes

    ShapedText ShapeTextOf(const UIText& t, float wrapWidth) const {
        FontRef font = ResolveFont(t.font, assets_);
        return oe::Shape(ParseRich(t.text, t.richText, t.color, t.bold), font, t.size * s_, t.letterSpacing * s_, t.lineSpacing, wrapWidth);
    }

    // Text box width decides wrapping: a width field or a stretched axis.
    float TextWrapWidth(const UIText& t, const Box& parent) const {
        if (!t.wrap) return 0.0f;
        AnchorSpec a = ParseAnchor(t.anchor);
        if (a.maxX > a.minX) return std::max(1.0f, parent.w + t.width * s_);
        return t.width > 0.0f ? t.width * s_ : 0.0f;
    }

    void ImageFrame(const UIImage& im, const Texture* tex, float* s0, float* t0, float* fw, float* fh) const {
        int cols = std::max(1, im.columns), rows = std::max(1, im.rows);
        *fw = static_cast<float>(tex->width) / static_cast<float>(cols);
        *fh = static_cast<float>(tex->height) / static_cast<float>(rows);
        int f = std::max(0, std::min(im.frame, cols * rows - 1));
        *s0 = static_cast<float>(f % cols) * *fw;
        *t0 = static_cast<float>(f / cols) * *fh;
    }

    std::shared_ptr<const Texture> TextureOf(const UIImage& im) const {
        if (!assets_ || im.texture.empty()) return nullptr;
        return assets_->GetTexture(im.texture);
    }

    // Natural size of one element in pixels (-1 = use the size fields).
    void Natural(const Node& n, const Elem& e, const Box& parent, float* w, float* h) const {
        *w = -1.0f;
        *h = -1.0f;
        if (e.kind == UIRect::Kind::Text) {
            const UIText* t = scene_.Get<UIText>(n.id);
            ShapedText sh = ShapeTextOf(*t, TextWrapWidth(*t, parent));
            if (t->width <= 0.0f) *w = sh.width;
            if (t->height <= 0.0f) *h = sh.height;
        } else if (e.kind == UIRect::Kind::Image) {
            const UIImage* im = scene_.Get<UIImage>(n.id);
            std::shared_ptr<const Texture> tex = TextureOf(*im);
            if (tex && tex->width > 0) {
                float s0, t0, fw, fh;
                ImageFrame(*im, tex.get(), &s0, &t0, &fw, &fh);
                if (im->width <= 0.0f) *w = im->height > 0.0f ? im->height * s_ * fw / fh : fw * s_;
                if (im->height <= 0.0f) *h = im->width > 0.0f ? im->width * s_ * fh / fw : fh * s_;
            }
        }
        if (&e == n.primary) {
            const UILayout* lay = LayoutOf(n);
            if (lay && lay->fit) {
                float cw, ch;
                Content(n, *lay, &cw, &ch);
                *w = cw;
                *h = ch;
            }
        }
    }

    // Size of a child inside a layout (pixels).
    void ChildSize(const Node& kid, const Box& parent, float* w, float* h) const {
        const Elem& e = *kid.primary;
        Natural(kid, e, parent, w, h);
        if (*w < 0.0f) *w = std::max(0.0f, e.place.w * s_);
        if (*h < 0.0f) *h = std::max(0.0f, e.place.h * s_);
    }

    void Content(const Node& n, const UILayout& lay, float* w, float* h) const {
        float pad = lay.padding * s_, gap = lay.spacing * s_;
        float sumW = 0, sumH = 0, maxW = 0, maxH = 0;
        int count = 0;
        for (EntityId k : n.kids) {
            const Node& kid = nodes_.at(k);
            if (!kid.primary->visible) continue;
            float cw, ch;
            ChildSize(kid, screen_, &cw, &ch);
            sumW += cw;
            sumH += ch;
            maxW = std::max(maxW, cw);
            maxH = std::max(maxH, ch);
            ++count;
        }
        float gaps = count > 1 ? gap * static_cast<float>(count - 1) : 0.0f;
        if (lay.direction == "horizontal") {
            *w = sumW + gaps;
            *h = maxH;
        } else if (lay.direction == "grid") {
            int cols = std::max(1, std::min(lay.columns, std::max(1, count)));
            int rowsN = (count + std::max(1, lay.columns) - 1) / std::max(1, lay.columns);
            *w = static_cast<float>(cols) * maxW + gap * static_cast<float>(cols - 1);
            *h = static_cast<float>(rowsN) * maxH + gap * static_cast<float>(std::max(0, rowsN - 1));
        } else {
            *w = maxW;
            *h = sumH + gaps;
        }
        *w += 2.0f * pad;
        *h += 2.0f * pad;
    }

    Box PlaceElem(const Node& n, const Elem& e, const Box& parent) const {
        float w, h;
        Natural(n, e, parent, &w, &h);
        return oe::Place(e.place, parent, s_, w, h);
    }

    Box Place(const Node& n, const Box& parent) const { return PlaceElem(n, *n.primary, parent); }

    // Boxes of the children of a node with a UILayout, placed in hierarchy (id) order.
    std::map<EntityId, Box> Arrange(const Node& n, const UILayout& lay, const Box& b) const {
        float pad = lay.padding * s_, gap = lay.spacing * s_;
        Box inner{b.x + pad, b.y + pad, std::max(0.0f, b.w - 2 * pad), std::max(0.0f, b.h - 2 * pad)};
        std::vector<EntityId> kids = n.kids;
        std::sort(kids.begin(), kids.end());
        std::vector<Box> boxes(kids.size());
        std::vector<size_t> shown;
        std::vector<float> ws(kids.size()), hs(kids.size());
        for (size_t i = 0; i < kids.size(); ++i) {
            const Node& kid = nodes_.at(kids[i]);
            if (!kid.primary->visible) continue;
            ChildSize(kid, inner, &ws[i], &hs[i]);
            shown.push_back(i);
        }
        auto factor = [](const std::string& a) { return a == "center" ? 0.5f : a == "end" ? 1.0f : 0.0f; };
        bool stretch = lay.crossAlign == "stretch";
        float cross = factor(lay.crossAlign);
        if (lay.direction == "grid") {
            int cols = std::max(1, lay.columns);
            float cw = 0, ch = 0;
            for (size_t i : shown) cw = std::max(cw, ws[i]), ch = std::max(ch, hs[i]);
            if (stretch) cw = (inner.w - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
            int used = std::min(cols, static_cast<int>(shown.size()));
            int rowsN = (static_cast<int>(shown.size()) + cols - 1) / cols;
            float gw = static_cast<float>(used) * cw + gap * static_cast<float>(std::max(0, used - 1));
            float gh = static_cast<float>(rowsN) * ch + gap * static_cast<float>(std::max(0, rowsN - 1));
            float ox = inner.x + (inner.w - gw) * factor(lay.align), oy = inner.y + (inner.h - gh) * (stretch ? 0.0f : cross);
            for (size_t k = 0; k < shown.size(); ++k) {
                int col = static_cast<int>(k) % cols, row = static_cast<int>(k) / cols;
                boxes[shown[k]] = {ox + static_cast<float>(col) * (cw + gap), oy + static_cast<float>(row) * (ch + gap), cw, ch};
            }
            return ToMap(kids, boxes);
        }
        bool horizontal = lay.direction == "horizontal";
        float total = 0;
        for (size_t i : shown) total += horizontal ? ws[i] : hs[i];
        if (shown.size() > 1) total += gap * static_cast<float>(shown.size() - 1);
        float pos = (horizontal ? inner.x + (inner.w - total) * factor(lay.align) : inner.y + (inner.h - total) * factor(lay.align));
        for (size_t i : shown) {
            Box& c = boxes[i];
            if (horizontal) {
                c.w = ws[i];
                c.h = stretch ? inner.h : hs[i];
                c.x = pos;
                c.y = inner.y + (inner.h - c.h) * cross;
                pos += c.w + gap;
            } else {
                c.h = hs[i];
                c.w = stretch ? inner.w : ws[i];
                c.y = pos;
                c.x = inner.x + (inner.w - c.w) * cross;
                pos += c.h + gap;
            }
        }
        return ToMap(kids, boxes);
    }

    static std::map<EntityId, Box> ToMap(const std::vector<EntityId>& ids, const std::vector<Box>& boxes) {
        std::map<EntityId, Box> m;
        for (size_t i = 0; i < ids.size(); ++i) m[ids[i]] = boxes[i];
        return m;
    }

    // Entrance animation: offset / scale of the node's rectangle and an opacity factor for its subtree.
    static void ApplyMotion(const UIMotion& m, float s, Box& box, float& alpha) {
        if (m.enter == "none") return;
        const float p = Clamp((m.time - m.delay) / std::max(m.duration, 1e-3f), 0.0f, 1.0f);
        if (p >= 1.0f) return;
        const float k = 1.0f - (1.0f - p) * (1.0f - p) * (1.0f - p);  // ease out
        alpha *= std::min(1.0f, p * 2.0f);
        const float d = m.distance * s * (1.0f - k);
        if (m.enter == "slide-up") box.y += d;
        else if (m.enter == "slide-down") box.y -= d;
        else if (m.enter == "slide-left") box.x += d;
        else if (m.enter == "slide-right") box.x -= d;
        else if (m.enter == "pop") {
            const float c = 1.70158f, q = p - 1.0f;
            ScaleBox(box, 0.85f + 0.15f * (1.0f + (c + 1.0f) * q * q * q + c * q * q));  // overshoots a little
        }
    }

    static void ScaleBox(Box& box, float scale) {
        const float cx = box.x + box.w * 0.5f, cy = box.y + box.h * 0.5f;
        box.w *= scale;
        box.h *= scale;
        box.x = cx - box.w * 0.5f;
        box.y = cy - box.h * 0.5f;
    }

    // `box` is the node's rectangle (from its parent or a layout); `inherit` the opacity factor of animated ancestors.
    void Visit(const Node& n, Box box, const Clip& clip, float inherit) {
        if (!n.primary->visible) return;
        if (const UIMotion* motion = scene_.Get<UIMotion>(n.id)) ApplyMotion(*motion, s_, box, inherit);
        if (n.primary->kind == UIRect::Kind::Button) {
            const float scale = scene_.Get<UIButton>(n.id)->scaleNow;
            if (scale != 1.0f) ScaleBox(box, scale);
        }
        // A scroll view lays its children out in a rectangle shifted by the scroll offset and clips them.
        const UILayout* lay = LayoutOf(n);
        const UIScroll* scroll = scene_.Get<UIScroll>(n.id);
        Box kidsBox = box;
        std::map<EntityId, Box> arranged;
        float extent = 0, offset = 0;
        bool horizontal = false;
        if (scroll) {
            horizontal = scroll->direction == "horizontal";
            if (lay) arranged = Arrange(n, *lay, box);
            for (EntityId k : n.kids) {
                const Node& kid = nodes_.at(k);
                if (!kid.primary->visible) continue;
                const Box kb = lay ? arranged[k] : Place(kid, box);
                extent = std::max(extent, horizontal ? kb.x + kb.w - box.x : kb.y + kb.h - box.y);
            }
            if (lay) extent += lay->padding * s_;
            const float most = std::max(0.0f, extent - (horizontal ? box.w : box.h));
            offset = Clamp(scroll->scroll * s_, 0.0f, most);
            (horizontal ? kidsBox.x : kidsBox.y) -= offset;
            scrollMax_[n.id] = most / s_;
        }
        const Node* parentNode = n.parent != kNullEntity ? &nodes_.at(n.parent) : nullptr;
        bool inLayout = parentNode && LayoutOf(*parentNode);
        for (const Elem& e : n.elems) {
            if (!e.visible) continue;
            Box b = box;
            if (&e != n.primary && !inLayout) b = PlaceElem(n, e, parentBox_);
            Emit(n, e, b, Clamp(e.opacity, 0.0f, 1.0f) * inherit, clip);
        }
        Clip kidsClip = clip;
        if (scroll || (n.primary->kind == UIRect::Kind::Panel && scene_.Get<UIPanel>(n.id)->clip)) kidsClip = clip.Intersect(box);
        if (lay) arranged = Arrange(n, *lay, kidsBox);
        for (EntityId k : n.kids) {
            const Node& kid = nodes_.at(k);
            Box saved = parentBox_;
            parentBox_ = kidsBox;
            Box kb = lay ? arranged[k] : Place(kid, kidsBox);
            Visit(kid, kb, kidsClip, inherit);
            parentBox_ = saved;
        }
        // Scroll bar thumb: its length shows how much of the content is visible.
        if (scroll && scroll->bar && quads_ && extent > (horizontal ? box.w : box.h) + 0.5f) {
            const UIThumb t = ScrollThumb(horizontal ? box.w : box.h, extent, offset, s_);
            Box thumb = horizontal ? Box{box.x + t.at, box.y + box.h - t.thickness - t.margin, t.length, t.thickness}
                                   : Box{box.x + box.w - t.thickness - t.margin, box.y + t.at, t.thickness, t.length};
            AddBox(*quads_, thumb, scroll->barColor, 0.6f * inherit, t.thickness * 0.5f, 0.0f, scroll->barColor, n.id, clip);
        }
    }

    void Emit(const Node& n, const Elem& e, const Box& b, float alpha, const Clip& clip) {
        if (rects_) {
            UIRect r;
            r.entity = n.id;
            r.kind = e.kind;
            r.x = b.x;
            r.y = b.y;
            r.w = b.w;
            r.h = b.h;
            r.order = e.order;
            r.parent = n.parent;
            r.opacity = alpha;
            if (e.kind == UIRect::Kind::Button) r.interactable = scene_.Get<UIButton>(n.id)->interactable;
            if (e.kind == UIRect::Kind::Panel) r.blocksInput = scene_.Get<UIPanel>(n.id)->blockInput;
            if (&e == n.primary) {
                if (const UIScroll* sc = scene_.Get<UIScroll>(n.id)) {
                    r.scroll = true;
                    r.scrollHorizontal = sc->direction == "horizontal";
                    r.scrollMax = scrollMax_.count(n.id) ? scrollMax_.at(n.id) : 0.0f;
                }
            }
            if (e.kind == UIRect::Kind::Slider) r.interactable = scene_.Get<UISlider>(n.id)->interactable;
            r.clipped = clip.on;
            r.clip[0] = clip.x0;
            r.clip[1] = clip.y0;
            r.clip[2] = clip.x1;
            r.clip[3] = clip.y1;
            rects_->push_back(r);
        }
        if (!quads_) return;
        std::vector<UIQuad>& out = *quads_;
        switch (e.kind) {
            case UIRect::Kind::Panel: {
                const UIPanel& p = *scene_.Get<UIPanel>(n.id);
                UIQuad q = RectQuad(b.x, b.y, b.x + b.w, b.y + b.h, p.color, alpha, n.id);
                Shape(q, b, p.radius * s_, p.borderWidth * s_, p.borderColor, alpha);
                AddQuad(out, std::move(q), clip);
                break;
            }
            case UIRect::Kind::Button: {
                const UIButton& bt = *scene_.Get<UIButton>(n.id);
                float k = !bt.interactable ? 1.0f : bt.pressed ? bt.pressedBrightness : bt.hovered ? bt.hoverBrightness : 1.0f;
                float a = bt.interactable ? alpha : alpha * 0.5f;
                AddBox(out, b, Brighten(bt.color, k), a, bt.radius * s_, bt.borderWidth * s_, bt.borderColor, n.id, clip);
                float pad = 8.0f * s_;
                Box inner{b.x + pad, b.y, std::max(1.0f, b.w - 2 * pad), b.h};
                FontRef font = ResolveFont(bt.font, assets_);
                ShapedText sh = oe::Shape(ParseRich(bt.text, true, bt.textColor, false), font, bt.size * s_, 0.0f, 1.0f, inner.w);
                TextDraw d;
                d.alpha = a;
                d.alignX = 0.5f;
                d.alignY = 0.5f;
                EmitText(out, sh, inner, d, n.id, clip);
                break;
            }
            case UIRect::Kind::Text: {
                const UIText& t = *scene_.Get<UIText>(n.id);
                ShapedText sh = ShapeTextOf(t, t.wrap && (t.width > 0.0f || ParseAnchor(t.anchor).maxX > ParseAnchor(t.anchor).minX) ? b.w : 0.0f);
                TextDraw d;
                d.alpha = alpha;
                d.alignX = t.align == "left" ? 0.0f : t.align == "center" ? 0.5f : t.align == "right" ? 1.0f : ParseAnchor(t.anchor).pivotX;
                d.alignY = t.verticalAlign == "middle" ? 0.5f : t.verticalAlign == "bottom" ? 1.0f : 0.0f;
                d.maxChars = t.visibleCharacters;
                d.outline = t.outlineWidth * s_;
                d.outlineColor = t.outlineColor;
                d.shadow = t.shadowDistance * s_;
                d.shadowColor = t.shadowColor;
                EmitText(out, sh, b, d, n.id, clip);
                break;
            }
            case UIRect::Kind::Image: EmitImage(n, b, alpha, clip); break;
            case UIRect::Kind::Slider: EmitSlider(n, b, alpha, clip); break;
        }
    }

    void EmitImage(const Node& n, Box b, float alpha, const Clip& clip) {
        const UIImage& im = *scene_.Get<UIImage>(n.id);
        std::vector<UIQuad>& out = *quads_;
        std::shared_ptr<const Texture> tex = TextureOf(im);
        float s0 = 0, t0 = 0, fw = 1, fh = 1;
        if (tex && tex->width > 0) ImageFrame(im, tex.get(), &s0, &t0, &fw, &fh);
        else tex.reset();
        if (im.preserveAspect && tex && b.w > 0 && b.h > 0) {
            float aspect = fw / fh;
            if (b.w / b.h > aspect) {
                float w = b.h * aspect;
                b.x += (b.w - w) * 0.5f;
                b.w = w;
            } else {
                float h = b.w / aspect;
                b.y += (b.h - h) * 0.5f;
                b.h = h;
            }
        }
        // fill < 1 shows the part next to fillOrigin.
        Clip c = clip;
        float fill = Clamp(im.fill, 0.0f, 1.0f);
        if (fill < 1.0f) {
            Box f = b;
            if (im.fillOrigin == "right") f.x = b.x + b.w * (1.0f - fill), f.w = b.w * fill;
            else if (im.fillOrigin == "top") f.h = b.h * fill;
            else if (im.fillOrigin == "bottom") f.y = b.y + b.h * (1.0f - fill), f.h = b.h * fill;
            else f.w = b.w * fill;
            c = clip.Intersect(f);
        }
        auto piece = [&](float x0, float y0, float x1, float y1, float ss0, float tt0, float ss1, float tt1) {
            UIQuad q = RectQuad(x0, y0, x1, y1, im.color, alpha, n.id);
            if (tex) {
                q.texture = tex;
                q.s0 = ss0, q.t0 = tt0, q.s1 = ss1, q.t1 = tt1;
                q.nearest = im.pixelArt;
            }
            Shape(q, b, im.radius * s_, 0.0f, im.color, alpha);
            AddQuad(out, std::move(q), c);
        };
        if (tex && im.slice > 0.0f) {
            float bs = std::min(im.slice, std::min(fw, fh) * 0.5f);
            float bp = std::min(bs * im.sliceScale * s_, std::min(b.w, b.h) * 0.5f);
            float xs[4] = {b.x, b.x + bp, b.x + b.w - bp, b.x + b.w};
            float ys[4] = {b.y, b.y + bp, b.y + b.h - bp, b.y + b.h};
            float ss[4] = {s0, s0 + bs, s0 + fw - bs, s0 + fw};
            float ts[4] = {t0, t0 + bs, t0 + fh - bs, t0 + fh};
            for (int j = 0; j < 3; ++j) {
                for (int i = 0; i < 3; ++i) piece(xs[i], ys[j], xs[i + 1], ys[j + 1], ss[i], ts[j], ss[i + 1], ts[j + 1]);
            }
        } else {
            piece(b.x, b.y, b.x + b.w, b.y + b.h, s0, t0, s0 + fw, t0 + fh);
        }
    }

    void EmitSlider(const Node& n, const Box& b, float alpha, const Clip& clip) {
        const UISlider& sl = *scene_.Get<UISlider>(n.id);
        std::vector<UIQuad>& out = *quads_;
        float t = sl.max > sl.min ? Clamp((sl.value - sl.min) / (sl.max - sl.min), 0.0f, 1.0f) : 0.0f;
        float a = alpha;
        float radius = sl.radius * s_;
        AddBox(out, b, sl.color, a, radius, 0.0f, sl.color, n.id, clip);
        Box f = b;
        float hx = 0, hy = 0;
        if (sl.direction == "right-to-left") f.x = b.x + b.w * (1 - t), f.w = b.w * t, hx = f.x, hy = b.y + b.h * 0.5f;
        else if (sl.direction == "bottom-to-top") f.y = b.y + b.h * (1 - t), f.h = b.h * t, hx = b.x + b.w * 0.5f, hy = f.y;
        else if (sl.direction == "top-to-bottom") f.h = b.h * t, hx = b.x + b.w * 0.5f, hy = b.y + f.h;
        else f.w = b.w * t, hx = b.x + f.w, hy = b.y + b.h * 0.5f;
        if (t > 0.0f) {
            UIQuad q = RectQuad(b.x, b.y, b.x + b.w, b.y + b.h, sl.fillColor, a, n.id);
            Shape(q, b, radius, 0.0f, sl.fillColor, a);
            AddQuad(out, std::move(q), clip.Intersect(f));
        }
        if (sl.handle && sl.interactable) {
            float d = std::min(b.w, b.h) * 1.5f;
            Box hb{hx - d * 0.5f, hy - d * 0.5f, d, d};
            float k = sl.pressed ? 0.85f : sl.hovered ? 1.0f : 0.95f;
            AddBox(out, hb, Brighten(sl.handleColor, k), a, d * 0.5f, 0.0f, sl.handleColor, n.id, clip);
        }
    }

    const Scene& scene_;
    AssetManager* assets_;
    Box screen_;
    Box parentBox_;
    float s_ = 1.0f;
    std::map<EntityId, Node> nodes_;
    std::map<EntityId, float> scrollMax_;  // per scroll view, reference pixels
    std::vector<UIRect>* rects_ = nullptr;
    std::vector<UIQuad>* quads_ = nullptr;
};

uint32_t Pack(const Color& c) {
    auto ch = [](float v) { return static_cast<uint32_t>(Clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return ch(c.r) | (ch(c.g) << 8) | (ch(c.b) << 16) | 0xFF000000u;
}

// Texel of a UI texture with clamp-to-edge, as the GPU sampler reads it.
void Texel(const Texture& t, int x, int y, float* rgba) {
    x = std::max(0, std::min(x, t.width - 1));
    y = std::max(0, std::min(y, t.height - 1));
    uint32_t c = t.texels[static_cast<size_t>(y) * static_cast<size_t>(t.width) + static_cast<size_t>(x)];
    rgba[0] = static_cast<float>(c & 0xFF) / 255.0f;
    rgba[1] = static_cast<float>((c >> 8) & 0xFF) / 255.0f;
    rgba[2] = static_cast<float>((c >> 16) & 0xFF) / 255.0f;
    rgba[3] = static_cast<float>((c >> 24) & 0xFF) / 255.0f;
}

}  // namespace

float UIScale(const Scene& scene, int width, int height) {
    const auto& canvases = scene.Pool<UICanvas>();
    if (canvases.empty()) return static_cast<float>(height) / kUIReferenceHeight;
    const UICanvas& c = canvases.begin()->second;
    float sw = static_cast<float>(width) / std::max(1.0f, c.referenceWidth);
    float sh = static_cast<float>(height) / std::max(1.0f, c.referenceHeight);
    float m = Clamp(c.match, 0.0f, 1.0f);
    return std::exp(std::log(std::max(sw, 1e-6f)) * (1.0f - m) + std::log(std::max(sh, 1e-6f)) * m);
}

std::vector<UIRect> LayoutUI(const Scene& scene, int width, int height, AssetManager* assets) {
    std::vector<UIRect> rects;
    Builder(scene, width, height, assets).Run(&rects, nullptr);
    return rects;
}

std::vector<UIQuad> BuildUIQuads(const Scene& scene, int width, int height, AssetManager* assets) {
    std::vector<UIQuad> quads;
    Builder(scene, width, height, assets).Run(nullptr, &quads);
    return quads;
}

void ShadeUIQuad(const UIQuad& q, float px, float py, Color* color, float* alpha) {
    Color c = q.color;
    float a = q.alpha;
    if (q.texture) {
        const Texture& t = *q.texture;
        float u = q.s0 + (px - static_cast<float>(q.x0)) / static_cast<float>(q.x1 - q.x0) * (q.s1 - q.s0);
        float v = q.t0 + (py - static_cast<float>(q.y0)) / static_cast<float>(q.y1 - q.y0) * (q.t1 - q.t0);
        float tx[4];
        if (q.nearest) {
            Texel(t, static_cast<int>(std::floor(u)), static_cast<int>(std::floor(v)), tx);
        } else {
            float fu = u - 0.5f, fv = v - 0.5f;
            int x0 = static_cast<int>(std::floor(fu)), y0 = static_cast<int>(std::floor(fv));
            float wx = fu - static_cast<float>(x0), wy = fv - static_cast<float>(y0);
            float c00[4], c10[4], c01[4], c11[4];
            Texel(t, x0, y0, c00);
            Texel(t, x0 + 1, y0, c10);
            Texel(t, x0, y0 + 1, c01);
            Texel(t, x0 + 1, y0 + 1, c11);
            for (int k = 0; k < 4; ++k) tx[k] = (c00[k] * (1 - wx) + c10[k] * wx) * (1 - wy) + (c01[k] * (1 - wx) + c11[k] * wx) * wy;
        }
        c = Color(c.r * tx[0], c.g * tx[1], c.b * tx[2]);
        a *= tx[3];
    }
    if (q.shaped) {
        // Signed distance to the rounded rectangle (negative inside), same as the GPU shader.
        float cx = (q.shape[0] + q.shape[2]) * 0.5f, cy = (q.shape[1] + q.shape[3]) * 0.5f;
        float hx = (q.shape[2] - q.shape[0]) * 0.5f, hy = (q.shape[3] - q.shape[1]) * 0.5f;
        float r = std::min(q.radius, std::min(hx, hy));
        float dx = std::fabs(px - cx) - hx + r, dy = std::fabs(py - cy) - hy + r;
        float ox = std::max(dx, 0.0f), oy = std::max(dy, 0.0f);
        float d = std::sqrt(ox * ox + oy * oy) + std::min(std::max(dx, dy), 0.0f) - r;
        float cover = Clamp(0.5f - d, 0.0f, 1.0f);
        if (q.border > 0.0f) {
            float inner = Clamp(0.5f - (d + q.border), 0.0f, 1.0f);
            c = q.borderColor * (1.0f - inner) + c * inner;
            a = q.borderAlpha * (1.0f - inner) + a * inner;
        }
        a *= cover;
    }
    *color = c;
    *alpha = a;
}

void DrawUI(const Scene& scene, RenderTarget& t, AssetManager* assets) {
    for (const UIQuad& q : BuildUIQuads(scene, t.width, t.height, assets)) {
        int x0 = std::max(0, q.x0), y0 = std::max(0, q.y0);
        int x1 = std::min(t.width, q.x1), y1 = std::min(t.height, q.y1);
        bool plain = !q.texture && !q.shaped;
        uint32_t solid = Pack(q.color);
        float idAlpha = 0.5f * std::max(q.alpha, 1e-6f);
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                size_t i = static_cast<size_t>(y) * static_cast<size_t>(t.width) + static_cast<size_t>(x);
                Color c = q.color;
                float a = q.alpha;
                if (!plain) ShadeUIQuad(q, static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, &c, &a);
                if (a <= 0.0f) continue;
                if (plain && a >= 0.999f) {
                    t.color[i] = solid;
                } else if (a >= 0.999f) {
                    t.color[i] = Pack(c);
                } else {
                    uint32_t d = t.color[i];
                    Color dc((d & 0xFF) / 255.0f, ((d >> 8) & 0xFF) / 255.0f, ((d >> 16) & 0xFF) / 255.0f);
                    t.color[i] = Pack(dc * (1.0f - a) + c * a);
                }
                if (plain || a >= idAlpha) t.ids[i] = q.entity;
            }
        }
    }
}

namespace {
template <class T>
bool Shown(const Scene& scene, EntityId id) {
    const T* c = scene.Get<T>(id);
    return !c || c->visible;
}
// Visible itself and through every ancestor with a UI element.
bool VisibleInTree(const Scene& scene, EntityId id) {
    for (int guard = 0; id != kNullEntity && guard < 1000; ++guard) {
        if (!Shown<UIPanel>(scene, id) || !Shown<UIButton>(scene, id) || !Shown<UIText>(scene, id) || !Shown<UIImage>(scene, id) ||
            !Shown<UISlider>(scene, id)) return false;
        const EntityRecord* rec = scene.Record(id);
        id = rec ? rec->parent : kNullEntity;
    }
    return true;
}
}  // namespace

UIThumb ScrollThumb(float view, float extent, float offset, float scale) {
    UIThumb t;
    t.thickness = 8.0f * scale;
    t.margin = 3.0f * scale;
    t.length = std::min(view - 2 * t.margin, std::max(28.0f * scale, view * view / std::max(extent, 1.0f)));
    t.travel = std::max(0.0f, view - t.length - 2 * t.margin);
    const float most = extent - view;
    t.at = t.margin + (most > 0.0f ? t.travel * Clamp(offset / most, 0.0f, 1.0f) : 0.0f);
    return t;
}

void UpdateUIMotion(Scene& scene, float dt) {
    for (auto& kv : scene.Pool<UIMotion>()) {
        UIMotion& m = kv.second;
        const bool visible = VisibleInTree(scene, kv.first);
        if (visible && !m.wasVisible) m.time = 0.0f;
        else if (visible && m.time < 1e8f) m.time += dt;
        m.wasVisible = visible;
    }
    for (auto& kv : scene.Pool<UIButton>()) {
        UIButton& b = kv.second;
        const float target = !b.interactable ? 1.0f : b.pressed ? b.pressedScale : b.hovered ? b.hoverScale : 1.0f;
        b.scaleNow += (target - b.scaleNow) * std::min(1.0f, dt * 18.0f);
        if (std::fabs(target - b.scaleNow) < 1e-3f) b.scaleNow = target;
    }
}

const UIRect* HitTestUI(const std::vector<UIRect>& rects, float px, float py) {
    const UIRect* hit = nullptr;
    for (const UIRect& r : rects) {
        if ((!r.interactable && !r.blocksInput) || px < r.x || px >= r.x + r.w || py < r.y || py >= r.y + r.h) continue;
        if (r.clipped && !(px >= r.clip[0] && px < r.clip[2] && py >= r.clip[1] && py < r.clip[3])) continue;
        if (r.blocksInput) {  // a modal backdrop: whatever was found under it is out of reach
            hit = nullptr;
            continue;
        }
        hit = &r;  // last = topmost
    }
    return hit;
}

EntityId HitTestButton(const Scene& scene, float px, float py, int width, int height, AssetManager* assets) {
    std::vector<UIRect> rects = LayoutUI(scene, width, height, assets);
    const UIRect* r = HitTestUI(rects, px, py);
    return r ? r->entity : kNullEntity;
}

float SliderValueAt(const UISlider& s, const UIRect& r, float px, float py) {
    float t;
    if (s.direction == "right-to-left") t = 1.0f - (px - r.x) / std::max(1.0f, r.w);
    else if (s.direction == "bottom-to-top") t = 1.0f - (py - r.y) / std::max(1.0f, r.h);
    else if (s.direction == "top-to-bottom") t = (py - r.y) / std::max(1.0f, r.h);
    else t = (px - r.x) / std::max(1.0f, r.w);
    t = Clamp(t, 0.0f, 1.0f);
    float v = s.min + t * (s.max - s.min);
    if (s.step > 0.0f) v = s.min + std::round((v - s.min) / s.step) * s.step;
    return Clamp(v, std::min(s.min, s.max), std::max(s.min, s.max));
}

void MeasureText(const std::string& text, float size, float* w, float* h, const std::string& font, AssetManager* assets) {
    ShapedText t = Shape(ParseRich(text, false, Color(), false), ResolveFont(font, assets), size, 0.0f, 1.0f, 0.0f);
    *w = t.width;
    *h = t.height;
}

}  // namespace oe
