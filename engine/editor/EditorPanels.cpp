// Native editor panels: Hierarchy, Inspector, Assets, Console, Scripts.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <functional>

#include "editor/EditorIcons.h"
#include "editor/EditorInternal.h"
#include "editor/EditorText.h"
#include "render/GpuRenderer.h"
#include "render/Mesh.h"
#include "imgui_internal.h"

#include "app/Engine.h"
#include "core/Log.h"

namespace oe {

namespace {

bool ContainsNoCase(const std::string& text, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(text.begin(), text.end(), needle.begin(), needle.end(),
                          [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
    return it != text.end();
}

std::string FileName(const std::string& path) { return path.substr(path.find_last_of('/') + 1); }

// An icon (and its color) that tells entities apart at a glance.
struct EntityIcon {
    const char* glyph;
    ImVec4 color;
};
EntityIcon EntityKindIcon(const EntityRow& r) {
    if (r.Has("Camera")) return {kIconVideo, ImVec4(0.58f, 0.74f, 1.0f, 1)};
    if (r.Has("DirectionalLight")) return {kIconSun, ImVec4(1.0f, 0.80f, 0.42f, 1)};
    if (r.Has("PointLight") || r.Has("SpotLight")) return {kIconLightbulb, ImVec4(1.0f, 0.80f, 0.42f, 1)};
    if (r.Has("Skybox")) return {kIconSun, ImVec4(0.55f, 0.75f, 1.0f, 1)};
    if (r.Has("ReflectionProbe")) return {kIconBox, ImVec4(0.55f, 0.75f, 1.0f, 1)};
    if (r.Has("PlanarReflection")) return {kIconGrid3x3, ImVec4(0.55f, 0.75f, 1.0f, 1)};
    if (r.Has("Decal")) return {kIconGrid3x3, ImVec4(0.8f, 0.55f, 0.4f, 1)};
    if (r.Has("CharacterBody") || r.Has("CharacterBody2D")) return {kIconPersonStanding, ImVec4(0.52f, 0.86f, 0.82f, 1)};
    if (r.Has("RigidBody") || r.Has("RigidBody2D")) return {kIconAtom, ImVec4(0.58f, 0.86f, 0.58f, 1)};
    if (r.Has("Tilemap")) return {kIconGrid3x3, ImVec4(0.78f, 0.66f, 1.0f, 1)};
    if (r.Has("VFXSystem") || r.Has("Trail")) return {kIconSparkles, ImVec4(1.0f, 0.70f, 0.42f, 1)};
    if (r.Has("Sprite")) return {kIconImage, ImVec4(0.92f, 0.62f, 0.80f, 1)};
    if (r.Has("MeshRenderer")) return {kIconBox, ImVec4(0.70f, 0.74f, 0.80f, 1)};
    if (r.Has("UIText")) return {kIconType, ImVec4(1.0f, 0.66f, 0.42f, 1)};
    if (r.Has("UIPanel") || r.Has("UIButton") || r.Has("UIImage") || r.Has("UISlider") || r.Has("UIInput") || r.Has("UIDropdown") ||
        r.Has("UIJoystick")) return {kIconLayoutTemplate, ImVec4(1.0f, 0.66f, 0.42f, 1)};
    if (r.Has("AudioSource")) return {kIconVolume2, ImVec4(0.60f, 0.84f, 1.0f, 1)};
    if (r.Has("Script")) return {kIconBraces, ImVec4(0.92f, 0.84f, 0.50f, 1)};
    return {kIconCircleDashed, ImVec4(0.50f, 0.52f, 0.56f, 1)};
}

// The icon on a component's Inspector header, in the same colors as the Hierarchy's entity icons.
EntityIcon ComponentIcon(const std::string& type) {
    const ImVec4 neutral(0.70f, 0.74f, 0.80f, 1), light(1.0f, 0.80f, 0.42f, 1), ui(1.0f, 0.66f, 0.42f, 1), physics(0.58f, 0.86f, 0.58f, 1);
    if (type == "Transform") return {kIconMove3d, ImVec4(0.58f, 0.74f, 1.0f, 1)};
    if (type == "Camera" || type == "CameraFollow") return {kIconVideo, ImVec4(0.58f, 0.74f, 1.0f, 1)};
    if (type == "PostProcess") return {kIconAperture, ImVec4(0.58f, 0.74f, 1.0f, 1)};
    if (type == "DirectionalLight") return {kIconSun, light};
    if (type == "Skybox") return {kIconSun, ImVec4(0.55f, 0.75f, 1.0f, 1)};
    if (type == "ReflectionProbe") return {kIconBox, ImVec4(0.55f, 0.75f, 1.0f, 1)};
    if (type == "PointLight" || type == "SpotLight" || type == "Light2D" || type == "Darkness2D") return {kIconLightbulb, light};
    if (type == "MeshRenderer") return {kIconBox, neutral};
    if (type == "Sprite") return {kIconImage, ImVec4(0.92f, 0.62f, 0.80f, 1)};
    if (type == "SpriteAnimation" || type == "Animator") return {kIconFilm, ImVec4(0.92f, 0.62f, 0.80f, 1)};
    if (type == "BoneAttachment") return {kIconPaperclip, ImVec4(0.92f, 0.62f, 0.80f, 1)};
    if (type == "BoneIK") return {kIconMapPin, ImVec4(0.92f, 0.62f, 0.80f, 1)};
    if (type == "SpringBone") return {kIconAtom, ImVec4(0.92f, 0.62f, 0.80f, 1)};
    if (type == "SpringCollider") return {kIconAtom, ImVec4(0.70f, 0.62f, 0.92f, 1)};
    if (type == "Script") return {kIconBraces, ImVec4(0.92f, 0.84f, 0.50f, 1)};
    if (type == "Collider" || type == "Collider2D") return {kIconSquareDashed, physics};
    if (type == "Joint" || type == "Joint2D") return {kIconWorkflow, physics};
    if (type == "RigidBody" || type == "RigidBody2D") return {kIconAtom, physics};
    if (type == "CharacterBody" || type == "CharacterBody2D") return {kIconPersonStanding, ImVec4(0.52f, 0.86f, 0.82f, 1)};
    if (type == "Tilemap") return {kIconGrid3x3, ImVec4(0.78f, 0.66f, 1.0f, 1)};
    if (type == "VFXSystem" || type == "Trail") return {kIconSparkles, ImVec4(1.0f, 0.70f, 0.42f, 1)};
    if (type == "AudioSource") return {kIconVolume2, ImVec4(0.60f, 0.84f, 1.0f, 1)};
    if (type == "UIText") return {kIconType, ui};
    if (type.compare(0, 2, "UI") == 0) return {kIconLayoutTemplate, ui};
    if (type == "NetSync" || type == "NetPlayer") return {kIconNetwork, neutral};
    if (type == "Tag") return {kIconLayers, neutral};
    return {kIconPackage, neutral};
}

// A component section header in the Inspector: chevron, colored icon and name on a soft bar. Clicking
// it folds the section; the state is remembered per header id and starts open. The header stays the
// last item, so a tooltip or context menu can follow.
bool SectionHeader(const char* name, const EntityIcon& icon) {
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID openId = ImGui::GetID("##open");
    bool open = storage->GetBool(openId, true);
    const float h = ImGui::GetFrameHeight() + 2.0f, font = ImGui::GetFontSize();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemAllowOverlap();
    if (ImGui::InvisibleButton("##header", ImVec2(w, h))) storage->SetBool(openId, open = !open);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(255, 255, 255, hovered ? 20 : 11), ImGui::GetStyle().FrameRounding);
    float x = p.x + ImGui::GetStyle().FramePadding.x * 0.6f;
    const float y = p.y + (h - font) * 0.5f;
    draw->AddText(ImVec2(x, y), ImGui::GetColorU32(ImGuiCol_TextDisabled), open ? kIconChevronDown : kIconChevronRight);
    x += font + 2.0f;
    draw->AddText(ImVec2(x, y), ImGui::ColorConvertFloat4ToU32(icon.color), icon.glyph);
    x += font + 6.0f;
    draw->AddText(ImVec2(x, y), ImGui::GetColorU32(ImGuiCol_Text), name);
    return open;
}

// FieldInfo::showIf against a component's values (missing values read as their defaults).
bool ShowIfHolds(const std::string& conditions, const Json& values, const Json& defaults) {
    size_t start = 0;
    while (start <= conditions.size()) {
        size_t end = conditions.find(',', start);
        if (end == std::string::npos) end = conditions.size();
        std::string condition = conditions.substr(start, end - start);
        start = end + 1;
        if (condition.empty()) continue;
        const bool negate = condition[0] == '!' && condition.find('=') == std::string::npos;
        if (negate) condition.erase(0, 1);
        const size_t eq = condition.find('=');
        const bool notEqual = eq != std::string::npos && eq > 0 && condition[eq - 1] == '!';
        const std::string field = eq == std::string::npos ? condition : condition.substr(0, notEqual ? eq - 1 : eq);
        const Json& value = values.has(field) ? values[field] : defaults[field];
        bool holds;
        if (eq == std::string::npos) {
            holds = value.isBool() ? value.asBool() : value.isString() ? !value.asString().empty() : value.isNumber() ? value.asNumber() != 0 : value.size() > 0;
            if (negate) holds = !holds;
        } else {
            const std::string text = value.isString() ? value.asString() : value.dump();
            holds = false;
            const std::string options = condition.substr(eq + 1);
            for (size_t at = 0; at <= options.size();) {
                size_t bar = options.find('|', at);
                if (bar == std::string::npos) bar = options.size();
                if (options.compare(at, bar - at, text) == 0 && bar - at == text.size()) holds = true;
                at = bar + 1;
            }
            if (notEqual) holds = !holds;
        }
        if (!holds) return false;
    }
    return true;
}

// The header row of a field section inside the inspector's two-column table; returns whether it is
// open. A closed section names how many of its fields differ from their defaults, so edits stay visible.
bool FieldGroupRow(const char* label, bool defaultOpen, int changed) {
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID openId = ImGui::GetID("##groupOpen");
    bool open = storage->GetBool(openId, defaultOpen);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    const float h = ImGui::GetFrameHeight() - 2.0f, font = ImGui::GetFontSize();
    ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(255, 255, 255, 14));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(255, 255, 255, 22));
    if (ImGui::Selectable("##group", false, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, h)))
        storage->SetBool(openId, open = !open);
    ImGui::PopStyleColor(3);
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float y = a.y + (h - font) * 0.5f;
    // A hairline over the row separates sections without a heavy bar.
    draw->AddLine(ImVec2(a.x, a.y - 1.0f), ImVec2(b.x, a.y - 1.0f), IM_COL32(255, 255, 255, 18));
    draw->AddText(ImVec2(a.x + 2.0f, y), ImGui::GetColorU32(ImGuiCol_TextDisabled), open ? kIconChevronDown : kIconChevronRight);
    draw->AddText(ImVec2(a.x + font + 6.0f, y), IM_COL32(200, 205, 214, 255), label);
    if (!open && changed > 0) {
        const std::string badge = std::to_string(changed);
        const float w = ImGui::CalcTextSize(badge.c_str()).x + 10.0f;
        const ImVec2 p(b.x - w - 4.0f, a.y + 2.0f);
        draw->AddRectFilled(p, ImVec2(p.x + w, b.y - 2.0f), IM_COL32(92, 148, 255, 70), (b.y - a.y) * 0.5f);
        draw->AddText(ImVec2(p.x + 5.0f, y), IM_COL32(220, 230, 255, 255), badge.c_str());
    }
    if (!open && changed > 0) HelpTooltip(Format(Tr("%d fields differ from their defaults"), changed));
    return open;
}

