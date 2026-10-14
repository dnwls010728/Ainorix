// Sprite Editor window: how a sprite sheet is cut into frames (Slice) and the collision outline of each
// frame (Collision). The sheet and the frame are drawn large, with zoom and pan, on a checkerboard so
// transparent pixels read as transparent. Every change is a command (atlas.create, component.set,
// sprite.collision, sprite.outline).
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "editor/EditorIcons.h"
#include "editor/EditorInternal.h"
#include "editor/EditorText.h"
#include "imgui_internal.h"
#include "sokol_imgui.h"

#include "app/Engine.h"
#include "assets/Assets.h"
#include "core/FileSystem.h"
#include "core/Log.h"
#include "render/GpuRenderer.h"
#include "render/RenderScene.h"
#include "scene/Components.h"
#include "scene/Scene.h"

namespace oe {

namespace {

// Pixel art previews sample the nearest texel (ImGui's default sampler is linear).
ImTextureID NearestTexture(sg_view view) {
    static sg_sampler sampler{};
    if (sg_query_sampler_state(sampler) != SG_RESOURCESTATE_VALID) {
        sg_sampler_desc desc{};
        desc.min_filter = desc.mag_filter = SG_FILTER_NEAREST;
        desc.wrap_u = desc.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
        desc.label = "oe-editor-nearest";
        sampler = sg_make_sampler(&desc);
    }
    return static_cast<ImTextureID>(simgui_imtextureid_with_sampler(view, sampler));
}

// Colors of the window: one accent for what is selected or about to be written, quiet lines elsewhere.
constexpr ImU32 kAccent = IM_COL32(92, 148, 255, 255);
constexpr ImU32 kPreview = IM_COL32(255, 176, 72, 255);
constexpr ImU32 kOutline = IM_COL32(110, 230, 150, 255);
constexpr ImU32 kFrameLine = IM_COL32(255, 255, 255, 120);

// Light/dark squares behind images: transparent pixels show as the pattern, not as the panel color.
void Checkerboard(ImDrawList* draw, ImVec2 a, ImVec2 b, float cell) {
    draw->AddRectFilled(a, b, IM_COL32(58, 60, 66, 255));
    cell = std::max(4.0f, cell);
    const ImVec4 clip = draw->_ClipRectStack.back();
    const float x0 = std::max(a.x, clip.x), y0 = std::max(a.y, clip.y), x1 = std::min(b.x, clip.z), y1 = std::min(b.y, clip.w);
    if (x1 <= x0 || y1 <= y0) return;
    const int i0 = static_cast<int>((x0 - a.x) / cell), j0 = static_cast<int>((y0 - a.y) / cell);
    for (int j = j0; a.y + static_cast<float>(j) * cell < y1; ++j)
        for (int i = i0 + ((i0 + j) & 1 ? 1 : 0); a.x + static_cast<float>(i) * cell < x1; i += 2) {
            const ImVec2 p(a.x + static_cast<float>(i) * cell, a.y + static_cast<float>(j) * cell);
            draw->AddRectFilled(ImVec2(std::max(p.x, a.x), std::max(p.y, a.y)), ImVec2(std::min(p.x + cell, b.x), std::min(p.y + cell, b.y)),
                                IM_COL32(78, 81, 88, 255));
        }
}

// A two-or-more-way choice drawn as joined buttons; returns the index clicked, else -1. Each button's
// bounds go to rects["<name>:<index>"] (automation).
int Segmented(const char* id, const std::vector<const char*>& labels, int current, std::map<std::string, std::array<float, 4>>* rects = nullptr,
              const char* name = nullptr) {
    int clicked = -1;
    ImGui::PushID(id);
    for (size_t i = 0; i < labels.size(); ++i) {
        if (i > 0) ImGui::SameLine(0, 1);
        const bool on = static_cast<int>(i) == current;
        ImGui::PushStyleColor(ImGuiCol_Button, on ? ImVec4(0.36f, 0.58f, 1.0f, 0.55f) : ImVec4(1, 1, 1, 0.06f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? ImVec4(0.36f, 0.58f, 1.0f, 0.65f) : ImVec4(1, 1, 1, 0.12f));
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Button(labels[i])) clicked = static_cast<int>(i);
        if (rects && name) {
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            (*rects)[std::string(name) + ":" + std::to_string(i)] = {a.x, a.y, b.x - a.x, b.y - a.y};
        }
        ImGui::PopID();
        ImGui::PopStyleColor(2);
    }
    ImGui::PopID();
    return clicked;
}

// A label column of fixed width followed by its control on the same line.
void Label(const char* text, float width) {
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.66f, 0.67f, 0.70f, 1.0f));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::SameLine(width);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

// A frame argument from its framePoints key: digits are a sheet index, anything else an atlas name.
Json FrameValue(const std::string& key) {
    if (!key.empty() && key.find_first_not_of("0123456789") == std::string::npos) return Json(std::atoi(key.c_str()));
    return Json(key);
}

}  // namespace

void NativeEditor::Impl::SpriteRect(const std::string& name) {
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    spriteRects[name] = {a.x, a.y, b.x - a.x, b.y - a.y};
}

