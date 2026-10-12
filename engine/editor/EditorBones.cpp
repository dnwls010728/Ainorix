// Bones in the editor: the skeleton of the selected model over the Scene view, name pickers for
// BoneIK / BoneAttachment / SpringBone / Animator fields and the bone list of a model with one-click
// attachments, IK goals and springs. Reads poses directly; every change goes through commands.
#include <algorithm>
#include <cctype>
#include <cmath>

#include "editor/EditorIcons.h"
#include "editor/EditorInternal.h"
#include "editor/EditorText.h"

#include "app/Engine.h"
#include "assets/Assets.h"
#include "core/Log.h"
#include "render/Mesh.h"
#include "scene/Components.h"
#include "scene/Pose.h"

namespace oe {

namespace {

const ImU32 kBoneColor = IM_COL32(120, 200, 255, 230);
const ImU32 kBoneHotColor = IM_COL32(255, 158, 26, 255);
const ImU32 kBoneShadow = IM_COL32(10, 12, 16, 170);

struct BoneRow {
    int node;
    int depth;
};

// Named nodes, parents before children; unnamed nodes are skipped and lend their depth to what is below them.
std::vector<BoneRow> BoneOrder(const Mesh& mesh) {
    std::vector<std::vector<int>> children(mesh.nodes.size());
    std::vector<int> roots;
    for (size_t i = 0; i < mesh.nodes.size(); ++i) {
        const int parent = mesh.nodes[i].parent;
        if (parent >= 0 && static_cast<size_t>(parent) < mesh.nodes.size()) children[static_cast<size_t>(parent)].push_back(static_cast<int>(i));
        else roots.push_back(static_cast<int>(i));
    }
    std::vector<BoneRow> rows;
    std::vector<BoneRow> stack;
    for (auto it = roots.rbegin(); it != roots.rend(); ++it) stack.push_back({*it, 0});
    while (!stack.empty() && rows.size() < mesh.nodes.size()) {
        const BoneRow row = stack.back();
        stack.pop_back();
        const bool named = !mesh.nodes[static_cast<size_t>(row.node)].name.empty();
        if (named) rows.push_back(row);
        const std::vector<int>& below = children[static_cast<size_t>(row.node)];
        for (auto it = below.rbegin(); it != below.rend(); ++it) stack.push_back({*it, row.depth + (named ? 1 : 0)});
    }
    return rows;
}

bool ContainsNoCase(const std::string& text, const std::string& part) {
    if (part.empty()) return true;
    auto it = std::search(text.begin(), text.end(), part.begin(), part.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
    return it != text.end();
}

// Same mapping as the Scene view image; false when the point is behind the camera.
bool ProjectPoint(const Mat4& viewProj, const Vec3& p, ImVec2 pos, ImVec2 size, ImVec2& out) {
    const Vec4 c = viewProj * Vec4(p, 1.0f);
    if (c.w <= 1e-4f) return false;
    out = ImVec2(pos.x + (c.x / c.w * 0.5f + 0.5f) * size.x, pos.y + (0.5f - c.y / c.w * 0.5f) * size.y);
    return true;
}

Vec3 PointOf(const Mat4& m) { return m.TransformPoint(Vec3(0, 0, 0)); }

}  // namespace

std::shared_ptr<const Mesh> NativeEditor::Impl::BoneModel(EntityId id, EntityId* model) {
    Scene& scene = engine.GetScene();
    const EntityRecord* record = scene.Record(id);
    if (!record) return nullptr;
    const bool child = scene.Get<BoneIK>(id) || scene.Get<BoneAttachment>(id) || scene.Get<SpringBone>(id);
    const EntityId owner = child ? record->parent : id;
    const MeshRenderer* renderer = scene.Get<MeshRenderer>(owner);
    if (!renderer || renderer->mesh.empty()) return nullptr;
    std::shared_ptr<const Mesh> mesh = engine.Assets().GetMesh(renderer->mesh);
    if (!mesh || mesh->nodes.size() < 2) return nullptr;
    if (model) *model = owner;
    return mesh;
}

bool NativeEditor::Impl::NameInModel(EntityId id, bool bone, const std::string& name) {
    EntityId model = kNullEntity;
    std::shared_ptr<const Mesh> mesh = BoneModel(id, &model);
    if (!mesh) return false;
    if (bone) return FindModelNode(*mesh, name) >= 0;
    return std::any_of(mesh->clips.begin(), mesh->clips.end(), [&](const AnimationClip& clip) { return clip.name == name; });
}

bool NativeEditor::Impl::NamePicker(const char* popupId, EntityId id, bool bone, const std::string& current, std::string& out) {
    bool picked = false;
    ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 16, 0), ImVec2(ImGui::GetFontSize() * 28, ImGui::GetFontSize() * 24));
    if (!ImGui::BeginPopup(popupId)) return false;
    EntityId model = kNullEntity;
    std::shared_ptr<const Mesh> mesh = BoneModel(id, &model);
    if (!mesh) {
        ImGui::TextDisabled("%s", bone ? Tr("The parent entity needs a MeshRenderer with a rigged model") : Tr("This entity needs a MeshRenderer with a model"));
        ImGui::EndPopup();
        return false;
    }
    if (ImGui::IsWindowAppearing()) {
        namePickFilter.clear();
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##find", WithIcon(kIconSearch, Tr("Search")).c_str(), &namePickFilter);
    auto item = [&](const std::string& value, const std::string& label, float indent) {
        if (indent > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
        if (ImGui::Selectable(label.c_str(), value == current)) {
            out = value;
            picked = true;
        }
    };
    if (namePickFilter.empty()) item("", Tr("(none)"), 0);
    if (bone) {
        for (const BoneRow& row : BoneOrder(*mesh)) {
            const std::string& name = mesh->nodes[static_cast<size_t>(row.node)].name;
            if (!ContainsNoCase(name, namePickFilter)) continue;
            ImGui::PushID(row.node);
            item(name, name, namePickFilter.empty() ? static_cast<float>(row.depth) * ImGui::GetFontSize() * 0.7f : 0.0f);
            if (ImGui::IsItemHovered()) boneHoverModel = model, boneHover = name, boneHoverFrame = ImGui::GetFrameCount();
            ImGui::PopID();
        }
    } else {
        if (mesh->clips.empty()) ImGui::TextDisabled("%s", Tr("This model has no clips"));
        for (size_t i = 0; i < mesh->clips.size(); ++i) {
            const AnimationClip& clip = mesh->clips[i];
            if (!ContainsNoCase(clip.name, namePickFilter)) continue;
            ImGui::PushID(static_cast<int>(i));
            item(clip.name, Format("%s   %.2f s", clip.name.c_str(), static_cast<double>(clip.duration)), 0);
            ImGui::PopID();
        }
    }
    ImGui::EndPopup();
    return picked;
}

void NativeEditor::Impl::AttachToBone(EntityId model, const std::string& bone, EntityId existing) {
    Scene& scene = engine.GetScene();
    EntityId id = existing;
    if (existing == kNullEntity) {
        Json r = Call("entity.create", ObjectOf({{"name", Json(UniqueName(bone))}, {"parent", Json(model)},
                                                 {"components", ObjectOf({{"BoneAttachment", ObjectOf({{"bone", Json(bone)}})}})}}));
        if (!Ok(r)) return;
        id = static_cast<EntityId>(r["result"]["id"].asNumber(0));
    } else {
        if (existing == model) return;
        const EntityRecord* record = scene.Record(existing);
        if (!record) return;
        if (record->parent != model && !Ok(Call("entity.set_parent", ObjectOf({{"id", Json(existing)}, {"parent", Json(model)}})))) return;
        const char* command = scene.Get<BoneAttachment>(existing) ? "component.set" : "component.add";
        if (!Ok(Call(command, ObjectOf({{"id", Json(existing)}, {"type", Json("BoneAttachment")}, {"values", ObjectOf({{"bone", Json(bone)}})}})))) return;
    }
    const EntityRecord* record = scene.Record(id);
    Notify(Format(Tr("%s now follows the bone %s"), record ? record->name.c_str() : "", bone.c_str()));
    SelectOnly(id);
    scrollToRow = id;
    Refresh(true);
}

void NativeEditor::Impl::AddIkGoal(EntityId model, const std::string& bone) {
    Scene& scene = engine.GetScene();
    EntityId owner = kNullEntity;
    std::shared_ptr<const Mesh> mesh = BoneModel(model, &owner);
    const int node = mesh ? FindModelNode(*mesh, bone) : -1;
    if (node < 0) return;
    int ancestors = 0;
    for (int up = mesh->nodes[static_cast<size_t>(node)].parent; up >= 0 && ancestors < 2; up = mesh->nodes[static_cast<size_t>(up)].parent) ++ancestors;
    if (ancestors == 0) {
        Notify(Format(Tr("%s has no parent bone to turn"), bone.c_str()), true);
        return;
    }
    // The goal starts on the bone, so nothing moves until it is dragged.
    const Vec3 start = PointOf(EvaluateEntityPose(scene, model, *mesh)[static_cast<size_t>(node)]);
    Json r = Call("entity.create", ObjectOf({{"name", Json(UniqueName(bone + " Goal"))}, {"parent", Json(model)},
                                             {"components", ObjectOf({{"Transform", ObjectOf({{"position", Vec3Json(start)}})},
                                                                      {"BoneIK", ObjectOf({{"bone", Json(bone)}, {"chain", Json(ancestors)}})}})}}));
    if (!Ok(r)) return;
    const EntityId id = static_cast<EntityId>(r["result"]["id"].asNumber(0));
    Notify(Format(Tr("IK goal for %s: move it to pose the limb"), bone.c_str()));
    SelectOnly(id);
    scrollToRow = id;
    Refresh(true);
}

void NativeEditor::Impl::AddSpringBone(EntityId model, const std::string& bone) {
    EntityId owner = kNullEntity;
    std::shared_ptr<const Mesh> mesh = BoneModel(model, &owner);
    if (!mesh || FindModelNode(*mesh, bone) < 0) return;
    if (SpringBoneCount(*mesh, bone) == 0) {
        Notify(Format(Tr("%s has no bone below it to swing"), bone.c_str()), true);
        return;
    }
    Json r = Call("entity.create", ObjectOf({{"name", Json(UniqueName(bone + " Spring"))}, {"parent", Json(model)},
                                             {"components", ObjectOf({{"SpringBone", ObjectOf({{"bone", Json(bone)}})}})}}));
    if (!Ok(r)) return;
    const EntityId id = static_cast<EntityId>(r["result"]["id"].asNumber(0));
    Notify(Format(Tr("Spring on %s: it swings while the game plays"), bone.c_str()));
    SelectOnly(id);
    scrollToRow = id;
    Refresh(true);
}

void NativeEditor::Impl::BonesSection(EntityId id) {
    EntityId model = kNullEntity;
    std::shared_ptr<const Mesh> mesh = BoneModel(id, &model);
    if (!mesh || model != id) return;
    const std::vector<BoneRow> bones = BoneOrder(*mesh);
    if (bones.empty()) return;
    const std::string title = Format("%s (%d)###bones", Tr("Bones"), static_cast<int>(bones.size()));
    if (!ImGui::TreeNodeEx(title.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##bonefilter", WithIcon(kIconSearch, Tr("Search bones")).c_str(), &boneFilter);
    const float rowHeight = ImGui::GetFrameHeightWithSpacing();
    const float listHeight = rowHeight * static_cast<float>(std::min<size_t>(bones.size(), 12)) + ImGui::GetStyle().WindowPadding.y;
    std::string attach, goal, spring;
    EntityId dropped = kNullEntity;
    if (ImGui::BeginChild("##bonelist", ImVec2(0, listHeight), ImGuiChildFlags_Borders)) {
        const float button = ImGui::GetFrameHeight();
        const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
        for (const BoneRow& row : bones) {
            const std::string& name = mesh->nodes[static_cast<size_t>(row.node)].name;
            if (!ContainsNoCase(name, boneFilter)) continue;
            ImGui::PushID(row.node);
            if (boneFilter.empty()) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + static_cast<float>(row.depth) * ImGui::GetFontSize() * 0.7f);
            const float nameWidth = std::max(ImGui::GetFontSize() * 3, ImGui::GetContentRegionAvail().x - (button + spacing) * 2);
            ImGui::AlignTextToFramePadding();
            if (ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_AllowOverlap, ImVec2(nameWidth, button))) {
                ImGui::SetClipboardText(name.c_str());
                Notify(Format(Tr("Copied the bone name %s"), name.c_str()));
            }
            if (ImGui::IsItemHovered()) boneHoverModel = model, boneHover = name, boneHoverFrame = ImGui::GetFrameCount();
            ImGui::SetItemTooltip("%s", Tr("Click copies the name. Drop an entity here to hold it on this bone; right-click for more."));
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kEntityPayload)) attach = name, dropped = *static_cast<const EntityId*>(p->Data);
                ImGui::EndDragDropTarget();
            }
            if (ImGui::BeginPopupContextItem("##bonectx")) {
                if (ImGui::MenuItem(WithIcon(kIconPaperclip, Tr("Attach New Entity")).c_str())) attach = name;
                if (ImGui::MenuItem(WithIcon(kIconMapPin, Tr("Add IK Goal")).c_str())) goal = name;
                if (ImGui::MenuItem(WithIcon(kIconAtom, Tr("Add Spring")).c_str())) spring = name;
                if (ImGui::MenuItem(Tr("Copy Name"))) ImGui::SetClipboardText(name.c_str());
                ImGui::EndPopup();
            }
            ImGui::SameLine(0, spacing);
            if (ImGui::Button(kIconPaperclip, ImVec2(button, button))) attach = name;
            ImGui::SetItemTooltip("%s", Tr("Attach a new entity to this bone (weapon, hat, effect)"));
            ImGui::SameLine(0, spacing);
            if (ImGui::Button(kIconMapPin, ImVec2(button, button))) goal = name;
            ImGui::SetItemTooltip("%s", Tr("Add an IK goal: an entity this bone reaches for"));
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::TreePop();
    // Applied after the list: they replace the selection the list is drawn for.
    if (!attach.empty()) AttachToBone(model, attach, dropped);
    else if (!goal.empty()) AddIkGoal(model, goal);
    else if (!spring.empty()) AddSpringBone(model, spring);
}

void NativeEditor::Impl::BonesOverlay(ImDrawList* dl) {
    if (!showBones) return;
    const EntityId selectedEntity = Primary();
    EntityId model = kNullEntity;
    std::shared_ptr<const Mesh> mesh = BoneModel(selectedEntity, &model);
    if (!mesh) return;
    Scene& scene = engine.GetScene();
    const std::vector<Mat4> pose = EvaluateEntityPose(scene, model, *mesh);
    const Mat4 world = scene.WorldMatrix(model);
    const Mat4 viewProj = sceneProjMat * sceneViewMat;
    const size_t count = mesh->nodes.size();

    // Skinned models show their joints; rigid ones every named node.
    std::vector<char> shown(count, 0), hot(count, 0);
    if (mesh->joints.empty()) {
        for (size_t i = 0; i < count; ++i) shown[i] = !mesh->nodes[i].name.empty();
    } else {
        for (const ModelJoint& joint : mesh->joints) shown[static_cast<size_t>(joint.node)] = 1;
    }
    auto mark = [&](const std::string& name, int chain) {
        int node = FindModelNode(*mesh, name);
        for (int i = 0; node >= 0 && i <= chain; ++i, node = mesh->nodes[static_cast<size_t>(node)].parent) hot[static_cast<size_t>(node)] = 1;
    };
    const BoneIK* ik = scene.Get<BoneIK>(selectedEntity);
    if (ik) mark(ik->bone, std::max(1, std::min(ik->chain, 8)));
    if (const BoneAttachment* attachment = scene.Get<BoneAttachment>(selectedEntity)) mark(attachment->bone, 0);
    const SpringBone* spring = scene.Get<SpringBone>(selectedEntity);
    if (const int root = spring ? FindModelNode(*mesh, spring->bone) : -1; root >= 0) {  // the whole branch that swings
        for (size_t i = 0; i < count; ++i) {
            int node = static_cast<int>(i);
            for (size_t steps = 0; node >= 0 && node != root && steps <= count; ++steps) node = mesh->nodes[static_cast<size_t>(node)].parent;
            if (node == root) hot[i] = 1;
        }
    }
    if (boneHoverModel == model && boneHoverFrame >= ImGui::GetFrameCount() - 1) mark(boneHover, 0);

    std::vector<ImVec2> points(count);
    std::vector<char> visible(count, 0);
    for (size_t i = 0; i < count; ++i)
        visible[i] = shown[i] && ProjectPoint(viewProj, PointOf(world * pose[i]), sceneImagePos, sceneImageSize, points[i]);

    dl->PushClipRect(sceneImagePos, sceneImagePos + sceneImageSize, true);
    for (size_t i = 0; i < count; ++i) {
        if (!visible[i]) continue;
        int parent = mesh->nodes[i].parent;
        while (parent >= 0 && !shown[static_cast<size_t>(parent)]) parent = mesh->nodes[static_cast<size_t>(parent)].parent;
        if (parent < 0 || !visible[static_cast<size_t>(parent)]) continue;
        const bool lit = hot[i] && hot[static_cast<size_t>(parent)];
        dl->AddLine(points[static_cast<size_t>(parent)], points[i], kBoneShadow, lit ? 5.0f : 3.5f);
        dl->AddLine(points[static_cast<size_t>(parent)], points[i], lit ? kBoneHotColor : kBoneColor, lit ? 2.5f : 1.5f);
    }
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    int hovered = -1;
    float nearest = 9.0f * 9.0f;
    for (size_t i = 0; i < count; ++i) {
        if (!visible[i]) continue;
        const float radius = hot[i] ? 4.5f : 3.0f;
        dl->AddCircleFilled(points[i], radius + 1.5f, kBoneShadow);
        dl->AddCircleFilled(points[i], radius, hot[i] ? kBoneHotColor : kBoneColor);
        const float dx = mouse.x - points[i].x, dy = mouse.y - points[i].y;
        if (sceneHovered && dx * dx + dy * dy < nearest) nearest = dx * dx + dy * dy, hovered = static_cast<int>(i);
    }
    // A selected goal shows what is left between the bone and it.
    if (ik) {
        const int node = FindModelNode(*mesh, ik->bone);
        ImVec2 target;
        if (node >= 0 && visible[static_cast<size_t>(node)] &&
            ProjectPoint(viewProj, PointOf(scene.WorldMatrix(selectedEntity)), sceneImagePos, sceneImageSize, target))
            dl->AddLine(points[static_cast<size_t>(node)], target, IM_COL32(255, 158, 26, 150), 1.0f);
    }
    auto label = [&](size_t node, ImU32 color) {
        const char* text = mesh->nodes[node].name.c_str();
        const ImVec2 at(points[node].x + 9, points[node].y - ImGui::GetFontSize() * 0.5f), size = ImGui::CalcTextSize(text);
        dl->AddRectFilled(at - ImVec2(4, 3), at + size + ImVec2(4, 3), IM_COL32(18, 21, 27, 240), 3.0f);
        dl->AddText(at, color, text);
    };
    // Names: the bone under the mouse, else the one the selection or the inspector points at.
    if (hovered >= 0) {
        label(static_cast<size_t>(hovered), IM_COL32(238, 240, 245, 255));
    } else {
        const std::string& named = boneHoverModel == model && boneHoverFrame >= ImGui::GetFrameCount() - 1 ? boneHover
                                 : ik ? ik->bone : spring ? spring->bone : scene.Get<BoneAttachment>(selectedEntity) ? scene.Get<BoneAttachment>(selectedEntity)->bone : std::string();
        const int node = FindModelNode(*mesh, named);
        if (node >= 0 && visible[static_cast<size_t>(node)]) label(static_cast<size_t>(node), kBoneHotColor);
    }
    dl->PopClipRect();
}

}  // namespace oe