// A borderless square icon button (a section's remove button): dim until hovered.
bool GhostIconButton(const char* id, const char* icon, float side, ImU32 hoverColor) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(side, side));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (hovered) draw->AddRectFilled(p, ImVec2(p.x + side, p.y + side), IM_COL32(255, 255, 255, 18), ImGui::GetStyle().FrameRounding);
    const ImVec2 size = ImGui::CalcTextSize(icon);
    draw->AddText(ImVec2(p.x + (side - size.x) * 0.5f, p.y + (side - size.y) * 0.5f), hovered ? hoverColor : ImGui::GetColorU32(ImGuiCol_TextDisabled), icon);
    return pressed;
}

// A rounded filter chip: colored icon, label and an optional count (-1 = none). Lifted when on, outlined
// and dimmed when off.
std::string ChipText(const char* label, int count) { return count >= 0 ? Format("%s  %d", label, count) : std::string(label); }
float ChipWidth(const char* label, int count) {
    return ImGui::GetFontSize() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(ChipText(label, count).c_str()).x +
           ImGui::GetStyle().FramePadding.x * 2.0f;
}
bool FilterChip(const char* id, const char* icon, ImU32 color, const char* label, int count, bool on) {
    const float h = ImGui::GetFrameHeight(), w = ChipWidth(label, count), pad = ImGui::GetStyle().FramePadding.x;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(p.x + w, p.y + h);
    if (on) draw->AddRectFilled(p, end, IM_COL32(255, 255, 255, hovered ? 34 : 22), h * 0.5f);
    else draw->AddRect(p, end, IM_COL32(255, 255, 255, hovered ? 40 : 22), h * 0.5f);
    const float y = p.y + (h - ImGui::GetFontSize()) * 0.5f;
    draw->AddText(ImVec2(p.x + pad, y), on ? color : ImGui::GetColorU32(ImGuiCol_TextDisabled), icon);
    const float x = p.x + pad + ImGui::GetFontSize() + ImGui::GetStyle().ItemInnerSpacing.x;
    draw->AddText(ImVec2(x, y), ImGui::GetColorU32(on ? ImGuiCol_Text : ImGuiCol_TextDisabled), label);
    if (count >= 0) {
        const float lx = x + ImGui::CalcTextSize(label).x + ImGui::CalcTextSize("  ").x;
        draw->AddText(ImVec2(lx, y), ImGui::GetColorU32(ImGuiCol_TextDisabled), std::to_string(count).c_str());
    }
    return pressed;
}

const char* KindLabel(const std::string& kind) {
    if (kind == "scene") return Tr("Scenes");
    if (kind == "prefab") return Tr("Prefabs");
    if (kind == "script") return Tr("Scripts");
    if (kind == "model") return Tr("Models");
    if (kind == "controller") return Tr("Animation Controller");
    if (kind == "animation") return Tr("Animation");
    if (kind == "texture") return Tr("Textures");
    if (kind == "material") return Tr("Materials");
    if (kind == "shader") return Tr("Shaders");
    if (kind == "vfx") return Tr("Effects");
    if (kind == "audio") return Tr("Sounds");
    if (kind == "font") return Tr("Fonts");
    return Tr("Other");
}

ImVec4 KindColor(const std::string& kind) {
    if (kind == "scene") return ImVec4(0.55f, 0.75f, 1.0f, 1);
    if (kind == "vfx") return ImVec4(1.0f, 0.65f, 0.35f, 1);
    if (kind == "prefab") return ImVec4(0.5f, 0.9f, 0.85f, 1);
    if (kind == "script") return ImVec4(0.95f, 0.85f, 0.45f, 1);
    if (kind == "model") return ImVec4(0.8f, 0.65f, 1.0f, 1);
    if (kind == "texture") return ImVec4(0.95f, 0.6f, 0.8f, 1);
    if (kind == "material") return ImVec4(1.0f, 0.62f, 0.35f, 1);
    if (kind == "audio") return ImVec4(0.55f, 0.9f, 0.55f, 1);
    return ImVec4(0.7f, 0.72f, 0.76f, 1);
}

// Editable text that only commits when the field loses focus (or Enter).
// The buffer follows the model value while the field is not being edited.
bool CommitText(const char* label, const std::string& current, std::string& result, bool multiline = false, float height = 0) {
    static std::map<ImGuiID, std::string> buffers;
    ImGuiID id = ImGui::GetID(label);
    std::string& buf = buffers[id];
    if (ImGui::GetActiveID() != id) buf = current;
    if (multiline) ImGui::InputTextMultiline(label, &buf, ImVec2(-FLT_MIN, height));
    else ImGui::InputText(label, &buf);
    if (ImGui::IsItemDeactivatedAfterEdit() && buf != current) {
        result = buf;
        return true;
    }
    return false;
}

// "jumpSpeed" -> "Jump Speed".
std::string Humanize(const std::string& field) {
    std::string pretty;
    for (size_t i = 0; i < field.size(); ++i) {
        char c = field[i];
        if (i == 0) pretty += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        else if (c == '_') pretty += ' ';
        else if (std::isupper(static_cast<unsigned char>(c)) && !std::isupper(static_cast<unsigned char>(field[i - 1]))) pretty += std::string(" ") + c;
        else pretty += c;
    }
    return pretty;
}

bool IsHexColor(const std::string& s) {
    if (s.size() != 7 || s[0] != '#') return false;
    for (size_t i = 1; i < 7; ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

// Accepts an asset dragged from the Assets panel onto the last item.
bool AcceptAsset(std::string& path) {
    bool got = false;
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kAssetPayload)) {
            path.assign(static_cast<const char*>(p->Data), static_cast<size_t>(p->DataSize));
            got = true;
        }
        ImGui::EndDragDropTarget();
    }
    return got;
}

}  // namespace

// ----- Hierarchy ---------------------------------------------------------------

void NativeEditor::Impl::EntityContextMenu(const EntityRow* row) {
    if (row) {
        if (!IsSelected(row->id)) SelectOnly(row->id);
        if (ImGui::MenuItem(Tr("Rename"), "F2")) {
            renaming = row->id;
            renameBuffer = row->name;
        }
        if (ImGui::MenuItem(Tr("Duplicate"), "Ctrl+D")) DuplicateSelection();
        if (ImGui::MenuItem(Tr("Copy"), "Ctrl+C", false, !InPlaySession())) CopySelection();
        if (ImGui::MenuItem(Tr("Paste"), "Ctrl+V", false, !InPlaySession())) PasteSelection();
        if (ImGui::MenuItem(Tr("Delete"), "Del")) DeleteSelection();
        ImGui::Separator();
        if (ImGui::MenuItem(Tr("Create Empty Child"))) {
            Json r = Call("entity.create", ObjectOf({{"name", Json(UniqueName("Empty"))}, {"parent", Json(row->id)}, {"components", ObjectOf({{"Transform", Json::MakeObject()}})}}));
            if (Ok(r)) {
                Refresh(true);
                SelectOnly(static_cast<EntityId>(r["result"]["id"].asNumber(0)));
            }
        }
        if (row->parent != kNullEntity && ImGui::MenuItem(Tr("Move to Root"))) Call("entity.set_parent", ObjectOf({{"id", Json(row->id)}, {"parent", Json(0)}}));
        if (ImGui::MenuItem(Tr("Save as Prefab..."))) {
            prefabPath = "prefabs/" + row->name + ".prefab.json";
            openPrefabPrompt = true;
        }
        if (ImGui::MenuItem(Tr("Frame in Scene View"), "F")) FrameSelection();
        ImGui::Separator();
    }
    if (ImGui::BeginMenu(Tr("Create"))) {
        CreateMenuItems();
        ImGui::EndMenu();
    }
    if (!row && ImGui::MenuItem(Tr("Paste"), "Ctrl+V", false, !InPlaySession())) PasteSelection();
}

