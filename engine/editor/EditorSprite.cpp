// Sprite Editor window: one sprite asset (*.sprite.json), the image every Sprite naming it shares. Slice cuts
// the image into frames (live preview), Frames sets the asset's pixels per unit and filtering and each frame's
// name and pivot, Collision draws each frame's collision outline (Collider2D shape "sprite" uses it). Every
// change is a sprite.* command that writes the file; the window keeps its own undo of the file (Ctrl+Z / Ctrl+Y
// while it is focused). It follows the selection: picking a sprite that uses an asset shows that asset.
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
#include "scene/Components.h"
#include "scene/Scene.h"
#include "scene/SpriteAsset.h"

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

// Colors of the window: one accent for what is selected, orange for what is not applied yet, green for
// collision, quiet lines elsewhere.
constexpr ImU32 kAccent = IM_COL32(92, 148, 255, 255);
constexpr ImU32 kPreview = IM_COL32(255, 176, 72, 255);
constexpr ImU32 kOutline = IM_COL32(110, 230, 150, 255);
constexpr ImU32 kFrameLine = IM_COL32(255, 255, 255, 120);
constexpr ImU32 kPivot = IM_COL32(255, 92, 92, 255);
const ImVec4 kMuted(0.66f, 0.67f, 0.70f, 1.0f);
const ImVec4 kError(0.96f, 0.45f, 0.40f, 1.0f);

}  // namespace

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
int Segmented(const char* id, const std::vector<const char*>& labels, int current, std::map<std::string, std::array<float, 4>>* rects,
              const char* name, float width) {
    int clicked = -1;
    ImGui::PushID(id);
    const float each = width > 0 ? (width - static_cast<float>(labels.size() - 1)) / static_cast<float>(labels.size()) : 0.0f;
    for (size_t i = 0; i < labels.size(); ++i) {
        if (i > 0) ImGui::SameLine(0, 1);
        const bool on = static_cast<int>(i) == current;
        ImGui::PushStyleColor(ImGuiCol_Button, on ? ImVec4(0.36f, 0.58f, 1.0f, 0.55f) : ImVec4(1, 1, 1, 0.06f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? ImVec4(0.36f, 0.58f, 1.0f, 0.65f) : ImVec4(1, 1, 1, 0.12f));
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Button(labels[i], ImVec2(each, 0))) clicked = static_cast<int>(i);
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

namespace {

// A label column of fixed width followed by its control on the same line.
void Label(const char* text, float width) {
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::SameLine(width);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

// A small caption above a group of controls.
void Section(const char* text) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.80f, 0.83f, 0.90f, 1));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(at, at + ImVec2(ImGui::GetContentRegionAvail().x, 0), IM_COL32(255, 255, 255, 28));
    ImGui::Dummy(ImVec2(0, 2));
}

// Text that only commits when the field loses focus (or Enter); the buffer follows the value otherwise.
bool CommitText(const char* label, const std::string& current, std::string& result) {
    static std::map<ImGuiID, std::string> buffers;
    const ImGuiID id = ImGui::GetID(label);
    std::string& buf = buffers[id];
    if (ImGui::GetActiveID() != id) buf = current;
    ImGui::InputText(label, &buf);
    if (ImGui::IsItemDeactivatedAfterEdit() && buf != current) {
        result = buf;
        return true;
    }
    return false;
}

// An asset dragged from the Assets panel onto the last item.
bool DroppedAsset(std::string& path) {
    bool got = false;
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload)) {
            path.assign(static_cast<const char*>(payload->Data), static_cast<size_t>(payload->DataSize));
            got = true;
        }
        ImGui::EndDragDropTarget();
    }
    return got;
}

// Nine pivot spots of a frame as a 3 x 3 grid of buttons; returns the one clicked as [x, y] (0..1, Y up).
bool PivotGrid(const char* id, float x, float y, Json& picked, std::map<std::string, std::array<float, 4>>* rects, const std::string& name) {
    bool clicked = false;
    const float cell = ImGui::GetFrameHeight();
    ImGui::PushID(id);
    ImGui::BeginGroup();  // the three rows start where the first one does (beside a label)
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            if (col > 0) ImGui::SameLine(0, 2);
            const float px = static_cast<float>(col) * 0.5f, py = 1.0f - static_cast<float>(row) * 0.5f;
            const bool on = std::fabs(px - x) < 1e-3f && std::fabs(py - y) < 1e-3f;
            ImGui::PushID(row * 3 + col);
            const ImVec2 at = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##spot", ImVec2(cell, cell))) {
                picked = Json::Array{px, py};
                clicked = true;
            }
            const bool hover = ImGui::IsItemHovered();
            if (rects) (*rects)[name + ":" + std::to_string(row * 3 + col)] = {at.x, at.y, cell, cell};
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(at, at + ImVec2(cell, cell), on ? IM_COL32(92, 148, 255, 120) : IM_COL32(255, 255, 255, hover ? 40 : 14), 3);
            draw->AddCircleFilled(at + ImVec2(cell * (0.2f + 0.6f * px), cell * (0.2f + 0.6f * (1 - py))), on ? 3.5f : 2.5f,
                                  on ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 120));
            ImGui::PopID();
        }
    }
    ImGui::EndGroup();
    ImGui::PopID();
    return clicked;
}

// Pixel rectangle [x, y, w, h] of a frame in the image and where it sits in the frame's canvas.
struct FrameGeometry {
    float x = 0, y = 0, w = 1, h = 1;      // in the image
    float trimX = 0, trimY = 0;            // in the canvas
    float canvasW = 1, canvasH = 1;        // the frame's original size
    float pivotX = 0.5f, pivotY = 0.5f;    // 0..1, Y up
};
FrameGeometry Geometry(const Json& frame) {
    FrameGeometry g;
    g.x = frame["rect"][0].asFloat(), g.y = frame["rect"][1].asFloat();
    g.w = std::max(1.0f, frame["rect"][2].asFloat(1)), g.h = std::max(1.0f, frame["rect"][3].asFloat(1));
    g.trimX = frame["trim"][0].asFloat(), g.trimY = frame["trim"][1].asFloat();
    g.canvasW = std::max(1.0f, frame["size"][0].asFloat(g.w)), g.canvasH = std::max(1.0f, frame["size"][1].asFloat(g.h));
    g.pivotX = frame["pivot"][0].asFloat(0.5f), g.pivotY = frame["pivot"][1].asFloat(0.5f);
    return g;
}

double Round(double v, double step) { return std::round(v / step) * step; }

// Element i of an array as an int (Json's index operator does not check bounds).
int IntAt(const Json& array, size_t i, int fallback = 0) { return array.isArray() && i < array.size() ? array[i].asInt(fallback) : fallback; }

// A frame's picture fitted into a square at p.
void FrameThumb(ImDrawList* draw, ImTextureID image, const Texture& texture, const Json& frame, ImVec2 p, float side) {
    Checkerboard(draw, p, p + ImVec2(side, side), 4);
    const FrameGeometry g = Geometry(frame);
    const float k = (side - 2) / std::max(g.w, g.h);
    const ImVec2 s(g.w * k, g.h * k), q = p + (ImVec2(side, side) - s) * 0.5f;
    const float tw = static_cast<float>(texture.width), th = static_cast<float>(texture.height);
    draw->AddImage(image, q, q + s, ImVec2(g.x / tw, g.y / th), ImVec2((g.x + g.w) / tw, (g.y + g.h) / th));
}

}  // namespace

void NativeEditor::Impl::SpriteRect(const std::string& name) {
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    spriteRects[name] = {a.x, a.y, b.x - a.x, b.y - a.y};
}

// ----- Asset state -----------------------------------------------------------------------------

