#pragma once
#include "imgui.h"

namespace oe {
// Shared screen-space interaction only; documents and wire semantics belong to each workspace.
class EditorNodeCanvas {
public:
    EditorNodeCanvas(const char* id, ImVec2& pan, float& zoom);
    ~EditorNodeCanvas();
    EditorNodeCanvas(const EditorNodeCanvas&) = delete;
    EditorNodeCanvas& operator=(const EditorNodeCanvas&) = delete;
    void Navigate();
    void Fit(ImVec2 low, ImVec2 high);
    ImVec2 Point(ImVec2 position) const;
    void Wire(ImVec2 from, ImVec2 to, ImU32 color) const;
    // Leaves the card as the current ImGui item, for selection and double-click handling.
    bool Card(ImVec2& position, ImVec2 dimensions, const char* title, ImU32 header, bool selected, bool headerOnly = false);
    // Leaves the pin as the current ImGui item; callers own activation and drop validation.
    void Pin(const char* id, ImVec2 screen, bool connected);
    ImVec2 origin, size;
    ImDrawList* draw;
private:
    ImVec2& pan_;
    float& zoom_;
};
}  // namespace oe