void NativeEditor::Impl::HierarchyNode(const EntityRow& row, const std::map<EntityId, std::vector<EntityId>>& children, const std::set<EntityId>& visible) {
    if (!visible.count(row.id)) return;
    auto kids = children.find(row.id);
    bool hasKids = kids != children.end() && !kids->second.empty();
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanFullWidth |
                               ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_FramePadding;
    if (!hasKids) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (IsSelected(row.id)) flags |= ImGuiTreeNodeFlags_Selected;
    if (!hierarchyFilter.empty()) ImGui::SetNextItemOpen(true);
    ImGui::PushID(static_cast<int>(row.id));
    bool open;
    if (renaming == row.id) {
        open = ImGui::TreeNodeEx("##node", flags | ImGuiTreeNodeFlags_AllowOverlap, "%s", "");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::IsWindowAppearing() || !ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
        bool enter = ImGui::InputText("##rename", &renameBuffer, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if (enter || ImGui::IsItemDeactivated()) {
            if (!renameBuffer.empty() && renameBuffer != row.name) Call("entity.rename", ObjectOf({{"id", Json(row.id)}, {"name", Json(renameBuffer)}}));
            renaming = kNullEntity;
        }
    } else {
        const EntityIcon icon = EntityKindIcon(row);
        ImGui::PushStyleColor(ImGuiCol_Text, icon.color);
        open = ImGui::TreeNodeEx("##node", flags | ImGuiTreeNodeFlags_AllowOverlap, "%s", icon.glyph);
        ImGui::PopStyleColor();
        if (scrollToRow == row.id) {
            ImGui::SetScrollHereY(0.4f);
            scrollToRow = kNullEntity;
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
            ImGuiIO& io = ImGui::GetIO();
            if (io.KeyCtrl) {
                ToggleSelect(row.id);
            } else if (io.KeyShift && anchorRow != kNullEntity && rowIndex.count(anchorRow)) {
                // Range in list order.
                size_t a = rowIndex[anchorRow], b = rowIndex[row.id];
                if (a > b) std::swap(a, b);
                selection.clear();
                selection.push_back(row.id);
                for (size_t i = a; i <= b; ++i) {
                    if (rows[i].id != row.id && visible.count(rows[i].id)) selection.push_back(rows[i].id);
                }
                selectedId = kNullEntity;
            } else {
                SelectOnly(row.id);
            }
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            SelectOnly(row.id);
            FrameSelection();
        }
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(kEntityPayload, &row.id, sizeof(EntityId));
            ImGui::Text("%s", row.name.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kEntityPayload)) {
                EntityId dragged = *static_cast<const EntityId*>(p->Data);
                if (dragged != row.id) Call("entity.set_parent", ObjectOf({{"id", Json(dragged)}, {"parent", Json(row.id)}}));
            }
            std::string asset;
            if (AcceptAsset(asset) && asset.find(".prefab.json") != std::string::npos) {
                Call("prefab.instantiate", ObjectOf({{"path", Json(asset)}, {"parent", Json(row.id)}}));
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::BeginPopupContextItem("##ctx")) {
            EntityContextMenu(&row);
            ImGui::EndPopup();
        }
        ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::TextUnformatted(row.name.c_str());
        // Eye: shows / hides what the entity draws (its UI element, sprite, mesh or tilemap).
        {
            bool shown = true;
            const std::string type = VisibilityComponent(row.id, &shown);
            if (!type.empty()) {
                const float side = ImGui::GetFrameHeight();
                ImGui::SameLine(ImGui::GetWindowWidth() - side - ImGui::GetStyle().ScrollbarSize);
                ImGui::SetNextItemAllowOverlap();
                const ImVec2 p = ImGui::GetCursorScreenPos();
                if (ImGui::InvisibleButton("##eye", ImVec2(side, side)))
                    Call("component.set", ObjectOf({{"id", Json(row.id)}, {"type", Json(type)}, {"values", ObjectOf({{"visible", Json(!shown)}})}}));
                const bool hot = ImGui::IsItemHovered();
                const ImVec2 c(p.x + side * 0.5f, p.y + side * 0.5f);
                const ImU32 col = shown ? (hot ? IM_COL32(255, 255, 255, 255) : IM_COL32(190, 196, 208, 200)) : IM_COL32(120, 124, 134, hot ? 255 : 170);
                ImDrawList* draw = ImGui::GetWindowDrawList();
                // A long name or its tag must not show through the eye: the icon sits on the row's own color.
                draw->AddRectFilled(ImVec2(p.x - 4.0f, p.y), ImVec2(p.x + side, p.y + side),
                                    ImGui::GetColorU32(IsSelected(row.id) ? ImGuiCol_Header : ImGuiCol_WindowBg));
                const char* glyph = shown ? kIconEye : kIconEyeOff;
                const ImVec2 glyphSize = ImGui::CalcTextSize(glyph);
                draw->AddText(ImVec2(c.x - glyphSize.x * 0.5f, c.y - glyphSize.y * 0.5f), col, glyph);
                HelpTooltip(Tr(shown ? "Visible - click to hide" : "Hidden - click to show"));
            }
        }
        // Fading dot on entities an agent (API call) just changed.
        auto edit = remoteEdits.find(row.id);
        if (edit != remoteEdits.end()) {
            float age = static_cast<float>(time - edit->second);
            if (age < 4.0f) {
                ImGui::SameLine();
                ImVec2 p = ImGui::GetCursorScreenPos();
                float r = ImGui::GetFontSize() * 0.22f;
                int alpha = static_cast<int>(255.0f * (1.0f - age / 4.0f));
                const auto agentColor = remoteEditColors.find(row.id);  // a team agent's edit: its color
                const ImU32 dot = agentColor == remoteEditColors.end() ? IM_COL32(255, 158, 26, alpha) : ((agentColor->second & 0x00FFFFFFu) | (static_cast<ImU32>(alpha) << 24));
                ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r, p.y + ImGui::GetFrameHeight() * 0.5f), r, dot);
                ImGui::Dummy(ImVec2(r * 2, ImGui::GetFrameHeight()));
                HelpTooltip(Tr("Changed through the API (agent) just now"));
            }
        }
    }
    if (open && hasKids) {
        for (EntityId child : kids->second) {
            const EntityRow* c = Row(child);
            if (c) HierarchyNode(*c, children, visible);
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void NativeEditor::Impl::HierarchyPanel() {
    if (!ImGui::Begin(WithIcon(kIconListTree, TrId("Hierarchy")).c_str(), &showHierarchy)) {
        ImGui::End();
        return;
    }
    if (ImGui::Button(kIconPlus)) ImGui::OpenPopup("##create");
    HelpTooltip(Tr("Create an entity"));
    if (ImGui::BeginPopup("##create")) {
        CreateMenuItems();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##filter", WithIcon(kIconSearch, Tr("Search")).c_str(), &hierarchyFilter);

    std::map<EntityId, std::vector<EntityId>> children;
    for (const EntityRow& r : rows) children[rowIndex.count(r.parent) ? r.parent : kNullEntity].push_back(r.id);
    // Matches plus their ancestors, so the tree stays readable when filtering.
    std::set<EntityId> visible;
    for (const EntityRow& r : rows) {
        bool match = ContainsNoCase(r.name, hierarchyFilter);
        for (const std::string& c : r.components) match = match || (hierarchyFilter.size() > 2 && ContainsNoCase(c, hierarchyFilter));
        if (!match) continue;
        for (const EntityRow* p = &r; p; p = Row(p->parent)) visible.insert(p->id);
    }

    ImGui::BeginChild("##tree", ImVec2(0, 0), ImGuiChildFlags_None);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 2));
    for (EntityId id : children[kNullEntity]) {
        const EntityRow* r = Row(id);
        if (r) HierarchyNode(*r, children, visible);
    }
    ImGui::PopStyleVar();
    // Empty space: drop to un-parent, click to deselect, right-click to create.
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##empty", ImVec2(std::max(1.0f, avail.x), std::max(ImGui::GetFrameHeight() * 2, avail.y)));
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) selection.clear();
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kEntityPayload)) {
            Call("entity.set_parent", ObjectOf({{"id", Json(*static_cast<const EntityId*>(p->Data))}, {"parent", Json(0)}}));
        }
        std::string asset;
        if (AcceptAsset(asset) && asset.find(".prefab.json") != std::string::npos) Call("prefab.instantiate", ObjectOf({{"path", Json(asset)}}));
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem("##emptyctx")) {
        EntityContextMenu(nullptr);
        ImGui::EndPopup();
    }
    ImGui::EndChild();
    ImGui::End();
}

// ----- Inspector ---------------------------------------------------------------

bool NativeEditor::Impl::AssetPicker(const char* popupId, const std::string& kind, const std::string& current, std::string& out) {
    bool picked = false;
    if (ImGui::BeginPopup(popupId)) {
        auto item = [&](const std::string& value, const char* label) {
            if (ImGui::Selectable(label, value == current)) {
                out = value;
                picked = true;
            }
        };
        item("", Tr("(none)"));
        if (kind == "model") {
            for (const char* b : {"cube", "sphere", "plane", "plane64", "pyramid", "quad"}) item(b, b);
            ImGui::Separator();
        }
        if (kind == "font") {
            item("default", "default (Roboto)");
            item("pixel", "pixel (5x7)");
            ImGui::Separator();
        }
        int n = 0;
        for (const Json& a : assets.items()) {
            if (a["kind"].asString("") != kind) continue;
            std::string p = a["path"].asString("");
            item(p, p.c_str());
            ++n;
        }
        if (n == 0) ImGui::TextDisabled(Tr("No %s files in the project yet"), kind.c_str());
        ImGui::EndPopup();
    }
    return picked;
}