void NativeEditor::Impl::SpriteOpen(const std::string& path) {
    if (path != spritePath) {
        spritePath = path;
        spriteUndo.clear();
        spriteRedo.clear();
        spriteFrame = 0;
        spriteFit = true;
        collisionPoint = -1;
        sliceEdited = false;
        slicePreviewKey.clear();
    }
    if (spritePath.empty() || !SpriteLoad()) return;
    // The settings the asset was cut with fill the Slice tab.
    const Json& s = spriteRaw["slicing"];
    const std::string mode = s["mode"].asString("cellCount");
    sliceMode = mode == "cellSize" ? 0 : mode == "auto" ? 2 : 1;
    sliceWidth = s["cellWidth"].asInt(spriteImageSize[0]), sliceHeight = s["cellHeight"].asInt(spriteImageSize[1]);
    sliceColumns = s["columns"].asInt(1), sliceRows = s["rows"].asInt(1), sliceGap = s["gap"].asInt(2);
    sliceOffset[0] = IntAt(s["offset"], 0), sliceOffset[1] = IntAt(s["offset"], 1);
    sliceSpacing[0] = IntAt(s["spacing"], 0), sliceSpacing[1] = IntAt(s["spacing"], 1);
    sliceTrim = s["trim"].asBool();
}

bool NativeEditor::Impl::SpriteLoad() {
    spriteError.clear();
    try {
        spriteLoadedTime = FileModifiedTime(engine.ResolvePath(spritePath));
    } catch (const std::exception&) {
        spriteLoadedTime = 0;
    }
    const Json r = Call("sprite.get", ObjectOf({{"path", spritePath}}), true);
    if (!Ok(r)) {
        spriteRaw = Json();
        spriteResolved = Json();
        spriteError = r["error"]["message"].asString();
        return false;
    }
    spriteRaw = r["result"]["values"];
    spriteResolved = r["result"]["resolved"];
    spriteImageSize[0] = IntAt(r["result"]["image"], 0), spriteImageSize[1] = IntAt(r["result"]["image"], 1);
    const int frames = static_cast<int>(spriteResolved["frames"].size());
    spriteFrame = std::clamp(spriteFrame, 0, std::max(0, frames - 1));
    return true;
}

bool NativeEditor::Impl::SpriteEdit(const std::string& command, Json args, int merge) {
    const Json before = spriteRaw;
    args["path"] = spritePath;
    if (!Ok(Call(command, args))) return false;
    if (merge < 0 || merge != spriteMergeSerial) {
        spriteUndo.push_back(before);
        if (spriteUndo.size() > 100) spriteUndo.erase(spriteUndo.begin());
        spriteRedo.clear();
    }
    spriteMergeSerial = merge;
    SpriteLoad();
    return true;
}

void NativeEditor::Impl::SpriteHistory(bool redo) {
    std::vector<Json>& from = redo ? spriteRedo : spriteUndo;
    std::vector<Json>& to = redo ? spriteUndo : spriteRedo;
    if (from.empty() || spritePath.empty()) return;
    const Json values = from.back();
    if (!Ok(Call("sprite.create", ObjectOf({{"path", spritePath}, {"values", values}, {"overwrite", true}})))) return;
    to.push_back(spriteRaw);
    from.pop_back();
    spriteMergeSerial = -1;
    collisionPoint = -1;
    SpriteLoad();
    slicePreviewKey.clear();
}

int NativeEditor::Impl::SpriteFrameIndex(const Json& frame) const {
    const Json& frames = spriteResolved["frames"];
    if (frame.isNumber()) {
        const int i = frame.asInt();
        return i >= 0 && static_cast<size_t>(i) < frames.size() ? i : -1;
    }
    const std::string name = frame.asString();
    for (size_t i = 0; i < frames.size(); ++i)
        if (frames[i]["name"].asString() == name) return static_cast<int>(i);
    if (!name.empty() && name.find_first_not_of("0123456789") == std::string::npos) return SpriteFrameIndex(Json(std::atoi(name.c_str())));
    return -1;
}

// ----- Window ----------------------------------------------------------------------------------

void NativeEditor::Impl::SpriteEditorPanel() {
    spriteRects.clear();
    if (focusSpriteEditor) ImGui::SetNextWindowFocus();
    focusSpriteEditor = false;
    if (!ImGui::Begin(WithIcon(kIconImage, TrId("Sprite Editor")).c_str(), &showSpriteEditor)) {
        spriteEditorFocused = false;
        ImGui::End();
        return;
    }
    spriteEditorFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    // Follow the selection: a sprite drawing from an asset shows that asset at the frame it shows.
    const EntityId selected = Primary();
    if (selected != spriteFollowed) {
        spriteFollowed = selected;
        const SpriteAssets lookup = engine.Assets().SpriteLookup();
        const std::string path = SpriteAssetOf(engine.GetScene(), selected, &lookup);
        if (IsSpriteAssetPath(path)) {  // an image named directly has no file to edit: the start page offers one
            SpriteOpen(path);
            if (const Sprite* sp = engine.GetScene().Get<Sprite>(selected)) {
                const int index = SpriteFrameIndex(sp->frame.ToJson());
                if (index >= 0) spriteFrame = index;
            }
        } else if (!path.empty()) {
            SpriteOpen("");
        }
    }
    // Someone else (an agent, a text editor) changed the file: read it again.
    if (!spritePath.empty()) {
        int64_t time = 0;
        try {
            time = FileModifiedTime(engine.ResolvePath(spritePath));
        } catch (const std::exception&) {
        }
        if (time != spriteLoadedTime) SpriteLoad();
    }

    // ----- Toolbar: which asset, the three tabs, undo / redo.
    const float font = ImGui::GetFontSize();
    {
        const std::string name = spritePath.empty() ? std::string(Tr("No sprite asset")) : spritePath.substr(spritePath.find_last_of('/') + 1);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1, 1, 1, 0.06f));
        if (ImGui::Button((WithIcon(kIconImage, name) + "  " + kIconChevronDown).c_str())) ImGui::OpenPopup("##spriteAssets");
        ImGui::PopStyleColor();
        SpriteRect("asset");
        HelpTooltip(spritePath.empty() ? std::string(Tr("Pick a sprite asset or make one from an image.")) : spritePath);
        if (ImGui::BeginPopup("##spriteAssets")) {
            std::vector<std::string> assets;
            for (const std::string& file : ListFiles(engine.ProjectDir(), ".json", true)) {
                const std::string relative = RelativePath(file, engine.ProjectDir());
                if (IsSpriteAssetPath(relative) && relative.rfind(".oe/", 0) != 0) assets.push_back(relative);
            }
            std::sort(assets.begin(), assets.end());
            if (assets.empty()) ImGui::TextDisabled("%s", Tr("No sprite assets in the project yet."));
            for (const std::string& path : assets)
                if (ImGui::Selectable(path.c_str(), path == spritePath)) SpriteOpen(path);
            ImGui::Separator();
            if (ImGui::Selectable(WithIcon(kIconPlus, Tr("New Sprite Asset from an Image...")).c_str())) SpriteOpen("");
            ImGui::EndPopup();
        }
        if (!spritePath.empty() && spriteRaw.isObject()) {
            ImGui::SameLine(0, font);
            const int tab = Segmented("##tabs", {Tr("Slice"), Tr("Frames"), Tr("Collision")}, spriteEditorTab, &spriteRects, "tab");
            if (tab >= 0 && tab != spriteEditorTab) {
                spriteEditorTab = tab;
                spriteFit = true;
                collisionPoint = -1;
            }
            const float button = ImGui::GetFrameHeight();
            ImGui::SameLine(std::max(ImGui::GetCursorPosX() + font, ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - button * 2 - 4));
            ImGui::BeginDisabled(spriteUndo.empty());
            if (ImGui::Button(kIconUndo, ImVec2(button, 0))) SpriteHistory(false);
            ImGui::EndDisabled();
            SpriteRect("undo");
            HelpTooltip(Tr("Undo the last change to this sprite asset (Ctrl+Z)."));
            ImGui::SameLine(0, 2);
            ImGui::BeginDisabled(spriteRedo.empty());
            if (ImGui::Button(kIconRedo, ImVec2(button, 0))) SpriteHistory(true);
            ImGui::EndDisabled();
            SpriteRect("redo");
            HelpTooltip(Tr("Redo (Ctrl+Y)."));
        }
    }
    if (spriteEditorFocused && !ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) SpriteHistory(false);
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z)) SpriteHistory(true);
    }
    ImGui::Separator();

    if (spritePath.empty() || !spriteRaw.isObject()) {
        if (!spritePath.empty()) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(kError, "%s", Format(Tr("Cannot read %s: %s"), spritePath.c_str(), spriteError.c_str()).c_str());
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
        }
        SpriteStartPage();
        ImGui::End();
        return;
    }

    // Frames and Collision step through frames with the arrow keys.
    const int frames = static_cast<int>(spriteResolved["frames"].size());
    if (spriteEditorFocused && spriteEditorTab > 0 && !ImGui::GetIO().WantTextInput && frames > 0) {
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) spriteFrame = (spriteFrame + 1) % frames, collisionPoint = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) spriteFrame = (spriteFrame + frames - 1) % frames, collisionPoint = -1;
    }
    const bool narrow = ImGui::GetContentRegionAvail().x < font * 42;
    const float side = narrow ? 0.0f : font * 18;
    if (spriteEditorTab == 0) SpriteSliceTab(side);
    else if (spriteEditorTab == 1) SpriteFramesTab(side);
    else SpriteCollisionTab(side);
    ImGui::End();
}

