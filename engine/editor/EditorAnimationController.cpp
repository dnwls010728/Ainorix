#include "editor/EditorInternal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "api/Commands.h"
#include "app/Engine.h"
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
    const float toolbarWidth = ImGui::GetContentRegionAvail().x;

    ImGui::SetNextItemWidth(std::max(120.0f, toolbarWidth - 420));
    if (ImGui::InputText("##controllerPath", &animationControllerPathInput, ImGuiInputTextFlags_EnterReturnsTrue))
        Call("editor.animation", ObjectOf({{"controller", animationControllerPathInput}}));
    AnimationRect("controller-path");
    ImGui::SameLine();
    if (ImGui::SmallButton(WithIcon(kIconSave, Tr("Save")).c_str()))
        Call("animation.controller_save", Json());
    AnimationRect("controller-save");
    HelpTooltip("Ctrl+S");
    ImGui::SameLine();
    ImGui::BeginDisabled(animationControllerHistoryIndex == 0);
    if (ImGui::SmallButton(Tr("Undo")))
        Call("animation.controller_undo", Json());
    AnimationRect("controller-undo");
    ImGui::EndDisabled();
    HelpTooltip("Ctrl+Z");
    ImGui::SameLine();
    ImGui::BeginDisabled(animationControllerHistoryIndex + 1 >= animationControllerHistory.size());
    if (ImGui::SmallButton(Tr("Redo")))
        Call("animation.controller_redo", Json());
    AnimationRect("controller-redo");
    ImGui::EndDisabled();
    HelpTooltip("Ctrl+Y");
    ImGui::SameLine();
    if (ImGui::SmallButton(kIconSquareDashed))
        animationControllerFit = true;
    HelpTooltip(Tr("Fit Controller Graph"));
    ImGui::SameLine();
    ImGui::TextDisabled("%s", kIconInfo);
    HelpTooltip(Tr("Controller draft: Ctrl+Z / Ctrl+Y undo or redo. Ctrl+S saves. Drag a state edge to create a transition."));
    ImGui::SameLine();
    if (ImGui::SmallButton(kIconFilm)) {
        Call("editor.animation", Json());
        if (animationControllerDocument != animationControllerSaved)
            animationControllerClosePrompt = true;
        else {
            Call("animation.controller_close", Json());
            Call("editor.animation", Json());
        }
    }
    AnimationRect("controller-back");
    HelpTooltip(Tr("Timeline"));
    if (animationControllerDocument.isNull()) {
        if (ImGui::Button(Tr("Create Controller"))) {
            Json definition = Json::parse(
                R"({"entry":"Idle","parameters":{"speed":{"type":"number","default":0}},"states":{"Idle":{"clip":"Idle","position":[40,100]}},"transitions":[]})");
            if (Ok(Call("animation.controller_create", ObjectOf({{"path", animationControllerPathInput}, {"values", definition}}))))
                Call("animation.controller_edit", ObjectOf({{"path", animationControllerPathInput}}));
        }
        ImGui::End();
        return;
    }
    if (animationControllerDocument != animationControllerSaved) {
        ImGui::SameLine();
        ImGui::TextUnformatted("*");
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
        ImGui::SameLine();
        ImGui::TextColored(matching ? ImVec4(0.47f, 0.84f, 0.63f, 1) : ImVec4(1, 0.83f, 0.37f, 1), "%s",
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
    const float properties = std::min(310.0f, ImGui::GetContentRegionAvail().x * 0.4f);
    ImGui::BeginChild("##controllerCanvasHost", ImVec2(std::max(180.0f, ImGui::GetContentRegionAvail().x - properties - 8), 0));
    {
        EditorNodeCanvas canvas("##controllerCanvas", animationControllerPan, animationControllerZoom);
        std::map<std::string, ImVec2> positions;
        for (size_t i = 0; i < states.size(); ++i) {
            const Json& p = static_cast<const Json&>(data)["states"][states[i]]["position"];
            positions[states[i]] = p.isArray() ? ImVec2(p[0].asFloat(), p[1].asFloat())
                                               : ImVec2(40 + static_cast<float>(i % 3) * 260, 100 + static_cast<float>(i / 3) * 180);
        }
        positions["*"] = ImVec2(40, -20);
        const ImVec2 entryPosition(260, -20), card(190, 76), special(160, 40);
        // A newly docked window acquires its final dimensions after its first frame.
        ImGuiStorage* storage = ImGui::GetStateStorage();
        const ImGuiID sizeX = ImGui::GetID("fitWidth"), sizeY = ImGui::GetID("fitHeight");
        if (storage->GetFloat(sizeX) != canvas.size.x || storage->GetFloat(sizeY) != canvas.size.y) {
            animationControllerFit = true;
            storage->SetFloat(sizeX, canvas.size.x);
            storage->SetFloat(sizeY, canvas.size.y);
        }
        if (animationControllerFit) {
            ImVec2 low(0, -60), high(420, 40);
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
        auto wire = [&](ImVec2 a, ImVec2 b, const std::string& label, int index, float offset) {
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
            const ImU32 color = flowing  ? IM_COL32(120, 214, 160, 255)
                                : chosen ? IM_COL32(255, 213, 116, 255)
                                         : IM_COL32(112, 202, 242, 255);
            if (flowing && chosen)
                canvas.draw->AddBezierCubic(from, c1, c2, to, IM_COL32(255, 213, 116, 255), 6);
            canvas.draw->AddBezierCubic(from, c1, c2, to, color, flowing ? 4.0f : chosen ? 3.5f : 1.7f);
            if (flowing)
                canvas.draw->AddCircleFilled(point(std::fmod(age * 2, 1.0f)), 4.5f, IM_COL32(245, 255, 249, 255));
            std::string caption = label;
            if (flowing && (*observation)["conditions"].size() > 0)
                caption = conditionText((*observation)["conditions"][0]);
            ImVec2 direction = to - point(0.98f);
            const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y);
            if (length > 0)
                direction = direction / length;
            const ImVec2 normal(-direction.y, direction.x);
            canvas.draw->AddTriangleFilled(to, to - direction * 10 + normal * 5, to - direction * 10 - normal * 5, color);
            const float font = ImGui::GetFontSize() * std::max(0.6f, animationControllerZoom);
            const ImVec2 labelSize = ImGui::GetFont()->CalcTextSizeA(font, 10000, 0, caption.c_str());
            const ImVec2 middle = point(0.5f), at = middle - ImVec2(labelSize.x * 0.5f, labelSize.y + 3);
            canvas.draw->AddRectFilled(at - ImVec2(3, 2), at + labelSize + ImVec2(3, 2), IM_COL32(22, 25, 31, 255), 3);
            canvas.draw->AddText(ImGui::GetFont(), font, at, color, caption.c_str());
            if (index >= 0) {
                bool hit = false;
                ImVec2 prev = from;
                for (int i = 1; i <= 24; ++i) {
                    ImVec2 next = point(i / 24.0f);
                    hit = hit || Distance(ImGui::GetIO().MousePos, prev, next) < 7;
                    prev = next;
                }
                hit = hit || ImGui::IsMouseHoveringRect(at, at + labelSize);
                wireHovered = wireHovered || hit;
                if (hit && observation && ImGui::IsWindowHovered()) {
                    ImGui::BeginTooltip();
                    for (const Json& condition : (*observation)["conditions"].items())
                        ImGui::TextUnformatted(conditionText(condition).c_str());
                    ImGui::EndTooltip();
                }
                animationRects["controller-transition:" + std::to_string(index)] = {at.x, at.y, labelSize.x, labelSize.y};
                if (flowing)
                    animationRects["controller-live-transition:" + std::to_string(index)] = {at.x, at.y, labelSize.x, labelSize.y};
                if (hit && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0)) {
                    animationControllerTransition = index;
                    animationControllerState.clear();
                }
                if (hit && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(1)) {
                    animationControllerTransition = index;
                    animationControllerState.clear();
                    ImGui::OpenPopup("##transitionMenu");
                }
            }
        };
        wire(Edge(entryPosition, special, positions[data["entry"].asString()] + card * 0.5f),
             Edge(positions[data["entry"].asString()], card, entryPosition + special * 0.5f), "", -1, 0);
        for (size_t i = 0; i < data["transitions"].size(); ++i) {
            const Json& t = data["transitions"][i];
            const std::string from = t["from"].asString(), to = t["to"].asString();
            const ImVec2 a = positions[from], b = positions[to];
            bool reverse = false;
            for (const Json& other : data["transitions"].items())
                reverse = reverse || (other["from"].asString() == to && other["to"].asString() == from);
            wire(Edge(a, from == "*" ? special : card, b + card * 0.5f), Edge(b, card, a + (from == "*" ? special : card) * 0.5f),
                 Summary(t), static_cast<int>(i), reverse ? (from < to ? -36.0f : 36.0f) : 24.0f);
        }
        ImVec2 entry = entryPosition;
        ImGui::PushID("entry");
        canvas.Card(entry, special, Tr("Entry State"), IM_COL32(52, 112, 84, 255), false);
        ImGui::PopID();
        ImVec2 any = positions["*"];
        ImGui::PushID("any");
        canvas.Card(any, special, Tr("Any State"), IM_COL32(92, 76, 120, 255), false);
        canvas.Pin("##output", canvas.Point(any + ImVec2(special.x, special.y * 0.5f)), true);
        if (ImGui::IsItemActivated())
            animationControllerLink = "*";
        ImGui::PopID();
        bool moved = false;
        for (const auto& name : states) {
            ImGui::PushID(name.c_str());
            ImVec2 position = positions[name], before = position;
            const bool activeState = matching && live["state"].asString() == name;
            canvas.Card(position, card, name.c_str(), activeState ? IM_COL32(52, 112, 84, 255) : IM_COL32(55, 95, 125, 255),
                        animationControllerState == name);
            if (activeState) {
                const ImVec2 p = canvas.Point(position), end = canvas.Point(position + card);
                canvas.draw->AddRect(p, end, IM_COL32(120, 214, 160, 255), 4, 2.5f);
                const double duration = live["playback"]["duration"].asNumber(), playbackTime = live["playback"]["time"].asNumber();
                const float fraction = duration > 0 ? static_cast<float>(std::clamp(playbackTime / duration, 0.0, 1.0)) : 0;
                canvas.draw->AddRectFilled(ImVec2(p.x, end.y - 4), ImVec2(p.x + (end.x - p.x) * fraction, end.y),
                                           IM_COL32(120, 214, 160, 255));
                char seconds[48];
                std::snprintf(seconds, sizeof(seconds), "%.2f / %.2f s", playbackTime, duration);
                canvas.draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * animationControllerZoom * 0.75f,
                                     canvas.Point(position + ImVec2(10, 59)), IM_COL32(185, 233, 207, 255), seconds);
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
                if (ImGui::MenuItem(Tr("Set Entry State"))) {
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
            const float font = ImGui::GetFontSize() * animationControllerZoom;
            const ImVec2 text = canvas.Point(position + ImVec2(10, 40));
            canvas.draw->PushClipRect(canvas.Point(position + ImVec2(0, 30)), canvas.Point(position + card), true);
            const std::string body =
                data["states"][name]["clip"].asString() + "  " +
                (static_cast<const Json&>(data)["states"][name]["loop"].asBool(true) ? kIconRefreshCw : kIconStepForward);
            canvas.draw->AddText(ImGui::GetFont(), font, text, IM_COL32(219, 232, 243, 255), body.c_str());
            canvas.draw->PopClipRect();
            canvas.Pin("##output", canvas.Point(position + ImVec2(card.x, card.y * 0.5f)), true);
            AnimationRect("controller-pin:" + name);
            if (ImGui::IsItemActivated())
                animationControllerLink = name;
            ImGui::PopID();
        }
        if (ImGui::IsMouseClicked(0))
            ++dragSerial["controller-position"];
        if (moved)
            replace("controller-drag:" + std::to_string(dragSerial["controller-position"]));
        if (!animationControllerLink.empty()) {
            canvas.Wire(canvas.Point(positions[animationControllerLink] + ImVec2(animationControllerLink == "*" ? special.x : card.x, 38)),
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
            if (ImGui::MenuItem(Tr("Add State")))
                addState(animationControllerNewAt);
            ImGui::EndPopup();
        }
        canvas.Footer(nullptr);
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##controllerProperties", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (matching) {
        const bool paused=live["paused"].asBool();
        ImGui::TextColored(ImVec4(0.47f, 0.84f, 0.63f, 1), "%s: %s", Tr(paused ? "Paused" : "Live"),
                           live["state"].asString().c_str());
        Property(Tr("State Time"), [&]() {
            ImGui::Text("%.3f s", live["elapsed"].asNumber());
            return false;
        });
        ImGui::TextUnformatted(Tr("Current Parameters"));
        for (const auto& parameter : live["parameters"].members())
            Property(parameter.first.c_str(), [&]() {
                ImGui::TextUnformatted(parameter.second.dump().c_str());
                return false;
            });
        ImGui::Separator();
        ImGui::TextUnformatted(Tr("Recent Transitions"));
        const Json* detail = nullptr;
        const auto& trace = live["transitions"].items();
        for (size_t i = trace.size() > 4 ? trace.size() - 4 : 0; i < trace.size(); ++i) {
            const Json& event = trace[i];
            ImGui::PushID(static_cast<int>(i));
            const std::string text = event["from"].asString() + " > " + event["to"].asString();
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
        ImGui::Separator();
    } else if (found) {
        ImGui::TextWrapped("%s", Tr("Runtime definition differs from this graph. Restart play after saving to monitor it."));
        ImGui::Separator();
    } else if (InPlaySession()) {
        ImGui::TextWrapped("%s", Tr("No live controller for the selected entity."));
        ImGui::Separator();
    }
    if (!animationControllerState.empty()) {
        const std::string selectedName = animationControllerState;
        Json& state = data["states"][selectedName];
        const Json& values = state;
        ImGui::TextUnformatted(Tr("Selected State"));
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
        if (Property(Tr("State Clip"), [&]() { return ImGui::InputText("##value", &clip, ImGuiInputTextFlags_EnterReturnsTrue); })) {
            state["clip"] = clip;
            replace();
        }
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
        if (ImGui::Button(Tr("Set Entry State"))) {
            data["entry"] = selectedName;
            replace();
        }
        if (ImGui::Button(Tr("Preview State")))
            Call("animation.preview",
                 ObjectOf({{"id", Primary()}, {"clip", clip}, {"speed", speed}, {"loop", loop}, {"time", 0}, {"playing", false}}));
        AnimationRect("controller-preview");
        if (states.size() > 1 && ImGui::Button(WithIcon(kIconTrash2, Tr("Remove State")).c_str()))
            removeSelection();
        if (ImGui::Button(Tr("Add State")))
            addState(ImVec2(40, 280));
        AnimationRect("controller-add-state");
        if (states.size() > 1 && ImGui::Button(Tr("Add Transition")))
            addTransition(selectedName, states[0] == selectedName ? states[1] : states[0]);
        AnimationRect("controller-add-transition");
    } else if (animationControllerTransition >= 0 && animationControllerTransition < static_cast<int>(data["transitions"].size())) {
        const int index = animationControllerTransition;
        Json& t = data["transitions"][index];
        const Json& values = t;
        ImGui::TextUnformatted(Tr("Controller Transitions"));
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
        for (size_t i = 0; i < t["conditions"].size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            Json& c = t["conditions"][i];
            std::string name = c["parameter"].asString();
            if (Choose(Tr("Condition Parameter"), name, parameters)) {
                c = MakeCondition(data, name);
                replace();
            }
            const std::string type = data["parameters"][name]["type"].asString();
            std::string op = c["op"].asString();
            if (Choose(Tr("Condition Operator"), op,
                       type == "trigger"   ? std::vector<std::string>{"trigger"}
                       : type == "boolean" ? std::vector<std::string>{"==", "!="}
                                           : std::vector<std::string>{"==", "!=", ">", ">=", "<", "<="})) {
                c["op"] = op;
                replace();
            }
            if (type == "number") {
                double value = c["value"].asNumber();
                if (Property(Tr("Condition Value"),
                             [&]() { return ImGui::InputDouble("##value", &value, 0, 0, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue); })) {
                    c["value"] = value;
                    replace();
                }
            } else if (type == "boolean") {
                bool value = c["value"].asBool();
                if (Property(Tr("Condition Value"), [&]() { return ImGui::Checkbox("##value", &value); })) {
                    c["value"] = value;
                    replace();
                }
            }
            if (t["conditions"].size() > 1 && ImGui::SmallButton(kIconTrash2)) {
                t["conditions"].items().erase(t["conditions"].items().begin() + static_cast<ptrdiff_t>(i));
                replace();
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        if (!parameters.empty() && ImGui::Button(Tr("Add Condition"))) {
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
        if (index > 0 && ImGui::Button(Tr("Transition Earlier"))) {
            std::swap(data["transitions"][index], data["transitions"][index - 1]);
            if (replace())
                --animationControllerTransition;
        }
        if (index + 1 < static_cast<int>(data["transitions"].size()) && ImGui::Button(Tr("Transition Later"))) {
            std::swap(data["transitions"][index], data["transitions"][index + 1]);
            if (replace())
                ++animationControllerTransition;
        }
        if (ImGui::Button(WithIcon(kIconTrash2, Tr("Remove Transition")).c_str()))
            removeSelection();
    } else {
        ImGui::TextUnformatted(Tr("Controller Parameters"));
        Property(Tr("Parameter Name"), [&]() {
            const bool changed = ImGui::InputText("##value", &animationControllerParameterName);
            AnimationRect("controller-parameter-name");
            return changed;
        });
        Choose(Tr("Parameter Type"), animationControllerParameterType, {"number", "boolean", "trigger"});
        if (ImGui::Button(Tr("Add Parameter")) && !data["parameters"].has(animationControllerParameterName)) {
            data["parameters"][animationControllerParameterName] =
                ObjectOf({{"type", animationControllerParameterType},
                          {"default", animationControllerParameterType == "number" ? Json(0) : Json(false)}});
            replace();
        }
        AnimationRect("controller-add-parameter");
        for (const auto& name : parameters) {
            ImGui::PushID(name.c_str());
            Json& p = data["parameters"][name];
            ImGui::Separator();
            ImGui::Text("%s  (%s)", name.c_str(), p["type"].asString().c_str());
            if (p["type"].asString() == "number") {
                double value = p["default"].asNumber();
                if (Property(Tr("Default Value"),
                             [&]() { return ImGui::InputDouble("##value", &value, 0, 0, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue); })) {
                    p["default"] = value;
                    replace();
                }
            } else if (p["type"].asString() == "boolean") {
                bool value = p["default"].asBool();
                if (Property(Tr("Default Value"), [&]() { return ImGui::Checkbox("##value", &value); })) {
                    p["default"] = value;
                    replace();
                }
            }
            if (ImGui::SmallButton(kIconTrash2)) {
                data["parameters"].erase(name);
                replace();
                ImGui::PopID();
                break;
            }
            HelpTooltip(Tr("Remove Parameter"));
            ImGui::PopID();
        }
        if (ImGui::Button(Tr("Add State")))
            addState(ImVec2(40, 280));
        AnimationRect("controller-add-state");
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