std::vector<NativeEditor::Impl::SpriteSheetFrame> NativeEditor::Impl::SpriteSheetFrames(const Sprite& sp, std::string* sheet) {
    std::vector<SpriteSheetFrame> out;
    std::string image = sp.texture;
    if (!sp.atlas.empty()) {
        if (auto atlas = engine.Assets().GetAtlas(sp.atlas)) {
            if (image.empty()) image = (*atlas)["texture"].asString();
            const Json& frames = (*atlas)["frames"];
            for (size_t i = 0; i < frames.size(); ++i) {
                const Json& r = frames[i]["rect"];
                out.push_back({frames[i]["name"].asString(std::to_string(i)), r[0].asFloat(), r[1].asFloat(), r[2].asFloat(), r[3].asFloat()});
            }
        }
    } else if (!image.empty()) {
        if (auto texture = engine.Assets().GetTexture(image)) {
            const int cols = std::max(1, sp.columns), gridRows = std::max(1, sp.rows);
            const float w = static_cast<float>(texture->width) / static_cast<float>(cols), h = static_cast<float>(texture->height) / static_cast<float>(gridRows);
            for (int i = 0; i < cols * gridRows && i < 4096; ++i)
                out.push_back({std::to_string(i), static_cast<float>(i % cols) * w, static_cast<float>(i / cols) * h, w, h});
        }
    }
    if (sheet) *sheet = image;
    return out;
}

void NativeEditor::Impl::SpriteEditorPanel() {
    spriteRects.clear();
    if (focusSpriteEditor) ImGui::SetNextWindowFocus();
    focusSpriteEditor = false;
    if (!ImGui::Begin(WithIcon(kIconImage, TrId("Sprite Editor")).c_str(), &showSpriteEditor)) {
        ImGui::End();
        return;
    }
    const EntityId id = Primary();
    const Sprite* sp = engine.GetScene().Get<Sprite>(id);
    if (!sp) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", Tr("Select an entity with a Sprite to slice its sheet and edit the collision outline of each frame."));
        ImGui::PopStyleColor();
        ImGui::End();
        return;
    }
    if (spriteEditorEntity != id) {  // another sprite: start from its own frame, fitted
        spriteEditorEntity = id;
        spriteFit = true;
        spriteFrameKey = SpriteFrameKey(sp->frame);
        slicePreviewKey.clear();
        sliceEdited = false;
        collisionPoint = -1;
    }
    const int tab = Segmented("##tabs", {Tr("Slice Sheet"), Tr("Collision")}, spriteEditorTab, &spriteRects, "tab");
    if (tab >= 0 && tab != spriteEditorTab) {
        spriteEditorTab = tab;
        spriteFit = true;
        collisionPoint = -1;
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (const EntityRow* row = Row(id)) ImGui::TextDisabled("%s", row->name.c_str());
    if (spriteEditorTab == 0) SpriteSliceTab(*sp);
    else SpriteCollisionTab(*sp);
    ImGui::End();
}

// ----- Slice ---------------------------------------------------------------------------------

