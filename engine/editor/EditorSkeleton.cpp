// Skeleton Editor window: one 2D skeleton (*.skel2d.json, docs/SKELETON2D.md). Rig shows the bones (drag a joint to
// move it, a bone to turn it) and the slots' draw order; Weights paints how much each mesh vertex follows a bone;
// Animate shows a clip on a timeline, where turning a bone keys its rotation at the current time. Every change is a
// skeleton2d.* command that writes the file; the window keeps its own undo of the file (Ctrl+Z / Ctrl+Y while it is
// focused). It follows the selection: picking an entity with a Skeleton2D shows its asset.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>

#include "editor/EditorIcons.h"
#include "editor/EditorInternal.h"
#include "editor/EditorText.h"
#include "imgui_internal.h"

#include "app/Engine.h"
#include "assets/Assets.h"
#include "core/FileSystem.h"
#include "core/Log.h"
#include "render/GpuRenderer.h"
#include "render/Skeleton2DMesh.h"
#include "scene/Components.h"
#include "scene/Skeleton2D.h"

namespace oe {
namespace {

// Bones are drawn as tapered shapes with a dark edge so they read over any picture.
constexpr ImU32 kBoneFill = IM_COL32(255, 200, 70, 140);
constexpr ImU32 kBoneHot = IM_COL32(255, 236, 150, 240);
constexpr ImU32 kBoneSelected = IM_COL32(86, 162, 255, 245);
constexpr ImU32 kBoneEdge = IM_COL32(18, 14, 6, 235);
constexpr ImU32 kMeshLine = IM_COL32(255, 255, 255, 60);
constexpr ImU32 kPlayhead = IM_COL32(255, 96, 86, 255);
// Key colors per bone timeline, in the order of kTimelines.
constexpr ImU32 kKeyColors[3] = {IM_COL32(120, 214, 120, 255), IM_COL32(96, 168, 255, 255), IM_COL32(255, 168, 80, 255)};
const char* const kTimelines[3] = {"rotate", "translate", "scale"};
const ImVec4 kMuted(0.62f, 0.64f, 0.68f, 1.0f);
const ImVec4 kError(0.96f, 0.45f, 0.40f, 1.0f);
const ImVec4 kGood(0.45f, 0.85f, 0.55f, 1.0f);
const char* const kRecipes[] = {"idle", "walk", "run", "jump", "attack", "hit", "death"};
constexpr float kKeyEpsilon = 1.0f / 120.0f;  // keys within half a frame are the same key

bool IsSkeletonPath(const std::string& p) { return p.size() > 12 && p.compare(p.size() - 12, 12, ".skel2d.json") == 0; }

// Puts a key [time, values..., curve] into a timeline: replaces the values of a key within half a frame of `time`
// (keeping its curve), else inserts one in time order.
void SetKey(Json& keys, float time, const std::vector<float>& values) {
    if (!keys.isArray()) keys = Json::MakeArray();
    std::vector<Json> list = keys.items();
    size_t at = list.size();
    for (size_t i = 0; i < list.size(); ++i) {
        const float t = list[i][0].asFloat();
        if (std::fabs(t - time) < kKeyEpsilon) {
            Json key = Json::MakeArray();
            key.push(t);
            for (float v : values) key.push(std::round(v * 1000.0f) / 1000.0f);
            const Json& last = list[i][static_cast<int>(list[i].size()) - 1];
            if (last.isString()) key.push(last);
            list[i] = key;
            keys = Json(list);
            return;
        }
        if (t > time && at == list.size()) at = i;
    }
    Json key = Json::MakeArray();
    key.push(std::round(time * 1000.0f) / 1000.0f);
    for (float v : values) key.push(std::round(v * 1000.0f) / 1000.0f);
    list.insert(list.begin() + static_cast<std::ptrdiff_t>(at), key);
    keys = Json(list);
}

float DistanceToSegment(ImVec2 p, ImVec2 a, ImVec2 b) {
    const float dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
    const float t = len2 > 0 ? Clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / len2, 0, 1) : 0;
    const float ex = a.x + dx * t - p.x, ey = a.y + dy * t - p.y;
    return std::sqrt(ex * ex + ey * ey);
}

// A label on a dark rounded plate: readable over any picture.
void Pill(ImDrawList* draw, ImVec2 at, ImU32 color, const char* text) {
    const ImVec2 size = ImGui::CalcTextSize(text), pad(5, 2);
    draw->AddRectFilled(at - pad, at + size + pad, IM_COL32(14, 16, 20, 220), 4);
    draw->AddText(at, color, text);
}

// How much a vertex follows a bone (0..1) as a heat color: blue, green, red.
ImU32 Heat(float w, int alpha) {
    w = Clamp(w, 0, 1);
    const float a[3] = {40, 90, 255}, b[3] = {70, 220, 90}, c[3] = {255, 60, 40};
    float rgb[3];
    for (int i = 0; i < 3; ++i) rgb[i] = w < 0.5f ? a[i] + (b[i] - a[i]) * w * 2 : b[i] + (c[i] - b[i]) * (w - 0.5f) * 2;
    return IM_COL32(static_cast<int>(rgb[0]), static_cast<int>(rgb[1]), static_cast<int>(rgb[2]), alpha);
}

// A bone from its joint to its tip: a slim diamond, widest near the joint, with the joint as a dot.
void BoneShape(ImDrawList* draw, ImVec2 joint, ImVec2 tip, ImU32 fill, float radius) {
    const ImVec2 d = tip - joint;
    const float length = std::sqrt(ImLengthSqr(d));
    if (length > radius * 1.5f) {
        const ImVec2 u = d / length, n(-u.y, u.x);
        const float width = std::min(length * 0.09f, radius * 0.9f);
        const ImVec2 at = joint + u * std::min(length * 0.22f, width * 2.2f);
        const ImVec2 points[4] = {joint, at + n * width, tip, at - n * width};
        draw->AddConvexPolyFilled(points, 4, fill);
        draw->AddPolyline(points, 4, kBoneEdge, 1.5f, ImDrawFlags_Closed);
    }
    draw->AddCircleFilled(joint, radius + 1.5f, kBoneEdge);
    draw->AddCircleFilled(joint, radius, fill | IM_COL32_A_MASK);
}

void Diamond(ImDrawList* draw, ImVec2 c, float r, ImU32 fill, bool ring) {
    const ImVec2 points[4] = {ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y)};
    draw->AddConvexPolyFilled(points, 4, fill);
    draw->AddPolyline(points, 4, ring ? IM_COL32_WHITE : IM_COL32(0, 0, 0, 200), ring ? 2.0f : 1.0f, ImDrawFlags_Closed);
}

bool Contains(const std::string& text, const char* filter) {
    if (!*filter) return true;
    std::string a = text, b = filter;
    for (char& c : a) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : b) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return a.find(b) != std::string::npos;
}

// A toolbar button that shows whether a view option is on.
bool ToggleButton(const char* label, bool on, const char* tip) {
    ImGui::PushStyleColor(ImGuiCol_Button, on ? ImVec4(0.36f, 0.58f, 1.0f, 0.45f) : ImVec4(1, 1, 1, 0.04f));
    const bool clicked = ImGui::Button(label);
    ImGui::PopStyleColor();
    HelpTooltip(tip);
    return clicked;
}

}  // namespace

void NativeEditor::Impl::SkeletonRect(const std::string& name) {
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    skeletonRects[name] = {a.x, a.y, b.x - a.x, b.y - a.y};
}

// ----- Asset state -----------------------------------------------------------------------------

void NativeEditor::Impl::SkeletonOpen(const std::string& path) {
    if (path != skeletonPath) {
        skeletonPath = path;
        skeletonUndo.clear();
        skeletonRedo.clear();
        skeletonBone = skeletonSlot = -1;
        skeletonClip.clear();
        skeletonTime = 0;
        skeletonPlaying = false;
        skeletonFit = true;
        skeletonProblems = Json();
    }
    if (!skeletonPath.empty()) SkeletonLoad();
}

