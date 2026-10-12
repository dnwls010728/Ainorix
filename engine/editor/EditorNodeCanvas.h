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
    // Background, grid, wheel zoom and panning (middle button anywhere, left button on empty canvas).
    void Navigate();
    void Fit(ImVec2 low, ImVec2 high);
    ImVec2 Point(ImVec2 position) const;
    // Canvas position under a screen point (where a new node goes).
    ImVec2 Position(ImVec2 screen) const;
    // `emphasis`: 1 = normal, above 1 = thicker and brighter (wires of the selected node), below 1 = dimmed.
    void Wire(ImVec2 from, ImVec2 to, ImU32 color, float emphasis = 1.0f) const;
    // Leaves the card as the current ImGui item, for selection and double-click handling.
    // `badge` (optional) is drawn dimmed at the right of the header (a node number, a type).
    bool Card(ImVec2& position, ImVec2 dimensions, const char* title, ImU32 header, bool selected, bool headerOnly = false,
              const char* badge = nullptr);
    // Leaves the pin as the current ImGui item; callers own activation and drop validation.
    void Pin(const char* id, ImVec2 screen, bool connected);
    // One dimmed line at the bottom left of the canvas (how to use it) and the zoom at the bottom right.
    void Footer(const char* hint) const;
    // True when the mouse is over the canvas and not over a card or pin submitted so far this frame.
    bool HoveringEmpty() const;
    // Height of the card header and the font size cards are drawn with, in canvas units.
    static constexpr float kHeader = 30.0f;
    ImVec2 origin, size;
    ImDrawList* draw;
private:
    ImVec2& pan_;
    float& zoom_;
    float font_;  // the interface font size when the canvas began: card text is this times the zoom
};
}  // namespace oe