void NativeEditor::Impl::SpriteSliceTab(const Sprite& sp) {
    std::string sheet;
    const std::vector<SpriteSheetFrame> current = SpriteSheetFrames(sp, &sheet);
    std::shared_ptr<const Texture> texture = sheet.empty() ? nullptr : engine.Assets().GetTexture(sheet);
    const float font = ImGui::GetFontSize();
    const bool narrow = ImGui::GetContentRegionAvail().x < font * 40;
    const float side = narrow ? 0.0f : font * 17;
    const float label = font * 6.5f;

    // ----- Settings column
    ImGui::BeginChild("##sliceSettings", ImVec2(narrow ? 0.0f : side, narrow ? font * 16 : 0.0f), ImGuiChildFlags_None);
    if (!texture) {
        ImGui::TextWrapped("%s", Tr("The sprite has no image. Set Sprite.texture first."));
        ImGui::EndChild();
        return;
    }
    ImGui::TextUnformatted(sheet.c_str());
    ImGui::TextDisabled("%d x %d px", texture->width, texture->height);
    // What the sprite uses now, in words.
    std::string now;
    if (!sp.atlas.empty()) now = Format(Tr("Atlas %s: %d frames"), sp.atlas.c_str(), static_cast<int>(current.size()));
    else if (sp.columns * sp.rows > 1)
        now = Format(Tr("Grid %d x %d: %d frames of %d x %d px"), sp.columns, sp.rows, sp.columns * sp.rows, texture->width / std::max(1, sp.columns),
                     texture->height / std::max(1, sp.rows));
    else now = Tr("Whole image (one frame)");
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(ImVec4(0.78f, 0.82f, 0.90f, 1), "%s", now.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Separator();

    bool changed = false;
    Label(Tr("Method"), label);
    ImGui::NewLine();
    const int mode = Segmented("##sliceMode", {Tr("Cell Size"), Tr("Cell Count"), Tr("Find Sprites")}, sliceMode, &spriteRects, "slice-mode");
    if (mode >= 0 && mode != sliceMode) sliceMode = mode, changed = true;
    if (sliceMode == 0) {
        Label(Tr("Cell Pixels"), label);
        int size[2] = {sliceWidth, sliceHeight};
        if (ImGui::InputInt2("##cell", size)) sliceWidth = std::clamp(size[0], 1, 4096), sliceHeight = std::clamp(size[1], 1, 4096), changed = true;
        SpriteRect("slice-size");
    } else if (sliceMode == 1) {
        Label(Tr("Columns, Rows"), label);
        int count[2] = {sliceColumns, sliceRows};
        if (ImGui::InputInt2("##count", count)) sliceColumns = std::clamp(count[0], 1, 1024), sliceRows = std::clamp(count[1], 1, 1024), changed = true;
        SpriteRect("slice-count");
    } else {
        Label(Tr("Join Gap"), label);
        if (ImGui::InputInt("##gap", &sliceGap)) sliceGap = std::clamp(sliceGap, 0, 64), changed = true;
        HelpTooltip(Tr("Parts closer than this many pixels become one sprite (a sword beside its hand)."));
    }
    if (sliceMode < 2) {
        Label(Tr("Offset"), label);
        if (ImGui::InputInt2("##offset", sliceOffset)) sliceOffset[0] = std::max(0, sliceOffset[0]), sliceOffset[1] = std::max(0, sliceOffset[1]), changed = true;
        HelpTooltip(Tr("Pixels before the first cell (a margin around the sheet)."));
        Label(Tr("Spacing"), label);
        if (ImGui::InputInt2("##spacing", sliceSpacing)) sliceSpacing[0] = std::max(0, sliceSpacing[0]), sliceSpacing[1] = std::max(0, sliceSpacing[1]), changed = true;
        HelpTooltip(Tr("Pixels between cells (gutters)."));
        Label(Tr("Trim"), label);
        if (ImGui::Checkbox("##trim", &sliceTrim)) changed = true;
        HelpTooltip(Tr("Cut each cell to its opaque pixels; frames keep the cell as their size, so they do not jump."));
    }
    Label(Tr("Pivot"), label);
    const char* pivots[] = {"Bottom Center", "Center", "Top Left", "Bottom Left"};
    if (ImGui::BeginCombo("##pivot", Tr(pivots[slicePivot]))) {
        for (int i = 0; i < 4; ++i)
            if (ImGui::Selectable(Tr(pivots[i]), slicePivot == i)) slicePivot = i, changed = true;
        ImGui::EndCombo();
    }
    HelpTooltip(Tr("The point of each frame placed at the entity position (Bottom Center: the feet)."));
    Label(Tr("Save As"), label);
    ImGui::InputText("##slicePath", &slicePath);
    SpriteRect("atlas-path");
    const bool exists = FileExists(engine.ResolvePath(slicePath));
    if (exists) {
        ImGui::Checkbox(Tr("Replace the existing file"), &sliceOverwrite);
        SpriteRect("slice-overwrite");
    }
    if (changed) sliceEdited = true;

    // Preview: the frames these settings would make (atlas.create dryRun), redone when a setting changes.
    Json args = Json::MakeObject();
    args["path"] = slicePath;
    args["texture"] = sheet;
    const float pivotValues[4][2] = {{0.5f, 0.0f}, {0.5f, 0.5f}, {0.0f, 1.0f}, {0.0f, 0.0f}};
    args["pivot"] = Json::Array{pivotValues[slicePivot][0], pivotValues[slicePivot][1]};
    if (sliceMode == 2) {
        args["auto"] = true;
        args["gap"] = sliceGap;
    } else {
        const int cw = sliceMode == 0 ? sliceWidth : std::max(1, (texture->width - sliceOffset[0] - sliceSpacing[0] * (sliceColumns - 1)) / sliceColumns);
        const int ch = sliceMode == 0 ? sliceHeight : std::max(1, (texture->height - sliceOffset[1] - sliceSpacing[1] * (sliceRows - 1)) / sliceRows);
        args["cellWidth"] = cw;
        args["cellHeight"] = ch;
        args["trim"] = sliceTrim;
        if (sliceOffset[0] || sliceOffset[1]) args["offset"] = Json::Array{sliceOffset[0], sliceOffset[1]};
        if (sliceSpacing[0] || sliceSpacing[1]) args["spacing"] = Json::Array{sliceSpacing[0], sliceSpacing[1]};
        if (sliceMode == 1) ImGui::TextDisabled(Tr("Cells of %d x %d px"), cw, ch);
    }
    Json dry = args;
    dry["dryRun"] = true;
    const std::string key = dry.dump();
    if (key != slicePreviewKey) {
        slicePreviewKey = key;
        slicePreview = Call("atlas.create", dry, true);
    }
    ImGui::Spacing();
    const bool previewOk = Ok(slicePreview);
    if (previewOk) {
        const int count = static_cast<int>(slicePreview["result"]["values"]["frames"].size());
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kPreview), Tr("Preview: %d frames"), count);
    } else {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ImVec4(0.96f, 0.45f, 0.40f, 1), "%s", slicePreview["error"]["message"].asString().c_str());
        if (!slicePreview["error"]["hint"].asString().empty()) ImGui::TextDisabled("%s", slicePreview["error"]["hint"].asString().c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::BeginDisabled(!previewOk || (exists && !sliceOverwrite));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.36f, 0.58f, 1.0f, 0.45f));
    if (ImGui::Button(WithIcon(kIconCircleCheck, Tr("Apply Slice")).c_str(), ImVec2(-FLT_MIN, ImGui::GetFrameHeight() + 4))) {
        args["overwrite"] = exists && sliceOverwrite;
        // The sprite then points at its new atlas; a frame name that still exists stays shown.
        if (Ok(Call("atlas.create", args))) {
            Call("component.set", ObjectOf({{"id", Primary()}, {"type", "Sprite"}, {"values", ObjectOf({{"atlas", slicePath}, {"frame", 0}})}}));
            sliceEdited = false;
            slicePreviewKey.clear();
            sliceOverwrite = false;
            Notify(Format(Tr("Sliced %s into %s"), sheet.c_str(), slicePath.c_str()));
        }
    }
    ImGui::PopStyleColor();
    SpriteRect("slice");
    ImGui::EndDisabled();
    if (exists && !sliceOverwrite) ImGui::TextDisabled("%s", Tr("A file with this name exists: replace it or pick another name."));
    // Plain grids need no atlas file: the sprite's columns and rows say it.
    const bool plainGrid = sliceMode < 2 && !sliceTrim && !sliceOffset[0] && !sliceOffset[1] && !sliceSpacing[0] && !sliceSpacing[1];
    if (plainGrid && previewOk) {
        const int cw = args["cellWidth"].asInt(), ch = args["cellHeight"].asInt();
        if (texture->width % cw == 0 && texture->height % ch == 0) {
            if (ImGui::Button(Tr("Use as Grid (no atlas file)"), ImVec2(-FLT_MIN, 0))) {
                Call("component.set", ObjectOf({{"id", Primary()},
                                                {"type", "Sprite"},
                                                {"values", ObjectOf({{"texture", sheet}, {"atlas", ""}, {"columns", texture->width / cw},
                                                                     {"rows", texture->height / ch}, {"frame", 0}})}}));
                sliceEdited = false;
            }
            SpriteRect("slice-grid");
            HelpTooltip(Tr("Sets Sprite.columns and rows; frames are numbered left to right, top to bottom."));
        }
    }
    ImGui::EndChild();
    if (!narrow) ImGui::SameLine();

    // ----- Canvas: the sheet with the current frames, or the preview while settings are being changed.
    const bool showPreview = sliceEdited && previewOk;
    std::vector<SpriteSheetFrame> frames;
    if (showPreview) {
        const Json& list = slicePreview["result"]["values"]["frames"];
        for (size_t i = 0; i < list.size(); ++i) {
            const Json& r = list[i]["rect"];
            frames.push_back({list[i]["name"].asString(), r[0].asFloat(), r[1].asFloat(), r[2].asFloat(), r[3].asFloat()});
        }
    } else {
        frames = current;
    }
    ImGui::BeginChild("##sliceCanvasArea", ImVec2(0, 0));
    ImGui::AlignTextToFramePadding();
    if (showPreview) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kPreview), "%s", Tr("Preview (not applied yet)"));
    else ImGui::TextDisabled("%s", Tr("Current frames: click one to show it on the sprite"));
    if (showPreview) {
        ImGui::SameLine();
        if (ImGui::SmallButton(Tr("Show Current"))) sliceEdited = false;
    }
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImMax(ImVec2(80, 80), ImGui::GetContentRegionAvail());
    ImGui::SetNextItemAllowOverlap();  // the zoom badge's buttons sit on top
    ImGui::InvisibleButton("##sliceCanvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle | ImGuiButtonFlags_MouseButtonRight);
    SpriteRect("slice-canvas");
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, origin + size, IM_COL32(30, 31, 35, 255));
    const ImVec2 imageSize(static_cast<float>(texture->width), static_cast<float>(texture->height));
    if (spriteFit) {
        spriteZoom = std::max(0.05f, std::min((size.x - 24) / imageSize.x, (size.y - 24) / imageSize.y));
        spritePan = (size - imageSize * spriteZoom) * 0.5f;
    }
    SpriteCanvasInput(origin, hovered, active);
    const ImVec2 a = origin + spritePan, b = a + imageSize * spriteZoom;
    draw->PushClipRect(origin, origin + size, true);
    Checkerboard(draw, a, b, std::max(6.0f, spriteZoom * 4));
    if (engine.Gpu()) {
        const sg_view view = engine.Gpu()->ImageView(texture);
        draw->AddImage(sp.pixelArt ? NearestTexture(view) : ViewTexture(view), a, b);
    }
    draw->AddRect(a - ImVec2(1, 1), b + ImVec2(1, 1), IM_COL32(255, 255, 255, 40));
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    int hit = -1;
    const ImU32 line = showPreview ? kPreview : kFrameLine;
    const std::string shown = SpriteFrameKey(sp.frame);
    for (size_t i = 0; i < frames.size(); ++i) {
        const SpriteSheetFrame& f = frames[i];
        const ImVec2 p0 = a + ImVec2(f.x, f.y) * spriteZoom, p1 = a + ImVec2(f.x + f.w, f.y + f.h) * spriteZoom;
        const bool shownNow = !showPreview && (f.key == shown || (sp.atlas.empty() && std::to_string(i) == shown));
        if (hovered && mouse.x >= p0.x && mouse.y >= p0.y && mouse.x < p1.x && mouse.y < p1.y) hit = static_cast<int>(i);
        if (hit == static_cast<int>(i)) draw->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 26));
        draw->AddRect(p0, p1, shownNow ? kAccent : line, 0, shownNow ? 2.0f : 1.0f);
        // The frame number where it fits: the order SpriteAnimation clips refer to.
        if (p1.x - p0.x > font * 1.6f && p1.y - p0.y > font * 1.2f) {
            const std::string number = std::to_string(i);
            const ImVec2 tag = p0 + ImVec2(2, 1);
            draw->AddRectFilled(tag, tag + ImGui::CalcTextSize(number.c_str()) * 0.8f + ImVec2(4, 1), shownNow ? kAccent : IM_COL32(0, 0, 0, 150), 2);
            draw->AddText(ImGui::GetFont(), font * 0.8f, tag + ImVec2(2, 0), IM_COL32(255, 255, 255, 230), number.c_str());
        }
    }
    draw->PopClipRect();
    SpriteZoomBadge(origin, size, 1.0f);
    if (hit >= 0) {
        const SpriteSheetFrame& f = frames[static_cast<size_t>(hit)];
        ImGui::SetTooltip("#%d  %s\n%.0f, %.0f   %.0f x %.0f px", hit, f.key.c_str(), f.x, f.y, f.w, f.h);
        if (!showPreview && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            Call("component.set", ObjectOf({{"id", Primary()}, {"type", "Sprite"}, {"values", ObjectOf({{"frame", FrameValue(f.key)}})}}));
            spriteFrameKey = f.key;
        }
    }
    ImGui::EndChild();
}

