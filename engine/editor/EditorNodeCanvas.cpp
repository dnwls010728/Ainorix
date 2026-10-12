#define IMGUI_DEFINE_MATH_OPERATORS
#include "editor/EditorNodeCanvas.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "imgui_internal.h"

namespace oe {
namespace {
const ImU32 kCanvasBack = IM_COL32(22, 25, 31, 255);
const ImU32 kGridMinor = IM_COL32(36, 40, 49, 255);
const ImU32 kGridMajor = IM_COL32(50, 56, 68, 255);
const ImU32 kCardBody = IM_COL32(33, 37, 46, 255);
const ImU32 kCardEdge = IM_COL32(74, 83, 99, 255);
const ImU32 kCardHover = IM_COL32(132, 146, 170, 255);
const ImU32 kAccent = IM_COL32(112, 202, 242, 255);

ImU32 Scaled(ImU32 color, float brightness, float alpha) {
    ImVec4 c = ImGui::ColorConvertU32ToFloat4(color);
    c.x = std::min(1.0f, c.x * brightness); c.y = std::min(1.0f, c.y * brightness); c.z = std::min(1.0f, c.z * brightness);
    c.w *= alpha;
    return ImGui::ColorConvertFloat4ToU32(c);
}
}  // namespace

EditorNodeCanvas::EditorNodeCanvas(const char* id, ImVec2& pan, float& zoom) : pan_(pan), zoom_(zoom), font_(ImGui::GetFontSize()) {
    const ImVec2 available = ImGui::GetContentRegionAvail();
    ImGui::BeginChild(id, ImVec2(std::max(160.0f, available.x), std::max(140.0f, available.y)), ImGuiChildFlags_Borders,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    origin = ImGui::GetCursorScreenPos(); size = ImGui::GetContentRegionAvail(); draw = ImGui::GetWindowDrawList();
}
EditorNodeCanvas::~EditorNodeCanvas() { ImGui::EndChild(); }
void EditorNodeCanvas::Fit(ImVec2 low, ImVec2 high) {
    zoom_ = std::clamp(std::min((size.x - 48) / std::max(1.0f, high.x - low.x),
        (size.y - 64) / std::max(1.0f, high.y - low.y)), 0.08f, 1.0f);
    // Centered, not pushed into the top left corner.
    pan_ = (size - (high - low) * zoom_) * 0.5f - low * zoom_;
}
void EditorNodeCanvas::Navigate() {
    ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsWindowHovered();
    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) pan_ += io.MouseDelta;
    // Dragging the empty canvas with the left button moves the view too (cards and pins take the button first).
    if (hovered && !ImGui::IsAnyItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f)) pan_ += io.MouseDelta;
    if (hovered && io.MouseWheel != 0) {
        const float before = zoom_; zoom_ = std::clamp(zoom_ * std::pow(1.15f, io.MouseWheel), 0.08f, 1.5f);
        pan_ = io.MousePos - origin - (io.MousePos - origin - pan_) * (zoom_ / before);
    }
    draw->AddRectFilled(origin, origin + size, kCanvasBack);
    // Minor lines fade out when zoomed far out, so the grid never turns into noise.
    const float grid = 32 * zoom_;
    if (grid >= 10) {
        for (float x = std::fmod(pan_.x, grid); x < size.x; x += grid) draw->AddLine(origin + ImVec2(x, 0), origin + ImVec2(x, size.y), kGridMinor);
        for (float y = std::fmod(pan_.y, grid); y < size.y; y += grid) draw->AddLine(origin + ImVec2(0, y), origin + ImVec2(size.x, y), kGridMinor);
    }
    const float major = grid * 4;
    for (float x = std::fmod(pan_.x, major); x < size.x; x += major) draw->AddLine(origin + ImVec2(x, 0), origin + ImVec2(x, size.y), kGridMajor);
    for (float y = std::fmod(pan_.y, major); y < size.y; y += major) draw->AddLine(origin + ImVec2(0, y), origin + ImVec2(size.x, y), kGridMajor);
}
ImVec2 EditorNodeCanvas::Point(ImVec2 position) const { return origin + pan_ + position * zoom_; }
ImVec2 EditorNodeCanvas::Position(ImVec2 screen) const { return (screen - origin - pan_) / zoom_; }
void EditorNodeCanvas::Wire(ImVec2 from, ImVec2 to, ImU32 color, float emphasis) const {
    const float bend = std::max(40.0f * zoom_, std::fabs(to.x - from.x) * 0.5f);
    const float width = std::max(1.5f, 2.6f * zoom_) * (emphasis > 1 ? 1.5f : 1.0f);
    // A dark line under the wire keeps it readable where it crosses cards and other wires.
    draw->AddBezierCubic(from, from + ImVec2(bend, 0), to - ImVec2(bend, 0), to, IM_COL32(12, 14, 18, emphasis < 1 ? 90 : 200), width + 2.5f);
    draw->AddBezierCubic(from, from + ImVec2(bend, 0), to - ImVec2(bend, 0), to,
                         emphasis > 1 ? Scaled(color, 1.25f, 1) : emphasis < 1 ? Scaled(color, 0.75f, 0.45f) : color, width);
}
bool EditorNodeCanvas::Card(ImVec2& position, ImVec2 dimensions, const char* title, ImU32 header, bool selected, bool headerOnly,
                            const char* badge) {
    const ImVec2 p = Point(position), end = p + dimensions * zoom_;
    const float round = 6 * zoom_, head = kHeader * zoom_;
    draw->AddRectFilled(p + ImVec2(0, 3) * zoom_, end + ImVec2(0, 5) * zoom_, IM_COL32(0, 0, 0, 70), round);  // shadow
    draw->AddRectFilled(p, end, kCardBody, round);
    draw->AddRectFilled(p, p + ImVec2(dimensions.x * zoom_, head), header, round, ImDrawFlags_RoundCornersTop);
    ImGui::SetCursorScreenPos(p + ImVec2(8, 2) * zoom_);
    ImGui::InvisibleButton("##node", ImVec2((dimensions.x - 16) * zoom_, (headerOnly ? kHeader - 4 : dimensions.y - 4) * zoom_));
    const bool hovered = ImGui::IsItemHovered();
    if (selected) draw->AddRect(p - ImVec2(2, 2), end + ImVec2(2, 2), Scaled(kAccent, 1, 0.35f), round + 2, 3.0f);
    draw->AddRect(p, end, selected ? kAccent : hovered ? kCardHover : kCardEdge, round, selected ? 2.0f : 1.2f);
    // Text below about 7 px is only noise: far out a card shows its color and shape.
    const float text = font_ * zoom_;
    if (text >= 7.0f) {
        ImFont* font = ImGui::GetFont();
        const float badgeWidth = badge && *badge ? font->CalcTextSizeA(text * 0.85f, FLT_MAX, 0, badge).x + 8 * zoom_ : 0;
        draw->PushClipRect(p, p + ImVec2(dimensions.x * zoom_ - badgeWidth - 6 * zoom_, head), true);
        draw->AddText(font, text, p + ImVec2(10 * zoom_, (head - text) * 0.5f), IM_COL32(244, 246, 250, 255), title);
        draw->PopClipRect();
        if (badgeWidth > 0)
            draw->AddText(font, text * 0.85f, ImVec2(end.x - badgeWidth, p.y + (head - text * 0.85f) * 0.5f), IM_COL32(255, 255, 255, 120), badge);
    }
    if (headerOnly && hovered) ImGui::SetTooltip("%s", title);
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        position += ImGui::GetIO().MouseDelta / zoom_; return true;
    }
    return false;
}
void EditorNodeCanvas::Pin(const char* id, ImVec2 screen, bool connected) {
    const float radius = std::max(3.0f, 5.5f * zoom_);
    ImGui::SetCursorScreenPos(screen - ImVec2(10, 10) * zoom_);
    ImGui::InvisibleButton(id, ImVec2(20, 20) * zoom_);
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    draw->AddCircleFilled(screen, radius + 1.5f, IM_COL32(14, 16, 21, 255));
    if (connected) draw->AddCircleFilled(screen, radius, kAccent);
    else { draw->AddCircleFilled(screen, radius, IM_COL32(46, 52, 64, 255)); draw->AddCircle(screen, radius, IM_COL32(140, 152, 173, 255), 0, 1.5f); }
    if (hovered) draw->AddCircle(screen, radius + 3.5f, IM_COL32(255, 255, 255, 190), 0, 1.5f);
}
void EditorNodeCanvas::Footer(const char* hint) const {
    const float pad = 8;
    ImFont* font = ImGui::GetFont();
    char zoom[16];
    std::snprintf(zoom, sizeof(zoom), "%d%%", static_cast<int>(zoom_ * 100 + 0.5f));
    const ImVec2 zoomSize = font->CalcTextSizeA(font_, FLT_MAX, 0, zoom);
    const float y = origin.y + size.y - font_ - pad;
    // A strip of its own: cards reach under it and their text must not run through the hint.
    draw->AddRectFilled(ImVec2(origin.x, y - pad * 0.5f), origin + size, IM_COL32(16, 19, 26, 235));
    draw->AddText(font, font_, ImVec2(origin.x + size.x - zoomSize.x - pad, y), IM_COL32(170, 180, 198, 200), zoom);
    if (!hint || !*hint) return;
    draw->PushClipRect(origin, ImVec2(origin.x + size.x - zoomSize.x - pad * 3, origin.y + size.y), true);
    draw->AddText(font, font_ * 0.92f, ImVec2(origin.x + pad, y), IM_COL32(150, 160, 178, 190), hint);
    draw->PopClipRect();
}
bool EditorNodeCanvas::HoveringEmpty() const { return ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered(); }
}  // namespace oe