bool NativeEditor::Impl::SkeletonLoad() {
    skeletonError.clear();
    try {
        skeletonLoadedTime = FileModifiedTime(engine.ResolvePath(skeletonPath));
    } catch (const std::exception&) {
        skeletonLoadedTime = 0;
    }
    engine.Assets().Forget(skeletonPath);
    const Json r = Call("skeleton2d.get", ObjectOf({{"path", skeletonPath}}), true);
    if (!Ok(r)) {
        skeletonRaw = Json();
        skeletonError = r["error"]["message"].asString();
        return false;
    }
    skeletonRaw = r["result"]["values"];
    const int bones = static_cast<int>(skeletonRaw["bones"].size());
    if (skeletonBone >= bones) skeletonBone = -1;
    if (skeletonSlot >= static_cast<int>(skeletonRaw["slots"].size())) skeletonSlot = -1;
    const Json& clips = skeletonRaw["clips"];
    if (!clips.isObject() || !clips.has(skeletonClip)) skeletonClip = clips.isObject() && clips.size() ? clips.members().begin()->first : std::string();
    return true;
}

bool NativeEditor::Impl::SkeletonSet(const Json& values, const std::string& clip, int merge) {
    const Json before = skeletonRaw;
    Json args = ObjectOf({{"path", skeletonPath}, {"values", values}});
    if (!clip.empty()) args["clip"] = clip;
    if (!Ok(Call("skeleton2d.set", args))) return false;
    if (merge < 0 || merge != skeletonMergeSerial) {
        skeletonUndo.push_back(before);
        if (skeletonUndo.size() > 100) skeletonUndo.erase(skeletonUndo.begin());
        skeletonRedo.clear();
    }
    skeletonMergeSerial = merge;
    SkeletonLoad();
    return true;
}

void NativeEditor::Impl::SkeletonHistory(bool redo) {
    std::vector<Json>& from = redo ? skeletonRedo : skeletonUndo;
    std::vector<Json>& to = redo ? skeletonUndo : skeletonRedo;
    if (from.empty() || skeletonPath.empty()) return;
    const Json values = from.back();
    if (!Ok(Call("skeleton2d.create", ObjectOf({{"path", skeletonPath}, {"values", values}, {"overwrite", true}})))) return;
    to.push_back(skeletonRaw);
    from.pop_back();
    skeletonMergeSerial = -1;
    SkeletonLoad();
}

// ----- Window ----------------------------------------------------------------------------------