// Wheel zooms at the pointer, middle or right drag pans, F (or double-click with the middle button) fits.
void NativeEditor::Impl::SpriteCanvasInput(ImVec2 origin, bool hovered, bool active) {
    ImGuiIO& io = ImGui::GetIO();
    if (hovered && io.MouseWheel != 0) {
        const ImVec2 at = io.MousePos - origin;
        const float before = spriteZoom;
        spriteZoom = std::clamp(spriteZoom * std::pow(1.2f, io.MouseWheel), 0.05f, 4096.0f);
        spritePan = at - (at - spritePan) * (spriteZoom / before);
        spriteFit = false;  // the view stays where the person put it
    }
    if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0) || ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0))) {
        spritePan += io.MouseDelta;
        spriteFit = false;
    }
    if (hovered && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F, false)) spriteFit = true;
}

void NativeEditor::Impl::SpriteZoomBadge(ImVec2 origin, ImVec2 size, float scale) {
    const ImVec2 keep = ImGui::GetCursorScreenPos();
    const float button = ImGui::GetFrameHeight();
    ImGui::SetCursorScreenPos(origin + ImVec2(size.x - button * 3 - 12, 6));
    const std::string zoom = Format("%d%%", static_cast<int>(std::round(spriteZoom * scale * 100)));
    ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetCursorScreenPos() - ImVec2(4, 2),
                                              ImGui::GetCursorScreenPos() + ImVec2(button * 3 + 8, button + 2), IM_COL32(20, 21, 24, 200), 4);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(zoom.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton(Tr("Fit"))) spriteFit = true;
    SpriteRect("fit");
    HelpTooltip(Tr("Fit the picture in the view (F). Wheel zooms, middle or right drag pans."));
    ImGui::SetCursorScreenPos(keep);
    ImGui::Dummy(ImVec2(0, 0));  // the cursor moved back: an item keeps the window's bounds valid
}

