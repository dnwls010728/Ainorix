// Native editor panels: Hierarchy, Inspector, Assets, Console, Scripts.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>

#include "editor/EditorInternal.h"
#include "editor/EditorText.h"
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

// A short colored tag that tells entities apart at a glance.
void EntityKindTag(const EntityRow& r) {
    const char* tag = "";
    ImVec4 col(0.55f, 0.58f, 0.63f, 1);
    if (r.Has("Camera")) tag = "CAM", col = ImVec4(0.55f, 0.75f, 1.0f, 1);
    else if (r.Has("DirectionalLight") || r.Has("PointLight")) tag = "LIT", col = ImVec4(1.0f, 0.82f, 0.4f, 1);
    else if (r.Has("CharacterBody")) tag = "CHR", col = ImVec4(0.5f, 0.9f, 0.85f, 1);
    else if (r.Has("RigidBody")) tag = "PHY", col = ImVec4(0.55f, 0.9f, 0.55f, 1);
    else if (r.Has("Tilemap")) tag = "MAP", col = ImVec4(0.8f, 0.65f, 1.0f, 1);
    else if (r.Has("Sprite")) tag = "SPR", col = ImVec4(0.95f, 0.6f, 0.8f, 1);
    else if (r.Has("MeshRenderer")) tag = "MSH", col = ImVec4(0.7f, 0.74f, 0.8f, 1);
    else if (r.Has("UIText") || r.Has("UIPanel") || r.Has("UIButton") || r.Has("UIImage") || r.Has("UISlider")) tag = "UI", col = ImVec4(1.0f, 0.62f, 0.35f, 1);
    else if (r.Has("AudioSource")) tag = "SND", col = ImVec4(0.6f, 0.85f, 1.0f, 1);
    if (!*tag) return;
    ImGui::SameLine(0, 4);
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.72f);
    ImGui::TextColored(col, "%s", tag);
    ImGui::PopFont();
}

const char* KindLabel(const std::string& kind) {
    if (kind == "scene") return Tr("Scenes");
    if (kind == "prefab") return Tr("Prefabs");
    if (kind == "script") return Tr("Scripts");
    if (kind == "model") return Tr("Models");
    if (kind == "texture") return Tr("Textures");
    if (kind == "material") return Tr("Materials");
    if (kind == "audio") return Tr("Sounds");
    if (kind == "font") return Tr("Fonts");
    return Tr("Other");
}

