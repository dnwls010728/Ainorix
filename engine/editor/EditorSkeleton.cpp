// Skeleton Editor window: one 2D skeleton (*.skel2d.json, docs/SKELETON2D.md). Rig shows the bones (drag a joint to
// move it, a bone to turn it) and the slots' draw order; Weights paints how much each mesh vertex follows a bone;
// Animate shows a clip on a timeline, where turning a bone keys its rotation at the current time. Every change is a
// skeleton2d.* command that writes the file; the window keeps its own undo of the file (Ctrl+Z / Ctrl+Y while it is
// focused). It follows the selection: picking an entity with a Skeleton2D shows its asset.
#include <algorithm>
#include <cmath>
#include <cstdio>

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

constexpr ImU32 kBone = IM_COL32(255, 215, 0, 255);
constexpr ImU32 kBoneSelected = IM_COL32(92, 148, 255, 255);
constexpr ImU32 kMeshLine = IM_COL32(255, 255, 255, 60);
const ImVec4 kMuted(0.66f, 0.67f, 0.70f, 1.0f);
const ImVec4 kError(0.96f, 0.45f, 0.40f, 1.0f);
const char* const kRecipes[] = {"idle", "walk", "run", "jump", "attack", "hit", "death"};

bool IsSkeletonPath(const std::string& p) { return p.size() > 12 && p.compare(p.size() - 12, 12, ".skel2d.json") == 0; }