void NativeEditor::Impl::SkeletonEditorPanel() {
    skeletonRects.clear();
    // Asked to come forward: for a few frames, since a window docked on its first frames can lose its tab again.
    if (focusSkeletonEditor) skeletonFocusFrames = 3, focusSkeletonEditor = false;
    if (skeletonFocusFrames > 0) --skeletonFocusFrames, ImGui::SetNextWindowFocus();
    if (!ImGui::Begin(WithIcon(kIconPersonStanding, TrId("Skeleton Editor")).c_str(), &showSkeletonEditor)) {
        skeletonEditorFocused = false;
        ImGui::End();
        return;
    }
    skeletonEditorFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    // Follow the selection: an entity with a Skeleton2D shows its asset.
    const EntityId picked = Primary();
    if (picked != skeletonFollowed) {
        skeletonFollowed = picked;
        if (const Skeleton2D* sk = engine.GetScene().Get<Skeleton2D>(picked); sk && IsSkeletonPath(sk->asset)) {
            SkeletonOpen(sk->asset);
            if (!sk->clip.empty() && skeletonRaw["clips"].has(sk->clip)) skeletonClip = sk->clip;
        }
    }
    if (!skeletonPath.empty()) {  // someone else (an agent, the build) changed the file: read it again
        int64_t modified = 0;
        try {
            modified = FileModifiedTime(engine.ResolvePath(skeletonPath));
        } catch (const std::exception&) {
        }
        if (modified != skeletonLoadedTime) SkeletonLoad();
    }
    // Reads of the file go through a const reference: Json's non-const [] would add empty fields that a later
    // write would save.
    const Json& raw = skeletonRaw;

    // ----- Toolbar: which asset, the three tabs, undo / redo, check, what the canvas shows.
    const float font = ImGui::GetFontSize();
    {
        const std::string name = skeletonPath.empty() ? std::string(Tr("No skeleton")) : skeletonPath.substr(skeletonPath.find_last_of('/') + 1);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1, 1, 1, 0.06f));
        if (ImGui::Button((WithIcon(kIconPersonStanding, name) + "  " + kIconChevronDown).c_str())) ImGui::OpenPopup("##skeletons");
        ImGui::PopStyleColor();
        SkeletonRect("asset");
        HelpTooltip(skeletonPath);
        if (ImGui::BeginPopup("##skeletons")) {
            std::vector<std::string> found;
            for (const std::string& file : ListFiles(engine.ProjectDir(), ".json", true)) {
                const std::string relative = RelativePath(file, engine.ProjectDir());
                if (IsSkeletonPath(relative) && relative.rfind(".oe/", 0) != 0) found.push_back(relative);
            }
            std::sort(found.begin(), found.end());
            if (found.empty()) ImGui::TextDisabled("%s", Tr("No skeletons in the project yet."));
            for (const std::string& path : found)
                if (ImGui::Selectable(path.c_str(), path == skeletonPath)) SkeletonOpen(path);
            ImGui::EndPopup();
        }
        if (!skeletonPath.empty() && raw.isObject()) {
            ImGui::SameLine(0, font);
            const std::string tabs[3] = {WithIcon(kIconBone, Tr("Rig")), WithIcon(kIconBrush, Tr("Weights")), WithIcon(kIconFilm, Tr("Animate"))};
            const int tab = Segmented("##tabs", {tabs[0].c_str(), tabs[1].c_str(), tabs[2].c_str()}, skeletonTab, &skeletonRects, "tab");
            if (tab >= 0) skeletonTab = tab, skeletonDrag = 0;
            ImGui::SameLine(0, font);
            ImGui::BeginDisabled(skeletonUndo.empty());
            if (ImGui::Button(kIconUndo)) SkeletonHistory(false);
            ImGui::EndDisabled();
            SkeletonRect("undo");
            HelpTooltip(Tr("Undo (Ctrl+Z)"));
            ImGui::SameLine();
            ImGui::BeginDisabled(skeletonRedo.empty());
            if (ImGui::Button(kIconRedo)) SkeletonHistory(true);
            ImGui::EndDisabled();
            SkeletonRect("redo");
            HelpTooltip(Tr("Redo (Ctrl+Y)"));
            ImGui::SameLine(0, font);
            if (ImGui::Button(WithIcon(kIconCircleCheck, Tr("Check")).c_str())) {
                Json args = ObjectOf({{"path", skeletonPath}, {"render", false}});
                if (skeletonTab == 2 && !skeletonClip.empty()) args["clip"] = skeletonClip;
                const Json r = Call("skeleton2d.analyze", args);
                skeletonProblems = Ok(r) ? r["result"] : Json();
            }
            SkeletonRect("check");
            HelpTooltip(Tr("Pose it (the clip shown, or every joint bent both ways) and list what goes wrong: hidden picture showing, joints opening, meshes folding."));
            // What the canvas shows.
            ImGui::SameLine(0, font);
            if (ToggleButton(kIconImage, skeletonShowImages, Tr("Show the pictures"))) skeletonShowImages = !skeletonShowImages;
            ImGui::SameLine(0, 2);
            if (ToggleButton(kIconBone, skeletonShowBones, Tr("Show the bones"))) skeletonShowBones = !skeletonShowBones;
            ImGui::SameLine(0, 2);
            if (ToggleButton(kIconGrid3x3, skeletonShowMesh, Tr("Show the mesh triangles"))) skeletonShowMesh = !skeletonShowMesh;
            ImGui::SameLine(0, 2);
            if (ToggleButton(kIconType, skeletonShowNames, Tr("Show every bone's name"))) skeletonShowNames = !skeletonShowNames;
            ImGui::SameLine(0, 2);
            if (ToggleButton(kIconScan, false, Tr("Fit the figure to the view (F)"))) skeletonFit = true;
            SkeletonRect("fit");
        }
    }
    const bool keys = skeletonEditorFocused && !ImGui::GetIO().WantTextInput;
    if (keys) {
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) SkeletonHistory(false);
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z)) SkeletonHistory(true);
    }
    ImGui::Separator();
    std::string error;
    const auto def = skeletonPath.empty() ? nullptr : engine.Assets().GetSkeleton2D(skeletonPath, &error);
    if (!def || !raw.isObject()) {
        ImGui::PushTextWrapPos(0);
        if (skeletonPath.empty()) ImGui::TextColored(kMuted, "%s", Tr("Select an entity with a Skeleton2D, or pick a skeleton above. skeleton2d.build makes one from a picture."));
        else ImGui::TextColored(kError, "%s", Format(Tr("Cannot read %s: %s"), skeletonPath.c_str(), (skeletonError.empty() ? error : skeletonError).c_str()).c_str());
        ImGui::PopTextWrapPos();
        ImGui::End();
        return;
    }
    const int boneCount = static_cast<int>(def->bones.size());
    auto boneName = [&](int b) -> const std::string& { return def->bones[static_cast<size_t>(b)].name; };
    const Skeleton2DClip* clip = skeletonTab == 2 ? def->FindClip(skeletonClip) : nullptr;
    const float duration = clip ? std::max(0.01f, clip->duration) : 1.0f;
    if (clip && skeletonPlaying) {
        skeletonTime += ImGui::GetIO().DeltaTime;
        skeletonTime = clip->loop ? std::fmod(skeletonTime, duration) : std::min(skeletonTime, duration);
    }
    Skeleton2DPose setup = Skeleton2DSetupPose(*def);
    Skeleton2DWorld(*def, setup);
    const Skeleton2DPose pose = clip ? Skeleton2DSample(*def, clip, skeletonTime) : Skeleton2DSample(*def, nullptr, 0);
    const Json& clipBones = raw["clips"][skeletonClip]["bones"];

    // The mesh slots (Weights): pick the first when none is picked yet.
    std::vector<int> meshSlots;
    for (size_t s = 0; s < def->slots.size(); ++s) {
        const Skeleton2DAttachment* a = def->FindAttachment("", static_cast<int>(s), pose.attachments[s]);
        if (a && a->IsMesh()) meshSlots.push_back(static_cast<int>(s));
    }
    if (skeletonTab == 1 && !meshSlots.empty() && std::find(meshSlots.begin(), meshSlots.end(), skeletonSlot) == meshSlots.end()) {
        skeletonSlot = meshSlots.front();
        if (skeletonBone < 0) skeletonBone = def->slots[static_cast<size_t>(skeletonSlot)].bone;
    }
    const Skeleton2DAttachment* paintMesh = skeletonTab == 1 && skeletonSlot >= 0
        ? def->FindAttachment("", skeletonSlot, pose.attachments[static_cast<size_t>(skeletonSlot)]) : nullptr;
    if (paintMesh && !paintMesh->IsMesh()) paintMesh = nullptr;

    // Keys of one bone at the current time / over the clip.
    auto keyedAt = [&](const std::string& bone, float time) {
        for (const char* type : kTimelines)
            for (const Json& k : clipBones[bone][type].items())
                if (std::fabs(k[0].asFloat() - time) < kKeyEpsilon) return true;
        return false;
    };
    auto deleteKeys = [&](const std::string& bone, float time) {
        Json bones = clipBones;
        for (const char* type : kTimelines) {
            if (!bones[bone].has(type)) continue;
            Json kept = Json::MakeArray();
            for (const Json& k : clipBones[bone][type].items())
                if (std::fabs(k[0].asFloat() - time) >= kKeyEpsilon) kept.push(k);
            if (kept.size()) bones[bone][type] = kept;
            else bones[bone].erase(type);
        }
        if (bones[bone].size() == 0) bones.erase(bone);
        SkeletonSet(ObjectOf({{"bones", bones}}), skeletonClip);
    };
    // The key time before / after the current time: the selected bone's, else any bone's.
    auto stepKey = [&](int direction) {
        float best = direction > 0 ? 1e30f : -1e30f;
        bool found = false;
        for (const auto& [bone, timelines] : clipBones.members()) {
            if (skeletonBone >= 0 && bone != boneName(skeletonBone) && clipBones.has(boneName(skeletonBone))) continue;
            for (const char* type : kTimelines)
                for (const Json& k : timelines[type].items()) {
                    const float t = k[0].asFloat();
                    if (direction > 0 ? (t > skeletonTime + kKeyEpsilon && t < best) : (t < skeletonTime - kKeyEpsilon && t > best)) best = t, found = true;
                }
        }
        if (found) skeletonTime = best, skeletonPlaying = false;
    };

    // ----- Side column.
    int listHover = -1;
    std::vector<std::vector<int>> children(static_cast<size_t>(boneCount));
    for (int b = 0; b < boneCount; ++b)
        if (def->bones[static_cast<size_t>(b)].parent >= 0) children[static_cast<size_t>(def->bones[static_cast<size_t>(b)].parent)].push_back(b);
    if (skeletonSideWidth <= 0) skeletonSideWidth = std::clamp(ImGui::GetContentRegionAvail().x * 0.32f, font * 15.0f, font * 20.0f);
    ImGui::BeginChild("##skeletonSide", ImVec2(skeletonSideWidth, 0), ImGuiChildFlags_ResizeX);
    skeletonSideWidth = ImGui::GetWindowWidth();
    auto section = [&](const std::string& title) {
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        ImGui::SeparatorText(title.c_str());
        ImGui::PopStyleColor();
    };
    // The bone hierarchy (or the bones matching the filter). `suffix` writes muted text after a row.
    auto boneList = [&](float visibleRows, const std::function<std::string(int)>& suffix) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##filter", (std::string(kIconSearch) + "  " + Tr("Filter bones")).c_str(), skeletonFilter, sizeof(skeletonFilter));
        const float row = ImGui::GetTextLineHeightWithSpacing();
        ImGui::BeginChild("##bones", ImVec2(0, std::min(visibleRows, static_cast<float>(boneCount) + 0.6f) * row + ImGui::GetStyle().WindowPadding.y * 2), ImGuiChildFlags_Borders);
        ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, font * 0.95f);
        auto after = [&](int b) {
            SkeletonRect("bone:" + std::to_string(b));
            if (ImGui::IsItemHovered()) listHover = b;
            if (b == skeletonHover && b != skeletonBone)
                ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(255, 220, 120, 34), 3);
            if (b == skeletonBone && !ImGui::IsItemVisible() && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY();
            const std::string extra = suffix ? suffix(b) : std::string();
            if (!extra.empty()) {
                ImGui::SameLine();
                ImGui::TextColored(kMuted, "%s", extra.c_str());
            }
        };
        const std::function<void(int)> node = [&](int b) {
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow;
            if (children[static_cast<size_t>(b)].empty()) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            if (b == skeletonBone) flags |= ImGuiTreeNodeFlags_Selected;
            const bool open = ImGui::TreeNodeEx(("##bone" + std::to_string(b)).c_str(), flags, "%s  %s", kIconBone, boneName(b).c_str());
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) skeletonBone = b;
            after(b);
            if (open && !children[static_cast<size_t>(b)].empty()) {
                for (int c : children[static_cast<size_t>(b)]) node(c);
                ImGui::TreePop();
            }
        };
        if (skeletonFilter[0]) {
            for (int b = 0; b < boneCount; ++b) {
                if (!Contains(boneName(b), skeletonFilter)) continue;
                if (ImGui::Selectable((std::string(kIconBone) + "  " + boneName(b) + "##bone" + std::to_string(b)).c_str(), b == skeletonBone)) skeletonBone = b;
                after(b);
            }
        } else {
            for (int b = 0; b < boneCount; ++b)
                if (def->bones[static_cast<size_t>(b)].parent < 0) node(b);
        }
        ImGui::PopStyleVar();
        ImGui::EndChild();
    };
    // A labeled row of a two-column property table.
    auto label = [&](const char* text) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kMuted, "%s", text);
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);
    };
    auto beginTable = [&](const char* id) {
        if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) return false;
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, font * 5.5f);
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
        return true;
    };

    if (skeletonTab == 0) {
        section(Format(Tr("Bones (%d)"), boneCount));
        boneList(12, nullptr);
        if (skeletonBone >= 0) {
            const int parent = def->bones[static_cast<size_t>(skeletonBone)].parent;
            section(WithIcon(kIconBone, boneName(skeletonBone)));
            if (parent >= 0) ImGui::TextColored(kMuted, "%s", Format(Tr("parent: %s"), boneName(parent).c_str()).c_str());
            Json bonesJson = raw["bones"];
            const Json& b = raw["bones"][skeletonBone];
            bool changed = false;
            auto field = [&](const char* key, const char* text, float fallback, float speed, const char* format) {
                float v = b[key].asFloat(fallback);
                label(text);
                if (ImGui::DragFloat((std::string("##") + key).c_str(), &v, speed, 0, 0, format)) {
                    bonesJson[skeletonBone][key] = std::round(v * 10000.0f) / 10000.0f;
                    changed = true;
                }
                SkeletonRect(std::string("field:") + key);
                if (ImGui::IsItemActivated()) ++skeletonDragSerial;
            };
            if (beginTable("##boneFields")) {
                if (skeletonBone > 0) {
                    field("x", "x", 0, 0.01f, "%.3f");
                    field("y", "y", 0, 0.01f, "%.3f");
                }
                field("rotation", Tr("rotation"), 0, 0.5f, "%.2f\xc2\xb0");
                field("scaleX", Tr("scale x"), 1, 0.01f, "%.3f");
                field("scaleY", Tr("scale y"), 1, 0.01f, "%.3f");
                field("length", Tr("length"), 0, 0.01f, "%.3f");
                ImGui::EndTable();
            }
            if (changed) SkeletonSet(ObjectOf({{"bones", bonesJson}}), "", skeletonDragSerial);
        }
        // Draw order: the slots back to front; a row picks the slot (and its bone), the arrows move it.
        section(Tr("Draw order (back to front)"));
        const Json& slots = raw["slots"];
        const float row = ImGui::GetFrameHeight(), button = row;
        ImGui::BeginChild("##slots", ImVec2(0, std::min(static_cast<float>(slots.size()), 10.0f) * (row + ImGui::GetStyle().ItemSpacing.y) + ImGui::GetStyle().WindowPadding.y * 2),
                          ImGuiChildFlags_Borders);
        for (size_t i = 0; i < slots.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const std::string slotName = slots[static_cast<int>(i)]["name"].asString(), slotBone = slots[static_cast<int>(i)]["bone"].asString();
            const float rowWidth = ImGui::GetContentRegionAvail().x - (button + ImGui::GetStyle().ItemSpacing.x) * 2;
            if (ImGui::Selectable("##slot", static_cast<int>(i) == skeletonSlot, 0, ImVec2(rowWidth, row))) {
                skeletonSlot = static_cast<int>(i);
                for (int bb = 0; bb < boneCount; ++bb)
                    if (boneName(bb) == slotBone) skeletonBone = bb;
            }
            const ImVec2 at = ImGui::GetItemRectMin();
            ImDrawList* list = ImGui::GetWindowDrawList();
            const float textY = at.y + (row - font) * 0.5f;
            list->AddText(ImVec2(at.x + 4, textY), ImGui::GetColorU32(ImGuiCol_Text), slotName.c_str());
            list->PushClipRect(at, ImGui::GetItemRectMax(), true);
            list->AddText(ImVec2(at.x + 4 + ImGui::CalcTextSize(slotName.c_str()).x + font * 0.6f, textY), ImGui::GetColorU32(kMuted),
                          (std::string(kIconBone) + " " + slotBone).c_str());
            list->PopClipRect();
            ImGui::SameLine();
            ImGui::BeginDisabled(i == 0);
            if (ImGui::Button(kIconChevronUp, ImVec2(button, row))) {
                Json order = slots;
                std::swap(order.items()[i], order.items()[i - 1]);
                SkeletonSet(ObjectOf({{"slots", order}}));
                if (skeletonSlot == static_cast<int>(i)) --skeletonSlot;
            }
            SkeletonRect("slot-up:" + std::to_string(i));
            ImGui::EndDisabled();
            HelpTooltip(Tr("Draw further back"));
            ImGui::SameLine();
            ImGui::BeginDisabled(i + 1 == slots.size());
            if (ImGui::Button(kIconChevronDown, ImVec2(button, row))) {
                Json order = slots;
                std::swap(order.items()[i], order.items()[i + 1]);
                SkeletonSet(ObjectOf({{"slots", order}}));
                if (skeletonSlot == static_cast<int>(i)) ++skeletonSlot;
            }
            SkeletonRect("slot-down:" + std::to_string(i));
            ImGui::EndDisabled();
            HelpTooltip(Tr("Draw further in front"));
            ImGui::PopID();
        }
        ImGui::EndChild();
    } else if (skeletonTab == 1) {
        // Only mesh attachments have weights: the slots showing one.
        section(Tr("Mesh"));
        if (meshSlots.empty()) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(kMuted, "%s", Tr("No part is a mesh: skeleton2d.build makes bending parts into meshes."));
            ImGui::PopTextWrapPos();
        } else {
            const float row = ImGui::GetTextLineHeightWithSpacing();
            ImGui::BeginChild("##meshes", ImVec2(0, std::min(static_cast<float>(meshSlots.size()), 6.0f) * row + ImGui::GetStyle().WindowPadding.y * 2), ImGuiChildFlags_Borders);
            for (int s : meshSlots) {
                const Skeleton2DAttachment* a = def->FindAttachment("", s, pose.attachments[static_cast<size_t>(s)]);
                if (ImGui::Selectable((def->slots[static_cast<size_t>(s)].name + "##mesh").c_str(), s == skeletonSlot)) skeletonSlot = s;
                ImGui::SameLine();
                ImGui::TextColored(kMuted, "%s", Format(Tr("%d vertices"), static_cast<int>(a->vertices.size() / 2)).c_str());
            }
            ImGui::EndChild();
        }
        SkeletonRect("slot");
        // The bones and how many of the mesh's vertices each one moves.
        std::vector<int> moved(static_cast<size_t>(boneCount), 0);
        if (paintMesh) {
            if (paintMesh->weights.empty()) moved[static_cast<size_t>(def->slots[static_cast<size_t>(skeletonSlot)].bone)] = static_cast<int>(paintMesh->vertices.size() / 2);
            for (const auto& vertex : paintMesh->weights)
                for (const auto& w : vertex)
                    if (w.first >= 0 && w.first < boneCount && w.second > 0) ++moved[static_cast<size_t>(w.first)];
        }
        section(Tr("Bone to paint"));
        boneList(8, [&](int b) { return moved[static_cast<size_t>(b)] ? Format(Tr("%d vertices"), moved[static_cast<size_t>(b)]) : std::string(); });
        section(Tr("Brush"));
        const std::string add = WithIcon(kIconBrush, Tr("Add")), remove = WithIcon(kIconEraser, Tr("Remove"));
        const int mode = Segmented("##paint", {add.c_str(), remove.c_str()}, skeletonSubtract ? 1 : 0, &skeletonRects, "paint", ImGui::GetContentRegionAvail().x);
        if (mode >= 0) skeletonSubtract = mode == 1;
        if (beginTable("##brush")) {
            label(Tr("size"));
            ImGui::SliderFloat("##radius", &skeletonBrush, 4, 200, "%.0f px");
            label(Tr("strength"));
            ImGui::SliderFloat("##strength", &skeletonStrength, 0.05f, 1, "%.2f");
            ImGui::EndTable();
        }
        // The heat colors' legend.
        {
            const ImVec2 a = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x, h = font * 0.6f;
            ImDrawList* list = ImGui::GetWindowDrawList();
            for (int i = 0; i < 16; ++i) {
                const float x0 = a.x + w * static_cast<float>(i) / 16, x1 = a.x + w * static_cast<float>(i + 1) / 16;
                list->AddRectFilledMultiColor(ImVec2(x0, a.y), ImVec2(x1, a.y + h), Heat(i / 16.0f, 255), Heat((i + 1) / 16.0f, 255), Heat((i + 1) / 16.0f, 255), Heat(i / 16.0f, 255));
            }
            ImGui::Dummy(ImVec2(w, h));
            ImGui::TextColored(kMuted, "%s", Tr("does not follow"));
            const char* full = Tr("follows fully");
            ImGui::SameLine(std::max(0.0f, w - ImGui::CalcTextSize(full).x));
            ImGui::TextColored(kMuted, "%s", full);
        }
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(kMuted, "%s", Tr("Drag over the mesh: vertices follow the bone more (red) or less (blue); the other bones share the rest."));
        ImGui::TextColored(kMuted, "%s", Tr("[ and ] change the brush size."));
        ImGui::PopTextWrapPos();
        if (keys && ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) skeletonBrush = std::max(4.0f, skeletonBrush / 1.25f);
        if (keys && ImGui::IsKeyPressed(ImGuiKey_RightBracket)) skeletonBrush = std::min(200.0f, skeletonBrush * 1.25f);
    } else {
        section(Tr("Clip"));
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##clip", skeletonClip.empty() ? Tr("No clips") : (std::string(kIconFilm) + "  " + skeletonClip).c_str())) {
            for (const Skeleton2DClip& c : def->clips)
                if (ImGui::Selectable(c.name.c_str(), c.name == skeletonClip)) skeletonClip = c.name, skeletonTime = 0;
            ImGui::EndCombo();
        }
        SkeletonRect("clip");
        if (clip) {
            int count = 0;
            for (const auto& [bone, timelines] : clipBones.members())
                for (const char* type : kTimelines) count += static_cast<int>(timelines[type].size());
            ImGui::TextColored(kMuted, "%s", Format(Tr("%.2f s  ·  %s  ·  %d keys"), static_cast<double>(clip->duration), clip->loop ? Tr("loops") : Tr("plays once"), count).c_str());
        }
        if (ImGui::Button(WithIcon(kIconSparkles, Tr("Make from recipe...")).c_str(), ImVec2(-FLT_MIN, 0))) ImGui::OpenPopup("##recipes");
        SkeletonRect("recipe");
        HelpTooltip(Tr("Writes a ready motion as a clip of that name (replacing it): bones are found by their role, directions from the rig."));
        if (ImGui::BeginPopup("##recipes")) {
            for (int i = 0; i < IM_ARRAYSIZE(kRecipes); ++i) {
                const bool exists = raw["clips"].has(kRecipes[i]);
                if (!ImGui::Selectable((std::string(kRecipes[i]) + (exists ? std::string("  ") + Tr("(replace)") : std::string())).c_str())) continue;
                const Json before = skeletonRaw;
                if (Ok(Call("skeleton2d.motion", ObjectOf({{"path", skeletonPath}, {"recipe", kRecipes[i]}})))) {
                    skeletonUndo.push_back(before);
                    skeletonRedo.clear();
                    skeletonMergeSerial = -1;
                    skeletonRecipe = i;
                    skeletonClip = kRecipes[i];
                    skeletonTime = 0;
                    SkeletonLoad();
                }
            }
            ImGui::EndPopup();
        }
        if (clip) {
            section(Tr("Bones"));
            boneList(10, [&](int b) { return clipBones.has(boneName(b)) ? std::string(kIconDiamond) : std::string(); });
            if (skeletonBone >= 0) {
                const std::string name = boneName(skeletonBone);
                section(WithIcon(kIconBone, name));
                const Skeleton2DPose::Local& local = pose.bones[static_cast<size_t>(skeletonBone)];
                ImGui::TextColored(kMuted, "%s", Format(Tr("rotation %.1f° (%+.1f° from setup)"), static_cast<double>(local.rotation),
                                                        static_cast<double>(local.rotation - def->bones[static_cast<size_t>(skeletonBone)].rotation)).c_str());
                const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
                if (ImGui::Button(WithIcon(kIconDiamond, Tr("Key rotation")).c_str(), ImVec2(half, 0))) {
                    Json bones = clipBones.isObject() ? clipBones : Json::MakeObject();
                    float delta = local.rotation - def->bones[static_cast<size_t>(skeletonBone)].rotation;
                    delta = std::fmod(delta + 540.0f, 360.0f) - 180.0f;
                    SetKey(bones[name]["rotate"], skeletonTime, {delta});
                    SkeletonSet(ObjectOf({{"bones", bones}}), skeletonClip);
                }
                SkeletonRect("add-key");
                HelpTooltip(Tr("Key the bone's rotation as it is now at this time (K)"));
                ImGui::SameLine();
                ImGui::BeginDisabled(!keyedAt(name, skeletonTime));
                if (ImGui::Button(WithIcon(kIconTrash2, Tr("Delete key")).c_str(), ImVec2(half, 0))) deleteKeys(name, skeletonTime);
                SkeletonRect("delete-key");
                ImGui::EndDisabled();
                HelpTooltip(Tr("Remove the bone's keys at this time (Delete)"));
                if (keys && ImGui::IsKeyPressed(ImGuiKey_Delete, false) && keyedAt(name, skeletonTime)) deleteKeys(name, skeletonTime);
            }
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(kMuted, "%s", Tr("Drag a bone to turn it: its rotation is keyed at this time."));
            ImGui::PopTextWrapPos();
        }
    }
    if (skeletonProblems.isObject()) {
        const bool passed = skeletonProblems["passed"].asBool();
        section(Tr("Check"));
        ImGui::TextColored(passed ? kGood : kError, "%s", WithIcon(passed ? kIconCircleCheck : kIconTriangleAlert, passed ? Tr("No serious problems") : Tr("Problems")).c_str());
        ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - ImGui::GetFrameHeight() - 4);
        if (ImGui::SmallButton(kIconX)) skeletonProblems = Json();
        SkeletonRect("problems-close");
        ImGui::PushTextWrapPos(0);
        for (const Json& p : skeletonProblems["problems"].items()) {
            const bool high = p["severity"].asString() == "high";
            ImGui::TextColored(high ? kError : kMuted, "%s", (std::string(high ? kIconTriangleAlert : kIconInfo) + "  " + p["message"].asString()).c_str());
        }
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // ----- Right side: the canvas, and the timeline under it in Animate.
    ImGui::BeginGroup();
    const ImVec2 area = ImGui::GetContentRegionAvail();
    std::vector<int> timelineRows;  // timeline rows: the keyed bones (and the selected one)
    if (clip) {
        for (int b = 0; b < boneCount; ++b)
            if (clipBones.has(boneName(b)) || b == skeletonBone) timelineRows.push_back(b);
    }
    const float rowHeight = ImGui::GetFrameHeight(), rulerHeight = font * 1.5f;
    const float timelineHeight = clip ? std::clamp(ImGui::GetFrameHeightWithSpacing() + rulerHeight + static_cast<float>(std::max<size_t>(timelineRows.size(), 1)) * rowHeight + font,
                                                   font * 7, std::max(font * 7, area.y * 0.34f))
                                      : 0.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size(std::max(64.0f, area.x), std::max(64.0f, area.y - timelineHeight - (clip ? ImGui::GetStyle().ItemSpacing.y : 0)));
    ImGui::InvisibleButton("##skeletonCanvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    SkeletonRect("canvas");
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(origin, origin + size, true);
    Checkerboard(draw, origin, origin + size, 16);
    draw->AddRectFilled(origin, origin + size, IM_COL32(16, 18, 22, 120));  // a quieter checkerboard: the figure stands out

    const std::vector<Skeleton2DPiece> pieces = Skeleton2DPieces(*def, "", pose, &engine.Assets());
    const std::vector<Mat4> palette = Skeleton2DPalette(*def, pose);
    std::vector<std::vector<Vec3>> posed;
    float lx = 1e30f, ly = 1e30f, hx = -1e30f, hy = -1e30f;
    for (const Skeleton2DPiece& piece : pieces) {
        std::vector<Vec3> moved;
        for (size_t v = 0; v < piece.positions.size(); ++v) {
            moved.push_back(Skeleton2DSkinPoint(palette, piece.skin[v], piece.positions[v]));
            lx = std::min(lx, moved.back().x), ly = std::min(ly, moved.back().y), hx = std::max(hx, moved.back().x), hy = std::max(hy, moved.back().y);
        }
        posed.push_back(std::move(moved));
    }
    if (skeletonFit && lx < hx) {
        skeletonZoom = 0.85f * std::min(size.x / (hx - lx), size.y / std::max(1e-3f, hy - ly));
        skeletonCenter = ImVec2((lx + hx) * 0.5f, (ly + hy) * 0.5f);
    }
    const ImVec2 middle = origin + size * 0.5f;
    auto screen = [&](const Vec3& p) { return ImVec2(middle.x + (p.x - skeletonCenter.x) * skeletonZoom, middle.y - (p.y - skeletonCenter.y) * skeletonZoom); };
    auto world = [&](ImVec2 s) { return Vec3(skeletonCenter.x + (s.x - middle.x) / skeletonZoom, skeletonCenter.y - (s.y - middle.y) / skeletonZoom, 0); };

    // The skeleton's origin as faint axes.
    {
        const ImVec2 o = screen(Vec3());
        draw->AddLine(ImVec2(origin.x, o.y), ImVec2(origin.x + size.x, o.y), IM_COL32(255, 255, 255, 28));
        draw->AddLine(ImVec2(o.x, origin.y), ImVec2(o.x, origin.y + size.y), IM_COL32(255, 255, 255, 28));
    }

    // The mesh being painted (Weights).
    const Skeleton2DPiece* paintPiece = nullptr;
    size_t paintIndex = 0;
    for (size_t i = 0; i < pieces.size(); ++i)
        if (skeletonTab == 1 && pieces[i].slot == skeletonSlot && pieces[i].attachment->IsMesh()) paintPiece = &pieces[i], paintIndex = i;

    // The pictures, back to front, as textured triangles; while painting, the other parts fade back.
    if (GpuRenderer* gpu = engine.Gpu(); gpu && skeletonShowImages) {
        for (size_t i = 0; i < pieces.size(); ++i) {
            if (!pieces[i].texture) continue;
            const ImU32 tint = skeletonTab == 1 && paintPiece && &pieces[i] != paintPiece ? IM_COL32(255, 255, 255, 70) : IM_COL32_WHITE;
            draw->PushTexture(ImTextureRef(ViewTexture(gpu->ImageView(pieces[i].texture))));
            const std::vector<uint32_t>& tris = pieces[i].triangles;
            draw->PrimReserve(static_cast<int>(tris.size()), static_cast<int>(tris.size()));
            for (uint32_t index : tris)
                draw->PrimVtx(screen(posed[i][index]), ImVec2(pieces[i].uvs[index * 2], pieces[i].uvs[index * 2 + 1]), tint);
            draw->PopTexture();
        }
    }
    // Mesh triangles of every part (the toggle), and the picked slot's outline.
    for (size_t i = 0; i < pieces.size(); ++i) {
        const bool outlined = pieces[i].slot == skeletonSlot && skeletonTab == 0;
        if (!(skeletonShowMesh || outlined) || &pieces[i] == paintPiece) continue;
        const ImU32 color = outlined ? IM_COL32(120, 200, 255, 150) : kMeshLine;
        for (size_t t = 0; t + 2 < pieces[i].triangles.size(); t += 3)
            draw->AddTriangle(screen(posed[i][pieces[i].triangles[t]]), screen(posed[i][pieces[i].triangles[t + 1]]), screen(posed[i][pieces[i].triangles[t + 2]]), color);
    }

    // Weights: the mesh filled with how much each vertex follows the chosen bone, its edges and vertices.
    auto weightOf = [&](size_t v) {
        float w = 0;
        for (size_t k = 0; k < 4; ++k)
            if (paintPiece->skin[v].joints[k] == skeletonBone && paintPiece->skin[v].weights[k] > 0) w += paintPiece->skin[v].weights[k];
        if (paintPiece->attachment->weights.empty() && def->slots[static_cast<size_t>(skeletonSlot)].bone == skeletonBone) w = 1;
        return w;
    };
    if (paintPiece) {
        const std::vector<Vec3>& at = posed[paintIndex];
        std::vector<ImU32> colors(at.size());
        for (size_t v = 0; v < at.size(); ++v) colors[v] = Heat(weightOf(v), 150);
        const ImVec2 white = draw->_Data->TexUvWhitePixel;
        const std::vector<uint32_t>& tris = paintPiece->triangles;
        draw->PrimReserve(static_cast<int>(tris.size()), static_cast<int>(tris.size()));
        for (uint32_t index : tris) draw->PrimVtx(screen(at[index]), white, colors[index]);
        for (size_t t = 0; t + 2 < tris.size(); t += 3)
            draw->AddTriangle(screen(at[tris[t]]), screen(at[tris[t + 1]]), screen(at[tris[t + 2]]), IM_COL32(255, 255, 255, 90));
        for (size_t v = 0; v < at.size(); ++v) {
            draw->AddCircleFilled(screen(at[v]), 3.5f, IM_COL32(0, 0, 0, 200));
            draw->AddCircleFilled(screen(at[v]), 2.5f, Heat(weightOf(v), 255));
        }
    }

    // Bones: from the joint to the tip (or a short stub when it has no length).
    std::vector<ImVec2> joints(static_cast<size_t>(boneCount)), tips(static_cast<size_t>(boneCount));
    for (int b = 0; b < boneCount; ++b) {
        const Mat4& m = pose.world[static_cast<size_t>(b)];
        joints[static_cast<size_t>(b)] = screen(m.TransformPoint(Vec3()));
        float length = def->bones[static_cast<size_t>(b)].length;
        if (length <= 0) length = 24 / skeletonZoom;  // a short stub to grab
        tips[static_cast<size_t>(b)] = screen(m.TransformPoint(Vec3(length, 0, 0)));
        const ImVec2 j = joints[static_cast<size_t>(b)], t = tips[static_cast<size_t>(b)];
        skeletonRects["joint:" + std::to_string(b)] = {j.x - 3, j.y - 3, 6, 6};
        skeletonRects["tip:" + std::to_string(b)] = {t.x - 3, t.y - 3, 6, 6};
    }
    if (paintPiece)
        for (size_t v = 0; v < posed[paintIndex].size(); ++v) {
            const ImVec2 p = screen(posed[paintIndex][v]);
            skeletonRects["vertex:" + std::to_string(v)] = {p.x - 2, p.y - 2, 4, 4};
        }
    // What is under the pointer: a joint wins over a bone's line, the nearest within reach.
    ImGuiIO& io = ImGui::GetIO();
    int pointJoint = -1, pointLine = -1;
    if (hovered && skeletonShowBones && skeletonTab != 1) {
        float jointDistance = 9, lineDistance = 7;
        for (int b = 0; b < boneCount; ++b) {
            const float dj = std::sqrt(ImLengthSqr(io.MousePos - joints[static_cast<size_t>(b)]));
            if (dj < jointDistance) jointDistance = dj, pointJoint = b;
            const float dl = DistanceToSegment(io.MousePos, joints[static_cast<size_t>(b)], tips[static_cast<size_t>(b)]);
            if (dl < lineDistance) lineDistance = dl, pointLine = b;
        }
    }
    const int canvasHover = skeletonDrag ? -1 : pointJoint > 0 && skeletonTab == 0 ? pointJoint : pointLine > 0 ? pointLine : pointJoint;
    skeletonHover = skeletonDrag && skeletonDrag != 3 ? skeletonBone : canvasHover >= 0 ? canvasHover : listHover;
    if (skeletonShowBones) {
        const float radius = std::clamp(font * 0.28f, 3.5f, 6.0f);
        // Unpicked bones first, then the hovered and the selected one on top; while painting the others fade.
        for (int pass = 0; pass < 3; ++pass)
            for (int b = 0; b < boneCount; ++b) {
                const int kind = b == skeletonBone ? 2 : b == skeletonHover ? 1 : 0;
                if (kind != pass) continue;
                ImU32 fill = kind == 2 ? kBoneSelected : kind == 1 ? kBoneHot : kBoneFill;
                if (skeletonTab == 1 && kind == 0) fill = (fill & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 70);
                BoneShape(draw, joints[static_cast<size_t>(b)], tips[static_cast<size_t>(b)], fill, radius);
            }
        const float small = font * 0.85f;
        for (int b = 0; b < boneCount; ++b) {
            const bool named = b == skeletonBone || b == skeletonHover;
            if (!named && !skeletonShowNames) continue;
            const ImVec2 at = joints[static_cast<size_t>(b)] + ImVec2(radius + 6, -font * 0.5f);
            if (named) {
                Pill(draw, at, b == skeletonBone ? IM_COL32(150, 200, 255, 255) : IM_COL32(255, 236, 150, 255), boneName(b).c_str());
            } else {
                draw->AddText(nullptr, small, at + ImVec2(1, 1), IM_COL32(0, 0, 0, 200), boneName(b).c_str());
                draw->AddText(nullptr, small, at, IM_COL32(235, 235, 235, 220), boneName(b).c_str());
            }
        }
    }
    if (skeletonTab == 1 && hovered) {
        draw->AddCircle(io.MousePos, skeletonBrush, IM_COL32(0, 0, 0, 160), 48, 3.0f);
        draw->AddCircle(io.MousePos, skeletonBrush, skeletonSubtract ? IM_COL32(120, 170, 255, 230) : IM_COL32(255, 140, 110, 230), 48, 1.5f);
    }
    // What a drag is doing, next to the pointer.
    if (skeletonDrag && skeletonBone >= 0 && skeletonDrag != 3) {
        const Skeleton2DPose::Local& local = pose.bones[static_cast<size_t>(skeletonBone)];
        char text[96];
        if (skeletonDrag == 1) std::snprintf(text, sizeof(text), "%.1f\xc2\xb0", static_cast<double>(local.rotation));
        else std::snprintf(text, sizeof(text), "x %.3f  y %.3f", static_cast<double>(local.x), static_cast<double>(local.y));
        Pill(draw, io.MousePos + ImVec2(16, 12), IM_COL32_WHITE, text);
    }
    // How to use the canvas, and the zoom.
    {
        const char* hint = skeletonTab == 0 ? Tr("Drag a joint: move  ·  drag a bone: turn  ·  right drag: pan  ·  wheel: zoom  ·  F: fit")
                           : skeletonTab == 1 ? Tr("Drag: paint  ·  [ ]: brush size  ·  right drag: pan  ·  wheel: zoom  ·  F: fit")
                                              : Tr("Drag a bone: key its rotation  ·  Space: play  ·  , .: previous / next key  ·  K: key  ·  Delete: remove key");
        const ImVec2 at(origin.x + 8, origin.y + size.y - font - 6);
        Pill(draw, at, IM_COL32(200, 202, 208, 255), hint);
        char zoom[32];
        std::snprintf(zoom, sizeof(zoom), "%.0f px/u", static_cast<double>(skeletonZoom));
        Pill(draw, ImVec2(origin.x + size.x - ImGui::CalcTextSize(zoom).x - 10, origin.y + 8), IM_COL32(200, 202, 208, 255), zoom);
        if (clip) {
            char clock[64];
            std::snprintf(clock, sizeof(clock), "%s  %.2f s", skeletonClip.c_str(), static_cast<double>(skeletonTime));
            Pill(draw, ImVec2(origin.x + 10, origin.y + 8), IM_COL32(255, 200, 190, 255), clock);
        }
    }
    draw->PopClipRect();

    // ----- Canvas input.
    if (hovered && io.MouseWheel != 0) {
        const Vec3 before = world(io.MousePos);
        skeletonZoom = std::clamp(skeletonZoom * std::pow(1.15f, io.MouseWheel), 4.0f, 20000.0f);
        const Vec3 after = world(io.MousePos);
        skeletonCenter.x += before.x - after.x, skeletonCenter.y += before.y - after.y;
        skeletonFit = false;
    }
    if (hovered && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F, false)) skeletonFit = true;
    if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle))) {
        skeletonCenter.x -= io.MouseDelta.x / skeletonZoom, skeletonCenter.y += io.MouseDelta.y / skeletonZoom;
        skeletonFit = false;
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        skeletonDrag = 0;
        ++skeletonDragSerial;
        if (skeletonTab == 1) {
            skeletonDrag = paintPiece && skeletonBone >= 0 ? 3 : 0;
        } else {
            if (pointJoint > 0 && skeletonTab == 0) skeletonBone = pointJoint, skeletonDrag = 2;
            else if (pointLine > 0 && (skeletonTab == 0 || clip)) skeletonBone = pointLine, skeletonDrag = 1;
            else if (pointJoint >= 0) skeletonBone = pointJoint;
            if (skeletonDrag == 1) {
                const Vec3 o = pose.world[static_cast<size_t>(skeletonBone)].TransformPoint(Vec3()), m = world(io.MousePos);
                skeletonDragAngle = Skeleton2DWorldAngle(pose.world[static_cast<size_t>(skeletonBone)]);
                skeletonDragPointer = Degrees(std::atan2(m.y - o.y, m.x - o.x));
                skeletonPlaying = false;
            }
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) skeletonDrag = 0;
    if (skeletonDrag && active && ImGui::IsMouseDown(ImGuiMouseButton_Left) && skeletonBone >= 0) {
        const size_t b = static_cast<size_t>(skeletonBone);
        const int parent = def->bones[b].parent;
        const Mat4 parentWorld = parent >= 0 ? pose.world[static_cast<size_t>(parent)] : Mat4{};
        const float parentAngle = parent >= 0 ? Skeleton2DWorldAngle(parentWorld) : 0;
        const Vec3 m = world(io.MousePos);
        Json bonesJson = raw["bones"];
        if (skeletonDrag == 1 && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
            const Vec3 o = pose.world[b].TransformPoint(Vec3());
            const float pointer = Degrees(std::atan2(m.y - o.y, m.x - o.x));
            float local = skeletonDragAngle + (pointer - skeletonDragPointer) - parentAngle;
            local = std::fmod(local + 540.0f, 360.0f) - 180.0f;
            if (skeletonTab == 0) {
                bonesJson[static_cast<int>(b)]["rotation"] = std::round(local * 100.0f) / 100.0f;
                SkeletonSet(ObjectOf({{"bones", bonesJson}}), "", skeletonDragSerial);
            } else if (clip) {
                // A rotate key holds degrees added to the setup rotation.
                Json bones = clipBones.isObject() ? clipBones : Json::MakeObject();
                float delta = local - def->bones[b].rotation;
                delta = std::fmod(delta + 540.0f, 360.0f) - 180.0f;
                SetKey(bones[def->bones[b].name]["rotate"], skeletonTime, {delta});
                SkeletonSet(ObjectOf({{"bones", bones}}), skeletonClip, skeletonDragSerial);
            }
        } else if (skeletonDrag == 2 && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
            // The joint where the pointer is, in its parent's space (the setup pose: Rig shows it).
            const Vec3 local = setup.world[static_cast<size_t>(parent >= 0 ? parent : 0)].Inverse().TransformPoint(m);
            bonesJson[static_cast<int>(b)]["x"] = std::round(local.x * 10000.0f) / 10000.0f;
            bonesJson[static_cast<int>(b)]["y"] = std::round(local.y * 10000.0f) / 10000.0f;
            SkeletonSet(ObjectOf({{"bones", bonesJson}}), "", skeletonDragSerial);
        } else if (skeletonDrag == 3 && paintPiece) {
            // Paint: vertices under the brush follow the bone more (or less); the other bones share the rest.
            const std::string slotName = def->slots[static_cast<size_t>(skeletonSlot)].name, paintBone = def->bones[b].name;
            Json skins = raw["skins"];
            Json& attachment = skins["default"][slotName][paintPiece->attachment->name];
            Json weights = attachment["weights"];
            const size_t count = posed[paintIndex].size();
            if (!weights.isArray() || weights.size() != count) {  // a rigid mesh: everything on the slot's bone so far
                weights = Json::MakeArray();
                for (size_t v = 0; v < count; ++v) weights.push(Json::parse("[[\"" + def->bones[static_cast<size_t>(def->slots[static_cast<size_t>(skeletonSlot)].bone)].name + "\", 1]]"));
            }
            bool changed = false;
            for (size_t v = 0; v < count; ++v) {
                const float d = std::sqrt(ImLengthSqr(io.MousePos - screen(posed[paintIndex][v])));
                if (d > skeletonBrush) continue;
                const float amount = skeletonStrength * (1 - d / skeletonBrush) * io.DeltaTime * 4 * (skeletonSubtract ? -1.0f : 1.0f);
                std::vector<std::pair<std::string, float>> list;
                float mine = 0;
                for (const Json& pair : weights[static_cast<int>(v)].items()) {
                    if (pair[0].asString() == paintBone) mine = pair[1].asFloat();
                    else list.emplace_back(pair[0].asString(), pair[1].asFloat());
                }
                float others = 0;
                for (const auto& p : list) others += p.second;
                const float next = Clamp(mine + amount, 0, 1);
                if (next == mine || (next < 1 && others <= 0)) continue;  // nothing else to give the rest to
                for (auto& p : list) p.second = others > 0 ? p.second / others * (1 - next) : 0;
                if (next > 0) list.emplace_back(paintBone, next);
                std::sort(list.begin(), list.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
                if (list.size() > 4) list.resize(4);
                Json out = Json::MakeArray();
                for (const auto& p : list)
                    if (p.second > 0.001f) out.push(Json(Json::Array{p.first, std::round(p.second * 1000.0f) / 1000.0f}));
                if (out.size() == 0) continue;
                weights[static_cast<int>(v)] = out;
                changed = true;
            }
            if (changed) {
                attachment["weights"] = weights;
                SkeletonSet(ObjectOf({{"skins", skins}}), "", skeletonDragSerial);
            }
        }
    }

    // ----- Timeline (Animate): transport, a ruler to scrub, a row of keys per keyed bone.
    if (clip) {
        if (ImGui::Button(skeletonPlaying ? kIconPause : kIconPlay)) skeletonPlaying = !skeletonPlaying;
        SkeletonRect("play");
        HelpTooltip(Tr("Play / pause (Space)"));
        ImGui::SameLine();
        if (ImGui::Button(kIconSkipBack)) stepKey(-1);
        SkeletonRect("previous-key");
        HelpTooltip(Tr("Previous key (,)"));
        ImGui::SameLine();
        if (ImGui::Button(kIconSkipForward)) stepKey(1);
        SkeletonRect("next-key");
        HelpTooltip(Tr("Next key (.)"));
        ImGui::SameLine(0, font);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%.2f / %.2f s", static_cast<double>(skeletonTime), static_cast<double>(clip->duration));
        ImGui::SameLine(0, font);
        ImGui::TextColored(kMuted, "%s", clip->loop ? WithIcon(kIconRepeat, Tr("loops")).c_str() : Tr("plays once"));
        for (int i = 0; i < 3; ++i) {  // the key colors' legend
            ImGui::SameLine(0, font);
            const ImVec2 p = ImGui::GetCursorScreenPos() + ImVec2(font * 0.4f, ImGui::GetFrameHeight() * 0.5f);
            Diamond(ImGui::GetWindowDrawList(), p, font * 0.3f, kKeyColors[i], false);
            ImGui::Dummy(ImVec2(font * 0.9f, ImGui::GetFrameHeight()));
            ImGui::SameLine(0, 2);
            ImGui::TextColored(kMuted, "%s", Tr(kTimelines[i]));
        }
        if (keys) {
            if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) skeletonPlaying = !skeletonPlaying;
            if (ImGui::IsKeyPressed(ImGuiKey_Comma)) stepKey(-1);
            if (ImGui::IsKeyPressed(ImGuiKey_Period)) stepKey(1);
            if (ImGui::IsKeyPressed(ImGuiKey_K, false) && skeletonBone >= 0) {
                Json bones = clipBones.isObject() ? clipBones : Json::MakeObject();
                float delta = pose.bones[static_cast<size_t>(skeletonBone)].rotation - def->bones[static_cast<size_t>(skeletonBone)].rotation;
                delta = std::fmod(delta + 540.0f, 360.0f) - 180.0f;
                SetKey(bones[boneName(skeletonBone)]["rotate"], skeletonTime, {delta});
                SkeletonSet(ObjectOf({{"bones", bones}}), skeletonClip);
            }
        }

        const float labelWidth = std::min(font * 9.0f, area.x * 0.3f);
        const ImVec2 top = ImGui::GetCursorScreenPos();
        const float stripWidth = std::max(64.0f, area.x);
        const float x0 = top.x + labelWidth + font * 0.6f, x1 = top.x + stripWidth - font * 0.8f;
        auto timeX = [&](float t) { return x0 + (x1 - x0) * Clamp(t / duration, 0, 1); };
        auto xTime = [&](float x) { return Clamp((x - x0) / std::max(1.0f, x1 - x0), 0, 1) * duration; };
        ImDrawList* list = ImGui::GetWindowDrawList();
        // Ruler: drag on it to scrub.
        ImGui::InvisibleButton("##ruler", ImVec2(stripWidth, rulerHeight));
        SkeletonRect("time");
        if (ImGui::IsItemActive()) skeletonTime = xTime(io.MousePos.x), skeletonPlaying = false;
        list->AddRectFilled(top, top + ImVec2(stripWidth, rulerHeight), IM_COL32(30, 32, 38, 255), 3);
        float step = 0.05f;
        for (float candidate : {0.05f, 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f})
            if ((x1 - x0) * candidate / duration >= font * 3.5f) { step = candidate; break; } else step = candidate;
        for (int i = 0; static_cast<float>(i) * step <= duration + 1e-4f; ++i) {
            const float t = static_cast<float>(i) * step, x = timeX(t);
            list->AddLine(ImVec2(x, top.y + rulerHeight * 0.55f), ImVec2(x, top.y + rulerHeight), IM_COL32(150, 154, 162, 255));
            const float half = timeX(t + step * 0.5f);
            if (t + step * 0.5f <= duration) list->AddLine(ImVec2(half, top.y + rulerHeight * 0.78f), ImVec2(half, top.y + rulerHeight), IM_COL32(100, 104, 112, 255));
            char text[16];
            std::snprintf(text, sizeof(text), step < 0.1f ? "%.2f" : "%.1f", static_cast<double>(t));
            list->AddText(nullptr, font * 0.85f, ImVec2(x + 3, top.y + 1), IM_COL32(170, 174, 182, 255), text);
        }
        // Rows: a bone's name, then its keys as diamonds (click one to go there, click the row to scrub).
        const ImVec2 rowsTop = ImGui::GetCursorScreenPos();
        ImGui::BeginChild("##timelineRows", ImVec2(stripWidth, 0), ImGuiChildFlags_None);
        ImDrawList* rowList = ImGui::GetWindowDrawList();
        for (size_t r = 0; r < timelineRows.size(); ++r) {
            const int b = timelineRows[r];
            const std::string& name = boneName(b);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton(("##row" + std::to_string(b)).c_str(), ImVec2(stripWidth, rowHeight));
            SkeletonRect("row:" + std::to_string(b));
            const bool rowHovered = ImGui::IsItemHovered();
            if (rowHovered) listHover = b, skeletonHover = b;
            const ImU32 back = b == skeletonBone ? IM_COL32(86, 162, 255, 50) : rowHovered || b == skeletonHover ? IM_COL32(255, 255, 255, 18)
                                                   : r % 2 ? IM_COL32(255, 255, 255, 6) : IM_COL32(0, 0, 0, 0);
            rowList->AddRectFilled(p, p + ImVec2(stripWidth, rowHeight), back);
            rowList->PushClipRect(p, p + ImVec2(labelWidth, rowHeight), true);
            rowList->AddText(p + ImVec2(font * 0.4f, (rowHeight - font) * 0.5f), b == skeletonBone ? IM_COL32(170, 210, 255, 255) : ImGui::GetColorU32(ImGuiCol_Text),
                             (std::string(kIconBone) + " " + name).c_str());
            rowList->PopClipRect();
            rowList->AddLine(ImVec2(x0, p.y + rowHeight * 0.5f), ImVec2(x1, p.y + rowHeight * 0.5f), IM_COL32(255, 255, 255, 22));
            float clickedKey = -1, clickedDistance = font * 0.6f;
            for (int type = 0; type < 3; ++type)
                for (const Json& k : clipBones[name][kTimelines[type]].items()) {
                    const float t = k[0].asFloat();
                    const ImVec2 c(timeX(t), p.y + rowHeight * 0.5f + static_cast<float>(type - 1) * rowHeight * 0.16f);
                    Diamond(rowList, c, font * 0.32f, kKeyColors[type], std::fabs(t - skeletonTime) < kKeyEpsilon && b == skeletonBone);
                    const float d = std::fabs(io.MousePos.x - c.x);
                    if (rowHovered && d < clickedDistance) clickedDistance = d, clickedKey = t;
                }
            if (ImGui::IsItemActivated()) {
                skeletonBone = b;
                skeletonPlaying = false;
                if (clickedKey >= 0) skeletonTime = clickedKey;
                else if (io.MousePos.x >= x0) skeletonScrubbing = true;
            }
            if (ImGui::IsItemActive() && skeletonScrubbing) skeletonTime = xTime(io.MousePos.x);
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) skeletonScrubbing = false;
        if (timelineRows.empty()) {
            ImGui::SetCursorScreenPos(ImGui::GetCursorScreenPos() + ImVec2(font * 0.4f, 4));
            ImGui::TextColored(kMuted, "%s", Tr("No keys yet: pick a bone and drag it on the canvas."));
        }
        const float rowsBottom = std::max(ImGui::GetCursorScreenPos().y, rowsTop.y + rowHeight);
        ImGui::EndChild();
        // The playhead across the ruler and the rows.
        const float x = timeX(skeletonTime);
        list->AddLine(ImVec2(x, top.y), ImVec2(x, std::min(rowsBottom, ImGui::GetItemRectMax().y)), kPlayhead, 2);
        list->AddTriangleFilled(ImVec2(x - 5, top.y), ImVec2(x + 5, top.y), ImVec2(x, top.y + 7), kPlayhead);
    }
    ImGui::EndGroup();
    ImGui::End();
}

std::array<float, 4> NativeEditor::SkeletonEditorControlRect(const std::string& name) const {
    auto found = impl_->skeletonRects.find(name);
    return found == impl_->skeletonRects.end() ? std::array<float, 4>{0, 0, 0, 0} : found->second;
}

}  // namespace oe