bool NativeEditor::Impl::FieldEditor(EntityId id, const std::string& type, const std::string& field, const Json& schema, const Json& value) {
    std::string oeType = schema["x-oe-type"].asString("");
    std::string label = "##" + field;
    std::string key = std::to_string(id) + ":" + type + ":" + field;
    Json newValue;
    bool changed = false;
    std::string merge;
    auto dragMerge = [&]() {
        // One undo step per drag: a new group each time the widget activates.
        if (ImGui::IsItemActivated()) ++dragSerial[key];
        merge = "insp:" + key + ":" + std::to_string(dragSerial[key]);
    };

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::PushTextWrapPos(0.0f);
    // Labels sit a step below the values so the eye lands on the numbers.
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.66f, 0.67f, 0.70f, 1.0f));
    ImGui::TextUnformatted(Humanize(field).c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    HelpTooltip(schema["description"].asString(""));
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);

    if (oeType == "float") {
        float v = value.asFloat(0);
        bool ranged = schema.has("minimum") && schema.has("maximum");
        if (ranged) changed = ImGui::SliderFloat(label.c_str(), &v, schema["minimum"].asFloat(), schema["maximum"].asFloat(), "%.3g");
        else changed = ImGui::DragFloat(label.c_str(), &v, 0.02f, 0, 0, "%.3g");
        dragMerge();
        if (changed) newValue = Json(v);
    } else if (oeType == "frame") {
        char text[160];
        std::snprintf(text, sizeof(text), "%s", (value.isString() ? value.asString() : value.dump()).c_str());
        if (ImGui::InputText(label.c_str(), text, sizeof(text), ImGuiInputTextFlags_EnterReturnsTrue)) {
            const std::string name(text);
            const bool number = !name.empty() && name.find_first_not_of("0123456789") == std::string::npos;
            newValue = number ? Json(std::atoi(text)) : Json(name);
            changed = true;
        }
    } else if (oeType == "int") {
        int v = value.asInt(0);
        bool ranged = schema.has("minimum") && schema.has("maximum");
        if (ranged) changed = ImGui::SliderInt(label.c_str(), &v, schema["minimum"].asInt(), schema["maximum"].asInt());
        else changed = ImGui::DragInt(label.c_str(), &v, 0.2f);
        dragMerge();
        if (changed) newValue = Json(v);
    } else if (oeType == "bool") {
        bool v = value.asBool(false);
        if (ImGui::Checkbox(label.c_str(), &v)) {
            changed = true;
            newValue = Json(v);
        }
    } else if (oeType == "vec3") {
        Vec3 v = JsonVec3(value);
        float f[3] = {v.x, v.y, v.z};
        float speed = field == "rotation" ? 0.5f : (field == "scale" ? 0.01f : 0.02f);
        const float available = ImGui::GetContentRegionAvail().x;
        const float gap = ImGui::GetStyle().ItemInnerSpacing.x;
        const bool stacked = available < ImGui::GetFontSize() * 9.5f;
        // Plain values with a thin axis-colored mark at the left edge of each field.
        const ImU32 axisColors[] = {IM_COL32(240, 104, 104, 255), IM_COL32(120, 210, 130, 255), IM_COL32(100, 156, 255, 255)};
        const char* format = field == "rotation" ? "%.3g\xc2\xb0" : "%.3g";  // rotations read in degrees
        for (int axis = 0; axis < 3; ++axis) {
            if (axis > 0 && !stacked) ImGui::SameLine(0, gap);
            ImGui::PushID(axis);
            ImGui::SetNextItemWidth(stacked ? available : (available - gap * 2) / 3);
            const bool edited = ImGui::DragFloat(label.c_str(), &f[axis], speed, 0, 0, format);
            {
                const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
                const float inset = ImGui::GetStyle().FrameRounding * 0.5f;
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(mn.x + 1.0f, mn.y + inset), ImVec2(mn.x + 3.0f, mx.y - inset), axisColors[axis], 1.0f);
            }
            HelpTooltip(Format("%s: %.7g", axis == 0 ? "X" : axis == 1 ? "Y" : "Z", static_cast<double>(f[axis])));
            if (edited || ImGui::IsItemActivated()) dragMerge();
            changed |= edited;
            ImGui::PopID();
        }
        if (changed) newValue = Vec3Json(Vec3(f[0], f[1], f[2]));
    } else if (oeType == "color") {
        Vec3 v = JsonVec3(value, Vec3(1, 1, 1));
        float f[3] = {v.x, v.y, v.z};
        changed = ImGui::ColorEdit3(label.c_str(), f, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_DisplayHex);
        dragMerge();
        if (changed) newValue = Vec3Json(Vec3(f[0], f[1], f[2]));
    } else if (oeType == "entity") {
        EntityId cur = static_cast<EntityId>(value.asNumber(0));
        const EntityRow* r = Row(cur);
        std::string preview = cur == kNullEntity ? std::string(Tr("(none)")) : (r ? r->name : "#" + std::to_string(cur));
        if (ImGui::BeginCombo(label.c_str(), preview.c_str())) {
            if (ImGui::Selectable(Tr("(none)"), cur == kNullEntity)) newValue = Json(0), changed = true;
            for (const EntityRow& e : rows) {
                if (e.id == id) continue;
                ImGui::PushID(static_cast<int>(e.id));
                if (ImGui::Selectable(e.name.c_str(), e.id == cur)) newValue = Json(e.id), changed = true;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kEntityPayload)) {
                newValue = Json(*static_cast<const EntityId*>(p->Data));
                changed = true;
            }
            ImGui::EndDragDropTarget();
        }
    } else if (oeType == "json") {
        std::string text;
        std::string current = value.dump(2);
        float lines = static_cast<float>(std::min<size_t>(12, 2 + static_cast<size_t>(std::count(current.begin(), current.end(), '\n'))));
        ImGui::PushFont(monoFont, 0.0f);
        if (CommitText(label.c_str(), current, text, true, ImGui::GetTextLineHeight() * lines + ImGui::GetStyle().FramePadding.y * 2)) {
            std::string err;
            Json parsed = Json::parse(text, &err);
            if (err.empty()) {
                newValue = parsed;
                changed = true;
            } else {
                Notify(Format(Tr("%s: invalid JSON - %s"), field.c_str(), err.c_str()), true);
            }
        }
        ImGui::PopFont();
    } else if (schema.has("enum")) {
        std::string cur = value.asString("");
        if (ImGui::BeginCombo(label.c_str(), cur.c_str())) {
            for (const Json& opt : schema["enum"].items()) {
                std::string o = opt.asString("");
                if (ImGui::Selectable(o.c_str(), o == cur)) newValue = Json(o), changed = true;
            }
            ImGui::EndCombo();
        }
    } else {
        std::string cur = value.asString("");
        std::string kind = AssetKindForField(type, field);
        std::string text;
        bool multiline = field == "text" || (field == "map" && type == "Tilemap");
        const bool boneName = ((type == "BoneIK" || type == "BoneAttachment" || type == "SpringBone") && field == "bone") ||
                              (type == "SpringCollider" && (field == "bone" || field == "tail"));
        if (boneName || (type == "Animator" && field == "clip")) {
            // Names that exist in the model: typed, or picked from its bones / clips.
            const float button = ImGui::GetFrameHeight();
            const bool missing = !cur.empty() && !NameInModel(id, boneName, cur);
            ImGui::SetNextItemWidth(-(button + ImGui::GetStyle().ItemInnerSpacing.x));
            if (missing) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.45f, 0.40f, 1.0f));
            if (CommitText(label.c_str(), cur, text)) newValue = Json(text), changed = true;
            if (missing) {
                ImGui::PopStyleColor();
                HelpTooltip(Format(boneName ? Tr("The model has no bone named %s") : Tr("The model has no clip named %s"), cur.c_str()));
            }
            ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
            if (ImGui::Button(("..." + label).c_str(), ImVec2(button, 0))) ImGui::OpenPopup("##pickname");
            HelpTooltip(boneName ? Tr("Pick a bone of the parent's model") : Tr("Pick a clip of the model"));
            std::string picked;
            if (NamePicker("##pickname", id, boneName, cur, picked)) newValue = Json(picked), changed = true;
        } else if (!kind.empty()) {
            float button = ImGui::GetFrameHeight();
            ImGui::SetNextItemWidth(-(button + ImGui::GetStyle().ItemInnerSpacing.x));
            if (CommitText(label.c_str(), cur, text)) newValue = Json(text), changed = true;
            std::string dropped;
            if (AcceptAsset(dropped)) newValue = Json(dropped), changed = true;
            ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
            std::string popup = "##pick" + field;
            if (ImGui::Button(("..." + label).c_str(), ImVec2(button, 0))) ImGui::OpenPopup(popup.c_str());
            if (!cur.empty() && showThumbnails && ImGui::IsItemHovered() && (kind == "texture" || kind == "model" || kind == "material" || kind == "prefab")) {
                // What the field points at, without opening the Assets panel.
                if (auto image = AssetThumbnail(cur)) {
                    ImGui::BeginTooltip();
                    const float side = ImGui::GetFontSize() * 9;
                    ImGui::Image(ViewTexture(engine.Gpu()->ImageView(image)), ImVec2(side, side));
                    ImGui::TextUnformatted(cur.c_str());
                    ImGui::EndTooltip();
                }
            } else
            HelpTooltip(Format(Tr("Pick a %s from the project (or drag one from Assets)"), kind.c_str()));
            std::string picked;
            if (AssetPicker(popup.c_str(), kind, cur, picked)) newValue = Json(picked), changed = true;
        } else if (multiline) {
            if (CommitText(label.c_str(), cur, text, true, ImGui::GetTextLineHeight() * 3 + ImGui::GetStyle().FramePadding.y * 2)) newValue = Json(text), changed = true;
        } else if (CommitText(label.c_str(), cur, text)) {
            newValue = Json(text);
            changed = true;
        }
    }

    const ImVec2 itemMin = ImGui::GetItemRectMin(), itemMax = ImGui::GetItemRectMax();
    inspectorRects[type + "." + field] = {itemMin.x, itemMin.y, itemMax.x - itemMin.x, itemMax.y - itemMin.y};
    if (changed) {
        Json args = ObjectOf({{"id", Json(id)}, {"type", Json(type)}, {"values", ObjectOf({{field.c_str(), newValue}})}});
        if (!merge.empty()) args["merge"] = Json(merge);
        Call("component.set", args);
        Refresh(true);
    }
    return changed;
}

// `group`X/Y/Z bools (RigidBody.lockPositionX..) as one row of axis toggles, like Unity's constraints.
void NativeEditor::Impl::AxisLockRow(EntityId id, const std::string& type, const std::string& group, const Json& properties, const Json& values) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.66f, 0.67f, 0.70f, 1.0f));
    ImGui::TextUnformatted(Humanize(group).c_str());
    ImGui::PopStyleColor();
    HelpTooltip(Tr("Locked axes (world space): the body never moves along / rotates around them."));
    ImGui::TableSetColumnIndex(1);

    const ImU32 axisColors[] = {IM_COL32(240, 104, 104, 255), IM_COL32(120, 210, 130, 255), IM_COL32(100, 156, 255, 255)};
    const float gap = ImGui::GetStyle().ItemInnerSpacing.x;
    const float chipWidth = (ImGui::GetContentRegionAvail().x - gap * 2) / 3;
    const float chipHeight = ImGui::GetFrameHeight();
    const float rounding = ImGui::GetStyle().FrameRounding;
    const ImVec2 rowStart = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    for (int axis = 0; axis < 3; ++axis) {
        const std::string field = group + "XYZ"[axis];
        if (!properties.has(field)) continue;  // its slot stays empty: X, Y and Z line up across components
        ImGui::SetCursorScreenPos(ImVec2(rowStart.x + axis * (chipWidth + gap), rowStart.y));
        const bool on = values[field].asBool(false);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushID(axis);
        const bool clicked = ImGui::InvisibleButton("##axis", ImVec2(chipWidth, chipHeight));
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        inspectorRects[type + "." + field] = {p.x, p.y, chipWidth, chipHeight};
        HelpTooltip(properties[field]["description"].asString(""));
        const ImVec2 q(p.x + chipWidth, p.y + chipHeight);
        if (on) {
            const ImU32 c = axisColors[axis];
            draw->AddRectFilled(p, q, (c & 0x00FFFFFF) | (hovered ? 0x70000000 : 0x55000000), rounding);
            draw->AddRect(p, q, c, rounding, 1.0f);
        } else {
            draw->AddRectFilled(p, q, IM_COL32(255, 255, 255, hovered ? 20 : 8), rounding);
        }
        const char label[2] = {"XYZ"[axis], 0};
        const ImVec2 size = ImGui::CalcTextSize(label);
        draw->AddText(ImVec2(p.x + (chipWidth - size.x) * 0.5f, p.y + (chipHeight - size.y) * 0.5f),
                      on ? IM_COL32(255, 255, 255, 255) : ImGui::GetColorU32(ImGuiCol_TextDisabled), label);
        if (clicked) {
            Call("component.set", ObjectOf({{"id", Json(id)}, {"type", Json(type)}, {"values", ObjectOf({{field.c_str(), Json(!on)}})}}));
            Refresh(true);
        }
    }
}