// ----- Collision -----------------------------------------------------------------------------

void NativeEditor::Impl::SpriteCollisionTab(const Sprite& sp) {
    const EntityId id = Primary();
    std::string sheet;
    const std::vector<SpriteSheetFrame> frames = SpriteSheetFrames(sp, &sheet);
    std::shared_ptr<const Texture> texture = sheet.empty() ? nullptr : engine.Assets().GetTexture(sheet);
    const float font = ImGui::GetFontSize();
    if (frames.empty() || !texture) {
        ImGui::TextWrapped("%s", Tr("The sprite has no image. Set Sprite.texture first."));
        return;
    }
    if (spriteFrameKey.empty()) spriteFrameKey = SpriteFrameKey(sp.frame);
    int chosen = 0;
    for (size_t i = 0; i < frames.size(); ++i)
        if (frames[i].key == spriteFrameKey) chosen = static_cast<int>(i);
    spriteFrameKey = frames[static_cast<size_t>(chosen)].key;

    const Collider2D* collider = engine.GetScene().Get<Collider2D>(id);
    const bool polygon = collider && collider->shape == "polygon";
    const Json framePoints = polygon && collider->framePoints.isObject() ? collider->framePoints : Json::MakeObject();

    // ----- Frame strip: every frame as a thumbnail; a dot marks frames with their own outline.
    const float thumb = font * 3.2f;
    ImGui::BeginChild("##collisionFrames", ImVec2(0, thumb + font * 1.9f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    const sg_view view = engine.Gpu() ? engine.Gpu()->ImageView(texture) : sg_view{};
    const ImTextureID image = engine.Gpu() ? (sp.pixelArt ? NearestTexture(view) : ViewTexture(view)) : ImTextureID();
    for (size_t i = 0; i < frames.size() && i < 1024; ++i) {
        const SpriteSheetFrame& f = frames[i];
        if (i > 0) ImGui::SameLine(0, 4);
        ImGui::PushID(static_cast<int>(i));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##frame", ImVec2(thumb, thumb + font))) {
            spriteFrameKey = f.key;
            collisionPoint = -1;
            Call("component.set", ObjectOf({{"id", id}, {"type", "Sprite"}, {"values", ObjectOf({{"frame", FrameValue(f.key)}})}}));
        }
        SpriteRect("frame:" + std::to_string(i));
        const bool on = static_cast<int>(i) == chosen, hover = ImGui::IsItemHovered();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        Checkerboard(draw, p, p + ImVec2(thumb, thumb), 5);
        if (engine.Gpu()) {
            const float k = thumb / std::max(f.w, f.h);
            const ImVec2 s(f.w * k, f.h * k), q = p + (ImVec2(thumb, thumb) - s) * 0.5f;
            const float tw = static_cast<float>(texture->width), th = static_cast<float>(texture->height);
            draw->AddImage(image, q, q + s, ImVec2(f.x / tw, f.y / th), ImVec2((f.x + f.w) / tw, (f.y + f.h) / th));
        }
        draw->AddRect(p, p + ImVec2(thumb, thumb), on ? kAccent : IM_COL32(255, 255, 255, hover ? 90 : 30), 3, on ? 2.0f : 1.0f);
        const bool own = framePoints.has(f.key);
        if (own) draw->AddCircleFilled(p + ImVec2(thumb - 6, 6), 4, kOutline);
        const std::string number = std::to_string(i);
        draw->AddText(p + ImVec2((thumb - ImGui::CalcTextSize(number.c_str()).x) * 0.5f, thumb + 1),
                      ImGui::GetColorU32(on ? ImGuiCol_Text : ImGuiCol_TextDisabled), number.c_str());
        if (hover) ImGui::SetTooltip("%s\n%s", f.key.c_str(), own ? Tr("Has its own outline") : Tr("Uses the default outline"));
        ImGui::PopID();
    }
    ImGui::EndChild();

    if (!polygon) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", collider ? Tr("The Collider2D is not a polygon: frame outlines need shape polygon.")
                                          : Tr("The sprite has no Collider2D yet. Make one from the image:"));
        if (ImGui::Button(WithIcon(kIconSquareDashed, Tr("Fit to Image")).c_str())) Call("sprite.outline", ObjectOf({{"id", id}, {"alphaCutoff", outlineCutoff}}));
        SpriteRect("collision-fit");
        ImGui::SameLine();
        if (ImGui::Button(Tr("Fit Every Frame")))
            Call("sprite.outline", ObjectOf({{"id", id}, {"alphaCutoff", outlineCutoff}, {"allFrames", true}}));
        SpriteRect("collision-fit-all");
        return;
    }

    // ----- What is being edited: this frame's own outline, or the default one every other frame uses.
    const std::string& key = spriteFrameKey;
    const bool own = framePoints.has(key);
    const int target = Segmented("##target", {Tr("This Frame"), Tr("Default (all other frames)")}, collisionEditDefault ? 1 : 0, &spriteRects,
                                 "collision-target");
    if (target >= 0) collisionEditDefault = target == 1, collisionPoint = -1;
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (collisionEditDefault) ImGui::TextDisabled("%s", Tr("Frames without their own outline use this one."));
    else if (own) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kOutline), Tr("Frame %s has its own outline"), key.c_str());
    else ImGui::TextDisabled(Tr("Frame %s uses the default outline; editing it gives the frame its own."), key.c_str());

    const Json& shownPoints = collisionEditDefault ? collider->points : own ? framePoints[key] : collider->points;
    Json points = shownPoints.isArray() ? shownPoints : Json::MakeArray();
    auto write = [&](const Json& value, const std::string& merge = std::string()) {
        Json args = ObjectOf({{"id", id}, {"points", value}});
        if (!collisionEditDefault) args["frame"] = FrameValue(key);
        if (!merge.empty()) args["merge"] = merge;
        return Call("sprite.collision", args);
    };
    auto fit = [&](bool all) {
        Json args = ObjectOf({{"id", id}, {"alphaCutoff", outlineCutoff}});
        if (all) args["allFrames"] = true;
        else if (!collisionEditDefault) args["frame"] = FrameValue(key);
        const Json r = Call("sprite.outline", args);
        if (Ok(r) && r["result"].has("skipped")) Notify(Format(Tr("%d empty frames were left without an outline"), static_cast<int>(r["result"]["skipped"].size())));
    };
    if (ImGui::Button(WithIcon(kIconSquareDashed, Tr("Fit to Image")).c_str())) fit(false);
    SpriteRect("collision-fit");
    HelpTooltip(Tr("A convex outline (8 corners at most) around the opaque pixels of the current frame."));
    ImGui::SameLine();
    if (ImGui::Button(Tr("Fit Every Frame"))) fit(true);
    SpriteRect("collision-fit-all");
    HelpTooltip(Tr("Give every frame its own outline fitted to its pixels."));
    ImGui::SameLine();
    if (ImGui::Button(Tr("Rectangle"))) {
        const float h = 0.5f;
        write(Json::Array{Json::Array{-h, -h}, Json::Array{h, -h}, Json::Array{h, h}, Json::Array{-h, h}});
    }
    SpriteRect("collision-create");
    ImGui::SameLine();
    ImGui::BeginDisabled(collisionEditDefault || !own);
    if (ImGui::Button(Tr("Use Default"))) {
        Call("sprite.collision", ObjectOf({{"id", id}, {"frame", FrameValue(key)}, {"clear", true}}));
        collisionPoint = -1;
    }
    SpriteRect("collision-clear");
    HelpTooltip(Tr("Remove this frame's own outline: it collides with the default one again."));
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(collisionPoint < 0 || static_cast<size_t>(collisionPoint) >= points.size() || points.size() <= 3);
    if (ImGui::Button(Tr("Remove Point"))) {
        points.items().erase(points.items().begin() + collisionPoint);
        write(points);
        collisionPoint = -1;
    }
    SpriteRect("collision-remove");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox(Tr("Snap to Pixels"), &collisionSnap);
    SpriteRect("collision-snap");
    ImGui::SameLine();
    ImGui::Checkbox(Tr("Onion Skin"), &collisionOnion);
    HelpTooltip(Tr("Show the previous frame's outline faintly, to line the outlines up."));

    // ----- Canvas: the frame drawn as the sprite draws it (its local space), the outline over it.
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImMax(ImVec2(80, 80), ImGui::GetContentRegionAvail());
    ImGui::SetNextItemAllowOverlap();  // the zoom badge's buttons sit on top
    ImGui::InvisibleButton("##collisionCanvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle | ImGuiButtonFlags_MouseButtonRight);
    SpriteRect("collision-canvas");
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, origin + size, IM_COL32(30, 31, 35, 255));
    // The frame on a one-entity scene at the origin: the same quad, pivot, trim and flips as in the game.
    Scene preview;
    const EntityId pid = preview.Create("frame");
    preview.Add<Transform>(pid);
    Sprite& copy = preview.Add<Sprite>(pid);
    copy = sp;
    copy.mask = kNullEntity;
    copy.visible = true;
    copy.frame = FrameValue(key);
    const RenderItem* picture = nullptr;
    const std::vector<RenderItem> items = GatherRenderItems(preview, &engine.Assets());
    for (const RenderItem& item : items)
        if (item.id == pid) picture = &item;
    // The view fits the frame's picture (not the outline, so dragging a point does not rescale it).
    Vec3 lo(-0.5f, -0.5f, 0), hi(0.5f, 0.5f, 0);
    if (picture && !picture->mesh->positions.empty()) {
        lo = Vec3(FLT_MAX, FLT_MAX, 0), hi = Vec3(-FLT_MAX, -FLT_MAX, 0);
        for (const Vec3& p : picture->mesh->positions) {
            const Vec3 v = picture->world.TransformPoint(p);
            lo = Vec3(std::min(lo.x, v.x), std::min(lo.y, v.y), 0), hi = Vec3(std::max(hi.x, v.x), std::max(hi.y, v.y), 0);
        }
    }
    // spriteZoom here is screen pixels per local unit; spritePan the screen offset of the local origin.
    if (spriteFit) {
        spriteZoom = std::min(size.x / (std::max(0.01f, hi.x - lo.x) * 1.5f), size.y / (std::max(0.01f, hi.y - lo.y) * 1.5f));
        spritePan = ImVec2(size.x * 0.5f - (lo.x + hi.x) * 0.5f * spriteZoom, size.y * 0.5f + (lo.y + hi.y) * 0.5f * spriteZoom);
    }
    SpriteCanvasInput(origin, hovered, active);
    const ImVec2 center = origin + spritePan;
    const float zoom = spriteZoom;
    auto screen = [&](float x, float y) { return ImVec2(center.x + x * zoom, center.y - y * zoom); };
    const float pixel = 1.0f / std::max(0.001f, sp.pixelsPerUnit);
    auto local = [&](const ImVec2& pos) {
        float x = (pos.x - center.x) / zoom, y = (center.y - pos.y) / zoom;
        if (collisionSnap) {
            x = lo.x + std::round((x - lo.x) / pixel) * pixel;  // on the frame's pixel edges
            y = lo.y + std::round((y - lo.y) / pixel) * pixel;
        }
        return Json::Array{std::round(static_cast<double>(x) * 1000) / 1000, std::round(static_cast<double>(y) * 1000) / 1000};
    };
    draw->PushClipRect(origin, origin + size, true);
    if (picture && picture->mesh->positions.size() >= 4) {
        // Checkerboard under the frame's quad only: where the sprite's own pixels are.
        ImVec2 screenLo(FLT_MAX, FLT_MAX), screenHi(-FLT_MAX, -FLT_MAX);
        for (const Vec3& p : picture->mesh->positions) {
            const Vec3 v = picture->world.TransformPoint(p);
            screenLo = ImMin(screenLo, screen(v.x, v.y));
            screenHi = ImMax(screenHi, screen(v.x, v.y));
        }
        Checkerboard(draw, screenLo, screenHi, std::max(6.0f, zoom * pixel * 2));
        if (picture->textureOverride && engine.Gpu()) {
            const ImTextureID frameImage = sp.pixelArt ? NearestTexture(engine.Gpu()->ImageView(picture->textureOverride))
                                                       : ViewTexture(engine.Gpu()->ImageView(picture->textureOverride));
            const auto& mesh = *picture->mesh;
            for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
                ImVec2 p[3], uv[3];
                for (int k = 0; k < 3; ++k) {
                    const uint32_t index = mesh.indices[i + static_cast<size_t>(k)];
                    const Vec3 v = picture->world.TransformPoint(mesh.positions[index]);
                    p[k] = screen(v.x, v.y);
                    uv[k] = ImVec2(mesh.uvs[index * 2] * picture->uvScale[0] + picture->uvOffset[0], mesh.uvs[index * 2 + 1] * picture->uvScale[1] + picture->uvOffset[1]);
                }
                draw->AddImageQuad(frameImage, p[0], p[1], p[2], p[2], uv[0], uv[1], uv[2], uv[2]);
            }
        }
        // Pixel grid once a pixel is big enough to aim at.
        if (zoom * pixel >= 10) {
            for (float x = lo.x; x <= hi.x + pixel * 0.5f; x += pixel)
                draw->AddLine(ImVec2(screen(x, 0).x, screenLo.y), ImVec2(screen(x, 0).x, screenHi.y), IM_COL32(255, 255, 255, 22));
            for (float y = lo.y; y <= hi.y + pixel * 0.5f; y += pixel)
                draw->AddLine(ImVec2(screenLo.x, screen(0, y).y), ImVec2(screenHi.x, screen(0, y).y), IM_COL32(255, 255, 255, 22));
        }
    }
    // The entity origin (the pivot) as a small cross.
    draw->AddLine(center - ImVec2(8, 0), center + ImVec2(8, 0), IM_COL32(255, 90, 90, 200), 1.5f);
    draw->AddLine(center - ImVec2(0, 8), center + ImVec2(0, 8), IM_COL32(120, 220, 120, 200), 1.5f);
    auto outline = [&](const Json& list, ImU32 color, float thickness, bool fill) {
        std::vector<ImVec2> path;
        for (const Json& p : list.items())
            if (p.isArray() && p.size() == 2) path.push_back(screen(p[0].asFloat(), p[1].asFloat()));
        if (path.size() < 2) return;
        if (fill && path.size() >= 3) draw->AddConvexPolyFilled(path.data(), static_cast<int>(path.size()), (color & 0x00FFFFFF) | 0x28000000);
        draw->AddPolyline(path.data(), static_cast<int>(path.size()), color, thickness, ImDrawFlags_Closed);
    };
    if (collisionOnion && chosen > 0 && !collisionEditDefault) {
        const std::string previous = frames[static_cast<size_t>(chosen - 1)].key;
        outline(framePoints.has(previous) ? framePoints[previous] : collider->points, IM_COL32(200, 160, 255, 110), 1.0f, false);
    }
    // A frame that still uses the default: its outline is drawn dashed-looking (thin, gray) until edited.
    const bool inherited = !collisionEditDefault && !own;
    outline(points, inherited ? IM_COL32(190, 190, 190, 200) : kOutline, inherited ? 1.5f : 2.0f, !inherited);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    int hit = -1;
    for (size_t i = 0; i < points.size(); ++i) {
        const Json& p = points[i];
        if (!p.isArray() || p.size() != 2) continue;
        const ImVec2 at = screen(p[0].asFloat(), p[1].asFloat());
        const bool picked = collisionPoint == static_cast<int>(i);
        const bool near = hovered && (mouse.x - at.x) * (mouse.x - at.x) + (mouse.y - at.y) * (mouse.y - at.y) < 100;
        if (near) hit = static_cast<int>(i);
        draw->AddCircleFilled(at, picked ? 6.0f : near ? 5.5f : 4.5f, picked ? kAccent : IM_COL32(245, 245, 245, 255));
        draw->AddCircle(at, picked ? 6.0f : 4.5f, IM_COL32(0, 0, 0, 160), 0, 1.0f);
        spriteRects["collision-point:" + std::to_string(i)] = {at.x - 7, at.y - 7, 14, 14};
    }
    draw->PopClipRect();
    SpriteZoomBadge(origin, size, pixel);  // 100% = one screen pixel per sprite pixel
    {
        const std::string tip = Tr("Drag points. Double-click an edge adds one, right-click removes one. Scene undo restores edits.");
        const ImVec2 at = origin + ImVec2(8, size.y - font - 8);
        draw->AddText(at, IM_COL32(255, 255, 255, 110), tip.c_str());
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        collisionPoint = hit;
        ++collisionSerial;
    }
    if (hovered && hit >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && points.size() > 3) {
        points.items().erase(points.items().begin() + hit);
        write(points);
        collisionPoint = -1;
    }
    if (hovered && hit < 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && points.size() >= 3 && points.size() < 64) {
        // The new corner goes into the edge nearest to the click.
        const Json at = local(mouse);
        size_t edge = points.size() - 1;
        float best = INFINITY;
        for (size_t i = 0; i < points.size(); ++i) {
            const ImVec2 pa = screen(points[i][0].asFloat(), points[i][1].asFloat());
            const ImVec2 pb = screen(points[(i + 1) % points.size()][0].asFloat(), points[(i + 1) % points.size()][1].asFloat());
            const ImVec2 ab = pb - pa;
            const float t = std::clamp(((mouse.x - pa.x) * ab.x + (mouse.y - pa.y) * ab.y) / std::max(1e-6f, ab.x * ab.x + ab.y * ab.y), 0.0f, 1.0f);
            const ImVec2 q = pa + ab * t;
            const float d = (mouse.x - q.x) * (mouse.x - q.x) + (mouse.y - q.y) * (mouse.y - q.y);
            if (d < best) best = d, edge = i;
        }
        points.items().insert(points.items().begin() + static_cast<std::ptrdiff_t>(edge + 1), at);
        write(points);
        collisionPoint = static_cast<int>(edge + 1);
    }
    if (collisionPoint >= 0 && static_cast<size_t>(collisionPoint) < points.size() && active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const Json at = local(mouse);
        if (points[collisionPoint] != at) {
            points[collisionPoint] = at;
            write(points, "polygon:" + key + ":" + std::to_string(collisionSerial));
        }
    }
    if (hovered && collisionPoint >= 0 && points.size() > 3 && !ImGui::GetIO().WantTextInput &&
        (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) {
        points.items().erase(points.items().begin() + collisionPoint);
        write(points);
        collisionPoint = -1;
    }
}

}  // namespace oe