// No asset shown: turn the selected sprite's own settings into one, or make one from an image.
void NativeEditor::Impl::SpriteStartPage() {
    const float font = ImGui::GetFontSize();
    const float label = font * 6.5f;
    ImGui::BeginChild("##spriteStart", ImVec2(0, 0));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(ImGui::GetContentRegionAvail().x, font * 36));
    ImGui::TextDisabled("%s", Tr("A sprite asset keeps what every copy of a sprite shares: how its image is cut into frames, pixels per unit, each frame's pivot and collision outline. Sprites name it in Sprite.asset."));
    ImGui::PopTextWrapPos();

    const EntityId id = Primary();
    const Sprite* sp = engine.GetScene().Get<Sprite>(id);
    const SpriteAnimation* animation = engine.GetScene().Get<SpriteAnimation>(id);
    // An image named directly ("hero.png?columns=6"): its settings live on the entity.
    SpriteSource direct;
    const bool legacy = sp && ParseSpriteSource(sp->asset, direct) && !direct.path.empty() && !(animation && !animation->asset.empty());
    auto stem = [](const std::string& path) {
        std::string name = path.substr(path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/') + 1);
        return name.substr(0, name.find('.'));
    };
    if (legacy) {
        Section(Format(Tr("Selected: %s"), Row(id) ? Row(id)->name.c_str() : "").c_str());
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(ImGui::GetContentRegionAvail().x, font * 36));
        ImGui::TextUnformatted(Tr("This sprite keeps its image settings on the entity. Move them into a sprite asset so every copy shares them and you can edit its frames and collision here."));
        ImGui::PopTextWrapPos();
        if (spriteNewPath.empty() || !IsSpriteAssetPath(spriteNewPath))
            spriteNewPath = "sprites/" + stem(direct.path) + ".sprite.json";
        Label(Tr("Save As"), label);
        ImGui::InputText("##convertPath", &spriteNewPath);
        SpriteRect("convert-path");
        ImGui::Checkbox(Tr("Also the other sprites drawing this image"), &spriteConvertAll);
        SpriteRect("convert-all");
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.36f, 0.58f, 1.0f, 0.45f));
        if (ImGui::Button(WithIcon(kIconCircleCheck, Tr("Make Sprite Asset")).c_str(), ImVec2(std::min(ImGui::GetContentRegionAvail().x, font * 20), 0))) {
            const Json r = Call("sprite.convert", ObjectOf({{"id", id}, {"path", spriteNewPath}, {"all", spriteConvertAll}}));
            if (Ok(r)) {
                Notify(Format(Tr("Made %s (%d sprites use it)"), spriteNewPath.c_str(), static_cast<int>(r["result"]["entities"].size())));
                SpriteOpen(spriteNewPath);
                spriteEditorTab = 1;
            }
        }
        ImGui::PopStyleColor();
        SpriteRect("convert");
    }

    Section(Tr("New sprite asset from an image"));
    if (spriteNewTexture.empty() && legacy && AssetManager::KindOf(direct.path) == "texture") spriteNewTexture = direct.path;
    Label(Tr("Image"), label);
    ImGui::InputTextWithHint("##newTexture", Tr("drag an image from Assets"), &spriteNewTexture);
    std::string dropped;
    if (DroppedAsset(dropped)) spriteNewTexture = dropped;
    SpriteRect("new-texture");
    const std::string suggested = "sprites/" + stem(spriteNewTexture) + ".sprite.json";
    static std::string newPath;
    static std::string newPathFor;
    if (newPathFor != spriteNewTexture) newPathFor = spriteNewTexture, newPath = suggested;
    Label(Tr("Save As"), label);
    ImGui::InputText("##newPath", &newPath);
    SpriteRect("new-path");
    const bool exists = !newPath.empty() && FileExists(engine.ResolvePath(newPath));
    const bool useOnSelection = sp && sp->asset.empty() && !legacy;
    ImGui::BeginDisabled(spriteNewTexture.empty() || exists);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.36f, 0.58f, 1.0f, 0.45f));
    if (ImGui::Button(WithIcon(kIconPlus, useOnSelection ? Tr("Create and Use on the Selection") : Tr("Create Sprite Asset")).c_str(),
                      ImVec2(std::min(ImGui::GetContentRegionAvail().x, font * 20), 0))) {
        if (Ok(Call("sprite.create", ObjectOf({{"path", newPath}, {"texture", spriteNewTexture}})))) {
            if (useOnSelection) Call("component.set", ObjectOf({{"id", id}, {"type", "Sprite"}, {"values", ObjectOf({{"asset", newPath}, {"frame", 0}})}}));
            SpriteOpen(newPath);
            spriteEditorTab = 0;  // next: cut it into frames
            spriteFollowed = Primary();
        }
    }
    ImGui::PopStyleColor();
    SpriteRect("create");
    ImGui::EndDisabled();
    if (exists) ImGui::TextDisabled("%s", Tr("A file with this name exists: pick it from the list above or choose another name."));
    ImGui::EndChild();
}

// Wheel zooms at the pointer, middle or right drag pans, F fits.
void NativeEditor::Impl::SpriteCanvasInput(ImVec2 origin, bool hovered, bool active) {
    ImGuiIO& io = ImGui::GetIO();
    if (hovered && io.MouseWheel != 0) {
        const ImVec2 at = io.MousePos - origin;
        const float before = spriteZoom;
        spriteZoom = std::clamp(spriteZoom * std::pow(1.2f, io.MouseWheel), 0.05f, 512.0f);
        spritePan = at - (at - spritePan) * (spriteZoom / before);
        spriteFit = false;  // the view stays where the person put it
    }
    if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0) || ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0))) {
        spritePan += io.MouseDelta;
        spriteFit = false;
    }
    if (hovered && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F, false)) spriteFit = true;
}

void NativeEditor::Impl::SpriteZoomBadge(ImVec2 origin, ImVec2 size) {
    const ImVec2 keep = ImGui::GetCursorScreenPos();
    const float button = ImGui::GetFrameHeight();
    ImGui::SetCursorScreenPos(origin + ImVec2(size.x - button * 3 - 12, 6));
    const std::string zoom = Format("%d%%", static_cast<int>(std::round(spriteZoom * 100)));
    ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetCursorScreenPos() - ImVec2(4, 2), ImGui::GetCursorScreenPos() + ImVec2(button * 3 + 8, button + 2),
                                              IM_COL32(20, 21, 24, 200), 4);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(zoom.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton(Tr("Fit"))) spriteFit = true;
    SpriteRect("fit");
    HelpTooltip(Tr("Fit the picture in the view (F). Wheel zooms, middle or right drag pans."));
    ImGui::SetCursorScreenPos(keep);
    ImGui::Dummy(ImVec2(0, 0));  // the cursor moved back: an item keeps the window's bounds valid
}

