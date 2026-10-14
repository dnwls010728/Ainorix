#include "editor/EditorInternal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include "api/Commands.h"
#include "app/Engine.h"
#include "assets/Assets.h"
#include "core/FileSystem.h"
#include "core/Log.h"
#include "assets/AnimationController.h"
#include "editor/EditorNodeCanvas.h"
#include "editor/EditorIcons.h"
#include "editor/EditorText.h"
#include "imgui_internal.h"

namespace oe {
void RegisterAnimationEditorCommands(CommandRegistry& registry) {
    struct Definition {
        const char* name;
        const char* op;
    };
    for (const Definition& item : {Definition{"animation.controller_edit", "edit"},
                                   {"animation.controller_edit_state", "state"},
                                   {"animation.controller_edit_replace", "replace"},
                                   {"animation.controller_undo", "undo"},
                                   {"animation.controller_redo", "redo"},
                                   {"animation.controller_save", "save"},
                                   {"animation.controller_close", "close"}}) {
        if (registry.Find(item.name))
            continue;
        Params schema;
        if (std::string(item.op) == "edit")
            schema.Req("path", "string", "Project .controller.json to open.")
                .Opt("discard", "boolean", "Discard a modified draft before opening another.");
        if (std::string(item.op) == "replace")
            schema.Req("values", "object", "Complete validated controller draft.")
                .Opt("merge", "string", "Coalesce one continuous gesture into one undo entry.");
        if (std::string(item.op) == "close")
            schema.Opt("discard", "boolean", "Discard unsaved edits; otherwise closing a dirty draft fails.");
        const std::string op = item.op;
        registry.Add(
            {item.name, "Native controller document: validated draft, local undo/redo and explicit save; requires an attached editor.",
             schema, false, [op](Engine& engine, const Json& args) {
                 if (const auto& handler = engine.GetEditorHandler())
                     return handler("controller_" + op, args);
                 throw ApiError("editor_unavailable", "controller document needs a native editor", "Attach with --connect to oe editor.");
                 return Json();
             }});
    }
}
Json NativeEditor::Impl::AnimationControllerCall(const std::string& op, const Json& args) {
    if (op == "edit") {
        const std::string path = args["path"].asString();
        if (path != animationController || animationControllerDocument.isNull() || args["discard"].asBool()) {
            if (!animationControllerDocument.isNull() && animationControllerDocument != animationControllerSaved &&
                !args["discard"].asBool())
                throw ApiError("unsaved_controller", "controller has unsaved edits", "Save first, or pass discard:true.");
            const Json source = engine.Call("animation.controller_get", ObjectOf({{"path", path}}));
            if (!source["ok"].asBool())
                throw ApiError(source["error"]["code"].asString(), source["error"]["message"].asString());
            animationController = path;
            animationControllerPathInput = path;
            animationControllerDocument = animationControllerSaved = source["result"]["values"];
            animationControllerHistory = {animationControllerDocument};
            animationControllerHistoryIndex = 0;
            animationControllerMerge.clear();
            animationControllerProject = engine.ProjectDir();
            animationControllerState.clear();
            animationControllerTransition = -1;
            animationControllerLink.clear();
            animationControllerFit = true;
        }
        showAnimationController = true;
        focusAnimationController = true;
    } else if (op == "replace") {
        if (animationControllerDocument.isNull())
            throw ApiError("no_controller", "no controller document is open");
        std::string error;
        if (!ValidateAnimationController(args["values"], &error))
            throw ApiError("invalid_controller", error);
        if (args["values"].dump(2).size() + 1 > 1024 * 1024)
            throw ApiError("invalid_controller", "controller exceeds 1 MiB");
        if (args["values"] != animationControllerDocument) {
            animationControllerDocument = args["values"];
            const std::string merge = args["merge"].asString();
            animationControllerHistory.resize(animationControllerHistoryIndex + 1);
            if (!merge.empty() && merge == animationControllerMerge && animationControllerHistoryIndex > 0)
                animationControllerHistory.back() = animationControllerDocument;
            else {
                animationControllerHistory.push_back(animationControllerDocument);
                if (animationControllerHistory.size() > 65)
                    animationControllerHistory.erase(animationControllerHistory.begin());
            }
            animationControllerHistoryIndex = animationControllerHistory.size() - 1;
            animationControllerMerge = merge;
        }
    } else if (op == "undo" || op == "redo") {
        if (op == "undo" && animationControllerHistoryIndex > 0)
            --animationControllerHistoryIndex;
        if (op == "redo" && animationControllerHistoryIndex + 1 < animationControllerHistory.size())
            ++animationControllerHistoryIndex;
        if (!animationControllerHistory.empty())
            animationControllerDocument = animationControllerHistory[animationControllerHistoryIndex];
        animationControllerLink.clear();
        animationControllerMerge.clear();
        animationControllerState.clear();
        animationControllerTransition = -1;
    } else if (op == "save") {
        if (animationControllerDocument.isNull())
            throw ApiError("no_controller", "no controller document is open");
        if (animationControllerProject != engine.ProjectDir())
            throw ApiError("controller_project_changed", "controller belongs to another project");
        const Json file = engine.Call("animation.controller_get", ObjectOf({{"path", animationController}}));
        if (!file["ok"].asBool() || file["result"]["values"] != animationControllerSaved)
            throw ApiError("controller_conflict", "controller file changed outside the editor",
                           "Reopen the file after saving your draft elsewhere.");
        const Json result =
            engine.Call("animation.controller_set", ObjectOf({{"path", animationController}, {"values", animationControllerDocument}}));
        if (!result["ok"].asBool())
            throw ApiError(result["error"]["code"].asString(), result["error"]["message"].asString());
        animationControllerSaved = animationControllerDocument;
        animationControllerMerge.clear();
    } else if (op == "close") {
        if (animationControllerDocument != animationControllerSaved && !args["discard"].asBool())
            throw ApiError("unsaved_controller", "controller has unsaved edits", "Save first, or pass discard:true.");
        animationControllerLive = Json();
        showAnimationController = false;
        animationControllerEditorFocused = false;
        animationControllerDocument = animationControllerSaved = Json();
        animationControllerHistory.clear();
        animationControllerHistoryIndex = 0;
        animationControllerState.clear();
        animationControllerTransition = -1;
        animationController.clear();
    }
    Json result = ObjectOf({{"open", showAnimationController},
                            {"path", animationController},
                            {"values", animationControllerDocument},
                            {"dirty", animationControllerDocument != animationControllerSaved},
                            {"canUndo", animationControllerHistoryIndex > 0},
                            {"canRedo", animationControllerHistoryIndex + 1 < animationControllerHistory.size()}});
    return result;
}
namespace {
template <class F> bool Property(const char* label, F function) {
    ImGui::PushID(label);
    bool changed = false;
    if (ImGui::BeginTable("##property", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, 112);
        ImGui::TableSetupColumn("##value");
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextWrapped("%s", label);
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        changed = function();
        ImGui::EndTable();
    }
    ImGui::PopID();
    return changed;
}
bool Choose(const char* label, std::string& value, const std::vector<std::string>& choices) {
    return Property(label, [&]() {
        bool changed = false;
        if (ImGui::BeginCombo("##value", value.c_str())) {
            for (const auto& name : choices)
                if (ImGui::Selectable(name.c_str(), name == value)) {
                    value = name;
                    changed = true;
                }
            ImGui::EndCombo();
        }
        return changed;
    });
}
Json MakeCondition(const Json& data, const std::string& name) {
    const std::string type = data["parameters"][name]["type"].asString();
    Json value = ObjectOf({{"parameter", name}, {"op", type == "trigger" ? "trigger" : "=="}});
    if (type != "trigger")
        value["value"] = data["parameters"][name]["default"];
    return value;
}
std::string Summary(const Json& transition) {
    const Json& condition = transition["conditions"][0];
    std::string text = condition["parameter"].asString();
    if (condition["op"].asString() != "trigger")
        text += " " + condition["op"].asString() + " " + condition["value"].dump();
    if (transition["conditions"].size() > 1) {
        char more[64];
        std::snprintf(more, sizeof(more), Tr("%d more"), static_cast<int>(transition["conditions"].size()) - 1);
        text += " + " + std::string(more);
    }
    return text;
}
float Distance(ImVec2 point, ImVec2 a, ImVec2 b) {
    const ImVec2 delta = b - a;
    const float length = delta.x * delta.x + delta.y * delta.y;
    const float t = length > 0 ? std::clamp(((point.x - a.x) * delta.x + (point.y - a.y) * delta.y) / length, 0.0f, 1.0f) : 0;
    const ImVec2 d = point - (a + delta * t);
    return std::sqrt(d.x * d.x + d.y * d.y);
}
ImVec2 Edge(ImVec2 position, ImVec2 dimensions, ImVec2 towards) {
    const ImVec2 center = position + dimensions * 0.5f, delta = towards - center;
    const float factor =
        std::min(dimensions.x * 0.5f / std::max(0.001f, std::fabs(delta.x)), dimensions.y * 0.5f / std::max(0.001f, std::fabs(delta.y)));
    return center + delta * factor;
}
} // namespace
void NativeEditor::Impl::AnimationControllerPanel() {
    animationControllerLive = Json();
    if (focusAnimationController) {
        ImGui::SetNextWindowFocus();
        focusAnimationController = false;
    }
    bool open = showAnimationController;
    const bool visible = ImGui::Begin(WithIcon(kIconWorkflow, TrId("Animation Controller")).c_str(), &open);
    if (!open) {
        if (animationControllerDocument != animationControllerSaved)
            animationControllerClosePrompt = true;
        else
            Call("animation.controller_close", Json());
    }
    if (!visible) {
        animationControllerEditorFocused = false;
        ImGui::End();
        return;
    }
    animationControllerEditorFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const float fontSize = ImGui::GetFontSize();
    const bool dirty = !animationControllerDocument.isNull() && animationControllerDocument != animationControllerSaved;

    // ----- Toolbar: the file (unsaved dot, picker), save / undo / redo, add state, fit, back to the timeline.
    {
        const std::string file = animationControllerPathInput.substr(animationControllerPathInput.find_last_of('/') == std::string::npos ? 0 : animationControllerPathInput.find_last_of('/') + 1);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1, 1, 1, 0.06f));
        if (ImGui::Button((WithIcon(kIconWorkflow, file) + "  " + kIconChevronDown).c_str())) ImGui::OpenPopup("##controllerFiles");
        ImGui::PopStyleColor();
        HelpTooltip(animationControllerPathInput);
        if (dirty) {
            // Unsaved changes: a dot by the name, like an editor tab.
            const ImVec2 at = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(at.x - 4, ImGui::GetItemRectMin().y + 4), 4, IM_COL32(255, 196, 90, 255));
        }
        if (ImGui::BeginPopup("##controllerFiles")) {
            ImGui::TextDisabled("%s", Tr("Controller file"));
            ImGui::SetNextItemWidth(fontSize * 22);
            if (ImGui::InputText("##controllerPath", &animationControllerPathInput, ImGuiInputTextFlags_EnterReturnsTrue)) {
                Call("editor.animation", ObjectOf({{"controller", animationControllerPathInput}}));
                ImGui::CloseCurrentPopup();
            }
            AnimationRect("controller-path");
            std::vector<std::string> files;
            for (const std::string& path : ListFiles(engine.ProjectDir(), ".json", true)) {
                const std::string relative = RelativePath(path, engine.ProjectDir());
                if (relative.size() > 16 && relative.compare(relative.size() - 16, 16, ".controller.json") == 0 && relative.rfind(".oe/", 0) != 0)
                    files.push_back(relative);
            }
            std::sort(files.begin(), files.end());
            if (!files.empty()) ImGui::Separator();
            for (const std::string& path : files)
                if (ImGui::Selectable(path.c_str(), path == animationController)) {
                    animationControllerPathInput = path;
                    Call("editor.animation", ObjectOf({{"controller", path}}));
                }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, dirty ? ImVec4(0.36f, 0.58f, 1.0f, 0.55f) : ImVec4(1, 1, 1, 0.06f));
        ImGui::BeginDisabled(animationControllerDocument.isNull());
        if (ImGui::Button(WithIcon(kIconSave, Tr("Save")).c_str())) Call("animation.controller_save", Json());
        ImGui::EndDisabled();
        ImGui::PopStyleColor();
        AnimationRect("controller-save");
        HelpTooltip(dirty ? std::string(Tr("Unsaved changes")) + "  (Ctrl+S)" : std::string("Ctrl+S"));
        ImGui::SameLine(0, 2);
        const float square = ImGui::GetFrameHeight();
        ImGui::BeginDisabled(animationControllerHistoryIndex == 0);
        if (ImGui::Button(kIconUndo, ImVec2(square, 0))) Call("animation.controller_undo", Json());
        AnimationRect("controller-undo");
        ImGui::EndDisabled();
        HelpTooltip(std::string(Tr("Undo")) + "  (Ctrl+Z)");
        ImGui::SameLine(0, 2);
        ImGui::BeginDisabled(animationControllerHistoryIndex + 1 >= animationControllerHistory.size());
        if (ImGui::Button(kIconRedo, ImVec2(square, 0))) Call("animation.controller_redo", Json());
        AnimationRect("controller-redo");
        ImGui::EndDisabled();
        HelpTooltip(std::string(Tr("Redo")) + "  (Ctrl+Y)");
        ImGui::SameLine(0, fontSize);
        animationControllerAddStateClicked = false;
        ImGui::BeginDisabled(animationControllerDocument.isNull());
        if (ImGui::Button(WithIcon(kIconPlus, Tr("Add State")).c_str())) animationControllerAddStateClicked = true;
        ImGui::EndDisabled();
        AnimationRect("controller-add-state");
        HelpTooltip(Tr("A new state using the entry state's clip; right-click the canvas to place one where you click."));
        ImGui::SameLine();
        if (ImGui::Button(WithIcon(kIconSquareDashed, Tr("Fit")).c_str())) animationControllerFit = true;
        HelpTooltip(Tr("Fit Controller Graph"));
        ImGui::SameLine();
        ImGui::BeginDisabled(animationControllerDocument.isNull());
        if (ImGui::Button(WithIcon(kIconLayoutGrid, Tr("Arrange")).c_str())) animationControllerArrangeClicked = true;
        ImGui::EndDisabled();
        AnimationRect("controller-arrange");
        HelpTooltip(Tr("Lay the states out in columns from the entry state along the transitions, with room for their labels (one undo step)."));
        ImGui::SameLine();
        if (ImGui::Button(WithIcon(kIconFilm, Tr("Timeline")).c_str())) {
            Call("editor.animation", Json());
            if (animationControllerDocument != animationControllerSaved)
                animationControllerClosePrompt = true;
            else {
                Call("animation.controller_close", Json());
                Call("editor.animation", Json());
            }
        }
        AnimationRect("controller-back");
        HelpTooltip(Tr("Back to the clip timeline (Animation panel)."));
    }
    if (animationControllerDocument.isNull()) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", Tr("No controller at this path yet."));
        if (ImGui::Button(WithIcon(kIconPlus, Tr("Create Controller")).c_str())) {
            Json definition = Json::parse(
                R"({"entry":"Idle","parameters":{"speed":{"type":"number","default":0}},"states":{"Idle":{"clip":"Idle","position":[40,100]}},"transitions":[]})");
            if (Ok(Call("animation.controller_create", ObjectOf({{"path", animationControllerPathInput}, {"values", definition}}))))
                Call("animation.controller_edit", ObjectOf({{"path", animationControllerPathInput}}));
        }
        ImGui::End();
        return;
    }
    Json data = animationControllerDocument;
    if (InPlaySession() && Primary() != kNullEntity && engine.GetScene().Exists(Primary())) {
        const Json response = Call("animation.controller_state", ObjectOf({{"id", Primary()}, {"definition", true}}), true);
        if (Ok(response))
            animationControllerLive = response["result"];
    }
    const bool found = animationControllerLive["found"].asBool();
    const bool matching = found && animationControllerProject == engine.ProjectDir() &&
                          engine.ResolvePath(animationControllerLive["path"].asString()) == engine.ResolvePath(animationController) &&
                          animationControllerLive["definition"] == data;
    animationControllerLive["matching"] = matching;
    if (found) {
        ImGui::SameLine(0, fontSize);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(matching ? ImVec4(0.47f, 0.84f, 0.63f, 1) : ImVec4(1, 0.83f, 0.37f, 1), "%s %s", kIconPlay,
                           Tr(matching ? "Live" : "Runtime differs"));
        HelpTooltip(Tr("Live display follows the selected entity and its loaded controller. Save does not reload a running controller."));
    }
    const Json& live = animationControllerLive;
    auto recent = [&](int index) -> const Json* {
        if (!matching)
            return nullptr;
        for (auto it = live["transitions"].items().rbegin(); it != live["transitions"].items().rend(); ++it)
            if ((*it)["index"].asInt() == index)
                return &*it;
        return nullptr;
    };
    auto conditionText = [](const Json& condition) {
        std::string result = condition["parameter"].asString() + " = " + condition["actual"].dump();
        if (condition["op"].asString() != "trigger")
            result += " " + condition["op"].asString() + " " + condition["value"].dump();
        return result;
    };
    auto replace = [&](const std::string& merge = std::string()) {
        return Ok(Call("animation.controller_edit_replace", ObjectOf({{"values", data}, {"merge", merge}})));
    };
    std::vector<std::string> states, parameters;
    for (const auto& s : data["states"].members())
        states.push_back(s.first);
    for (const auto& p : data["parameters"].members())
        parameters.push_back(p.first);
    if (!animationControllerState.empty() && !data["states"].has(animationControllerState))
        animationControllerState.clear();
    auto addState = [&](ImVec2 position) {
        std::string name = "State";
        int i = 1;
        while (data["states"].has(name))
            name = "State" + std::to_string(i++);
        Json state = data["states"][data["entry"].asString()];
        state["position"] = Json(Json::Array{position.x, position.y});
        data["states"][name] = state;
        if (replace()) {
            animationControllerState = name;
            animationControllerTransition = -1;
        }
    };
    auto addTransition = [&](const std::string& from, const std::string& to) {
        if (parameters.empty()) {
            Notify(Tr("Add a parameter before creating a transition."), true);
            return;
        }
        Json conditions = Json::MakeArray();
        conditions.push(MakeCondition(data, parameters[0]));
        data["transitions"].push(ObjectOf({{"from", from}, {"to", to}, {"blend", 0.2}, {"conditions", conditions}}));
        if (replace()) {
            animationControllerState.clear();
            animationControllerTransition = static_cast<int>(data["transitions"].size()) - 1;
        }
    };
    auto removeSelection = [&]() {
        if (!animationControllerState.empty() && states.size() > 1) {
            const std::string name = animationControllerState;
            data["states"].erase(name);
            if (data["entry"].asString() == name)
                data["entry"] = data["states"].members()[0].first;
            auto& items = data["transitions"].items();
            items.erase(std::remove_if(items.begin(), items.end(),
                                       [&](const Json& t) { return t["from"].asString() == name || t["to"].asString() == name; }),
                        items.end());
            if (replace())
                animationControllerState.clear();
        } else if (animationControllerTransition >= 0 && animationControllerTransition < static_cast<int>(data["transitions"].size())) {
            data["transitions"].items().erase(data["transitions"].items().begin() + animationControllerTransition);
            if (replace())
                animationControllerTransition = -1;
        }
    };
    if (animationControllerAddStateClicked) {
        // Beside the selected state, else below the others.
        ImVec2 at(40, 280);
        const Json& selected = static_cast<const Json&>(data)["states"][animationControllerState]["position"];
        if (selected.isArray()) at = ImVec2(selected[0].asFloat() + 40, selected[1].asFloat() + 140);
        addState(at);
        animationControllerAddStateClicked = false;
    }
    if (animationControllerArrangeClicked) {
        // Columns by how many transitions away from the entry state; states nothing reaches go in a last column.
        animationControllerArrangeClicked = false;
        std::map<std::string, int> depth;
        std::vector<std::string> queue{data["entry"].asString()};
        depth[queue[0]] = 0;
        for (size_t at = 0; at < queue.size(); ++at)
            for (const Json& t : data["transitions"].items())
                if (t["from"].asString() == queue[at] && data["states"].has(t["to"].asString()) && !depth.count(t["to"].asString())) {
                    depth[t["to"].asString()] = depth[queue[at]] + 1;
                    queue.push_back(t["to"].asString());
                }
        int last = 0;
        for (const auto& d : depth) last = std::max(last, d.second);
        for (const Json& t : data["transitions"].items())  // reached only from Any State: one column after the others
            if (t["from"].asString() == "*" && !depth.count(t["to"].asString())) depth[t["to"].asString()] = last + 1;
        for (const auto& name : states)
            if (!depth.count(name)) depth[name] = last + 2;
        std::map<int, int> rows;
        for (const auto& name : states) {
            const int column = depth[name], row = rows[column]++;
            data["states"][name]["position"] = Json(Json::Array{40 + column * 340, 80 + row * 190});
        }
        if (replace()) animationControllerFit = true;
    }
    if (animationControllerEditorFocused && !ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z))
            Call("animation.controller_undo", Json());
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y))
            Call("animation.controller_redo", Json());
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))
            Call("animation.controller_save", Json());
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
            removeSelection();
    }

    // ----- What the graph gets wrong, found here so the canvas can mark it: clips the selected entity does not have,
    // states nothing leads to, conditions on parameters that are gone.
    std::set<std::string> clips;  // the selected entity's clips (empty: unknown, not checked)
    if (const SpriteAnimation* animation = engine.GetScene().Get<SpriteAnimation>(Primary())) {
        const Json* list = &animation->clips;
        std::shared_ptr<const Json> definition;
        if (!animation->asset.empty() && (definition = engine.Assets().GetAnimation(animation->asset))) list = &(*definition)["clips"];
        for (const auto& clip : list->members()) clips.insert(clip.first);
    } else if (const MeshRenderer* mesh = engine.GetScene().Get<MeshRenderer>(Primary())) {
        if (auto model = engine.Assets().GetMesh(mesh->mesh))
            for (const auto& clip : model->clips) clips.insert(clip.name);
    }
    std::map<std::string, std::string> stateIssues;
    std::vector<std::string> issues;
    for (const auto& name : states) {
        const std::string clip = static_cast<const Json&>(data)["states"][name]["clip"].asString();
        if (!clips.empty() && !clips.count(clip)) {
            stateIssues[name] = Format(Tr("Clip '%s' is not one of the selected entity's clips."), clip.c_str());
            issues.push_back(name + ": " + stateIssues[name]);
        }
        bool reached = name == data["entry"].asString();
        for (const Json& t : data["transitions"].items()) reached = reached || t["to"].asString() == name;
        if (!reached) {
            const std::string text = Tr("No transition leads here.");
            stateIssues[name] += stateIssues[name].empty() ? text : "\n" + text;
            issues.push_back(name + ": " + text);
        }
    }
    for (size_t i = 0; i < data["transitions"].size(); ++i)
        for (const Json& c : data["transitions"][i]["conditions"].items())
            if (!data["parameters"].has(c["parameter"].asString()))
                issues.push_back(Format(Tr("Transition %s > %s: no parameter '%s'."), data["transitions"][i]["from"].asString().c_str(),
                                        data["transitions"][i]["to"].asString().c_str(), c["parameter"].asString().c_str()));
    if (!issues.empty()) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(1, 0.78f, 0.35f, 1), "%s  %s", kIconTriangleAlert,
                           Format(Tr("%d issues: %s"), static_cast<int>(issues.size()), issues[0].c_str()).c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            for (const auto& issue : issues) ImGui::BulletText("%s", issue.c_str());
            ImGui::EndTooltip();
        }
        AnimationRect("controller-issues");
    }

    const float properties = std::clamp(ImGui::GetContentRegionAvail().x * 0.32f, fontSize * 16, fontSize * 24);
    ImGui::BeginChild("##controllerCanvasHost", ImVec2(std::max(180.0f, ImGui::GetContentRegionAvail().x - properties - 8), 0));
    {
        EditorNodeCanvas canvas("##controllerCanvas", animationControllerPan, animationControllerZoom);
        std::map<std::string, ImVec2> positions;
        for (size_t i = 0; i < states.size(); ++i) {
            const Json& p = static_cast<const Json&>(data)["states"][states[i]]["position"];
            positions[states[i]] = p.isArray() ? ImVec2(p[0].asFloat(), p[1].asFloat())
                                               : ImVec2(40 + static_cast<float>(i % 3) * 260, 100 + static_cast<float>(i / 3) * 180);
        }
        positions["*"] = ImVec2(40, -30);
        const ImVec2 entryPosition(260, -30), card(200, 84), special(170, 40);
        // A newly docked window acquires its final dimensions after its first frame.
        ImGuiStorage* storage = ImGui::GetStateStorage();
        const ImGuiID sizeX = ImGui::GetID("fitWidth"), sizeY = ImGui::GetID("fitHeight");
        if (storage->GetFloat(sizeX) != canvas.size.x || storage->GetFloat(sizeY) != canvas.size.y) {
            animationControllerFit = true;
            storage->SetFloat(sizeX, canvas.size.x);
            storage->SetFloat(sizeY, canvas.size.y);
        }
        if (animationControllerFit) {
            ImVec2 low(0, -70), high(440, 40);
            for (const auto& p : positions) {
                low.x = std::min(low.x, p.second.x - 30);
                low.y = std::min(low.y, p.second.y - 30);
                high.x = std::max(high.x, p.second.x + card.x + 30);
                high.y = std::max(high.y, p.second.y + card.y + 30);
            }
            canvas.Fit(low, high);
            if (animationControllerZoom < 0.6f) {
                animationControllerZoom = 0.6f;
                animationControllerPan = ImVec2(20, 50);
            }
            animationControllerFit = false;
        }
        canvas.Navigate();
        animationRects["controller-canvas"] = {canvas.origin.x, canvas.origin.y, canvas.size.x, canvas.size.y};
        bool wireHovered = false;
        const ImU32 entryColor = IM_COL32(96, 196, 132, 255), anyColor = IM_COL32(168, 140, 220, 255), wireColor = IM_COL32(112, 202, 242, 255);
        auto wire = [&](ImVec2 a, ImVec2 b, const std::string& label, int index, float offset, ImU32 baseColor) {
            const ImVec2 from = canvas.Point(a), to = canvas.Point(b);
            const float bend = std::max(30.0f, std::fabs(to.x - from.x) * 0.4f);
            const ImVec2 c1 = from + ImVec2(bend, offset), c2 = to - ImVec2(bend, -offset);
            auto point = [&](float t) {
                float u = 1 - t;
                return from * (u * u * u) + c1 * (3 * u * u * t) + c2 * (3 * u * t * t) + to * (t * t * t);
            };
            const bool chosen = index >= 0 && index == animationControllerTransition;
            const Json* observation = recent(index);
            const float age = observation ? static_cast<float>(live["clock"].asNumber() - (*observation)["time"].asNumber()) : 10;
            const bool flowing = observation && age >= 0 && age < 0.8f;
            std::string caption = label;
            if (flowing && (*observation)["conditions"].size() > 0)
                caption = conditionText((*observation)["conditions"][0]);
            // The label sits beside the middle of the curve, off the line, in a pill.
            const float font = ImGui::GetFontSize() * std::clamp(animationControllerZoom, 0.75f, 1.0f);
            const ImVec2 labelSize = caption.empty() ? ImVec2() : ImGui::GetFont()->CalcTextSizeA(font, 10000, 0, caption.c_str());
            const ImVec2 middle = point(0.5f);
            ImVec2 along = point(0.52f) - point(0.48f);
            const float alongLength = std::sqrt(along.x * along.x + along.y * along.y);
            if (alongLength > 0) along = along / alongLength;
            const ImVec2 side(-along.y, along.x);
            const float lift = labelSize.y * 0.5f + 8;
            const ImVec2 at = middle + side * (side.y > 0 ? -lift : lift) - labelSize * 0.5f;
            bool hit = false;
            if (index >= 0) {
                ImVec2 prev = from;
                for (int i = 1; i <= 24; ++i) {
                    ImVec2 next = point(i / 24.0f);
                    hit = hit || Distance(ImGui::GetIO().MousePos, prev, next) < 7;
                    prev = next;
                }
                hit = (hit || ImGui::IsMouseHoveringRect(at - ImVec2(6, 3), at + labelSize + ImVec2(6, 3))) && ImGui::IsWindowHovered();
            }
            const ImU32 color = flowing ? IM_COL32(120, 214, 160, 255) : chosen ? IM_COL32(255, 213, 116, 255) : baseColor;
            const float width = flowing ? 4.0f : chosen ? 3.5f : hit ? 3.0f : 2.0f;
            canvas.draw->AddBezierCubic(from, c1, c2, to, IM_COL32(12, 14, 18, 200), width + 3);  // keeps it readable over the grid
            if (flowing && chosen)
                canvas.draw->AddBezierCubic(from, c1, c2, to, IM_COL32(255, 213, 116, 255), 6);
            canvas.draw->AddBezierCubic(from, c1, c2, to, color, width);
            if (flowing)
                canvas.draw->AddCircleFilled(point(std::fmod(age * 2, 1.0f)), 4.5f, IM_COL32(245, 255, 249, 255));
            // Arrowheads at the middle and the end show the direction at a glance.
            auto arrow = [&](ImVec2 tip, ImVec2 direction, float size) {
                const ImVec2 normal(-direction.y, direction.x);
                canvas.draw->AddTriangleFilled(tip, tip - direction * size + normal * size * 0.55f, tip - direction * size - normal * size * 0.55f, color);
            };
            ImVec2 direction = to - point(0.97f);
            const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y);
            if (length > 0) direction = direction / length;
            arrow(to, direction, 11);
            arrow(middle + along * 6, along, 9);
            if (!caption.empty()) {
                canvas.draw->AddRectFilled(at - ImVec2(6, 3), at + labelSize + ImVec2(6, 3), IM_COL32(24, 28, 36, 245), labelSize.y);
                canvas.draw->AddRect(at - ImVec2(6, 3), at + labelSize + ImVec2(6, 3), chosen || hit ? color : IM_COL32(color >> IM_COL32_R_SHIFT & 0xFF, color >> IM_COL32_G_SHIFT & 0xFF, color >> IM_COL32_B_SHIFT & 0xFF, 110), labelSize.y, 1.2f);
                canvas.draw->AddText(ImGui::GetFont(), font, at, chosen || hit ? IM_COL32(255, 255, 255, 255) : IM_COL32(210, 226, 240, 255), caption.c_str());
            }
            if (index >= 0) {
                wireHovered = wireHovered || hit;
                if (hit && observation) {
                    ImGui::BeginTooltip();
                    for (const Json& condition : (*observation)["conditions"].items())
                        ImGui::TextUnformatted(conditionText(condition).c_str());
                    ImGui::EndTooltip();
                } else if (hit) {
                    const Json& t = data["transitions"][index];
                    ImGui::BeginTooltip();
                    ImGui::Text("%s  %s  %s", t["from"].asString() == "*" ? Tr("Any State") : t["from"].asString().c_str(), kIconChevronRight, t["to"].asString().c_str());
                    for (const Json& c : t["conditions"].items())
                        ImGui::BulletText("%s %s %s", c["parameter"].asString().c_str(), c["op"].asString().c_str(), c.has("value") ? c["value"].dump().c_str() : "");
                    ImGui::EndTooltip();
                }
                animationRects["controller-transition:" + std::to_string(index)] = {at.x - 6, at.y - 3, labelSize.x + 12, labelSize.y + 6};
                if (flowing)
                    animationRects["controller-live-transition:" + std::to_string(index)] = {at.x, at.y, labelSize.x, labelSize.y};
                if (hit && ImGui::IsMouseClicked(0)) {
                    animationControllerTransition = index;
                    animationControllerState.clear();
                }
                if (hit && ImGui::IsMouseClicked(1)) {
                    animationControllerTransition = index;
                    animationControllerState.clear();
                    ImGui::OpenPopup("##transitionMenu");
                }
            }
        };
        wire(Edge(entryPosition, special, positions[data["entry"].asString()] + card * 0.5f),
             Edge(positions[data["entry"].asString()], card, entryPosition + special * 0.5f), "", -1, 0, entryColor);
        for (size_t i = 0; i < data["transitions"].size(); ++i) {
            const Json& t = data["transitions"][i];
            const std::string from = t["from"].asString(), to = t["to"].asString();
            const ImVec2 a = positions[from], b = positions[to];
            bool reverse = false;
            for (const Json& other : data["transitions"].items())
                reverse = reverse || (other["from"].asString() == to && other["to"].asString() == from);
            wire(Edge(a, from == "*" ? special : card, b + card * 0.5f), Edge(b, card, a + (from == "*" ? special : card) * 0.5f),
                 Summary(t), static_cast<int>(i), reverse ? (from < to ? -44.0f : 44.0f) : 24.0f, from == "*" ? anyColor : wireColor);
        }
        ImVec2 entry = entryPosition;
        ImGui::PushID("entry");
        canvas.Card(entry, special, WithIcon(kIconPlay, Tr("Entry State")).c_str(), IM_COL32(46, 112, 76, 255), false);
        HelpTooltip(Tr("Where the controller starts. Right-click a state: Set Entry State."));
        ImGui::PopID();
        ImVec2 any = positions["*"];
        ImGui::PushID("any");
        canvas.Card(any, special, WithIcon(kIconGlobe, Tr("Any State")).c_str(), IM_COL32(92, 72, 132, 255), false);
        HelpTooltip(Tr("Transitions from here are checked whatever state is playing (hits, deaths). Drag its dot onto a state."));
        canvas.Pin("##output", canvas.Point(any + ImVec2(special.x, special.y * 0.5f)), true);
        if (ImGui::IsItemActivated())
            animationControllerLink = "*";
        ImGui::PopID();
        bool moved = false;
        for (const auto& name : states) {
            ImGui::PushID(name.c_str());
            ImVec2 position = positions[name], before = position;
            const bool activeState = matching && live["state"].asString() == name;
            const bool isEntry = data["entry"].asString() == name;
            const auto issue = stateIssues.find(name);
            std::string badge = isEntry ? std::string(Tr("Entry")) : std::string();
            if (issue != stateIssues.end()) badge = std::string(kIconTriangleAlert) + (badge.empty() ? "" : " " + badge);
            canvas.Card(position, card, name.c_str(), activeState ? IM_COL32(46, 112, 76, 255) : isEntry ? IM_COL32(52, 104, 92, 255) : IM_COL32(55, 88, 125, 255),
                        animationControllerState == name, false, badge.c_str());
            const bool cardHovered = ImGui::IsItemHovered();
            const ImVec2 p = canvas.Point(position), end = canvas.Point(position + card);
            if (isEntry && !activeState) canvas.draw->AddRect(p, end, entryColor, 6 * animationControllerZoom, 1.5f);
            if (issue != stateIssues.end()) canvas.draw->AddRect(p, end, IM_COL32(255, 196, 90, 200), 6 * animationControllerZoom, 1.5f);
            if (cardHovered) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(name.c_str());
                ImGui::TextDisabled("%s %s", Tr("State Clip"), static_cast<const Json&>(data)["states"][name]["clip"].asString().c_str());
                if (issue != stateIssues.end()) ImGui::TextColored(ImVec4(1, 0.78f, 0.35f, 1), "%s", issue->second.c_str());
                ImGui::EndTooltip();
            }
            if (activeState) {
                canvas.draw->AddRect(p, end, IM_COL32(120, 214, 160, 255), 4, 2.5f);
                const double duration = live["playback"]["duration"].asNumber(), playbackTime = live["playback"]["time"].asNumber();
                const float fraction = duration > 0 ? static_cast<float>(std::clamp(playbackTime / duration, 0.0, 1.0)) : 0;
                canvas.draw->AddRectFilled(ImVec2(p.x, end.y - 4), ImVec2(p.x + (end.x - p.x) * fraction, end.y), IM_COL32(120, 214, 160, 255));
                char seconds[48];
                std::snprintf(seconds, sizeof(seconds), "%.2f / %.2f s", playbackTime, duration);
                canvas.draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * animationControllerZoom * 0.75f,
                                     canvas.Point(position + ImVec2(12, 64)), IM_COL32(185, 233, 207, 255), seconds);
            }
            AnimationRect("controller-state:" + name);
            if (activeState)
                AnimationRect("controller-live-state:" + name);
            if (ImGui::IsItemClicked()) {
                animationControllerState = name;
                animationControllerTransition = -1;
            }
            if (ImGui::BeginPopupContextItem("##stateMenu")) {
                animationControllerState = name;
                animationControllerTransition = -1;
                if (ImGui::MenuItem(Tr("Set Entry State"), nullptr, isEntry)) {
                    data["entry"] = name;
                    replace();
                }
                if (states.size() > 1 && ImGui::MenuItem(Tr("Remove State")))
                    removeSelection();
                ImGui::EndPopup();
            }
            if (position.x != before.x || position.y != before.y) {
                position.x = std::clamp(position.x, -100000.0f, 100000.0f);
                position.y = std::clamp(position.y, -100000.0f, 100000.0f);
                data["states"][name]["position"] = Json(Json::Array{position.x, position.y});
                moved = true;
            }
            // Body: the clip it plays, whether it loops, its speed when not 1.
            const float font = ImGui::GetFontSize() * animationControllerZoom;
            if (font >= 7) {
                const Json& values = static_cast<const Json&>(data)["states"][name];
                canvas.draw->PushClipRect(canvas.Point(position + ImVec2(0, 30)), canvas.Point(position + card - ImVec2(14, 0)), true);
                const std::string clipLine = std::string(kIconFilm) + "  " + values["clip"].asString();
                canvas.draw->AddText(ImGui::GetFont(), font, canvas.Point(position + ImVec2(12, 38)),
                                     issue != stateIssues.end() && !clips.empty() && !clips.count(values["clip"].asString()) ? IM_COL32(255, 196, 90, 255) : IM_COL32(222, 232, 243, 255),
                                     clipLine.c_str());
                std::string detail = values["loop"].asBool(true) ? std::string(kIconRefreshCw) + " " + Tr("Loop") : std::string(kIconStepForward) + " " + Tr("Once");
                if (values["speed"].asFloat(1) != 1.0f) detail += Format("   x%.2g", values["speed"].asFloat(1));
                canvas.draw->AddText(ImGui::GetFont(), font * 0.85f, canvas.Point(position + ImVec2(12, 60)), IM_COL32(160, 172, 190, 255), detail.c_str());
                canvas.draw->PopClipRect();
            }
            canvas.Pin("##output", canvas.Point(position + ImVec2(card.x, card.y * 0.5f)), true);
            AnimationRect("controller-pin:" + name);
            HelpTooltip(Tr("Drag onto another state to add a transition."));
            if (ImGui::IsItemActivated())
                animationControllerLink = name;
            ImGui::PopID();
        }
        if (ImGui::IsMouseClicked(0))
            ++dragSerial["controller-position"];
        if (moved)
            replace("controller-drag:" + std::to_string(dragSerial["controller-position"]));
        if (!animationControllerLink.empty()) {
            canvas.Wire(canvas.Point(positions[animationControllerLink] + ImVec2(animationControllerLink == "*" ? special.x : card.x,
                                                                                 (animationControllerLink == "*" ? special.y : card.y) * 0.5f)),
                        ImGui::GetIO().MousePos, IM_COL32(255, 213, 116, 255), 1.6f);
            if (ImGui::IsMouseReleased(0)) {
                const std::string from = animationControllerLink;
                animationControllerLink.clear();
                for (const auto& name : states) {
                    const ImVec2 p = canvas.Point(positions[name]);
                    if (name != from && ImGui::IsMouseHoveringRect(p, p + card * animationControllerZoom)) {
                        addTransition(from, name);
                        break;
                    }
                }
            }
        }
        if (ImGui::BeginPopup("##transitionMenu")) {
            if (ImGui::MenuItem(Tr("Remove Transition")))
                removeSelection();
            ImGui::EndPopup();
        }
        if (!wireHovered && (ImGui::IsWindowHovered() && ImGui::GetCurrentContext()->HoveredId == 0) && ImGui::IsMouseClicked(0) &&
            animationControllerLink.empty()) {
            bool onLine = false;
            for (size_t i = 0; i < data["transitions"].size(); ++i) {
                const auto r = animationRects["controller-transition:" + std::to_string(i)];
                onLine = onLine || ImGui::IsMouseHoveringRect(ImVec2(r[0], r[1]), ImVec2(r[0] + r[2], r[1] + r[3]));
            }
            if (!onLine) {
                animationControllerState.clear();
                animationControllerTransition = -1;
            }
        }
        if (!wireHovered && (ImGui::IsWindowHovered() && ImGui::GetCurrentContext()->HoveredId == 0) && ImGui::IsMouseClicked(1)) {
            animationControllerNewAt = canvas.Position(ImGui::GetIO().MousePos);
            ImGui::OpenPopup("##canvasMenu");
        }
        if (ImGui::BeginPopup("##canvasMenu")) {
            if (ImGui::MenuItem(WithIcon(kIconPlus, Tr("Add State")).c_str()))
                addState(animationControllerNewAt);
            ImGui::EndPopup();
        }
        canvas.Footer(Tr("Drag a state's right dot onto another state for a transition. Right-click empty space to add a state. Wheel zooms."));
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // ----- Side panel: what is running (play), the selection, the parameters.
    ImGui::BeginChild("##controllerProperties", ImVec2(0, 0), ImGuiChildFlags_Borders);
    auto heading = [&](const char* icon, const std::string& text) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.82f, 0.85f, 0.92f, 1));
        ImGui::TextUnformatted(WithIcon(icon, text).c_str());
        ImGui::PopStyleColor();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(at, at + ImVec2(ImGui::GetContentRegionAvail().x, 0), IM_COL32(255, 255, 255, 28));
        ImGui::Dummy(ImVec2(0, 3));
    };
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (matching) {
        const bool paused = live["paused"].asBool();
        heading(kIconPlay, Format("%s: %s", Tr(paused ? "Paused" : "Live"), live["state"].asString().c_str()));
        Property(Tr("State Time"), [&]() {
            ImGui::Text("%.3f s", live["elapsed"].asNumber());
            return false;
        });
        ImGui::TextDisabled("%s", Tr("Current Parameters"));
        for (const auto& parameter : live["parameters"].members())
            Property(parameter.first.c_str(), [&]() {
                ImGui::TextUnformatted(parameter.second.dump().c_str());
                return false;
            });
        ImGui::TextDisabled("%s", Tr("Recent Transitions"));
        const Json* detail = nullptr;
        const auto& trace = live["transitions"].items();
        for (size_t i = trace.size() > 4 ? trace.size() - 4 : 0; i < trace.size(); ++i) {
            const Json& event = trace[i];
            ImGui::PushID(static_cast<int>(i));
            const std::string text = event["from"].asString() + "  " + kIconChevronRight + "  " + event["to"].asString();
            if (ImGui::Selectable(text.c_str(), animationControllerTransition == event["index"].asInt())) {
                animationControllerTransition = event["index"].asInt();
                animationControllerState.clear();
            }
            if (animationControllerTransition == event["index"].asInt())
                detail = &event;
            ImGui::PopID();
        }
        if (!detail && !trace.empty())
            detail = &trace.back();
        if (detail)
            for (const Json& condition : (*detail)["conditions"].items())
                ImGui::TextWrapped("%s", conditionText(condition).c_str());
    } else if (found) {
        ImGui::TextWrapped("%s", Tr("Runtime definition differs from this graph. Restart play after saving to monitor it."));
    } else if (InPlaySession()) {
        ImGui::TextWrapped("%s", Tr("No live controller for the selected entity."));
    }
    std::vector<std::string> clipChoices(clips.begin(), clips.end());
    if (!animationControllerState.empty()) {
        const std::string selectedName = animationControllerState;
        Json& state = data["states"][selectedName];
        const Json& values = state;
        heading(kIconSquare, std::string(Tr("Selected State")) + ": " + selectedName);
        std::string name = selectedName;
        if (Property(Tr("State Name"),
                     [&]() {
                         const bool changed = ImGui::InputText("##value", &name, ImGuiInputTextFlags_EnterReturnsTrue);
                         AnimationRect("controller-state-name");
                         return changed;
                     }) &&
            name != selectedName && !data["states"].has(name)) {
            const Json copy = state;
            data["states"][name] = copy;
            data["states"].erase(selectedName);
            if (data["entry"].asString() == selectedName)
                data["entry"] = name;
            for (Json& t : data["transitions"].items()) {
                if (t["from"].asString() == selectedName)
                    t["from"] = name;
                if (t["to"].asString() == selectedName)
                    t["to"] = name;
            }
            if (replace())
                animationControllerState = name;
            ImGui::EndChild();
            ImGui::End();
            return;
        }
        std::string clip = values["clip"].asString();
        if (Property(Tr("State Clip"), [&]() {
                // The selected entity's clips to pick from; any name can still be typed.
                bool changed = false;
                const float pick = clipChoices.empty() ? 0.0f : ImGui::GetFrameHeight();
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - pick - (pick > 0 ? 2 : 0));
                changed = ImGui::InputText("##value", &clip, ImGuiInputTextFlags_EnterReturnsTrue);
                if (pick > 0) {
                    ImGui::SameLine(0, 2);
                    if (ImGui::Button(kIconChevronDown, ImVec2(pick, 0))) ImGui::OpenPopup("##clips");
                    if (ImGui::BeginPopup("##clips")) {
                        for (const auto& choice : clipChoices)
                            if (ImGui::Selectable(choice.c_str(), choice == clip)) clip = choice, changed = true;
                        ImGui::EndPopup();
                    }
                }
                return changed;
            })) {
            state["clip"] = clip;
            replace();
        }
        if (!clips.empty() && !clips.count(clip)) ImGui::TextColored(ImVec4(1, 0.78f, 0.35f, 1), "%s %s", kIconTriangleAlert, Tr("Not a clip of the selected entity."));
        float speed = values["speed"].asFloat(1);
        if (Property(Tr("State Speed"),
                     [&]() { return ImGui::InputFloat("##value", &speed, 0, 0, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue); })) {
            state["speed"] = speed;
            replace();
        }
        bool loop = values["loop"].asBool(true);
        if (Property(Tr("Loop Clip"), [&]() { return ImGui::Checkbox("##value", &loop); })) {
            state["loop"] = loop;
            replace();
        }
        ImGui::BeginDisabled(data["entry"].asString() == selectedName);
        if (ImGui::Button(WithIcon(kIconPlay, Tr("Set Entry State")).c_str(), ImVec2(half, 0))) {
            data["entry"] = selectedName;
            replace();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(WithIcon(kIconEye, Tr("Preview State")).c_str(), ImVec2(-FLT_MIN, 0)))
            Call("animation.preview",
                 ObjectOf({{"id", Primary()}, {"clip", clip}, {"speed", speed}, {"loop", loop}, {"time", 0}, {"playing", false}}));
        AnimationRect("controller-preview");
        HelpTooltip(Tr("Play this state's clip in the Animation panel."));
        if (states.size() > 1 && ImGui::Button(WithIcon(kIconPlus, Tr("Add Transition")).c_str(), ImVec2(half, 0)))
            addTransition(selectedName, states[0] == selectedName ? states[1] : states[0]);
        AnimationRect("controller-add-transition");
        if (states.size() > 1) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.28f, 0.25f, 0.45f));
            if (ImGui::Button(WithIcon(kIconTrash2, Tr("Remove State")).c_str(), ImVec2(-FLT_MIN, 0)))
                removeSelection();
            ImGui::PopStyleColor();
        }
    } else if (animationControllerTransition >= 0 && animationControllerTransition < static_cast<int>(data["transitions"].size())) {
        const int index = animationControllerTransition;
        Json& t = data["transitions"][index];
        const Json& values = t;
        heading(kIconChevronRight, Format("%s %d / %d", Tr("Transition"), index + 1, static_cast<int>(data["transitions"].size())));
        std::string from = t["from"].asString(), to = t["to"].asString();
        auto origins = states;
        origins.push_back("*");
        if (Choose(Tr("From State"), from, origins)) {
            t["from"] = from;
            replace();
        }
        if (Choose(Tr("To State"), to, states)) {
            t["to"] = to;
            replace();
        }
        ImGui::TextDisabled("%s", Tr("Conditions (all must hold)"));
        // One row per condition: parameter, operator, value, remove.
        if (ImGui::BeginTable("##conditions", 4, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("##p", ImGuiTableColumnFlags_WidthStretch, 0.4f);
            ImGui::TableSetupColumn("##o", ImGuiTableColumnFlags_WidthFixed, fontSize * 4.2f);
            ImGui::TableSetupColumn("##v", ImGuiTableColumnFlags_WidthStretch, 0.3f);
            ImGui::TableSetupColumn("##x", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
            for (size_t i = 0; i < t["conditions"].size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                ImGui::TableNextRow();
                Json& c = t["conditions"][i];
                std::string name = c["parameter"].asString();
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo("##parameter", name.c_str())) {
                    for (const auto& p : parameters)
                        if (ImGui::Selectable(p.c_str(), p == name)) {
                            c = MakeCondition(data, p);
                            replace();
                        }
                    ImGui::EndCombo();
                }
                const std::string type = data["parameters"][name]["type"].asString();
                std::string op = c["op"].asString();
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                const std::vector<std::string> ops = type == "trigger"   ? std::vector<std::string>{"trigger"}
                                                     : type == "boolean" ? std::vector<std::string>{"==", "!="}
                                                                         : std::vector<std::string>{"==", "!=", ">", ">=", "<", "<="};
                if (ImGui::BeginCombo("##op", op.c_str())) {
                    for (const auto& choice : ops)
                        if (ImGui::Selectable(choice.c_str(), choice == op)) {
                            c["op"] = choice;
                            replace();
                        }
                    ImGui::EndCombo();
                }
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (type == "number") {
                    double value = c["value"].asNumber();
                    if (ImGui::InputDouble("##value", &value, 0, 0, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) {
                        c["value"] = value;
                        replace();
                    }
                } else if (type == "boolean") {
                    bool value = c["value"].asBool();
                    if (ImGui::Checkbox("##value", &value)) {
                        c["value"] = value;
                        replace();
                    }
                } else {
                    ImGui::TextDisabled("%s", Tr("fires once"));
                }
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(t["conditions"].size() <= 1);
                const bool remove = ImGui::Button(kIconTrash2, ImVec2(-FLT_MIN, 0));
                ImGui::EndDisabled();
                HelpTooltip(Tr("Remove Condition"));
                ImGui::PopID();
                if (remove) {
                    t["conditions"].items().erase(t["conditions"].items().begin() + static_cast<ptrdiff_t>(i));
                    replace();
                    break;
                }
            }
            ImGui::EndTable();
        }
        if (!parameters.empty() && ImGui::Button(WithIcon(kIconPlus, Tr("Add Condition")).c_str())) {
            t["conditions"].push(MakeCondition(data, parameters[0]));
            replace();
        }
        float blend = values["blend"].asFloat(), minimum = values["minTime"].asFloat();
        if (Property(Tr("Blend Seconds"),
                     [&]() { return ImGui::InputFloat("##value", &blend, 0, 0, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue); })) {
            t["blend"] = blend;
            replace();
        }
        if (Property(Tr("Minimum State Seconds"),
                     [&]() { return ImGui::InputFloat("##value", &minimum, 0, 0, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue); })) {
            t["minTime"] = minimum;
            replace();
        }
        // Order: earlier transitions win when several hold.
        ImGui::BeginDisabled(index == 0);
        if (ImGui::Button(Tr("Transition Earlier"), ImVec2(half, 0))) {
            std::swap(data["transitions"][index], data["transitions"][index - 1]);
            if (replace())
                --animationControllerTransition;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(index + 1 >= static_cast<int>(data["transitions"].size()));
        if (ImGui::Button(Tr("Transition Later"), ImVec2(-FLT_MIN, 0))) {
            std::swap(data["transitions"][index], data["transitions"][index + 1]);
            if (replace())
                ++animationControllerTransition;
        }
        ImGui::EndDisabled();
        HelpTooltip(Tr("When several transitions could fire, the earlier one wins."));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.28f, 0.25f, 0.45f));
        if (ImGui::Button(WithIcon(kIconTrash2, Tr("Remove Transition")).c_str(), ImVec2(-FLT_MIN, 0)))
            removeSelection();
        ImGui::PopStyleColor();
    } else {
        heading(kIconMousePointer2, Tr("Selection"));
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", Tr("Click a state or a transition label to edit it here. Drag a state's right dot onto another state to connect them."));
        ImGui::PopTextWrapPos();
    }

    // Parameters: what scripts set (animator:set / trigger) and transitions test. One row each.
    auto typeLabel = [](const std::string& type) { return type == "number" ? Tr("Number") : type == "boolean" ? Tr("Boolean") : Tr("Trigger"); };
    heading(kIconSlidersHorizontal, Format("%s (%d)", Tr("Controller Parameters"), static_cast<int>(parameters.size())));
    if (ImGui::BeginTable("##parameters", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableSetupColumn("##type", ImGuiTableColumnFlags_WidthFixed, fontSize * 4.5f);
        ImGui::TableSetupColumn("##default", ImGuiTableColumnFlags_WidthStretch, 0.35f);
        ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        for (const auto& name : parameters) {
            ImGui::PushID(name.c_str());
            Json& p = data["parameters"][name];
            const std::string type = p["type"].asString();
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(name.c_str());
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(type == "number" ? ImVec4(0.55f, 0.75f, 1, 1) : type == "boolean" ? ImVec4(0.55f, 0.9f, 0.6f, 1) : ImVec4(1, 0.72f, 0.45f, 1), "%s",
                               typeLabel(type));
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            bool changed = false;
            if (type == "number") {
                double value = p["default"].asNumber();
                if (ImGui::InputDouble("##default", &value, 0, 0, "%.3g", ImGuiInputTextFlags_EnterReturnsTrue)) p["default"] = value, changed = true;
                HelpTooltip(Tr("Default Value"));
            } else if (type == "boolean") {
                bool value = p["default"].asBool();
                if (ImGui::Checkbox("##default", &value)) p["default"] = value, changed = true;
                HelpTooltip(Tr("Default Value"));
            }
            ImGui::TableNextColumn();
            const bool remove = ImGui::Button(kIconTrash2, ImVec2(-FLT_MIN, 0));
            HelpTooltip(Tr("Remove Parameter"));
            ImGui::PopID();
            if (changed) replace();
            if (remove) {
                data["parameters"].erase(name);
                replace();
                break;
            }
        }
        ImGui::EndTable();
    }
    if (parameters.empty()) ImGui::TextDisabled("%s", Tr("None yet. Transitions test parameters; add one first."));
    {
        // New parameter: name, type, add - one row.
        const float type = fontSize * 5.5f, add = ImGui::CalcTextSize(WithIcon(kIconPlus, Tr("Add")).c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SetNextItemWidth(std::max(fontSize * 4, ImGui::GetContentRegionAvail().x - type - add - ImGui::GetStyle().ItemSpacing.x * 2));
        ImGui::InputTextWithHint("##parameterName", Tr("Parameter Name"), &animationControllerParameterName);
        AnimationRect("controller-parameter-name");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(type);
        if (ImGui::BeginCombo("##parameterType", typeLabel(animationControllerParameterType))) {
            for (const char* choice : {"number", "boolean", "trigger"})
                if (ImGui::Selectable(typeLabel(choice),
                                      animationControllerParameterType == choice))
                    animationControllerParameterType = choice;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        const bool exists = data["parameters"].has(animationControllerParameterName);
        ImGui::BeginDisabled(animationControllerParameterName.empty() || exists);
        if (ImGui::Button(WithIcon(kIconPlus, Tr("Add")).c_str())) {
            data["parameters"][animationControllerParameterName] =
                ObjectOf({{"type", animationControllerParameterType},
                          {"default", animationControllerParameterType == "number" ? Json(0) : Json(false)}});
            replace();
        }
        ImGui::EndDisabled();
        AnimationRect("controller-add-parameter");
        HelpTooltip(exists ? std::string(Tr("A parameter with this name exists.")) : std::string(Tr("Add Parameter")));
    }
    ImGui::EndChild();
    if (animationControllerClosePrompt) {
        ImGui::OpenPopup(TrId("Save Controller Changes?").c_str());
        animationControllerClosePrompt = false;
    }
    if (ImGui::BeginPopupModal(TrId("Save Controller Changes?").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::Button(Tr("Save")) && Ok(Call("animation.controller_save", Json()))) {
            Call("animation.controller_close", Json());
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr("Discard"))) {
            Call("animation.controller_close", ObjectOf({{"discard", true}}));
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr("Cancel")))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::End();
}
} // namespace oe
