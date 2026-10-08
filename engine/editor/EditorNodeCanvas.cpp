#define IMGUI_DEFINE_MATH_OPERATORS
#include "editor/EditorNodeCanvas.h"
#include <algorithm>
#include <cmath>
#include "imgui_internal.h"

namespace oe {
EditorNodeCanvas::EditorNodeCanvas(const char* id, ImVec2& pan, float& zoom) : pan_(pan), zoom_(zoom) {
    const ImVec2 available = ImGui::GetContentRegionAvail();
    ImGui::BeginChild(id, ImVec2(std::max(160.0f, available.x), std::max(140.0f, available.y)), ImGuiChildFlags_Borders,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    origin = ImGui::GetCursorScreenPos(); size = ImGui::GetContentRegionAvail(); draw = ImGui::GetWindowDrawList();
}
EditorNodeCanvas::~EditorNodeCanvas() { ImGui::EndChild(); }
void EditorNodeCanvas::Fit(ImVec2 low, ImVec2 high) {
    zoom_ = std::clamp(std::min((size.x - 40) / std::max(1.0f, high.x - low.x),
        (size.y - 40) / std::max(1.0f, high.y - low.y)), 0.08f, 1.0f);
    pan_ = ImVec2(20, 20) - low * zoom_;
}
void EditorNodeCanvas::Navigate() {
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsWindowHovered() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) pan_ += io.MouseDelta;
    if (ImGui::IsWindowHovered() && io.MouseWheel != 0) {
        const float before = zoom_; zoom_ = std::clamp(zoom_ * std::pow(1.15f, io.MouseWheel), 0.08f, 1.5f);
        pan_ = io.MousePos - origin - (io.MousePos - origin - pan_) * (zoom_ / before);
    }
    const float grid = 32 * zoom_;
    for (float x = std::fmod(pan_.x, grid); x < size.x; x += grid)
        draw->AddLine(origin + ImVec2(x, 0), origin + ImVec2(x, size.y), IM_COL32(48, 53, 63, 255));
    for (float y = std::fmod(pan_.y, grid); y < size.y; y += grid)
        draw->AddLine(origin + ImVec2(0, y), origin + ImVec2(size.x, y), IM_COL32(48, 53, 63, 255));
}
ImVec2 EditorNodeCanvas::Point(ImVec2 position) const { return origin + pan_ + position * zoom_; }
void EditorNodeCanvas::Wire(ImVec2 from, ImVec2 to, ImU32 color) const {
    const float bend = std::max(45.0f, std::fabs(to.x - from.x) * 0.5f);
    draw->AddBezierCubic(from, from + ImVec2(bend, 0), to - ImVec2(bend, 0), to, color, 2);
}
bool EditorNodeCanvas::Card(ImVec2& position, ImVec2 dimensions, const char* title, ImU32 header, bool selected, bool headerOnly) {
    const ImVec2 p = Point(position), end = p + dimensions * zoom_;
    draw->AddRectFilled(p, end, IM_COL32(30, 34, 42, 255), 5);
    draw->AddRectFilled(p, p + ImVec2(dimensions.x, 36) * zoom_, header, 5);
    draw->AddRect(p, end, selected ? IM_COL32(112, 202, 242, 255) : IM_COL32(98, 109, 127, 255), 5, 1.5f);
    if (headerOnly) draw->PushClipRect(p, p + ImVec2(dimensions.x - 10, 36) * zoom_, true);
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * zoom_, p + ImVec2(10, 9) * zoom_, IM_COL32(240, 243, 248, 255), title);
    if (headerOnly) draw->PopClipRect();
    ImGui::SetCursorScreenPos(p + ImVec2(8, 2) * zoom_);
    ImGui::InvisibleButton("##node", ImVec2((dimensions.x - 16) * zoom_, (headerOnly ? 32 : dimensions.y - 4) * zoom_));
    if (headerOnly && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", title);
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        position += ImGui::GetIO().MouseDelta / zoom_; return true;
    }
    return false;
}
void EditorNodeCanvas::Pin(const char* id, ImVec2 screen, bool connected) {
    draw->AddCircleFilled(screen, 5 * zoom_, connected ? IM_COL32(112, 202, 242, 255) : IM_COL32(110, 120, 139, 255));
    ImGui::SetCursorScreenPos(screen - ImVec2(8, 8) * zoom_);
    ImGui::InvisibleButton(id, ImVec2(16, 16) * zoom_);
}
}  // namespace oe