// Every frame as a thumbnail; a green dot marks frames with their own collision outline, a blue one their own pivot.
void NativeEditor::Impl::SpriteFrameStrip(float height) {
    const Json& frames = spriteResolved["frames"];
    const float font = ImGui::GetFontSize();
    const float thumb = std::max(font * 2.0f, height - font * 1.9f);
    ImGui::BeginChild("##spriteFrames", ImVec2(0, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    std::shared_ptr<const Texture> texture = engine.Assets().GetTexture(spriteResolved["texture"].asString());
    ImTextureID image{};
    if (texture && engine.Gpu()) {
        const sg_view view = engine.Gpu()->ImageView(texture);
        image = spriteResolved["pixelArt"].asBool(true) ? NearestTexture(view) : ViewTexture(view);
    }
    for (size_t i = 0; i < frames.size() && i < 4096; ++i) {
        const Json& f = frames[i];
        const FrameGeometry g = Geometry(f);
        if (i > 0) ImGui::SameLine(0, 4);
        ImGui::PushID(static_cast<int>(i));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##frame", ImVec2(thumb, thumb + font))) {
            spriteFrame = static_cast<int>(i);
            collisionPoint = -1;
        }
        SpriteRect("frame:" + std::to_string(i));
        const bool on = static_cast<int>(i) == spriteFrame, hover = ImGui::IsItemHovered();
        if (on && ImGui::IsWindowAppearing()) ImGui::SetScrollHereX();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        Checkerboard(draw, p, p + ImVec2(thumb, thumb), 5);
        if (texture && engine.Gpu()) {
            const float k = (thumb - 4) / std::max(g.w, g.h);
            const ImVec2 s(g.w * k, g.h * k), q = p + (ImVec2(thumb, thumb) - s) * 0.5f;
            const float tw = static_cast<float>(texture->width), th = static_cast<float>(texture->height);
            draw->AddImage(image, q, q + s, ImVec2(g.x / tw, g.y / th), ImVec2((g.x + g.w) / tw, (g.y + g.h) / th));
        }
        draw->AddRect(p, p + ImVec2(thumb, thumb), on ? kAccent : IM_COL32(255, 255, 255, hover ? 90 : 30), 3, on ? 2.0f : 1.0f);
        const bool collision = f.has("collision");
        const bool ownPivot = spriteRaw["frames"][i].has("pivot");
        if (collision) draw->AddCircleFilled(p + ImVec2(thumb - 6, 6), 3.5f, kOutline);
        if (ownPivot) draw->AddCircleFilled(p + ImVec2(thumb - (collision ? 15 : 6), 6), 3.5f, kAccent);
        const std::string number = std::to_string(i);
        draw->AddText(p + ImVec2((thumb - ImGui::CalcTextSize(number.c_str()).x) * 0.5f, thumb + 1), ImGui::GetColorU32(on ? ImGuiCol_Text : ImGuiCol_TextDisabled),
                      number.c_str());
        if (hover)
            ImGui::SetTooltip("#%d  %s\n%.0f x %.0f px%s%s", static_cast<int>(i), f["name"].asString().c_str(), g.canvasW, g.canvasH,
                              collision ? Format("\n%s", Tr("Has its own collision outline")).c_str() : "",
                              ownPivot ? Format("\n%s", Tr("Has its own pivot")).c_str() : "");
        ImGui::PopID();
    }
    ImGui::EndChild();
}

// ----- Slice ---------------------------------------------------------------------------------

void NativeEditor::Impl::SpriteSliceTab(float side) {
    const std::string sheet = spriteResolved["texture"].asString();
    std::shared_ptr<const Texture> texture = engine.Assets().GetTexture(sheet);
    const float font = ImGui::GetFontSize();
    const float label = font * 6.5f;
    const bool narrow = side <= 0;
    if (!texture) {
        ImGui::TextColored(kError, "%s", Format(Tr("Cannot read the image %s."), sheet.c_str()).c_str());
        return;
    }

    // Canvas first (left), settings on the right.
    Json slicing = Json::MakeObject();
    slicing["mode"] = sliceMode == 0 ? "cellSize" : sliceMode == 1 ? "cellCount" : "auto";
    if (sliceMode == 0) slicing["cellWidth"] = sliceWidth, slicing["cellHeight"] = sliceHeight;
    if (sliceMode == 1) slicing["columns"] = sliceColumns, slicing["rows"] = sliceRows;
    if (sliceMode == 2) slicing["gap"] = sliceGap;
    if (sliceMode < 2) {
        if (sliceOffset[0] || sliceOffset[1]) slicing["offset"] = Json::Array{sliceOffset[0], sliceOffset[1]};
        if (sliceSpacing[0] || sliceSpacing[1]) slicing["spacing"] = Json::Array{sliceSpacing[0], sliceSpacing[1]};
        if (sliceTrim) slicing["trim"] = true;
    }
    // Preview: the frames these settings would make (sprite.slice dryRun), redone when a setting changes.
    const std::string key = slicing.dump() + spritePath;
    if (key != slicePreviewKey) {
        slicePreviewKey = key;
        slicePreview = Call("sprite.slice", ObjectOf({{"path", spritePath}, {"slicing", slicing}, {"dryRun", true}}), true);
    }
    const bool previewOk = Ok(slicePreview);
    const bool showPreview = sliceEdited && previewOk;
    const Json& frames = showPreview ? slicePreview["result"]["resolved"]["frames"] : spriteResolved["frames"];

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("##sliceCanvasArea", ImVec2(narrow ? 0.0f : avail.x - side - 8, narrow ? std::max(font * 14, avail.y - font * 22) : 0.0f));
    ImGui::AlignTextToFramePadding();
    if (showPreview) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kPreview), "%s", Tr("Preview: not applied yet"));
    else ImGui::TextDisabled("%s", Tr("Frames of the sprite asset; click one to select it"));
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
        draw->AddImage(spriteResolved["pixelArt"].asBool(true) ? NearestTexture(view) : ViewTexture(view), a, b);
    }
    draw->AddRect(a - ImVec2(1, 1), b + ImVec2(1, 1), IM_COL32(255, 255, 255, 40));
    // Offset / spacing: the margin and gutters are shaded so they read as "not part of any frame".
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    int hit = -1;
    for (size_t i = 0; i < frames.size(); ++i) {
        const FrameGeometry g = Geometry(frames[i]);
        const ImVec2 p0 = a + ImVec2(g.x, g.y) * spriteZoom, p1 = a + ImVec2(g.x + g.w, g.y + g.h) * spriteZoom;
        const bool selected = !showPreview && static_cast<int>(i) == spriteFrame;
        if (hovered && mouse.x >= p0.x && mouse.y >= p0.y && mouse.x < p1.x && mouse.y < p1.y) hit = static_cast<int>(i);
        if (hit == static_cast<int>(i)) draw->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 26));
        draw->AddRect(p0, p1, selected ? kAccent : showPreview ? kPreview : kFrameLine, 0, selected ? 2.0f : 1.0f);
        // The frame number where it fits: the order SpriteAnimation clips refer to.
        if (p1.x - p0.x > font * 1.6f && p1.y - p0.y > font * 1.2f) {
            const std::string number = std::to_string(i);
            const ImVec2 tag = p0 + ImVec2(2, 1);
            draw->AddRectFilled(tag, tag + ImGui::CalcTextSize(number.c_str()) * 0.8f + ImVec2(4, 1), selected ? kAccent : IM_COL32(0, 0, 0, 150), 2);
            draw->AddText(ImGui::GetFont(), font * 0.8f, tag + ImVec2(2, 0), IM_COL32(255, 255, 255, 230), number.c_str());
        }
    }
    draw->PopClipRect();
    SpriteZoomBadge(origin, size);
    if (hit >= 0) {
        const FrameGeometry g = Geometry(frames[hit]);
        ImGui::SetTooltip("#%d  %s\n%.0f, %.0f   %.0f x %.0f px", hit, frames[hit]["name"].asString().c_str(), g.x, g.y, g.w, g.h);
        if (!showPreview && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) spriteFrame = hit;
        if (!showPreview && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) spriteEditorTab = 1, spriteFit = true;
    }
    ImGui::EndChild();
    if (!narrow) ImGui::SameLine();

    // ----- Settings
    ImGui::BeginChild("##sliceSettings", ImVec2(0, 0));
    ImGui::TextUnformatted(sheet.c_str());
    ImGui::TextDisabled("%d x %d px  -  %d %s", texture->width, texture->height, static_cast<int>(spriteResolved["frames"].size()), Tr("frames"));
    bool changed = false;
    Section(Tr("Cut into frames by"));
    const int mode = Segmented("##sliceMode", {Tr("Cell Size"), Tr("Cell Count"), Tr("Auto")}, sliceMode, &spriteRects, "slice-mode",
                               ImGui::GetContentRegionAvail().x);
    if (mode >= 0 && mode != sliceMode) sliceMode = mode, changed = true;
    if (sliceMode == 0) {
        Label(Tr("Cell Pixels"), label);
        int cell[2] = {sliceWidth, sliceHeight};
        if (ImGui::InputInt2("##cell", cell)) sliceWidth = std::clamp(cell[0], 1, 4096), sliceHeight = std::clamp(cell[1], 1, 4096), changed = true;
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
        HelpTooltip(Tr("Cut each cell to its opaque pixels and skip empty cells; frames keep the cell as their size, so they do not jump."));
    }
    if (changed) sliceEdited = true;
    ImGui::Spacing();
    if (previewOk) {
        const Json& result = slicePreview["result"];
        if (sliceMode == 1 && result["resolved"]["frames"].size() > 0) {
            const FrameGeometry g = Geometry(result["resolved"]["frames"][0]);
            ImGui::TextDisabled(Tr("Cells of %.0f x %.0f px"), g.canvasW, g.canvasH);
        }
        ImGui::TextColored(sliceEdited ? ImGui::ColorConvertU32ToFloat4(kPreview) : kMuted, Tr("%d frames"), static_cast<int>(result["frames"].asInt()));
        if (sliceEdited && result["kept"].size() > 0) ImGui::TextDisabled(Tr("%d keep their pivot or collision"), static_cast<int>(result["kept"].size()));
    } else {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(kError, "%s", slicePreview["error"]["message"].asString().c_str());
        if (!slicePreview["error"]["hint"].asString().empty()) ImGui::TextDisabled("%s", slicePreview["error"]["hint"].asString().c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::BeginDisabled(!previewOk || !sliceEdited);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.36f, 0.58f, 1.0f, 0.45f));
    if (ImGui::Button(WithIcon(kIconCircleCheck, Tr("Apply Slice")).c_str(), ImVec2(-FLT_MIN, ImGui::GetFrameHeight() + 4))) {
        if (SpriteEdit("sprite.slice", ObjectOf({{"slicing", slicing}}))) {
            sliceEdited = false;
            Notify(Format(Tr("%s: %d frames"), spritePath.c_str(), static_cast<int>(spriteResolved["frames"].size())));
        }
    }
    ImGui::PopStyleColor();
    SpriteRect("slice");
    ImGui::EndDisabled();
    if (sliceEdited) {
        if (ImGui::Button(Tr("Discard Changes"), ImVec2(-FLT_MIN, 0))) SpriteOpen(spritePath);
        SpriteRect("slice-discard");
    }
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", Tr("Frames keep their pivot and collision when they keep their cell. Frame numbers go left to right, top to bottom."));
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