ImVec4 KindColor(const std::string& kind) {
    if (kind == "scene") return ImVec4(0.55f, 0.75f, 1.0f, 1);
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
        open = ImGui::TreeNodeEx("##node", flags, "%s", row.name.c_str());
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
        EntityKindTag(row);
        // Fading dot on entities an agent (API call) just changed.
        auto edit = remoteEdits.find(row.id);
        if (edit != remoteEdits.end()) {
            float age = static_cast<float>(time - edit->second);
            if (age < 4.0f) {
                ImGui::SameLine();
                ImVec2 p = ImGui::GetCursorScreenPos();
                float r = ImGui::GetFontSize() * 0.22f;
                int alpha = static_cast<int>(255.0f * (1.0f - age / 4.0f));
                ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r, p.y + ImGui::GetFrameHeight() * 0.5f), r, IM_COL32(255, 158, 26, alpha));
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
    if (!ImGui::Begin(TrId("Hierarchy").c_str(), &showHierarchy)) {
        ImGui::End();
        return;
    }
    if (ImGui::Button("+")) ImGui::OpenPopup("##create");
    HelpTooltip(Tr("Create an entity"));
    if (ImGui::BeginPopup("##create")) {
        CreateMenuItems();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##filter", Tr("Search"), &hierarchyFilter);

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
            for (const char* b : {"cube", "sphere", "plane", "pyramid", "quad"}) item(b, b);
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
    std::string pretty;
    for (size_t i = 0; i < field.size(); ++i) {
        char c = field[i];
        if (i == 0) pretty += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        else if (std::isupper(static_cast<unsigned char>(c)) && !std::isupper(static_cast<unsigned char>(field[i - 1]))) pretty += std::string(" ") + c;
        else pretty += c;
    }
    ImGui::TextUnformatted(pretty.c_str());
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
        changed = ImGui::DragFloat3(label.c_str(), f, speed, 0, 0, "%.3g");
        dragMerge();
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
        if (!kind.empty()) {
            float button = ImGui::GetFrameHeight();
            ImGui::SetNextItemWidth(-(button + ImGui::GetStyle().ItemInnerSpacing.x));
            if (CommitText(label.c_str(), cur, text)) newValue = Json(text), changed = true;
            std::string dropped;
            if (AcceptAsset(dropped)) newValue = Json(dropped), changed = true;
            ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
            std::string popup = "##pick" + field;
            if (ImGui::Button(("..." + label).c_str(), ImVec2(button, 0))) ImGui::OpenPopup(popup.c_str());
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

    if (changed) {
        Json args = ObjectOf({{"id", Json(id)}, {"type", Json(type)}, {"values", ObjectOf({{field.c_str(), newValue}})}});
        if (!merge.empty()) args["merge"] = Json(merge);
        Call("component.set", args);
        Refresh(true);
    }
    return changed;
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
    const Json& comps = selected["components"];
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
    if (!ImGui::Begin(TrId("Inspector").c_str(), &showInspector)) {
        ImGui::End();
        return;
    }
    EntityId id = Primary();
    if (id != kNullEntity && selectedId != id) Refresh(true);
    if (id == kNullEntity || !selected.isObject()) {
        ImGui::TextDisabled("%s", Tr("Select an entity in the Hierarchy or the Scene view."));
        ImGui::End();
        return;
    }
    if (selection.size() > 1) ImGui::TextDisabled(Tr("%d entities selected - showing the first"), static_cast<int>(selection.size()));

    // Name + id
    std::string name;
    ImGui::SetNextItemWidth(-ImGui::CalcTextSize("#00000").x - ImGui::GetStyle().ItemSpacing.x);
    if (CommitText("##name", selected["name"].asString(""), name) && !name.empty()) {
        Call("entity.rename", ObjectOf({{"id", Json(id)}, {"name", Json(name)}}));
        Refresh(true);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("#%u", id);

    const Json& comps = selected["components"];
    std::string removeType;
    for (const auto& kv : comps.members()) {
        const std::string& type = kv.first;
        ImGui::PushID(type.c_str());
        auto typeIt = typeByName.find(type);
        const Json* typeInfo = typeIt == typeByName.end() ? nullptr : typeIt->second;
        float headerRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        bool open = ImGui::CollapsingHeader(type.c_str(), ImGuiTreeNodeFlags_AllowOverlap);
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
        ImGui::SameLine(headerRight - ImGui::GetFrameHeight());
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        if (ImGui::SmallButton("x")) removeType = type;
        ImGui::PopStyleColor();
        HelpTooltip(Format(Tr("Remove %s"), type.c_str()));
        if (open && typeInfo) {
            if (type == "Script") {
                std::string path = kv.second["path"].asString("");
                if (!path.empty() && ImGui::Button(Tr("Edit Script"))) OpenScript(path);
            }
            if (ImGui::BeginTable("##fields", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
                ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.38f);
                ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.62f);
                for (const auto& f : (*typeInfo)["schema"]["properties"].members()) {
                    ImGui::PushID(f.first.c_str());
                    FieldEditor(id, type, f.first, f.second, kv.second[f.first]);
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        } else if (open) {
            ImGui::TextDisabled("%s", Tr("Unknown component type"));
        }
        ImGui::PopID();
    }
    if (!removeType.empty()) {
        Call("component.remove", ObjectOf({{"id", Json(id)}, {"type", Json(removeType)}}));
        Refresh(true);
    }
    ImGui::Spacing();
    float w = ImGui::GetFontSize() * 12;
    ImGui::SetCursorPosX(std::max(0.0f, (ImGui::GetContentRegionAvail().x - w) * 0.5f));
    if (ImGui::Button(Tr("Add Component"), ImVec2(w, 0))) ImGui::OpenPopup("##addcomp");
    AddComponentPopup(id);
    ImGui::End();
}

// ----- Assets --------------------------------------------------------------------

void NativeEditor::Impl::AssetsPanel() {
    if (!ImGui::Begin(TrId("Assets").c_str(), &showAssets)) {
        ImGui::End();
        return;
    }
    if (ImGui::Button(Tr("Refresh"))) RefreshAssets(true);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    ImGui::InputTextWithHint("##filter", Tr("Search"), &assetFilter);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", Tr("Drop files on the window to import. Drag assets into the Scene, Hierarchy or Inspector."));
    ImGui::Separator();
    ImGui::BeginChild("##assets");
    std::map<std::string, std::vector<std::string>> byKind;
    for (const Json& a : assets.items()) {
        std::string p = a["path"].asString("");
        if (ContainsNoCase(p, assetFilter)) byKind[a["kind"].asString("other")].push_back(p);
    }
    const char* order[] = {"scene", "prefab", "script", "model", "texture", "material", "audio", "font"};
    std::vector<std::string> kinds(std::begin(order), std::end(order));
    for (const auto& kv : byKind) {
        if (std::find(kinds.begin(), kinds.end(), kv.first) == kinds.end()) kinds.push_back(kv.first);
    }
    for (const std::string& kind : kinds) {
        auto it = byKind.find(kind);
        if (it == byKind.end()) continue;
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        char header[64];
        std::snprintf(header, sizeof(header), "%s (%d)###%s", KindLabel(kind), static_cast<int>(it->second.size()), kind.c_str());
        if (!ImGui::TreeNodeEx(header, ImGuiTreeNodeFlags_SpanAvailWidth)) continue;
        for (const std::string& path : it->second) {
            ImGui::PushID(path.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, KindColor(kind));
            ImGui::Bullet();
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::Selectable(FileName(path).c_str(), selectedAsset == path, ImGuiSelectableFlags_AllowDoubleClick)) {
                selectedAsset = path;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (kind == "scene") RequestAction({PendingAction::Kind::LoadScene, path});
                    else if (kind == "script") OpenScript(path);
                    else if (kind == "prefab") Call("prefab.instantiate", ObjectOf({{"path", Json(path)}}));
                }
            }
            const char* hint = kind == "scene" ? Tr("Double-click to open") : kind == "script" ? Tr("Double-click to edit") : kind == "prefab" ? Tr("Double-click to instantiate") : nullptr;
            HelpTooltip(hint ? path + "\n" + hint : path);
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload(kAssetPayload, path.data(), path.size());
                ImGui::TextUnformatted(path.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginPopupContextItem("##assetctx")) {
                if (kind == "scene" && ImGui::MenuItem(Tr("Open"))) RequestAction({PendingAction::Kind::LoadScene, path});
                if (kind == "script" && ImGui::MenuItem(Tr("Edit"))) OpenScript(path);
                if (kind == "prefab" && ImGui::MenuItem(Tr("Instantiate"))) Call("prefab.instantiate", ObjectOf({{"path", Json(path)}}));
                if (ImGui::MenuItem(Tr("Copy Path"))) ImGui::SetClipboardText(path.c_str());
                if (ImGui::MenuItem(Tr("Info"))) {
                    Json r = Call("asset.info", ObjectOf({{"path", Json(path)}}));
                    if (Ok(r)) OE_LOG_INFO("asset", "%s: %s", path.c_str(), r["result"].dump(2).c_str());
                    showConsole = true;
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    if (assets.size() == 0) ImGui::TextDisabled("%s", Tr("The project has no assets yet."));
    ImGui::EndChild();
    ImGui::End();
}

// ----- Console -------------------------------------------------------------------

void NativeEditor::Impl::ConsolePanel() {
    if (!ImGui::Begin(TrId("Console").c_str(), &showConsole)) {
        ImGui::End();
        return;
    }
    static const char* levels[] = {"debug", "info", "warn", "error"};
    const char* labels[] = {Tr("Debug"), Tr("Info"), Tr("Warnings"), Tr("Errors")};
    for (int i = 0; i < 4; ++i) {
        ImGui::Checkbox(labels[i], &logShow[i]);
        ImGui::SameLine();
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    ImGui::InputTextWithHint("##logfilter", Tr("Filter"), &logFilter);
    ImGui::SameLine();
    if (ImGui::Button(Tr("Clear"))) {
        log.clear();
        warnCount = errorCount = 0;
        Call("script.errors", ObjectOf({{"clear", Json(true)}}), true);
    }
    ImGui::SameLine();
    ImGui::Checkbox(Tr("Auto-scroll"), &logAutoScroll);

    float inputHeight = ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("##log", ImVec2(0, -inputHeight), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushFont(monoFont, 0.0f);
    for (const LogLine& l : log) {
        int li = 1;
        for (int i = 0; i < 4; ++i) {
            if (l.level == levels[i]) li = i;
        }
        if (!logShow[li]) continue;
        if (!logFilter.empty() && !ContainsNoCase(l.message, logFilter) && !ContainsNoCase(l.category, logFilter)) continue;
        ImVec4 col = li == 3 ? ImVec4(1, 0.45f, 0.4f, 1) : li == 2 ? ImVec4(0.95f, 0.78f, 0.35f, 1) : li == 0 ? ImVec4(0.55f, 0.58f, 0.63f, 1) : ImVec4(0.85f, 0.87f, 0.9f, 1);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(("[" + l.category + "] " + l.message).c_str());
        ImGui::PopStyleColor();
        if (ImGui::BeginPopupContextItem(("##l" + std::to_string(l.seq)).c_str())) {
            if (ImGui::MenuItem(Tr("Copy"))) ImGui::SetClipboardText(l.message.c_str());
            ImGui::EndPopup();
        }
    }
    ImGui::PopFont();
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
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGuiInputTextFlags f = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackCompletion;
    if (ImGui::InputTextWithHint("##cmd", Tr("command.name {\"json\": \"args\"}   (Tab completes, Up/Down history)"), &consoleInput, f, callback, &ctx)) {
        RunConsoleCommand(consoleInput);
        consoleInput.clear();
        ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::End();
}

// ----- Scripts ---------------------------------------------------------------------

void NativeEditor::Impl::ScriptsPanel() {
    if (focusScript >= 0) ImGui::SetNextWindowFocus();  // bring the tab to front; Begin() skips hidden tabs
    if (!ImGui::Begin(TrId("Scripts").c_str(), &showScripts)) {
        ImGui::End();
        return;
    }
    if (scripts.empty()) {
        ImGui::TextDisabled("%s", Tr("Double-click a script in Assets (or use \"Edit Script\" on a Script component) to edit it here."));
        ImGui::TextDisabled("%s", Tr("Ctrl+S saves the script; it hot-reloads, also while the game runs."));
        ImGui::End();
        return;
    }
    if (ImGui::BeginTabBar("##scripts", ImGuiTabBarFlags_AutoSelectNewTabs | ImGuiTabBarFlags_FittingPolicyScroll)) {
        for (size_t i = 0; i < scripts.size(); ++i) {
            ScriptTab& t = scripts[i];
            if (!t.open) continue;
            bool modified = t.text != t.saved;
            ImGuiTabItemFlags flags = modified ? ImGuiTabItemFlags_UnsavedDocument : 0;
            if (focusScript == static_cast<int>(i)) {
                flags |= ImGuiTabItemFlags_SetSelected;
                focusScript = -1;
            }
            std::string label = FileName(t.path) + "###" + t.path;
            if (ImGui::BeginTabItem(label.c_str(), &t.open, flags)) {
                bool save = ImGui::Button(Tr("Save")) ||
                            (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S));
                ImGui::SameLine();
                if (ImGui::Button(Tr("Revert"))) t.text = t.saved;
                ImGui::SameLine();
                ImGui::TextDisabled("%s%s", t.path.c_str(), modified ? Tr("  (modified)") : "");
                // Errors of this file.
                Json errs = Call("script.errors", Json(), true)["result"];
                std::vector<std::string> mine;
                for (const Json& e : errs.items()) {
                    std::string msg = e["message"].asString("");
                    if (e["script"].asString("") == t.path || msg.find(t.path) != std::string::npos) mine.push_back(msg);
                }
                float errH = mine.empty() ? 0 : ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(std::min<size_t>(4, mine.size())) + 8;
                ImGui::PushFont(monoFont, 0.0f);
                ImGui::InputTextMultiline("##src", &t.text, ImVec2(-FLT_MIN, mine.empty() ? -FLT_MIN : -errH), ImGuiInputTextFlags_AllowTabInput);
                ImGui::PopFont();
                for (const std::string& e : mine) ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", e.c_str());
                if (save) {
                    if (Ok(Call("script.write", ObjectOf({{"path", Json(t.path)}, {"source", Json(t.text)}})))) {
                        t.saved = t.text;
                        Notify(Format(Tr("Saved %s"), t.path.c_str()));
                    }
                }
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    scripts.erase(std::remove_if(scripts.begin(), scripts.end(), [](const ScriptTab& t) { return !t.open && t.text == t.saved; }), scripts.end());
    for (ScriptTab& t : scripts) t.open = true;  // closing a modified tab keeps it (unsaved work)
    ImGui::End();
}

}  // namespace oe