void NativeEditor::Impl::AddComponentPopup(EntityId id) {
    if (!ImGui::BeginPopup("##addcomp")) return;
    if (ImGui::IsWindowAppearing()) {
        addComponentFilter.clear();
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
    ImGui::InputTextWithHint("##find", Tr("Search components"), &addComponentFilter);
    ImGui::Separator();
    ImGui::BeginChild("##list", ImVec2(ImGui::GetFontSize() * 16, ImGui::GetFontSize() * 18));
    const Json comps = selected["components"];  // a copy: Refresh below replaces `selected`
    for (const Json& t : componentTypes.items()) {
        std::string name = t["name"].asString("");
        if (comps.has(name) || !ContainsNoCase(name, addComponentFilter)) continue;
        if (ImGui::Selectable(name.c_str())) {
            Call("component.add", ObjectOf({{"id", Json(id)}, {"type", Json(name)}}));
            Refresh(true);
            ImGui::CloseCurrentPopup();
        }
        HelpTooltip(t["doc"].asString(""));
    }
    ImGui::EndChild();
    ImGui::EndPopup();
}

void NativeEditor::Impl::InspectorPanel() {
    if (!ImGui::Begin(WithIcon(kIconSlidersHorizontal, TrId("Inspector")).c_str(), &showInspector)) {
        ImGui::End();
        return;
    }
    EntityId id = Primary();
    inspectorRects.clear();
    if (id != kNullEntity && selectedId != id) Refresh(true);
    if (id == kNullEntity || !selected.isObject()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", Tr("Select an entity in the Hierarchy or the Scene view."));
        ImGui::PopStyleColor();
        ImGui::End();
        return;
    }
    if (selection.size() > 1) ImGui::TextDisabled(Tr("%d entities selected - showing the first"), static_cast<int>(selection.size()));
    // The frame draws from a copy: an edit below calls Refresh(true), which replaces `selected`
    // while its components are still being drawn (the rest of the panel then went missing for a
    // frame, so it flickered and lost its scroll position). The next frame shows the new values.
    const Json entity = selected;

    // Entity icon, name and id.
    std::string name;
    if (const EntityRow* row = Row(id)) {
        const EntityIcon icon = EntityKindIcon(*row);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(icon.color, "%s", icon.glyph);
        ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    }
    const std::string idText = Format("#%u", id);
    ImGui::SetNextItemWidth(-ImGui::CalcTextSize(idText.c_str()).x - ImGui::GetStyle().ItemSpacing.x);
    if (CommitText("##name", entity["name"].asString(""), name) && !name.empty()) {
        Call("entity.rename", ObjectOf({{"id", Json(id)}, {"name", Json(name)}}));
        Refresh(true);
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", idText.c_str());
    ImGui::Dummy(ImVec2(0, 2));

    const Json& comps = entity["components"];
    std::string removeType;
    for (const auto& kv : comps.members()) {
        const std::string& type = kv.first;
        ImGui::PushID(type.c_str());
        auto typeIt = typeByName.find(type);
        const Json* typeInfo = typeIt == typeByName.end() ? nullptr : typeIt->second;
        const float headerRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        const float headerHeight = ImGui::GetFrameHeight() + 2.0f;
        // Section bars are neutral: the accent color is kept for what is selected.
        const bool open = SectionHeader(type.c_str(), ComponentIcon(type));
        if (typeInfo) HelpTooltip((*typeInfo)["doc"].asString(""));
        if (ImGui::BeginPopupContextItem("##compctx")) {
            if (ImGui::MenuItem(Tr("Remove Component"))) removeType = type;
            if (ImGui::MenuItem(Tr("Reset to Defaults")) && typeInfo) {
                Call("component.set", ObjectOf({{"id", Json(id)}, {"type", Json(type)}, {"values", (*typeInfo)["defaults"]}}));
                Refresh(true);
            }
            if (ImGui::MenuItem(Tr("Copy as JSON"))) ImGui::SetClipboardText(kv.second.dump(2).c_str());
            ImGui::EndPopup();
        }
        // Remove button on the header's right edge.
        ImGui::SameLine(headerRight - headerHeight - 1.0f);
        if (GhostIconButton("##remove", kIconX, headerHeight, IM_COL32(236, 110, 100, 255))) removeType = type;
        HelpTooltip(Format(Tr("Remove %s"), type.c_str()));
        if (open && typeInfo) {
            // Shortcuts into the component's own editor.
            const char* editIcon = nullptr;
            const char* editLabel = nullptr;
            std::string editPath;
            if (type == "Script") editIcon = kIconFileCode, editLabel = "Edit Script", editPath = kv.second["path"].asString("");
            if (type == "VFXSystem") editIcon = kIconSparkles, editLabel = "Edit Effect", editPath = kv.second["asset"].asString("");
            if (type == "MeshRenderer") editIcon = kIconPalette, editLabel = "Edit Material", editPath = kv.second["material"].asString("");
            if (type == "Sprite") editIcon = kIconImage, editLabel = "Slice Sheet", editPath = "slice";
            if (type == "Collider2D" && comps.has("Sprite")) editIcon = kIconSquareDashed, editLabel = "Edit Outline per Frame", editPath = "collision";
            if (editLabel && !editPath.empty()) {
                ImGui::Dummy(ImVec2(0, 1));
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1, 1, 1, 0.06f));
                if (ImGui::Button(WithIcon(editIcon, Tr(editLabel)).c_str(), ImVec2(-FLT_MIN, 0))) {
                    if (type == "Script") OpenScript(editPath);
                    else if (type == "VFXSystem") OpenEffect(editPath);
                    else if (type == "Sprite" || type == "Collider2D") Call("editor.sprite", ObjectOf({{"tab", editPath}}));
                    else OpenShader(editPath);
                }
                inspectorRects[type + "#edit"] = {ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectSize().x, ImGui::GetItemRectSize().y};
                ImGui::PopStyleColor();
            }
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ImGui::GetStyle().CellPadding.x, 3.0f));
            const bool table = ImGui::BeginTable("##fields", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX);
            ImGui::PopStyleVar();
            if (table) {
                ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.34f);
                ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.66f);
                const Json& properties = (*typeInfo)["schema"]["properties"];
                const Json& defaults = (*typeInfo)["defaults"];
                // Ungrouped fields first, then each section (x-oe-groups) under its own header row; fields
                // whose showIf does not hold for the current values are left out.
                using Member = std::pair<std::string, Json>;
                std::vector<std::pair<std::string, std::vector<const Member*>>> sections(1);
                for (const Json& g : (*typeInfo)["schema"]["x-oe-groups"].items()) sections.push_back({g["name"].asString(), {}});
                for (const auto& f : properties.members()) {
                    const std::string group = f.second["x-oe-group"].asString("");
                    size_t at = 0;
                    for (size_t i = 1; i < sections.size(); ++i)
                        if (sections[i].first == group) at = i;
                    sections[at].second.push_back(&f);
                }
                std::vector<const Member*> ordered;
                for (size_t s = 0; s < sections.size(); ++s) {
                    if (s > 0) {
                        int changed = 0;
                        for (const auto* f : sections[s].second)
                            if (kv.second.has(f->first) && kv.second[f->first] != defaults[f->first]) ++changed;
                        const Json& info = (*typeInfo)["schema"]["x-oe-groups"][s - 1];
                        ImGui::PushID(sections[s].first.c_str());
                        const bool groupOpen = FieldGroupRow(Tr(sections[s].first.c_str()), !info["collapsed"].asBool(false), changed);
                        ImGui::PopID();
                        inspectorRects[type + "#" + sections[s].first] = {ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y,
                                                                         ImGui::GetItemRectSize().x, ImGui::GetItemRectSize().y};
                        if (!groupOpen) continue;
                    }
                    ordered.clear();
                    for (const auto* f : sections[s].second)
                        if (ShowIfHolds(f->second["x-oe-showIf"].asString(""), kv.second, defaults)) ordered.push_back(f);
                    for (const auto* fp : ordered) {
                        const auto& f = *fp;
                        // lockPositionX/Y/Z, lockRotationX/Y/Z: one row of axis toggles, drawn at the group's first field.
                        const std::string& field = f.first;
                        const bool axisLock = field.size() > 5 && field.compare(0, 4, "lock") == 0 &&
                                              std::strchr("XYZ", field.back()) && f.second["x-oe-type"].asString("") == "bool";
                        const std::string group = axisLock ? field.substr(0, field.size() - 1) : std::string();
                        if (axisLock) {
                            std::string first;
                            for (const char* axis : {"X", "Y", "Z"})
                                if (first.empty() && properties.has(group + axis)) first = group + axis;
                            if (field != first) continue;
                        }
                        ImGui::PushID(field.c_str());
                        if (type == "Script" && field == "params") ScriptParamsRows(id, kv.second);
                        else if (axisLock) AxisLockRow(id, type, group, properties, kv.second);
                        else if (type == "SpriteAnimation" && field == "clips") {
                            if (ImGui::Button(Tr("Edit Sprite Frames"))) Call("editor.animation", ObjectOf({{"frames", true}}));
                        } else if (type == "Collider2D" && field == "framePoints" && comps.has("Sprite")) {
                            // Outlines per frame are pictures, not text: a count here, editing in the Sprite Editor.
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::AlignTextToFramePadding();
                            ImGui::TextDisabled("%s", Humanize(field).c_str());
                            HelpTooltip(f.second["description"].asString(""));
                            ImGui::TableSetColumnIndex(1);
                            const int count = static_cast<int>(kv.second[field].size());
                            if (ImGui::Button(count ? Format(Tr("%d frames: Edit"), count).c_str() : Tr("None: Edit"), ImVec2(-FLT_MIN, 0)))
                                Call("editor.sprite", ObjectOf({{"tab", "collision"}}));
                            inspectorRects[type + "." + field] = {ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectSize().x,
                                                                  ImGui::GetItemRectSize().y};
                        } else FieldEditor(id, type, field, f.second, kv.second[field]);
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
            }
            if (type == "MeshRenderer") BlendShapesSection(id), BonesSection(id);
        } else if (open) {
            ImGui::TextDisabled("%s", Tr("Unknown component type"));
        }
        ImGui::Dummy(ImVec2(0, open ? 4.0f : 1.0f));
        ImGui::PopID();
    }
    if (!removeType.empty()) {
        Call("component.remove", ObjectOf({{"id", Json(id)}, {"type", Json(removeType)}}));
        Refresh(true);
    }
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.36f, 0.58f, 1.0f, 0.16f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.36f, 0.58f, 1.0f, 0.28f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.36f, 0.58f, 1.0f, 0.38f));
    const bool add = ImGui::Button(WithIcon(kIconPlus, Tr("Add Component")).c_str(), ImVec2(-FLT_MIN, ImGui::GetFrameHeight() + 4.0f));
    ImGui::PopStyleColor(3);
    if (add) ImGui::OpenPopup("##addcomp");
    AddComponentPopup(id);
    ImGui::End();
}