// ----- Frames: the asset's settings and each frame's name and pivot -------------------------------

void NativeEditor::Impl::SpriteFramesTab(float side) {
    const Json& frames = spriteResolved["frames"];
    if (frames.size() == 0) return;
    const Json& frame = frames[spriteFrame];
    const Json& rawFrame = spriteRaw["frames"][spriteFrame];
    const FrameGeometry g = Geometry(frame);
    std::shared_ptr<const Texture> texture = engine.Assets().GetTexture(spriteResolved["texture"].asString());
    const float font = ImGui::GetFontSize();
    const float label = font * 6.5f;
    const bool narrow = side <= 0;
    const float strip = font * 5.2f;
    const ImVec2 avail = ImGui::GetContentRegionAvail();

    ImGui::BeginChild("##framesLeft", ImVec2(narrow ? 0.0f : avail.x - side - 8, narrow ? std::max(font * 16, avail.y - font * 24) : 0.0f));
    {
        // The frame large on its canvas; the pivot (red cross) drags.
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 size = ImMax(ImVec2(80, 80), ImGui::GetContentRegionAvail() - ImVec2(0, strip + 4));
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##pivotCanvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle | ImGuiButtonFlags_MouseButtonRight);
        SpriteRect("frame-canvas");
        const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(origin, origin + size, IM_COL32(30, 31, 35, 255));
        if (spriteFit) {
            spriteZoom = std::max(0.05f, std::min(size.x / (g.canvasW * 1.5f), size.y / (g.canvasH * 1.5f)));
            spritePan = (size - ImVec2(g.canvasW, g.canvasH) * spriteZoom) * 0.5f;
        }
        SpriteCanvasInput(origin, hovered, active);
        const ImVec2 corner = origin + spritePan;
        auto screen = [&](float x, float y) { return corner + ImVec2(x, y) * spriteZoom; };
        draw->PushClipRect(origin, origin + size, true);
        Checkerboard(draw, screen(0, 0), screen(g.canvasW, g.canvasH), std::max(6.0f, spriteZoom * 2));
        if (texture && engine.Gpu()) {
            const sg_view view = engine.Gpu()->ImageView(texture);
            const float tw = static_cast<float>(texture->width), th = static_cast<float>(texture->height);
            draw->AddImage(spriteResolved["pixelArt"].asBool(true) ? NearestTexture(view) : ViewTexture(view), screen(g.trimX, g.trimY),
                           screen(g.trimX + g.w, g.trimY + g.h), ImVec2(g.x / tw, g.y / th), ImVec2((g.x + g.w) / tw, (g.y + g.h) / th));
        }
        draw->AddRect(screen(0, 0) - ImVec2(1, 1), screen(g.canvasW, g.canvasH) + ImVec2(1, 1), IM_COL32(255, 255, 255, 50));
        if (spriteZoom >= 10)
            for (float x = 0; x <= g.canvasW; x += 1) draw->AddLine(screen(x, 0), screen(x, g.canvasH), IM_COL32(255, 255, 255, 18));
        if (spriteZoom >= 10)
            for (float y = 0; y <= g.canvasH; y += 1) draw->AddLine(screen(0, y), screen(g.canvasW, y), IM_COL32(255, 255, 255, 18));
        // The pivot: where the entity's position is on this frame.
        const ImVec2 pivot = screen(g.pivotX * g.canvasW, (1 - g.pivotY) * g.canvasH);
        const bool own = rawFrame.has("pivot");
        draw->AddLine(pivot - ImVec2(10, 0), pivot + ImVec2(10, 0), kPivot, 2.0f);
        draw->AddLine(pivot - ImVec2(0, 10), pivot + ImVec2(0, 10), kPivot, 2.0f);
        draw->AddCircle(pivot, 6, own ? kAccent : IM_COL32(255, 255, 255, 200), 0, 1.5f);
        spriteRects["pivot"] = {pivot.x - 8, pivot.y - 8, 16, 16};
        draw->PopClipRect();
        SpriteZoomBadge(origin, size);
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool nearPivot = hovered && (mouse.x - pivot.x) * (mouse.x - pivot.x) + (mouse.y - pivot.y) * (mouse.y - pivot.y) < 144;
        if (nearPivot && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) pivotDrag = true, ++spriteDragSerial;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) pivotDrag = false;
        if (nearPivot || pivotDrag) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        if (pivotDrag && active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1)) {
            // Half pixels: a pivot on a pixel's middle or its edge.
            const double px = std::clamp(Round((mouse.x - corner.x) / spriteZoom, 0.5) / g.canvasW, 0.0, 1.0);
            const double py = std::clamp(1.0 - Round((mouse.y - corner.y) / spriteZoom, 0.5) / g.canvasH, 0.0, 1.0);
            const Json value = Json::Array{Round(px, 0.0001), Round(py, 0.0001)};
            if (value != frame["pivot"]) SpriteEdit("sprite.set", ObjectOf({{"frame", spriteFrame}, {"values", ObjectOf({{"pivot", value}})}}), spriteDragSerial);
        }
        const std::string tip = Tr("Drag the red cross to move this frame's pivot. Left / right arrows step through frames.");
        draw->AddText(origin + ImVec2(8, size.y - font - 8), IM_COL32(255, 255, 255, 110), tip.c_str());
    }
    SpriteFrameStrip(strip);
    ImGui::EndChild();
    if (!narrow) ImGui::SameLine();

    ImGui::BeginChild("##framesSide", ImVec2(0, 0));
    // ----- This frame
    Section(Format(Tr("Frame %d"), spriteFrame).c_str());
    Label(Tr("Name"), label);
    std::string name;
    if (CommitText("##frameName", frame["name"].asString(), name) && !name.empty())
        SpriteEdit("sprite.set", ObjectOf({{"frame", spriteFrame}, {"values", ObjectOf({{"name", name}})}}));
    SpriteRect("frame-name");
    HelpTooltip(Tr("Sprite.frame and animation clips can name a frame instead of numbering it."));
    ImGui::TextDisabled(Tr("%.0f x %.0f px at %.0f, %.0f in the image"), g.w, g.h, g.x, g.y);
    const bool own = rawFrame.has("pivot");
    Label(Tr("Pivot"), label);
    const int mode = Segmented("##pivotMode", {Tr("Default"), Tr("Own")}, own ? 1 : 0, &spriteRects, "pivot-mode", ImGui::GetContentRegionAvail().x);
    if (mode == 0 && own) SpriteEdit("sprite.set", ObjectOf({{"frame", spriteFrame}, {"values", ObjectOf({{"pivot", Json()}})}}));
    if (mode == 1 && !own) SpriteEdit("sprite.set", ObjectOf({{"frame", spriteFrame}, {"values", ObjectOf({{"pivot", frame["pivot"]}})}}));
    if (own) {
        ImGui::Dummy(ImVec2(0, 0));
        ImGui::SameLine(label);
        Json picked;
        if (PivotGrid("##framePivot", g.pivotX, g.pivotY, picked, &spriteRects, "frame-pivot"))
            SpriteEdit("sprite.set", ObjectOf({{"frame", spriteFrame}, {"values", ObjectOf({{"pivot", picked}})}}));
        Label("X, Y", label);
        float value[2] = {g.pivotX, g.pivotY};
        if (ImGui::DragFloat2("##pivotXY", value, 0.005f, 0.0f, 1.0f, "%.3f")) {
            if (ImGui::IsItemActivated()) ++spriteDragSerial;
            SpriteEdit("sprite.set",
                       ObjectOf({{"frame", spriteFrame},
                                 {"values", ObjectOf({{"pivot", Json::Array{Round(std::clamp(value[0], 0.0f, 1.0f), 0.0001), Round(std::clamp(value[1], 0.0f, 1.0f), 0.0001)}}})}}),
                       spriteDragSerial);
        }
        if (ImGui::IsItemActivated()) ++spriteDragSerial;
        if (ImGui::Button(Tr("Use for Every Frame"), ImVec2(-FLT_MIN, 0))) {
            // This pivot becomes the default and the frames' own pivots go.
            Json values = ObjectOf({{"pivot", frame["pivot"]}, {"frames", spriteRaw["frames"]}});
            for (Json& f : values["frames"].items()) f.erase("pivot");
            SpriteEdit("sprite.set", ObjectOf({{"values", values}}));
        }
        SpriteRect("pivot-all");
    } else {
        ImGui::TextDisabled("%s", Tr("Uses the sprite's default pivot (below)."));
    }

    // ----- The whole asset
    Section(Tr("Every frame"));
    Label(Tr("Pivot"), label);
    {
        const Json& pivot = spriteResolved["pivot"];
        Json picked;
        if (PivotGrid("##defaultPivot", pivot[0].asFloat(0.5f), pivot[1].asFloat(0.5f), picked, &spriteRects, "default-pivot"))
            SpriteEdit("sprite.set", ObjectOf({{"values", ObjectOf({{"pivot", picked}})}}));
        HelpTooltip(Tr("Frames without their own pivot use this one. Bottom center puts a character's feet on its position."));
    }
    Label(Tr("Pixels/Unit"), label);
    {
        float ppu = spriteResolved["pixelsPerUnit"].asFloat(16);
        ImGui::InputFloat("##ppu", &ppu, 0, 0, "%g");
        if (ImGui::IsItemDeactivatedAfterEdit() && ppu >= 0.001f)
            SpriteEdit("sprite.set", ObjectOf({{"values", ObjectOf({{"pixelsPerUnit", Round(ppu, 0.001)}})}}));
        SpriteRect("ppu");
        HelpTooltip(Tr("Image pixels per world unit: 16 makes a 16 px frame one unit tall."));
    }
    Label(Tr("Pixel Art"), label);
    {
        bool pixelArt = spriteResolved["pixelArt"].asBool(true);
        if (ImGui::Checkbox("##pixelArt", &pixelArt)) SpriteEdit("sprite.set", ObjectOf({{"values", ObjectOf({{"pixelArt", pixelArt}})}}));
        HelpTooltip(Tr("Sharp nearest-neighbour pixels instead of smooth filtering."));
    }
    Label(Tr("Normal Map"), label);
    {
        std::string normal;
        if (CommitText("##normal", spriteRaw["normalTexture"].asString(), normal))
            SpriteEdit("sprite.set", ObjectOf({{"values", ObjectOf({{"normalTexture", normal.empty() ? Json() : Json(normal)}})}}));
        std::string dropped;
        if (DroppedAsset(dropped)) SpriteEdit("sprite.set", ObjectOf({{"values", ObjectOf({{"normalTexture", dropped}})}}));
        HelpTooltip(Tr("Optional normal image laid out like this one, for sprites lit by Light2D."));
    }
    ImGui::EndChild();
}

