// Retarget Animation window: choose a character and a model with clips, map the body bones (guessed
// from their names, edited by hand), pick the clips and write them as a clip file of the character.
// Reads models for their bone and clip names; the work is done by the animation.retarget commands.
#include <algorithm>

#include "editor/EditorIcons.h"
#include "editor/EditorInternal.h"
#include "editor/EditorText.h"

#include "app/Engine.h"
#include "assets/Assets.h"
#include "core/Log.h"
#include "render/Mesh.h"

namespace oe {

namespace {

bool HasText(const std::string& text, const std::string& part) {
    if (part.empty()) return true;
    auto it = std::search(text.begin(), text.end(), part.begin(), part.end(),
                          [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
    return it != text.end();
}

// "assets/models/Hero.fbx" -> "assets/anims/Hero_clips.gltf"
std::string DefaultClipFile(const std::string& target) {
    const size_t slash = target.find_last_of('/');
    std::string stem = slash == std::string::npos ? target : target.substr(slash + 1);
    const size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot);
    return "assets/anims/" + stem + "_clips.gltf";
}

}  // namespace

void NativeEditor::Impl::OpenRetarget(const std::string& target, const std::string& source) {
    showRetarget = true;
    if (!target.empty()) retargetTarget = target;
    if (!source.empty()) retargetSource = source;
    focusRetarget = true;
}

// The clips of the source and, when nothing is mapped yet, a guessed bone map.
void NativeEditor::Impl::RetargetLoad() {
    const std::string key = retargetSource + "\n" + retargetTarget;
    if (key == retargetLoaded) return;
    const bool newTarget = retargetLoaded.substr(retargetLoaded.find('\n') == std::string::npos ? 0 : retargetLoaded.find('\n') + 1) != retargetTarget;
    retargetLoaded = key;
    retargetStatus.clear();
    retargetClips.clear();
    if (std::shared_ptr<const Mesh> source = retargetSource.empty() ? nullptr : engine.Assets().GetMesh(retargetSource)) {
        for (const AnimationClip& clip : source->clips) retargetClips.push_back({clip.name, clip.name, clip.duration, true});
    }
    if (newTarget || retargetOut.empty()) retargetOut = retargetTarget.empty() ? std::string() : DefaultClipFile(retargetTarget);
    if (!retargetSource.empty() && !retargetTarget.empty()) RetargetAutoMap();
}

void NativeEditor::Impl::RetargetAutoMap() {
    Json guess = Call("animation.retarget_map", ObjectOf({{"source", Json(retargetSource)}, {"target", Json(retargetTarget)}}));
    if (!Ok(guess)) return;
    const Json& result = guess["result"];
    retargetRows.clear();
    for (const auto& pair : result["bones"].members()) {
        bool end = false;
        for (const Json& name : result["ends"].items()) end = end || name.asString("") == pair.first;
        retargetRows.push_back({pair.first, pair.second.asString(""), end});
    }
    retargetHips = result["hips"].asString("");
    if (retargetRows.empty()) {
        retargetStatus = Tr("No body bone was recognized by its name. Add the bones by hand.");
        retargetError = true;
    } else if (result["unmatched"].size() != 0) {
        std::string parts;
        for (const Json& part : result["unmatched"].items()) parts += (parts.empty() ? "" : ", ") + part.asString("");
        retargetStatus = Format(Tr("Not found in both models: %s"), parts.c_str());
        retargetError = false;
    }
}

void NativeEditor::Impl::RetargetRun() {
    Json bones = Json::MakeObject(), ends = Json::MakeArray(), clips = Json::MakeObject();
    for (const RetargetRow& row : retargetRows) {
        if (row.target.empty() || row.source.empty()) continue;
        bones[row.target] = row.source;
        if (row.end) ends.push(row.target);
    }
    for (const RetargetClip& clip : retargetClips) if (clip.use && !clip.name.empty()) clips[clip.name] = clip.source;
    Json args = ObjectOf({{"source", Json(retargetSource)}, {"target", Json(retargetTarget)}, {"out", Json(retargetOut)}, {"bones", bones},
                          {"ends", ends}, {"clips", clips}, {"attach", Json(retargetAttach)}});
    if (!retargetHips.empty()) args["hips"] = retargetHips;
    Json done = Call("animation.retarget", args, true);
    retargetError = !Ok(done);
    if (retargetError) {
        retargetStatus = done["error"]["message"].asString("");
        return;
    }
    const Json& result = done["result"];
    retargetStatus = Format(Tr("Wrote %d clips for %d bones to %s"), static_cast<int>(result["clips"].size()), result["bones"].asInt(), retargetOut.c_str());
    if (result["missing"].size() != 0) {
        std::string names;
        for (const Json& name : result["missing"].items()) names += (names.empty() ? "" : ", ") + name.asString("");
        retargetStatus += "\n" + Format(Tr("Not found: %s"), names.c_str());
    }
    Notify(Format(Tr("Wrote %d clips for %d bones to %s"), static_cast<int>(result["clips"].size()), result["bones"].asInt(), retargetOut.c_str()));
    Refresh(true);
}

void NativeEditor::Impl::RetargetWindow() {
    if (!showRetarget) return;
    const float font = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(font * 46.0f, font * 46.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(font * 34.0f, font * 22.0f), ImVec2(FLT_MAX, FLT_MAX));
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    const std::string title = WithIcon(kIconPersonStanding, TrId("Retarget Animation"));
    if (focusRetarget) ImGui::SetNextWindowFocus();
    focusRetarget = false;
    if (!ImGui::Begin(title.c_str(), &showRetarget, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    auto rect = [&](const char* name) {
        const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
        retargetRects[name] = {lo.x, lo.y, hi.x - lo.x, hi.y - lo.y};
    };
    ImGui::TextWrapped("%s", Tr("Plays the clips of another model on this character: the body bones of both are matched, the result is written as a clip file and added to the character."));
    ImGui::Spacing();
    // The lists scroll; the Retarget button and its message stay at the bottom of the window.
    const float footer = ImGui::GetFrameHeightWithSpacing() * 2.2f + ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("##rtbody", ImVec2(0, -footer));

    const float label = font * 9.0f, button = ImGui::GetFrameHeight();
    auto modelRow = [&](const char* text, const char* id, std::string& path) {
        ImGui::PushID(id);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SameLine(label);
        ImGui::SetNextItemWidth(-(button + ImGui::GetStyle().ItemInnerSpacing.x));
        ImGui::InputText("##path", &path);
        rect(id);
        ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
        if (ImGui::Button("...", ImVec2(button, button))) ImGui::OpenPopup("##pick");
        std::string picked;
        if (AssetPicker("##pick", "model", path, picked)) path = picked;
        ImGui::PopID();
    };
    modelRow(Tr("Character"), "target", retargetTarget);
    ImGui::SetItemTooltip("%s", Tr("The model that should play the clips"));
    modelRow(Tr("Clips from"), "source", retargetSource);
    ImGui::SetItemTooltip("%s", Tr("A model file with animation clips, e.g. a motion-capture file"));
    RetargetLoad();
    std::shared_ptr<const Mesh> target = retargetTarget.empty() ? nullptr : engine.Assets().GetMesh(retargetTarget);
    std::shared_ptr<const Mesh> source = retargetSource.empty() ? nullptr : engine.Assets().GetMesh(retargetSource);
    if (!target || !source) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", Tr("Choose both models."));
        ImGui::EndChild();
        ImGui::End();
        return;
    }

    // A bone name of `mesh`, typed or picked from a filtered list.
    auto boneCombo = [&](const char* id, const Mesh& mesh, std::string& value) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (!ImGui::BeginCombo(id, value.c_str(), ImGuiComboFlags_HeightLarge)) return;
        if (ImGui::IsWindowAppearing()) { retargetFilter.clear(); ImGui::SetKeyboardFocusHere(); }
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##find", WithIcon(kIconSearch, Tr("Search bones")).c_str(), &retargetFilter);
        for (const ModelNode& node : mesh.nodes) {
            if (node.name.empty() || !HasText(node.name, retargetFilter)) continue;
            if (ImGui::Selectable(node.name.c_str(), node.name == value)) value = node.name;
        }
        ImGui::EndCombo();
    };

    ImGui::SeparatorText(Tr("Bones"));
    if (ImGui::Button(WithIcon(kIconRefreshCw, Tr("Match by Name")).c_str())) RetargetAutoMap();
    rect("auto");
    ImGui::SetItemTooltip("%s", Tr("Finds the body bones of both models by their names (Mixamo, Biped, Unreal, Unity / VRM styles) and replaces the list"));
    ImGui::SameLine();
    if (ImGui::Button(WithIcon(kIconPlus, Tr("Add Bone")).c_str())) retargetRows.push_back({});
    ImGui::SameLine();
    ImGui::TextDisabled("%s", Format(Tr("%d mapped"), static_cast<int>(retargetRows.size())).c_str());
    const float rows = ImGui::GetFrameHeightWithSpacing();
    int remove = -1;
    if (ImGui::BeginTable("##rtbones", 4, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                          ImVec2(0, rows * 8.5f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(Tr("Character bone"), ImGuiTableColumnFlags_WidthStretch, 1);
        ImGui::TableSetupColumn(Tr("Follows"), ImGuiTableColumnFlags_WidthStretch, 1);
        ImGui::TableSetupColumn(Tr("End"), ImGuiTableColumnFlags_WidthFixed, font * 3.0f);
        ImGui::TableSetupColumn("##x", ImGuiTableColumnFlags_WidthFixed, button);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < retargetRows.size(); ++i) {
            RetargetRow& row = retargetRows[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            boneCombo("##target", *target, row.target);
            ImGui::TableSetColumnIndex(1);
            boneCombo("##source", *source, row.source);
            ImGui::TableSetColumnIndex(2);
            ImGui::Checkbox("##end", &row.end);
            ImGui::SetItemTooltip("%s", Tr("Feet and toes: the bone keeps its own rest orientation instead of being laid along the other model's bone (rigs put toe bones in different places, heels tip the foot)"));
            ImGui::TableSetColumnIndex(3);
            if (ImGui::Button(kIconX, ImVec2(button, button))) remove = static_cast<int>(i);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (remove >= 0) retargetRows.erase(retargetRows.begin() + remove);

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Tr("Hips"));
    ImGui::SetItemTooltip("%s", Tr("The bone that also takes the other model's movement: the bob and sway of the body"));
    ImGui::SameLine(label);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##hips", retargetHips.empty() ? Tr("(none)") : retargetHips.c_str())) {
        if (ImGui::Selectable(Tr("(none)"), retargetHips.empty())) retargetHips.clear();
        for (const RetargetRow& row : retargetRows)
            if (!row.target.empty() && ImGui::Selectable(row.target.c_str(), row.target == retargetHips)) retargetHips = row.target;
        ImGui::EndCombo();
    }

    ImGui::SeparatorText(Tr("Clips"));
    if (retargetClips.empty()) {
        ImGui::TextDisabled("%s", Tr("This model has no clips."));
    } else if (ImGui::BeginTable("##rtclips", 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                                 ImVec2(0, rows * (std::min<float>(static_cast<float>(retargetClips.size()), 4.0f) + 1.5f)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(Tr("Clip"), ImGuiTableColumnFlags_WidthStretch, 1);
        ImGui::TableSetupColumn(Tr("Seconds"), ImGuiTableColumnFlags_WidthFixed, font * 5.0f);
        ImGui::TableSetupColumn(Tr("Name on the character"), ImGuiTableColumnFlags_WidthStretch, 1);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < retargetClips.size(); ++i) {
            RetargetClip& clip = retargetClips[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Checkbox(clip.source.c_str(), &clip.use);
            ImGui::TableSetColumnIndex(1);
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%.2f", clip.duration);
            ImGui::TableSetColumnIndex(2);
            ImGui::BeginDisabled(!clip.use);
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputText("##name", &clip.name);
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText(Tr("Result"));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Tr("Clip file"));
    ImGui::SameLine(label);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##out", &retargetOut);
    ImGui::Checkbox(Tr("Add the clips to the character"), &retargetAttach);
    ImGui::SetItemTooltip("%s", Tr("Lists the file next to the model (<model>.clips.json): the character then has these clips like its own"));

    ImGui::EndChild();
    int clips = 0, bones = 0;
    for (const RetargetClip& clip : retargetClips) clips += clip.use && !clip.name.empty();
    for (const RetargetRow& row : retargetRows) bones += !row.target.empty() && !row.source.empty();
    ImGui::Separator();
    ImGui::BeginDisabled(clips == 0 || bones == 0 || retargetOut.empty());
    if (ImGui::Button(WithIcon(kIconPersonStanding, Tr("Retarget")).c_str(), ImVec2(font * 10.0f, 0))) RetargetRun();
    ImGui::EndDisabled();
    rect("run");
    if (!retargetStatus.empty()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, retargetError ? ImVec4(0.96f, 0.45f, 0.40f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", retargetStatus.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::End();
}

}  // namespace oe