// ----- Assets --------------------------------------------------------------------

void NativeEditor::Impl::AssetsPanel() {
    if (!ImGui::Begin(WithIcon(kIconFolder, TrId("Assets")).c_str(), &showAssets)) {
        ImGui::End();
        return;
    }
    std::map<std::string, std::vector<std::string>> byKind;
    for (const Json& a : assets.items()) {
        std::string p = a["path"].asString("");
        byKind[a["kind"].asString("other")].push_back(p);
    }
    const char* order[] = {"scene", "prefab", "script", "model", "texture", "material", "audio", "font"};
    std::vector<std::string> kinds(std::begin(order), std::end(order));
    for (const auto& kv : byKind) {
        if (std::find(kinds.begin(), kinds.end(), kv.first) == kinds.end()) kinds.push_back(kv.first);
    }
    const float font = ImGui::GetFontSize();
    // Keep the toolbar usable when the panel is docked into a narrow column.
    ImGui::SetNextItemWidth(std::max(font * 4, ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x));
    ImGui::InputTextWithHint("##filter", WithIcon(kIconSearch, Tr("Search assets by name or path")).c_str(), &assetFilter);
    ImGui::SameLine();
    if (GhostIconButton("##help", kIconCircleHelp, ImGui::GetFrameHeight(), ImGui::GetColorU32(ImGuiCol_Text))) ImGui::OpenPopup("##assethelp");
    if (ImGui::BeginPopup("##assethelp")) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + font * 22);
        ImGui::TextUnformatted(Tr("Drop files on the window to import. Drag assets into the Scene, Hierarchy or Inspector."));
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }
    ImGui::SetNextItemWidth(font * 9);
    if (ImGui::BeginCombo("##assetkind", assetKindFilter.empty() ? Tr("All assets") : KindLabel(assetKindFilter))) {
        if (ImGui::Selectable(Tr("All assets"), assetKindFilter.empty())) assetKindFilter.clear();
        for (const std::string& kind : kinds) {
            auto it = byKind.find(kind);
            if (it == byKind.end()) continue;
            const std::string label = std::string(KindLabel(kind)) + " (" + std::to_string(it->second.size()) + ")";
            if (ImGui::Selectable(label.c_str(), assetKindFilter == kind)) assetKindFilter = kind;
        }
        ImGui::EndCombo();
    }
    // Grid/List as a two-icon segmented control, then Refresh; all square, frame-high buttons.
    const float square = ImGui::GetFrameHeight();
    const float actionsWidth = square * 3 + 2 + ImGui::GetStyle().ItemSpacing.x * 2;
    if (ImGui::GetContentRegionAvail().x > font * 9 + actionsWidth) ImGui::SameLine();
    auto viewButton = [&](const char* id, const char* icon, const char* tip, bool grid) {
        // Drawn by hand: a Button would place the glyph after its frame padding, off center in a square.
        const bool on = assetGrid == grid;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton(id, ImVec2(square, square));
        const int alpha = on ? 36 : ImGui::IsItemHovered() ? 20 : 8;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(p, ImVec2(p.x + square, p.y + square), IM_COL32(255, 255, 255, alpha), ImGui::GetStyle().FrameRounding);
        const ImVec2 size = ImGui::CalcTextSize(icon);
        draw->AddText(ImVec2(p.x + (square - size.x) * 0.5f, p.y + (square - size.y) * 0.5f),
                      ImGui::GetColorU32(on ? ImGuiCol_Text : ImGuiCol_TextDisabled), icon);
        HelpTooltip(tip);
        if (clicked) assetGrid = grid;
    };
    viewButton("##grid", kIconLayoutGrid, Tr("Grid"), true);
    ImGui::SameLine(0, 2);
    viewButton("##list", kIconList, Tr("List"), false);
    ImGui::SameLine();
    if (GhostIconButton("##refresh", kIconRefreshCw, square, ImGui::GetColorU32(ImGuiCol_Text))) RefreshAssets(true);
    HelpTooltip(Tr("Refresh"));

    size_t matched = 0;
    for (const auto& kv : byKind) {
        if (!assetKindFilter.empty() && assetKindFilter != kv.first) continue;
        for (const std::string& path : kv.second) if (ContainsNoCase(path, assetFilter)) ++matched;
    }
    const std::string countLabel = Format(Tr("%d / %d assets"), static_cast<int>(matched), static_cast<int>(assets.size()));
    const float contentRight = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    if (contentRight - ImGui::GetItemRectMax().x > ImGui::CalcTextSize(countLabel.c_str()).x + ImGui::GetStyle().ItemSpacing.x)
        ImGui::SameLine();
    ImGui::TextDisabled("%s", countLabel.c_str());
    ImGui::Separator();
    ImGui::BeginChild("##assets", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()));
    // Honor the explicit view selection even in shallow docks; cards remain scrollable.
    const bool useGrid = assetGrid;
    const float available = ImGui::GetContentRegionAvail().x;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const int columns = useGrid ? std::max(1, static_cast<int>((available + gap) / (font * 9 + gap))) : 1;
    const float itemWidth = std::max(1.0f, (available - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns));
    const float itemHeight = useGrid ? font * 6.6f : font * 3.4f;
    int index = 0;
    for (const std::string& kind : kinds) {
        auto it = byKind.find(kind);
        if (it == byKind.end() || (!assetKindFilter.empty() && assetKindFilter != kind)) continue;
        std::sort(it->second.begin(), it->second.end());
        for (const std::string& path : it->second) {
            if (!ContainsNoCase(path, assetFilter)) continue;
            if (index++ % columns != 0) ImGui::SameLine(0, gap);
            ImGui::PushID(path.c_str());
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const bool visible = ImGui::IsRectVisible(ImVec2(itemWidth, itemHeight));
            if (ImGui::Selectable("##asset", selectedAsset == path, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(itemWidth, itemHeight))) {
                selectedAsset = path;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (kind == "scene") RequestAction({PendingAction::Kind::LoadScene, path});
                    else if (kind == "script") OpenScript(path);
                    else if (kind == "shader" || kind == "material") OpenShader(path);
                    else if (kind == "vfx") OpenEffect(path);
                    else if (kind == "controller") Call("editor.animation", ObjectOf({{"controller", path}}));
                    else if (kind == "prefab") Call("prefab.instantiate", ObjectOf({{"path", Json(path)}}));
                }
            }
            const char* hint = kind == "scene" ? Tr("Double-click to open") : (kind == "script" || kind == "shader" || kind == "material" || kind == "vfx" || kind == "controller") ? Tr("Double-click to edit") : kind == "prefab" ? Tr("Double-click to instantiate") : nullptr;
            HelpTooltip(hint ? path + "\n" + hint : path);
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload(kAssetPayload, path.data(), path.size());
                ImGui::TextUnformatted(path.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginPopupContextItem("##assetctx")) {
                selectedAsset = path;
                if (kind == "scene" && ImGui::MenuItem(Tr("Open"))) RequestAction({PendingAction::Kind::LoadScene, path});
                if (kind == "script" && ImGui::MenuItem(Tr("Edit"))) OpenScript(path);
                if (kind == "vfx" && ImGui::MenuItem(Tr("Edit Effect"))) OpenEffect(path);
                if ((kind == "shader" || kind == "material") && ImGui::MenuItem(Tr("Edit Graph"))) OpenShader(path);
                if (kind == "prefab" && ImGui::MenuItem(Tr("Instantiate"))) Call("prefab.instantiate", ObjectOf({{"path", Json(path)}}));
                if (kind == "prefab" && ImGui::MenuItem(Tr("Edit Prefab"), nullptr, false, !InPlaySession()))
                    RequestAction({PendingAction::Kind::EditPrefab, path});
                if (ImGui::MenuItem(Tr("Copy Path"))) ImGui::SetClipboardText(path.c_str());
                if (ImGui::MenuItem(Tr("Info"))) {
                    Json r = Call("asset.info", ObjectOf({{"path", Json(path)}}));
                    if (Ok(r)) OE_LOG_INFO("asset", "%s: %s", path.c_str(), r["result"].dump(2).c_str());
                    showConsole = true;
                }
                ImGui::EndPopup();
            }
            if (visible) {
                ImDrawList* draw = ImGui::GetWindowDrawList();
                const bool isSelected = selectedAsset == path;
                const float padding = font * 0.5f;
                const ImVec2 end(pos.x + itemWidth, pos.y + itemHeight);
                // Soft cards; the selected one gets an accent wash and outline.
                draw->AddRectFilled(pos, end, isSelected ? IM_COL32(92, 148, 255, 40) : IM_COL32(255, 255, 255, 8), 6);
                if (isSelected) draw->AddRect(pos, end, IM_COL32(92, 148, 255, 160), 6);
                const float side = useGrid ? font * 3.5f : itemHeight - padding * 2;
                const ImVec2 preview(useGrid ? pos.x + (itemWidth - side) * 0.5f : pos.x + padding, pos.y + padding);
                draw->AddRectFilled(preview, ImVec2(preview.x + side, preview.y + side), IM_COL32(0, 0, 0, 60), 4);
                const bool previewable = kind == "texture" || kind == "model" || kind == "material" || kind == "prefab" || kind == "scene";
                auto image = showThumbnails && previewable ? AssetThumbnail(path) : nullptr;
                if (image) {
                    const float scale = side / static_cast<float>(std::max(image->width, image->height));
                    const ImVec2 size(static_cast<float>(image->width) * scale, static_cast<float>(image->height) * scale);
                    const ImVec2 start(preview.x + (side - size.x) * 0.5f, preview.y + (side - size.y) * 0.5f);
                    draw->AddImage(ViewTexture(engine.Gpu()->ImageView(image)), start, start + size);
                } else {
                    const char* symbol = kind == "script" ? kIconFileCode : kind == "audio" ? kIconMusic : kind == "font" ? kIconType :
                                         kind == "vfx" ? kIconSparkles : kind == "shader" ? kIconWorkflow : kind == "material" ? kIconPalette :
                                         kind == "prefab" ? kIconPackage : kind == "scene" ? kIconFileJson : kind == "model" ? kIconBox :
                                         kind == "texture" ? kIconImage : kIconFile;
                    const float iconSize = std::min(side * 0.5f, ImGui::GetFontSize() * 2.0f);
                    const ImVec2 size = ImGui::GetFont()->CalcTextSizeA(iconSize, FLT_MAX, 0.0f, symbol);
                    draw->AddText(ImGui::GetFont(), iconSize, ImVec2(preview.x + (side - size.x) * 0.5f, preview.y + (side - size.y) * 0.5f),
                                  ImGui::GetColorU32(KindColor(kind)), symbol);
                }
                const float textX = useGrid ? pos.x + padding : preview.x + side + padding;
                const float textY = useGrid ? preview.y + side + padding * 0.7f : pos.y + padding;
                const float right = end.x - padding;
                // Clip at UTF-8 boundaries; the tooltip retains the complete filename and path.
                auto label = [&](std::string text, float y, ImU32 color) {
                    if (ImGui::CalcTextSize(text.c_str()).x > right - textX) {
                        while (!text.empty() && ImGui::CalcTextSize((text + "...").c_str()).x > right - textX) {
                            size_t last = text.size() - 1;
                            while (last > 0 && (static_cast<unsigned char>(text[last]) & 0xc0) == 0x80) --last;
                            text.resize(last);
                        }
                        text += "...";
                    }
                    draw->PushClipRect(ImVec2(textX, pos.y), ImVec2(right, end.y), true);
                    draw->AddText(ImVec2(textX, y), color, text.c_str());
                    draw->PopClipRect();
                };
                label(FileName(path), textY, ImGui::GetColorU32(ImGuiCol_Text));
                const size_t slash = path.find_last_of('/');
                const std::string detail = useGrid ? KindLabel(kind) :
                    std::string(KindLabel(kind)) + "  /  " + (slash == std::string::npos ? "." : path.substr(0, slash));
                label(detail, textY + font * 1.25f, ImGui::GetColorU32(useGrid ? KindColor(kind) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)));
            }
            ImGui::PopID();
        }
    }
    if (assets.size() == 0) ImGui::TextDisabled("%s", Tr("The project has no assets yet."));
    else if (matched == 0) {
        ImGui::TextUnformatted(Tr("No matching assets."));
        if (ImGui::Button(Tr("Clear filters"))) { assetFilter.clear(); assetKindFilter.clear(); }
    }
    ImGui::EndChild();
    ImGui::Separator();
    if (selectedAsset.empty()) ImGui::TextDisabled("%s", Tr("Select an asset to see its path."));
    else {
        ImGui::TextUnformatted(selectedAsset.c_str());
        HelpTooltip(selectedAsset);
    }
    ImGui::End();
}