// ----- Collision: each frame's outline, in the frame's pixels -------------------------------------

void NativeEditor::Impl::SpriteCollisionTab(float side) {
    const Json& frames = spriteResolved["frames"];
    if (frames.size() == 0) return;
    const Json& frame = frames[spriteFrame];
    const FrameGeometry g = Geometry(frame);
    std::shared_ptr<const Texture> texture = engine.Assets().GetTexture(spriteResolved["texture"].asString());
    const float font = ImGui::GetFontSize();
    const bool narrow = side <= 0;
    const float strip = font * 5.2f;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const bool own = frame.has("collision");
    // A frame without an outline collides as the rectangle of its pixels; editing that makes it its own.
    const Json rectangle = Json::Array{Json::Array{g.trimX, g.trimY}, Json::Array{g.trimX + g.w, g.trimY}, Json::Array{g.trimX + g.w, g.trimY + g.h},
                                       Json::Array{g.trimX, g.trimY + g.h}};
    Json points = own ? frame["collision"] : rectangle;
    auto write = [&](const Json& value, int merge = -1) {
        return SpriteEdit("sprite.set", ObjectOf({{"frame", spriteFrame}, {"values", ObjectOf({{"collision", value}})}}), merge);
    };
    auto fit = [&](bool all) {
        Json args = ObjectOf({{"alphaCutoff", outlineCutoff}, {"detail", outlineDetail}, {"convex", outlineConvex}});
        if (!all) args["frame"] = spriteFrame;
        Json before = spriteRaw;
        args["path"] = spritePath;
        const Json r = Call("sprite.fit_collision", args);
        if (!Ok(r)) return;
        spriteUndo.push_back(before);
        spriteRedo.clear();
        spriteMergeSerial = -1;
        SpriteLoad();
        if (r["result"].has("skipped"))
            Notify(Format(Tr("%d frames had nothing opaque to outline"), static_cast<int>(r["result"]["skipped"].size())));
        collisionPoint = -1;
    };

    ImGui::BeginChild("##collisionLeft", ImVec2(narrow ? 0.0f : avail.x - side - 8, narrow ? std::max(font * 16, avail.y - font * 24) : 0.0f));
    {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 size = ImMax(ImVec2(80, 80), ImGui::GetContentRegionAvail() - ImVec2(0, strip + 4));
        ImGui::SetNextItemAllowOverlap();  // the zoom badge's buttons sit on top
        ImGui::InvisibleButton("##collisionCanvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle | ImGuiButtonFlags_MouseButtonRight);
        SpriteRect("collision-canvas");
        const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(origin, origin + size, IM_COL32(30, 31, 35, 255));
        if (spriteFit) {
            spriteZoom = std::max(0.05f, std::min(size.x / (g.canvasW * 1.5f), size.y / (g.canvasH * 1.5f)));
            spritePan = (size - ImVec2(g.canvasW, g.canvasH) * spriteZoom) * 0.5f;
        }
        SpriteCanvasInput(origin, hovered, active);
        const ImVec2 corner = origin + spritePan;
        auto screen = [&](float x, float y) { return corner + ImVec2(x, y) * spriteZoom; };
        auto pixel = [&](const ImVec2& s) {
            double x = (s.x - corner.x) / spriteZoom, y = (s.y - corner.y) / spriteZoom;
            const double step = collisionSnap ? 1.0 : 0.01;
            return Json::Array{Round(x, step), Round(y, step)};
        };
        draw->PushClipRect(origin, origin + size, true);
        Checkerboard(draw, screen(0, 0), screen(g.canvasW, g.canvasH), std::max(6.0f, spriteZoom * 2));
        if (texture && engine.Gpu()) {
            const sg_view view = engine.Gpu()->ImageView(texture);
            const float tw = static_cast<float>(texture->width), th = static_cast<float>(texture->height);
            draw->AddImage(spriteResolved["pixelArt"].asBool(true) ? NearestTexture(view) : ViewTexture(view), screen(g.trimX, g.trimY),
                           screen(g.trimX + g.w, g.trimY + g.h), ImVec2(g.x / tw, g.y / th), ImVec2((g.x + g.w) / tw, (g.y + g.h) / th));
        }
        draw->AddRect(screen(0, 0) - ImVec2(1, 1), screen(g.canvasW, g.canvasH) + ImVec2(1, 1), IM_COL32(255, 255, 255, 50));
        if (spriteZoom >= 10) {  // pixel grid once a pixel is big enough to aim at
            for (float x = 0; x <= g.canvasW; x += 1) draw->AddLine(screen(x, 0), screen(x, g.canvasH), IM_COL32(255, 255, 255, 18));
            for (float y = 0; y <= g.canvasH; y += 1) draw->AddLine(screen(0, y), screen(g.canvasW, y), IM_COL32(255, 255, 255, 18));
        }
        const ImVec2 pivot = screen(g.pivotX * g.canvasW, (1 - g.pivotY) * g.canvasH);
        draw->AddLine(pivot - ImVec2(7, 0), pivot + ImVec2(7, 0), IM_COL32(255, 90, 90, 170), 1.5f);
        draw->AddLine(pivot - ImVec2(0, 7), pivot + ImVec2(0, 7), IM_COL32(255, 90, 90, 170), 1.5f);
        auto outline = [&](const Json& list, ImU32 color, float thickness, bool fill) {
            std::vector<ImVec2> path;
            for (const Json& p : list.items())
                if (p.isArray() && p.size() == 2) path.push_back(screen(p[0].asFloat(), p[1].asFloat()));
            if (path.size() < 2) return;
            if (fill && path.size() >= 3) draw->AddConcavePolyFilled(path.data(), static_cast<int>(path.size()), (color & 0x00FFFFFF) | 0x30000000);
            draw->AddPolyline(path.data(), static_cast<int>(path.size()), color, thickness, ImDrawFlags_Closed);
        };
        if (collisionOnion && frames.size() > 1) {  // the previous frame's outline, faintly: line them up
            const Json& previous = frames[(spriteFrame + static_cast<int>(frames.size()) - 1) % static_cast<int>(frames.size())];
            if (previous.has("collision")) {
                const FrameGeometry pg = Geometry(previous);
                // In the previous frame's canvas, placed so both pivots meet.
                const float dx = g.pivotX * g.canvasW - pg.pivotX * pg.canvasW, dy = (1 - g.pivotY) * g.canvasH - (1 - pg.pivotY) * pg.canvasH;
                Json moved = Json::MakeArray();
                for (const Json& p : previous["collision"].items()) moved.push(Json::Array{p[0].asFloat() + dx, p[1].asFloat() + dy});
                outline(moved, IM_COL32(200, 160, 255, 110), 1.0f, false);
            }
        }
        outline(points, own ? kOutline : IM_COL32(190, 190, 190, 200), own ? 2.0f : 1.5f, own);
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        int hit = -1;
        for (size_t i = 0; i < points.size(); ++i) {
            const Json& p = points[i];
            const ImVec2 at = screen(p[0].asFloat(), p[1].asFloat());
            const bool picked = collisionPoint == static_cast<int>(i);
            const bool near = hovered && (mouse.x - at.x) * (mouse.x - at.x) + (mouse.y - at.y) * (mouse.y - at.y) < 100;
            if (near) hit = static_cast<int>(i);
            draw->AddCircleFilled(at, picked ? 6.0f : near ? 5.5f : 4.5f, picked ? kAccent : IM_COL32(245, 245, 245, 255));
            draw->AddCircle(at, picked ? 6.0f : 4.5f, IM_COL32(0, 0, 0, 160), 0, 1.0f);
            spriteRects["collision-point:" + std::to_string(i)] = {at.x - 7, at.y - 7, 14, 14};
        }
        if (!own) {
            const std::string note = Tr("No outline yet: this frame collides as its rectangle (gray). Fit or drag to make one.");
            draw->AddText(origin + ImVec2(8, 8), IM_COL32(255, 255, 255, 150), note.c_str());
        }
        draw->PopClipRect();
        SpriteZoomBadge(origin, size);
        {
            const std::string tip = Tr("Drag points. Double-click an edge adds one, right-click or Delete removes one. Arrows step frames.");
            draw->AddText(origin + ImVec2(8, size.y - font - 8), IM_COL32(255, 255, 255, 110), tip.c_str());
        }
        if (hit >= 0 || collisionPoint >= 0) ImGui::SetMouseCursor(hit >= 0 ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_Arrow);
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            collisionPoint = hit;
            ++spriteDragSerial;
        }
        if (hovered && hit >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && points.size() > 3) {
            points.items().erase(points.items().begin() + hit);
            write(points);
            collisionPoint = -1;
        }
        if (hovered && hit < 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && points.size() >= 3 && points.size() < 256) {
            // The new corner goes into the edge nearest to the click.
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
            points.items().insert(points.items().begin() + static_cast<std::ptrdiff_t>(edge + 1), pixel(mouse));
            write(points);
            collisionPoint = static_cast<int>(edge + 1);
        }
        if (collisionPoint >= 0 && static_cast<size_t>(collisionPoint) < points.size() && active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            const Json at = pixel(mouse);
            if (points[collisionPoint] != at) {
                points[collisionPoint] = at;
                write(points, spriteDragSerial);
            }
        }
        if (spriteEditorFocused && collisionPoint >= 0 && static_cast<size_t>(collisionPoint) < points.size() && points.size() > 3 &&
            !ImGui::GetIO().WantTextInput && (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) {
            points.items().erase(points.items().begin() + collisionPoint);
            write(points);
            collisionPoint = -1;
        }
    }
    SpriteFrameStrip(strip);
    ImGui::EndChild();
    if (!narrow) ImGui::SameLine();

    ImGui::BeginChild("##collisionSide", ImVec2(0, 0));
    const float label = font * 6.5f;
    Section(Format(Tr("Frame %d  %s"), spriteFrame, frame["name"].asString().c_str()).c_str());
    if (own) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kOutline), Tr("Outline of %d points"), static_cast<int>(points.size()));
    else ImGui::TextDisabled("%s", Tr("No outline: collides as its rectangle"));
    Section(Tr("Fit to the pixels"));
    Label(Tr("Alpha Cutoff"), label);
    ImGui::SliderFloat("##cutoff", &outlineCutoff, 0.01f, 1.0f, "%.2f");
    HelpTooltip(Tr("Pixels at least this opaque count as solid."));
    Label(Tr("Detail"), label);
    ImGui::SliderFloat("##detail", &outlineDetail, 0.0f, 8.0f, "%.1f px");
    HelpTooltip(Tr("How far the outline may stray from the pixel edges: higher = fewer corners, smoother sliding."));
    Label(Tr("Convex"), label);
    ImGui::Checkbox("##convex", &outlineConvex);
    HelpTooltip(Tr("Wrap the pixels in a convex shape instead of following dents."));
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.36f, 0.58f, 1.0f, 0.45f));
    if (ImGui::Button(WithIcon(kIconSquareDashed, Tr("This Frame")).c_str(), ImVec2(half, 0))) fit(false);
    SpriteRect("collision-fit");
    ImGui::SameLine();
    if (ImGui::Button(Tr("Every Frame"), ImVec2(-FLT_MIN, 0))) fit(true);
    SpriteRect("collision-fit-all");
    ImGui::PopStyleColor();
    HelpTooltip(Tr("Trace an outline around the opaque pixels of every frame (empty frames are left out)."));

    Section(Tr("Edit"));
    if (ImGui::Button(Tr("Rectangle"), ImVec2(half, 0))) write(rectangle);
    SpriteRect("collision-rectangle");
    HelpTooltip(Tr("An outline on the frame's pixel rectangle, to start from."));
    ImGui::SameLine();
    ImGui::BeginDisabled(!own);
    if (ImGui::Button(Tr("Remove Outline"), ImVec2(-FLT_MIN, 0))) {
        write(Json());
        collisionPoint = -1;
    }
    SpriteRect("collision-clear");
    ImGui::EndDisabled();
    const int previousIndex = (spriteFrame + static_cast<int>(frames.size()) - 1) % static_cast<int>(frames.size());
    ImGui::BeginDisabled(frames.size() < 2 || !frames[previousIndex].has("collision"));
    if (ImGui::Button(Tr("Copy Previous Frame"), ImVec2(half, 0))) write(frames[previousIndex]["collision"]);
    SpriteRect("collision-copy");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(collisionPoint < 0 || static_cast<size_t>(collisionPoint) >= points.size() || points.size() <= 3);
    if (ImGui::Button(Tr("Remove Point"), ImVec2(-FLT_MIN, 0))) {
        points.items().erase(points.items().begin() + collisionPoint);
        write(points);
        collisionPoint = -1;
    }
    SpriteRect("collision-remove");
    ImGui::EndDisabled();
    ImGui::Checkbox(Tr("Snap to Pixels"), &collisionSnap);
    SpriteRect("collision-snap");
    ImGui::Checkbox(Tr("Onion Skin"), &collisionOnion);
    HelpTooltip(Tr("Show the previous frame's outline faintly, to line the outlines up."));
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", Tr("Collider2D with shape sprite collides with the outline of the frame its Sprite shows, mirrored with flipX/flipY."));
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