// Puts a key [time, values..., curve] into a timeline: replaces the values of a key within half a frame of `time`
// (keeping its curve), else inserts one in time order.
void SetKey(Json& keys, float time, const std::vector<float>& values) {
    if (!keys.isArray()) keys = Json::MakeArray();
    std::vector<Json> list = keys.items();
    size_t at = list.size();
    for (size_t i = 0; i < list.size(); ++i) {
        const float t = list[i][0].asFloat();
        if (std::fabs(t - time) < 1.0f / 120.0f) {
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

    // ----- Toolbar: which asset, the three tabs, undo / redo, check.
    const float font = ImGui::GetFontSize();
    {
        const std::string name = skeletonPath.empty() ? std::string(Tr("No skeleton")) : skeletonPath.substr(skeletonPath.find_last_of('/') + 1);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1, 1, 1, 0.06f));
        if (ImGui::Button((WithIcon(kIconPersonStanding, name) + "  " + kIconChevronDown).c_str())) ImGui::OpenPopup("##skeletons");
        ImGui::PopStyleColor();
        SkeletonRect("asset");
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
        if (!skeletonPath.empty() && skeletonRaw.isObject()) {
            ImGui::SameLine(0, font);
            const int tab = Segmented("##tabs", {Tr("Rig"), Tr("Weights"), Tr("Animate")}, skeletonTab, &skeletonRects, "tab");
            if (tab >= 0) skeletonTab = tab, skeletonDrag = 0;
            ImGui::SameLine(0, font);
            ImGui::BeginDisabled(skeletonUndo.empty());
            if (ImGui::Button(kIconUndo)) SkeletonHistory(false);
            ImGui::EndDisabled();
            SkeletonRect("undo");
            ImGui::SameLine();
            ImGui::BeginDisabled(skeletonRedo.empty());
            if (ImGui::Button(kIconRedo)) SkeletonHistory(true);
            ImGui::EndDisabled();
            SkeletonRect("redo");
            ImGui::SameLine(0, font);
            if (ImGui::Button(WithIcon(kIconCircleCheck, Tr("Check")).c_str())) {
                Json args = ObjectOf({{"path", skeletonPath}, {"render", false}});
                if (skeletonTab == 2 && !skeletonClip.empty()) args["clip"] = skeletonClip;
                const Json r = Call("skeleton2d.analyze", args);
                skeletonProblems = Ok(r) ? r["result"] : Json();
            }
            SkeletonRect("check");
            HelpTooltip(Tr("Pose it (the clip shown, or every joint bent both ways) and list what goes wrong: hidden picture showing, joints opening, meshes folding."));
        }
    }
    if (skeletonEditorFocused && !ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) SkeletonHistory(false);
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z)) SkeletonHistory(true);
    }
    ImGui::Separator();
    std::string error;
    const auto def = skeletonPath.empty() ? nullptr : engine.Assets().GetSkeleton2D(skeletonPath, &error);
    if (!def || !skeletonRaw.isObject()) {
        ImGui::PushTextWrapPos(0);
        if (skeletonPath.empty()) ImGui::TextColored(kMuted, "%s", Tr("Select an entity with a Skeleton2D, or pick a skeleton above. skeleton2d.build makes one from a picture."));
        else ImGui::TextColored(kError, "%s", Format(Tr("Cannot read %s: %s"), skeletonPath.c_str(), (skeletonError.empty() ? error : skeletonError).c_str()).c_str());
        ImGui::PopTextWrapPos();
        ImGui::End();
        return;
    }
    const int boneCount = static_cast<int>(def->bones.size());
    const Skeleton2DClip* clip = skeletonTab == 2 ? def->FindClip(skeletonClip) : nullptr;
    if (clip && skeletonPlaying) {
        skeletonTime += ImGui::GetIO().DeltaTime;
        if (clip->duration > 0) skeletonTime = clip->loop ? std::fmod(skeletonTime, clip->duration) : std::min(skeletonTime, clip->duration);
    }
    Skeleton2DPose setup = Skeleton2DSetupPose(*def);
    Skeleton2DWorld(*def, setup);
    const Skeleton2DPose pose = clip ? Skeleton2DSample(*def, clip, skeletonTime) : Skeleton2DSample(*def, nullptr, 0);

    // ----- Side column.
    const float side = std::min(ImGui::GetContentRegionAvail().x * 0.38f, font * 18.0f);
    ImGui::BeginChild("##skeletonSide", ImVec2(side, 0), ImGuiChildFlags_None);
    Json bonesJson = skeletonRaw["bones"];
    auto boneList = [&](float height) {
        ImGui::BeginChild("##bones", ImVec2(0, height), ImGuiChildFlags_Borders);
        for (int b = 0; b < boneCount; ++b) {
            int depth = 0;
            for (int p = def->bones[static_cast<size_t>(b)].parent; p >= 0; p = def->bones[static_cast<size_t>(p)].parent) ++depth;
            ImGui::Indent(static_cast<float>(depth) * font * 0.6f + 1);
            if (ImGui::Selectable((def->bones[static_cast<size_t>(b)].name + "##bone" + std::to_string(b)).c_str(), b == skeletonBone)) skeletonBone = b;
            SkeletonRect("bone:" + std::to_string(b));
            ImGui::Unindent(static_cast<float>(depth) * font * 0.6f + 1);
        }
        ImGui::EndChild();
    };
    if (skeletonTab == 0) {
        ImGui::TextColored(kMuted, "%s", Tr("Bones"));
        boneList(font * 12);
        if (skeletonBone >= 0) {
            Json& b = bonesJson[skeletonBone];
            bool changed = false, done = false;
            auto field = [&](const char* key, const char* label, float fallback, float speed) {
                float v = b[key].asFloat(fallback);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::DragFloat((std::string("##") + key).c_str(), &v, speed, 0, 0, (std::string(label) + " %.3f").c_str())) {
                    b[key] = std::round(v * 10000.0f) / 10000.0f;
                    changed = true;
                }
                SkeletonRect(std::string("field:") + key);
                if (ImGui::IsItemActivated()) ++skeletonDragSerial;
                done = done || ImGui::IsItemDeactivatedAfterEdit();
            };
            if (skeletonBone > 0) {
                field("x", "x", 0, 0.01f);
                field("y", "y", 0, 0.01f);
            }
            field("rotation", Tr("rotation"), 0, 0.5f);
            field("scaleX", Tr("scale x"), 1, 0.01f);
            field("scaleY", Tr("scale y"), 1, 0.01f);
            field("length", Tr("length"), 0, 0.01f);
            if (changed) SkeletonSet(ObjectOf({{"bones", bonesJson}}), "", skeletonDragSerial);
        }
        ImGui::Spacing();
        ImGui::TextColored(kMuted, "%s", Tr("Draw order (back to front)"));
        const Json& slots = skeletonRaw["slots"];
        for (size_t i = 0; i < slots.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::BeginDisabled(i == 0);
            if (ImGui::SmallButton("^")) {
                Json list = slots;
                std::swap(list.items()[i], list.items()[i - 1]);
                SkeletonSet(ObjectOf({{"slots", list}}));
            }
            SkeletonRect("slot-up:" + std::to_string(i));
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(i + 1 == slots.size());
            if (ImGui::SmallButton("v")) {
                Json list = slots;
                std::swap(list.items()[i], list.items()[i + 1]);
                SkeletonSet(ObjectOf({{"slots", list}}));
            }
            SkeletonRect("slot-down:" + std::to_string(i));
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextUnformatted(slots[static_cast<int>(i)]["name"].asString().c_str());
            ImGui::PopID();
        }
    } else if (skeletonTab == 1) {
        // Only mesh attachments have weights: the slots showing one.
        ImGui::TextColored(kMuted, "%s", Tr("Mesh"));
        std::string slotLabel = skeletonSlot >= 0 ? def->slots[static_cast<size_t>(skeletonSlot)].name : std::string(Tr("Pick a mesh"));
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##slot", slotLabel.c_str())) {
            for (size_t s = 0; s < def->slots.size(); ++s) {
                const Skeleton2DAttachment* a = def->FindAttachment("", static_cast<int>(s), pose.attachments[s]);
                if (!a || !a->IsMesh()) continue;
                if (ImGui::Selectable(def->slots[s].name.c_str(), static_cast<int>(s) == skeletonSlot)) skeletonSlot = static_cast<int>(s);
            }
            ImGui::EndCombo();
        }
        SkeletonRect("slot");
        ImGui::TextColored(kMuted, "%s", Tr("Bone to paint"));
        boneList(font * 10);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##radius", &skeletonBrush, 4, 200, (std::string(Tr("brush")) + " %.0f px").c_str());
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##strength", &skeletonStrength, 0.05f, 1, (std::string(Tr("strength")) + " %.2f").c_str());
        const int mode = Segmented("##paint", {Tr("Add"), Tr("Remove")}, skeletonSubtract ? 1 : 0, &skeletonRects, "paint");
        if (mode >= 0) skeletonSubtract = mode == 1;
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(kMuted, "%s", Tr("Drag over the mesh: vertices follow the bone more (red) or less (blue); the other bones share the rest."));
        ImGui::PopTextWrapPos();
    } else {
        ImGui::TextColored(kMuted, "%s", Tr("Clip"));
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##clip", skeletonClip.empty() ? Tr("No clips") : skeletonClip.c_str())) {
            for (const Skeleton2DClip& c : def->clips)
                if (ImGui::Selectable(c.name.c_str(), c.name == skeletonClip)) skeletonClip = c.name, skeletonTime = 0;
            ImGui::EndCombo();
        }
        SkeletonRect("clip");
        static int recipe = 1;
        ImGui::SetNextItemWidth(font * 6);
        ImGui::Combo("##recipe", &recipe, kRecipes, IM_ARRAYSIZE(kRecipes));
        ImGui::SameLine();
        if (ImGui::Button(WithIcon(kIconSparkles, Tr("Make from recipe")).c_str())) {
            const Json before = skeletonRaw;
            if (Ok(Call("skeleton2d.motion", ObjectOf({{"path", skeletonPath}, {"recipe", kRecipes[recipe]}})))) {
                skeletonUndo.push_back(before);
                skeletonRedo.clear();
                skeletonMergeSerial = -1;
                skeletonClip = kRecipes[recipe];
                skeletonTime = 0;
                SkeletonLoad();
            }
        }
        SkeletonRect("recipe");
        if (clip) {
            if (ImGui::Button(skeletonPlaying ? kIconPause : kIconPlay)) skeletonPlaying = !skeletonPlaying;
            SkeletonRect("play");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::SliderFloat("##time", &skeletonTime, 0, std::max(0.01f, clip->duration), "%.2f s");
            SkeletonRect("time");
            ImGui::TextColored(kMuted, "%s", Tr("Bones"));
            boneList(font * 9);
            if (skeletonBone > 0) {
                // Keys of the selected bone's rotation: click one to go there.
                const std::string boneName = def->bones[static_cast<size_t>(skeletonBone)].name;
                const Json& rotate = skeletonRaw["clips"][skeletonClip]["bones"][boneName]["rotate"];
                ImGui::TextColored(kMuted, "%s", Format(Tr("%s rotation keys"), boneName.c_str()).c_str());
                for (size_t k = 0; k < rotate.size(); ++k) {
                    ImGui::PushID(static_cast<int>(k));
                    if (k % 4) ImGui::SameLine();
                    const float t = rotate[static_cast<int>(k)][0].asFloat();
                    char label[32];
                    std::snprintf(label, sizeof(label), "%.2f", static_cast<double>(t));
                    if (ImGui::SmallButton(label)) skeletonTime = t, skeletonPlaying = false;
                    ImGui::PopID();
                }
                bool atKey = false;
                for (size_t k = 0; k < rotate.size(); ++k) atKey = atKey || std::fabs(rotate[static_cast<int>(k)][0].asFloat() - skeletonTime) < 1.0f / 120.0f;
                ImGui::BeginDisabled(!atKey);
                if (ImGui::Button(WithIcon(kIconTrash2, Tr("Delete key")).c_str())) {
                    Json bones = skeletonRaw["clips"][skeletonClip]["bones"];
                    Json kept = Json::MakeArray();
                    for (const Json& k : rotate.items())
                        if (std::fabs(k[0].asFloat() - skeletonTime) >= 1.0f / 120.0f) kept.push(k);
                    if (kept.size()) bones[boneName]["rotate"] = kept;
                    else bones[boneName].erase("rotate");
                    if (bones[boneName].size() == 0) bones.erase(boneName);
                    SkeletonSet(ObjectOf({{"bones", bones}}), skeletonClip);
                }
                SkeletonRect("delete-key");
                ImGui::EndDisabled();
            }
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(kMuted, "%s", Tr("Drag a bone to turn it: its rotation is keyed at this time."));
            ImGui::PopTextWrapPos();
        }
    }
    if (skeletonProblems.isObject()) {
        ImGui::Spacing();
        ImGui::TextColored(skeletonProblems["passed"].asBool() ? ImVec4(0.45f, 0.85f, 0.55f, 1) : kError, "%s",
                           skeletonProblems["passed"].asBool() ? Tr("No serious problems") : Tr("Problems"));
        ImGui::PushTextWrapPos(0);
        for (const Json& p : skeletonProblems["problems"].items())
            ImGui::TextColored(p["severity"].asString() == "high" ? kError : kMuted, "%s", p["message"].asString().c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // ----- Canvas: the skeleton drawn with its pictures, the rig on top.
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size(std::max(64.0f, ImGui::GetContentRegionAvail().x), std::max(64.0f, ImGui::GetContentRegionAvail().y));
    ImGui::InvisibleButton("##skeletonCanvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    SkeletonRect("canvas");
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(origin, origin + size, true);
    Checkerboard(draw, origin, origin + size, 16);

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

    // The pictures, back to front, as textured triangles.
    if (GpuRenderer* gpu = engine.Gpu()) {
        for (size_t i = 0; i < pieces.size(); ++i) {
            if (!pieces[i].texture) continue;
            const ImTextureID image = ViewTexture(gpu->ImageView(pieces[i].texture));
            draw->PushTexture(ImTextureRef(image));
            const std::vector<uint32_t>& tris = pieces[i].triangles;
            draw->PrimReserve(static_cast<int>(tris.size()), static_cast<int>(tris.size()));
            for (uint32_t index : tris)
                draw->PrimVtx(screen(posed[i][index]), ImVec2(pieces[i].uvs[index * 2], pieces[i].uvs[index * 2 + 1]), IM_COL32_WHITE);
            draw->PopTexture();
        }
    }

    // Weights: the chosen mesh's edges and its vertices coloured by how much they follow the chosen bone.
    const Skeleton2DPiece* paintPiece = nullptr;
    size_t paintIndex = 0;
    for (size_t i = 0; i < pieces.size(); ++i)
        if (pieces[i].slot == skeletonSlot && pieces[i].attachment->IsMesh()) paintPiece = &pieces[i], paintIndex = i;
    if (skeletonTab == 1 && paintPiece) {
        for (size_t t = 0; t + 2 < paintPiece->triangles.size(); t += 3)
            draw->AddTriangle(screen(posed[paintIndex][paintPiece->triangles[t]]), screen(posed[paintIndex][paintPiece->triangles[t + 1]]),
                              screen(posed[paintIndex][paintPiece->triangles[t + 2]]), kMeshLine);
        for (size_t v = 0; v < posed[paintIndex].size(); ++v) {
            float w = 0;
            for (size_t k = 0; k < 4; ++k)
                if (paintPiece->skin[v].joints[k] == skeletonBone && paintPiece->skin[v].weights[k] > 0) w += paintPiece->skin[v].weights[k];
            if (paintPiece->attachment->weights.empty() && def->slots[static_cast<size_t>(skeletonSlot)].bone == skeletonBone) w = 1;
            draw->AddCircleFilled(screen(posed[paintIndex][v]), 4, IM_COL32(static_cast<int>(255 * w), 40, static_cast<int>(255 * (1 - w)), 255));
        }
    }

    // Bones: a line along each (to its tip, or to its first child when it has no length), the joint as a dot.
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
    for (int b = 0; b < boneCount; ++b) {
        const ImU32 color = b == skeletonBone ? kBoneSelected : kBone;
        draw->AddLine(joints[static_cast<size_t>(b)], tips[static_cast<size_t>(b)], IM_COL32(0, 0, 0, 160), 4);
        draw->AddLine(joints[static_cast<size_t>(b)], tips[static_cast<size_t>(b)], color, 2);
        draw->AddCircleFilled(joints[static_cast<size_t>(b)], 4.5f, IM_COL32(0, 0, 0, 200));
        draw->AddCircleFilled(joints[static_cast<size_t>(b)], 3.5f, color);
        if (b == skeletonBone) draw->AddText(joints[static_cast<size_t>(b)] + ImVec2(7, -font), color, def->bones[static_cast<size_t>(b)].name.c_str());
    }
    if (skeletonTab == 1 && hovered) draw->AddCircle(ImGui::GetIO().MousePos, skeletonBrush, IM_COL32(255, 255, 255, 160), 32, 1.5f);
    draw->PopClipRect();

    // ----- Input.
    ImGuiIO& io = ImGui::GetIO();
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
            // A joint (Rig: move it) wins over a bone's line (turn it); the nearest within reach.
            int bestJoint = -1, bestLine = -1;
            float jointDistance = 9, lineDistance = 7;
            for (int b = 0; b < boneCount; ++b) {
                const float dj = std::sqrt(ImLengthSqr(io.MousePos - joints[static_cast<size_t>(b)]));
                if (dj < jointDistance) jointDistance = dj, bestJoint = b;
                const float dl = DistanceToSegment(io.MousePos, joints[static_cast<size_t>(b)], tips[static_cast<size_t>(b)]);
                if (dl < lineDistance) lineDistance = dl, bestLine = b;
            }
            if (bestJoint > 0 && skeletonTab == 0) skeletonBone = bestJoint, skeletonDrag = 2;
            else if (bestLine > 0 && (skeletonTab == 0 || clip)) skeletonBone = bestLine, skeletonDrag = 1;
            else if (bestJoint >= 0) skeletonBone = bestJoint;
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
                Json bones = skeletonRaw["clips"][skeletonClip]["bones"];
                if (!bones.isObject()) bones = Json::MakeObject();
                Json& rotate = bones[def->bones[b].name]["rotate"];
                float delta = local - def->bones[b].rotation;
                delta = std::fmod(delta + 540.0f, 360.0f) - 180.0f;
                SetKey(rotate, skeletonTime, {delta});
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
            const std::string slotName = def->slots[static_cast<size_t>(skeletonSlot)].name, boneName = def->bones[b].name;
            Json skins = skeletonRaw["skins"];
            std::string skinName = "default";
            Json& attachment = skins[skinName][slotName][paintPiece->attachment->name];
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
                    if (pair[0].asString() == boneName) mine = pair[1].asFloat();
                    else list.emplace_back(pair[0].asString(), pair[1].asFloat());
                }
                float others = 0;
                for (const auto& p : list) others += p.second;
                const float next = Clamp(mine + amount, 0, 1);
                if (next == mine || (next < 1 && others <= 0)) continue;  // nothing else to give the rest to
                for (auto& p : list) p.second = others > 0 ? p.second / others * (1 - next) : 0;
                if (next > 0) list.emplace_back(boneName, next);
                std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
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
    ImGui::End();
}

std::array<float, 4> NativeEditor::SkeletonEditorControlRect(const std::string& name) const {
    auto found = impl_->skeletonRects.find(name);
    return found == impl_->skeletonRects.end() ? std::array<float, 4>{0, 0, 0, 0} : found->second;
}

}  // namespace oe