// ----- Console -------------------------------------------------------------------

void NativeEditor::FocusConsole() { impl_->showConsole = impl_->requestConsoleFocus = true; }

void NativeEditor::Impl::ConsolePanel() {
    if (requestConsoleFocus) {
        ImGui::SetNextWindowFocus();
        requestConsoleFocus = false;
    }
    if (!ImGui::Begin(WithIcon(kIconTerminal, TrId("Console")).c_str(), &showConsole)) {
        ImGui::End();
        return;
    }
    static const char* levels[] = {"debug", "info", "warn", "error"};
    const char* labels[] = {Tr("Debug"), Tr("Info"), Tr("Warnings"), Tr("Errors")};
    const char* icons[] = {kIconBug, kIconInfo, kIconTriangleAlert, kIconCircleX};
    const ImVec4 colors[] = {ImVec4(0.66f, 0.69f, 0.75f, 1), ImVec4(0.58f, 0.74f, 1.0f, 1), ImVec4(0.95f, 0.78f, 0.35f, 1), ImVec4(1, 0.45f, 0.4f, 1)};
    auto levelOf = [&](const LogLine& l) {
        int li = 1;
        for (int i = 0; i < 4; ++i) {
            if (l.level == levels[i]) li = i;
        }
        return li;
    };
    int counts[4] = {};
    for (const LogLine& l : log) ++counts[levelOf(l)];

    // Level chips (with counts) and auto-scroll, wrapping onto a new row when the dock is narrow.
    auto fits = [&](float width) {
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        return ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width <= right;
    };
    for (int i = 0; i < 4; ++i) {
        if (i > 0 && fits(ChipWidth(labels[i], counts[i]))) ImGui::SameLine(0, 4);
        if (FilterChip(levels[i], icons[i], ImGui::ColorConvertFloat4ToU32(colors[i]), labels[i], counts[i], logShow[i])) logShow[i] = !logShow[i];
    }
    if (fits(ChipWidth(Tr("Auto-scroll"), -1))) ImGui::SameLine();
    if (FilterChip("##autoscroll", kIconArrowDownToLine, IM_COL32(92, 148, 255, 255), Tr("Auto-scroll"), -1, logAutoScroll)) logAutoScroll = !logAutoScroll;
    const float side = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(std::max(ImGui::GetFontSize() * 3, ImGui::GetContentRegionAvail().x - side - ImGui::GetStyle().ItemSpacing.x));
    ImGui::InputTextWithHint("##logfilter", WithIcon(kIconSearch, Tr("Filter")).c_str(), &logFilter);
    ImGui::SameLine();
    if (GhostIconButton("##clear", kIconTrash2, side, ImGui::GetColorU32(ImGuiCol_Text))) {
        log.clear();
        warnCount = errorCount = 0;
        Call("script.errors", ObjectOf({{"clear", Json(true)}}), true);
    }
    HelpTooltip(Tr("Clear"));

    // Lines: level icon, dim category, message; on a slightly darker well.
    float inputHeight = ImGui::GetFrameHeightWithSpacing();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.22f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ImGui::GetStyle().FrameRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
    ImGui::BeginChild("##log", ImVec2(0, -inputHeight), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    for (const LogLine& l : log) {
        const int li = levelOf(l);
        if (!logShow[li]) continue;
        if (!logFilter.empty() && !ContainsNoCase(l.message, logFilter) && !ContainsNoCase(l.category, logFilter)) continue;
        ImGui::TextColored(colors[li], "%s", icons[li]);
        ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::PushFont(monoFont, 0.0f);
        ImGui::TextDisabled("%s", l.category.c_str());
        ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x * 2);
        ImGui::PushStyleColor(ImGuiCol_Text, li == 1 ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : colors[li]);
        ImGui::TextUnformatted(l.message.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        if (ImGui::BeginPopupContextItem(("##l" + std::to_string(l.seq)).c_str())) {
            if (ImGui::MenuItem(Tr("Copy"))) ImGui::SetClipboardText(l.message.c_str());
            ImGui::EndPopup();
        }
    }
    if (logAutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    // Command line: `command.name {json args}`, Up/Down for history, Tab completes.
    struct Ctx {
        NativeEditor::Impl* m;
    } ctx{this};
    auto callback = [](ImGuiInputTextCallbackData* d) -> int {
        auto* m = static_cast<Ctx*>(d->UserData)->m;
        if (d->EventFlag == ImGuiInputTextFlags_CallbackHistory && !m->consoleHistory.empty()) {
            int n = static_cast<int>(m->consoleHistory.size());
            if (d->EventKey == ImGuiKey_UpArrow) m->historyPos = m->historyPos < 0 ? n - 1 : std::max(0, m->historyPos - 1);
            else if (d->EventKey == ImGuiKey_DownArrow) m->historyPos = m->historyPos < 0 ? -1 : (m->historyPos + 1 >= n ? -1 : m->historyPos + 1);
            d->DeleteChars(0, d->BufTextLen);
            if (m->historyPos >= 0) d->InsertChars(0, m->consoleHistory[static_cast<size_t>(m->historyPos)].c_str());
        } else if (d->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
            std::string word(d->Buf, static_cast<size_t>(d->CursorPos));
            if (word.find(' ') != std::string::npos) return 0;
            std::vector<std::string> matches;
            for (const std::string& c : m->commandNames) {
                if (c.rfind(word, 0) == 0) matches.push_back(c);
            }
            if (matches.size() == 1) {
                d->DeleteChars(0, d->CursorPos);
                d->InsertChars(0, (matches[0] + " ").c_str());
            } else if (matches.size() > 1) {
                std::string list;
                for (const std::string& c : matches) list += c + "  ";
                OE_LOG_INFO("console", "%s", list.c_str());
            }
        }
        return 0;
    };
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", kIconChevronRight);
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGuiInputTextFlags f =ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackCompletion;
    if (ImGui::InputTextWithHint("##cmd", Tr("command.name {\"json\": \"args\"}   (Tab completes, Up/Down history)"), &consoleInput, f, callback, &ctx)) {
        RunConsoleCommand(consoleInput);
        consoleInput.clear();
        ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::End();
}

// ----- Script params -------------------------------------------------------------------

const Json& NativeEditor::Impl::ScriptParamSchema(const std::string& path) {
    auto it = paramSchemas.find(path);
    // Re-read now and then: the script may change on disk (hot reload, agents).
    if (it == paramSchemas.end() || time - it->second.first > 3.0) {
        Json r = path.empty() ? Json() : Call("script.params", ObjectOf({{"path", Json(path)}}), true);
        paramSchemas[path] = {time, r["ok"].asBool() ? r["result"] : Json::MakeArray()};
        it = paramSchemas.find(path);
    }
    return it->second.second;
}

void NativeEditor::Impl::ScriptParamsRows(EntityId id, const Json& script) {
    const Json& params = script["params"];
    std::string path = script["path"].asString("");
    const Json& schema = ScriptParamSchema(path);
    bool raw = fieldParams.count(id) == 0 || !(params.isObject() || params.isNull());

    // Header row: "Params" + view toggle + add.
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImVec4(0.66f, 0.67f, 0.70f, 1.0f), "%s", Tr("Params"));
    HelpTooltip(Tr("Values the script reads as self.params.<name>. Rows come from the script; dimmed values are the script's defaults."));
    ImGui::TableSetColumnIndex(1);
    if (ImGui::SmallButton(raw ? Tr("Fields") : "JSON")) {
        if (raw) fieldParams.insert(id);
        else fieldParams.erase(id);
    }
    HelpTooltip(raw ? Tr("Edit params as fields") : Tr("Edit params as JSON text"));
    if (!raw) {
        ImGui::SameLine();
        if (ImGui::SmallButton(Tr("+ Add"))) {
            newParamName.clear();
            ImGui::OpenPopup("##addparam");
        }
        HelpTooltip(Tr("Add a param the script does not read yet"));
    }

    Json current = params.isObject() ? params : Json::MakeObject();
    auto write = [&](const Json& next, const std::string& merge) {
        Json args = ObjectOf({{"id", Json(id)}, {"type", Json("Script")}, {"values", ObjectOf({{"params", next}})}});
        if (!merge.empty()) args["merge"] = Json(merge);
        Call("component.set", args);
        Refresh(true);
    };
    auto setKey = [&](const std::string& key, const Json& value, const std::string& merge) {
        Json next = current;
        next[key] = value;
        write(next, merge);
    };
    auto removeKey = [&](const std::string& key) {
        Json next = current;
        next.erase(key);
        write(next, "");
    };

    if (ImGui::BeginPopup("##addparam")) {
        static const char* types[] = {"number", "boolean", "string", "vec3"};
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        bool enter = ImGui::InputTextWithHint("##name", Tr("name"), &newParamName, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
        ImGui::Combo("##type", &newParamType, types, 4);
        ImGui::SameLine();
        if ((ImGui::Button(Tr("Add")) || enter) && !newParamName.empty()) {
            Json v = newParamType == 0 ? Json(0) : newParamType == 1 ? Json(false) : newParamType == 2 ? Json("") : Vec3Json(Vec3(0, 0, 0));
            setKey(newParamName, v, "");
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (raw) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        std::string text;
        std::string cur = params.dump(2);
        float lines = static_cast<float>(std::min<size_t>(12, 2 + static_cast<size_t>(std::count(cur.begin(), cur.end(), '\n'))));
        ImGui::PushFont(monoFont, 0.0f);
        if (CommitText("##rawparams", cur, text, true, ImGui::GetTextLineHeight() * lines + ImGui::GetStyle().FramePadding.y * 2)) {
            std::string err;
            Json parsed = Json::parse(text, &err);
            if (err.empty()) write(parsed, "");
            else Notify(Format(Tr("%s: invalid JSON - %s"), "params", err.c_str()), true);
        }
        ImGui::PopFont();
        return;
    }

    // Rows: the script's params in source order, then keys it does not read.
    struct Row {
        std::string name;
        const Json* info;  // script.params entry or null
    };
    std::vector<Row> rowsToShow;
    std::set<std::string> known;
    for (const Json& p : schema.items()) {
        rowsToShow.push_back({p["name"].asString(""), &p});
        known.insert(p["name"].asString(""));
    }
    for (const auto& kv : current.members()) {
        if (!known.count(kv.first)) rowsToShow.push_back({kv.first, nullptr});
    }
    if (rowsToShow.empty()) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled("%s", Tr(path.empty() ? "Set a script path first" : "The script reads no params"));
    }

    for (const Row& row : rowsToShow) {
        const std::string& name = row.name;
        bool set = current.has(name);
        Json def = row.info ? (*row.info)["default"] : Json();
        const Json& value = set ? current[name] : def;
        std::string type = row.info ? (*row.info)["type"].asString("any") : "any";
        if (type == "any") {  // no default in the script: go by the value
            type = value.isNumber() ? "number" : value.isBool() ? "boolean" : value.isString() ? "string"
                 : (value.isArray() && value.size() == 3) ? "vec3" : (value.isArray() || value.isObject()) ? "object" : "string";
        }
        std::string key = std::to_string(id) + ":params:" + name;
        std::string merge;
        auto dragMerge = [&]() {
            if (ImGui::IsItemActivated()) ++dragSerial[key];
            merge = "insp:" + key + ":" + std::to_string(dragSerial[key]);
        };

        ImGui::PushID(name.c_str());
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::Indent(ImGui::GetStyle().IndentSpacing * 0.6f);
        ImGui::PushStyleColor(ImGuiCol_Text, row.info ? ImVec4(0.66f, 0.67f, 0.70f, 1) : ImVec4(0.95f, 0.72f, 0.3f, 1));
        ImGui::TextUnformatted(Humanize(name).c_str());
        ImGui::PopStyleColor();
        ImGui::Unindent(ImGui::GetStyle().IndentSpacing * 0.6f);
        {
            std::string tip = name;
            if (row.info) {
                std::string desc = (*row.info)["description"].asString("");
                if (!desc.empty()) tip += "\n" + desc;
                if (!def.isNull()) tip += "\n" + Format(Tr("Default: %s"), def.dump().c_str());
                tip += "\n" + Format(Tr("Read at line %d"), (*row.info)["line"].asInt(0));
            } else {
                tip += "\n" + std::string(Tr("The script does not read this param (typo, or an old name?)"));
            }
            HelpTooltip(tip);
        }

        ImGui::TableSetColumnIndex(1);
        // Right side: reset (set values) or remove (unknown keys).
        float button = ImGui::CalcTextSize(Tr("Reset")).x + ImGui::GetStyle().FramePadding.x * 2;
        // Reset only when it differs from the script's default; Remove for keys the script ignores.
        bool showButton = set && (!row.info || def.isNull() || !(value == def));
        ImGui::SetNextItemWidth(showButton ? -(button + ImGui::GetStyle().ItemInnerSpacing.x) : -FLT_MIN);
        // Dimmed while the script's default is in use.
        if (!set) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.55f);

        Json next;
        bool changed = false;
        if (type == "number") {
            float v = value.asFloat(0);
            float speed = std::max(0.01f, std::fabs(def.isNumber() ? def.asFloat() : v) * 0.01f);
            changed = ImGui::DragFloat("##v", &v, speed, 0, 0, "%g");
            dragMerge();
            if (changed) next = Json(v);
        } else if (type == "boolean") {
            bool v = value.asBool(false);
            if (ImGui::Checkbox("##v", &v)) next = Json(v), changed = true;
        } else if (type == "vec3") {
            Vec3 v3 = JsonVec3(value);
            float f[3] = {v3.x, v3.y, v3.z};
            changed = ImGui::DragFloat3("##v", f, 0.05f, 0, 0, "%g");
            dragMerge();
            if (changed) next = Vec3Json(Vec3(f[0], f[1], f[2]));
        } else if (type == "string" && row.info && (*row.info)["options"].size() > 0) {
            std::string cur = value.asString("");
            if (ImGui::BeginCombo("##v", cur.c_str())) {
                for (const Json& o : (*row.info)["options"].items()) {
                    std::string opt = o.asString("");
                    if (ImGui::Selectable(opt.empty() ? "\"\"" : opt.c_str(), opt == cur)) next = Json(opt), changed = true;
                }
                ImGui::EndCombo();
            }
        } else if (type == "string" && IsHexColor(value.asString(""))) {
            std::string hex = value.asString("");
            float c[3];
            for (int k = 0; k < 3; ++k) c[k] = static_cast<float>(std::strtol(hex.substr(1 + 2 * k, 2).c_str(), nullptr, 16)) / 255.0f;
            if (ImGui::ColorEdit3("##v", c, ImGuiColorEditFlags_DisplayHex)) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", static_cast<int>(c[0] * 255 + 0.5f), static_cast<int>(c[1] * 255 + 0.5f), static_cast<int>(c[2] * 255 + 0.5f));
                next = Json(std::string(buf));
                changed = true;
            }
            dragMerge();
        } else if (type == "string") {
            std::string text;
            std::string lower;
            for (char c : name) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            // Scene / prefab / sound params get the asset picker.
            std::string kind = lower.find("scene") != std::string::npos ? "scene" : lower.find("prefab") != std::string::npos ? "prefab"
                             : (lower.find("sound") != std::string::npos || lower.find("clip") != std::string::npos) ? "audio" : "";
            if (!kind.empty()) {
                float b = ImGui::GetFrameHeight();
                ImGui::SetNextItemWidth(-(b + ImGui::GetStyle().ItemInnerSpacing.x + (showButton ? button + ImGui::GetStyle().ItemInnerSpacing.x : 0)));
            }
            if (CommitText("##v", value.asString(""), text)) next = Json(text), changed = true;
            std::string dropped;
            if (AcceptAsset(dropped)) next = Json(dropped), changed = true;
            if (!kind.empty()) {
                ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
                if (ImGui::Button("...", ImVec2(ImGui::GetFrameHeight(), 0))) ImGui::OpenPopup("##pickparam");
                std::string picked;
                if (AssetPicker("##pickparam", kind, value.asString(""), picked)) next = Json(picked), changed = true;
            }
        } else {
            std::string text;
            ImGui::PushFont(monoFont, 0.0f);
            if (CommitText("##v", value.dump(), text)) {
                std::string err;
                Json parsed = Json::parse(text, &err);
                if (err.empty()) next = parsed, changed = true;
                else Notify(Format(Tr("%s: invalid JSON - %s"), name.c_str(), err.c_str()), true);
            }
            ImGui::PopFont();
        }
        if (!set) ImGui::PopStyleVar();

        if (showButton) {
            ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
            if (ImGui::Button(row.info ? Tr("Reset") : Tr("Remove"))) {
                removeKey(name);
                changed = false;
            }
            HelpTooltip(row.info ? Tr("Back to the script's default (removes it from params)") : Tr("Remove from params"));
        }
        if (changed) setKey(name, next, merge);
        ImGui::PopID();
    }
}

}  // namespace oe