// ----- Inspector: Sprite.frame of a sprite with an asset -------------------------------------------

void NativeEditor::Impl::SpriteFrameRow(EntityId id, const Json& values, const Json& schema) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", Tr("Frame"));
    HelpTooltip(schema["description"].asString(""));
    ImGui::TableSetColumnIndex(1);
    std::string error;
    const auto asset = engine.Assets().GetSprite(values["asset"].asString(), &error);
    if (!asset) {
        ImGui::TextColored(kError, "%s", Tr("Cannot read the sprite asset"));
        HelpTooltip(error);
        return;
    }
    const Json& frames = (*asset)["frames"];
    SpriteFrame current;
    current = values["frame"];
    const Json* shown = SpriteAssetFrame(*asset, current);
    int index = -1;
    for (size_t i = 0; i < frames.size(); ++i)
        if (&frames[i] == shown) index = static_cast<int>(i);
    std::shared_ptr<const Texture> texture = engine.Assets().GetTexture((*asset)["texture"].asString());
    ImTextureID image{};
    if (texture && engine.Gpu()) {
        const sg_view view = engine.Gpu()->ImageView(texture);
        image = (*asset)["pixelArt"].asBool(true) ? NearestTexture(view) : ViewTexture(view);
    }
    const float height = ImGui::GetFrameHeight() * 1.6f;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    if (ImGui::Button("##frameButton", ImVec2(-FLT_MIN, height))) ImGui::OpenPopup("##framePick");
    inspectorRects["Sprite.frame"] = {ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectSize().x, ImGui::GetItemRectSize().y};
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (shown && texture && engine.Gpu()) FrameThumb(draw, image, *texture, *shown, at + ImVec2(3, 3), height - 6);
    const std::string text = shown ? Format("%d   %s", index, (*shown)["name"].asString().c_str()) : Format(Tr("%s: no such frame"), current.ToJson().dump().c_str());
    draw->AddText(at + ImVec2(height + 4, (height - ImGui::GetFontSize()) * 0.5f), shown ? ImGui::GetColorU32(ImGuiCol_Text) : ImGui::GetColorU32(kError),
                  text.c_str());
    HelpTooltip(Tr("Pick the frame this sprite shows (SpriteAnimation changes it while playing)."));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(ImGui::GetFontSize() * 30, ImGui::GetFontSize() * 24));
    if (ImGui::BeginPopup("##framePick")) {
        const float side = ImGui::GetFontSize() * 3.2f;
        const int columns = 7;
        for (size_t i = 0; i < frames.size() && i < 4096; ++i) {
            if (i % columns != 0) ImGui::SameLine(0, 4);
            ImGui::PushID(static_cast<int>(i));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##pick", ImVec2(side, side))) {
                // Keep the kind of value the sprite had: a name stays a name.
                const Json value = current.name.empty() ? Json(static_cast<int>(i)) : frames[i]["name"];
                Call("component.set", ObjectOf({{"id", id}, {"type", "Sprite"}, {"values", ObjectOf({{"frame", value}})}}));
                ImGui::CloseCurrentPopup();
            }
            inspectorRects["Sprite.frame:" + std::to_string(i)] = {p.x, p.y, side, side};
            const bool hover = ImGui::IsItemHovered();
            ImDrawList* popupDraw = ImGui::GetWindowDrawList();
            if (texture && engine.Gpu()) FrameThumb(popupDraw, image, *texture, frames[i], p, side);
            popupDraw->AddRect(p, p + ImVec2(side, side), static_cast<int>(i) == index ? kAccent : IM_COL32(255, 255, 255, hover ? 110 : 30), 3,
                               static_cast<int>(i) == index ? 2.0f : 1.0f);
            if (hover) ImGui::SetTooltip("#%d  %s", static_cast<int>(i), frames[i]["name"].asString().c_str());
            ImGui::PopID();
        }
        ImGui::Separator();
        if (ImGui::Selectable(WithIcon(kIconImage, Tr("Open Sprite Editor")).c_str())) Call("editor.sprite", ObjectOf({{"tab", "frames"}}));
        ImGui::EndPopup();
    }
}

}  // namespace oe
