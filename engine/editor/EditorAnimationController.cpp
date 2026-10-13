#include "editor/EditorInternal.h"
#include <algorithm>
#include "app/Engine.h"
#include "editor/EditorNodeCanvas.h"
#include "editor/EditorText.h"

namespace oe {
namespace {
Json DefaultController() {
    return Json::parse(R"({"entry":"Idle","parameters":{"speed":{"type":"number","default":0}},"states":{"Idle":{"clip":"Idle","position":[40,80]}},"transitions":[]})");
}
bool Pick(const char* label, std::string& value, const std::vector<std::string>& choices) {
    bool changed = false;
    if (ImGui::BeginCombo(label, value.c_str())) {
        for (const auto& choice : choices) if (ImGui::Selectable(choice.c_str(), choice == value)) { value = choice; changed = true; }
        ImGui::EndCombo();
    }
    return changed;
}
Json Condition(const Json& data, const std::string& parameter) {
    const std::string type = data["parameters"][parameter]["type"].asString();
    Json result = ObjectOf({{"parameter", parameter}, {"op", type == "trigger" ? "trigger" : "=="}});
    if (type != "trigger") result["value"] = data["parameters"][parameter]["default"];
    return result;
}
}  // namespace

void NativeEditor::Impl::AnimationControllerPanel() {
    if (ImGui::Button(Tr("Preview Timeline"))) { Call("editor.animation", ObjectOf({{"controller", ""}})); return; }
    AnimationRect("controller-back"); ImGui::SameLine(); ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - 200));
    ImGui::InputText("##controllerPath", &animationController); AnimationRect("controller-path");
    ImGui::SameLine();
    if (ImGui::Button(Tr("Create Controller"))) {
        Call("animation.controller_create", ObjectOf({{"path", animationController}, {"values", DefaultController()}}));
    }
    AnimationRect("controller-create");
    const Json response = Call("animation.controller_get", ObjectOf({{"path", animationController}}), true);
    if (!Ok(response)) { ImGui::TextWrapped("%s", response["error"]["message"].asString().c_str()); return; }
    Json data = response["result"]["values"];
    auto write = [&](const Json& changed) {
        return Ok(Call("animation.controller_set", ObjectOf({{"path", animationController}, {"values", changed}})));
    };
    std::vector<std::string> states, parameters;
    for (const auto& state : data["states"].members()) states.push_back(state.first);
    for (const auto& parameter : data["parameters"].members()) parameters.push_back(parameter.first);
    if (!data["states"].has(animationControllerState)) animationControllerState = data["entry"].asString();
    ImGui::TextWrapped("%s", Tr("Controller files save immediately; scripts call controller:update(dt)."));
    ImGui::SetNextItemWidth(120); ImGui::InputText("##newStateName", &animationControllerNewState); AnimationRect("controller-new-state-name"); ImGui::SameLine();
    if (ImGui::Button(Tr("Add State"))) {
        std::string name = animationControllerNewState; int suffix = 1;
        while (data["states"].has(name)) name = animationControllerNewState + std::to_string(suffix++);
        Json state = data["states"][data["entry"].asString()];
        state["position"] = Json(Json::Array{40 + static_cast<int>(states.size() % 4) * 240, 80 + static_cast<int>(states.size() / 4) * 140});
        data["states"][name] = state;
        if (write(data)) animationControllerState = name;
        return;
    }
    AnimationRect("controller-add-state"); ImGui::SameLine();
    if (ImGui::Button(Tr("Add Transition")) && !parameters.empty() && states.size() > 1) {
        const std::string target = states[0] == animationControllerState ? states[1] : states[0];
        Json conditions = Json::MakeArray(); conditions.push(Condition(data, parameters[0]));
        data["transitions"].push(ObjectOf({{"from", animationControllerState}, {"to", target}, {"blend", 0.2}, {"conditions", conditions}}));
        if (write(data)) animationControllerTransition = static_cast<int>(data["transitions"].size()) - 1;
        return;
    }
    AnimationRect("controller-add-transition"); ImGui::SameLine();
    if (ImGui::Button(Tr("Fit Controller Graph"))) animationControllerFit = true;
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    ImGui::BeginChild("##controllerCanvasChild", ImVec2(std::max(180.0f, availableWidth * 0.58f), 0), ImGuiChildFlags_Borders);
    {
        EditorNodeCanvas canvas("##controllerCanvas", animationControllerPan, animationControllerZoom);
        std::map<std::string, ImVec2> positions;
        for (size_t i = 0; i < states.size(); ++i) {
            const Json& p = static_cast<const Json&>(data)["states"][states[i]]["position"];
            positions[states[i]] = p.isArray() ? ImVec2(p[0].asFloat(), p[1].asFloat()) : ImVec2(40 + static_cast<float>(i % 4) * 240, 80 + static_cast<float>(i / 4) * 140);
        }
        if (animationControllerFit) {
            ImVec2 low(100000, 100000), high(-100000, -100000);
            for (const auto& p : positions) { low.x = std::min(low.x, p.second.x); low.y = std::min(low.y, p.second.y); high.x = std::max(high.x, p.second.x + 180); high.y = std::max(high.y, p.second.y + 60); }
            canvas.Fit(low - ImVec2(30, 30), high + ImVec2(30, 30)); animationControllerFit = false;
        }
        canvas.Navigate();
        for (size_t i = 0; i < data["transitions"].size(); ++i) {
            const Json& transition = data["transitions"][i];
            const ImVec2 from = transition["from"].asString() == "*" ? ImVec2(0, 0) : positions[transition["from"].asString()] + ImVec2(180, 30);
            const ImVec2 to = positions[transition["to"].asString()] + ImVec2(0, 30);
            canvas.Wire(canvas.Point(from), canvas.Point(to), IM_COL32(110, 190, 230, 255), animationControllerTransition == static_cast<int>(i) ? 2.0f : 1.0f);
            const ImVec2 end = canvas.Point(to);
            canvas.draw->AddTriangleFilled(end, end + ImVec2(-9, -5), end + ImVec2(-9, 5), IM_COL32(110, 190, 230, 255));
            const ImVec2 label = canvas.Point((from + to) * 0.5f);
            const float textSize = ImGui::GetFontSize() * animationControllerZoom;
            if (textSize >= 7) canvas.draw->AddText(ImGui::GetFont(), textSize, label, IM_COL32(200, 220, 235, 255), (std::to_string(i + 1) + " >").c_str());
        }
        bool moved = false;
        for (const auto& name : states) {
            ImGui::PushID(name.c_str());
            ImVec2& position = positions[name];
            const ImVec2 before = position;
            const std::string badge = name == data["entry"].asString() ? Tr("Entry State") : data["states"][name]["clip"].asString();
            if (canvas.Card(position, ImVec2(180, 60), name.c_str(), IM_COL32(55, 95, 125, 255), name == animationControllerState, false, badge.c_str()))
                animationControllerState = name;
            if (ImGui::IsItemClicked()) animationControllerState = name;
            AnimationRect("controller-state:" + name);
            const float textSize = ImGui::GetFontSize() * animationControllerZoom;
            if (textSize >= 7) {
                canvas.draw->PushClipRect(canvas.Point(position + ImVec2(0, 30)), canvas.Point(position + ImVec2(180, 60)), true);
                canvas.draw->AddText(ImGui::GetFont(), textSize, canvas.Point(position + ImVec2(10, 34)),
                    IM_COL32(180, 200, 210, 255), data["states"][name]["clip"].asString().c_str());
                canvas.draw->PopClipRect();
            }
            if (position.x != before.x || position.y != before.y) {
                position.x = std::max(-100000.0f, std::min(position.x, 100000.0f));
                position.y = std::max(-100000.0f, std::min(position.y, 100000.0f));
                data["states"][name]["position"] = Json(Json::Array{position.x, position.y}); moved = true;
            }
            ImGui::PopID();
        }
        // Position edits share the validated asset contract. Card drag writes once per input frame.
        if (moved) write(data);
        canvas.Footer(Tr("Drag states; wheel zoom; middle mouse pan."));
    }
    ImGui::EndChild(); ImGui::SameLine();
    ImGui::BeginChild("##controllerInspector", ImVec2(0, 0));
    ImGui::TextUnformatted(Tr("Controller States"));
    std::string entry = data["entry"].asString();
    if (Pick(Tr("Entry State"), entry, states)) { data["entry"] = entry; write(data); ImGui::EndChild(); return; }
    Pick(Tr("Selected State"), animationControllerState, states);
    std::string rename = animationControllerState;
    if (ImGui::InputText(Tr("State Name"), &rename, ImGuiInputTextFlags_EnterReturnsTrue) && rename != animationControllerState) {
        if (!data["states"].has(rename)) {
            // Copy before insertion: Json object growth may invalidate the source member.
            const Json renamedState = data["states"][animationControllerState];
            data["states"][rename] = renamedState; data["states"].erase(animationControllerState);
            if (data["entry"].asString() == animationControllerState) data["entry"] = rename;
            for (Json& t : data["transitions"].items()) { if (t["from"].asString() == animationControllerState) t["from"] = rename; if (t["to"].asString() == animationControllerState) t["to"] = rename; }
            if (write(data)) animationControllerState = rename;
        }
        ImGui::EndChild(); return;
    }
    AnimationRect("controller-state-name");
    Json& state = data["states"][animationControllerState];
    std::string clip = state["clip"].asString();
    if (ImGui::InputText(Tr("State Clip"), &clip, ImGuiInputTextFlags_EnterReturnsTrue)) { state["clip"] = clip; write(data); }
    AnimationRect("controller-clip");
    const Json& stateValues = state;
    float speed = stateValues["speed"].asFloat(1); bool loop = stateValues["loop"].asBool(true);
    if (ImGui::InputFloat(Tr("State Speed"), &speed, 0, 0, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue)) { state["speed"] = speed; write(data); }
    if (ImGui::Checkbox(Tr("Loop Clip"), &loop)) { state["loop"] = loop; write(data); }
    if (ImGui::Button(Tr("Preview State"))) {
        Call("animation.preview", ObjectOf({{"id", Primary()}, {"clip", clip}, {"speed", speed}, {"loop", loop}, {"time", 0}, {"playing", false}}));
    }
    AnimationRect("controller-preview");
    if (states.size() > 1 && ImGui::Button(Tr("Remove State"))) {
        data["states"].erase(animationControllerState);
        if (entry == animationControllerState) data["entry"] = states[0] == entry ? states[1] : states[0];
        auto& transitions = data["transitions"].items();
        transitions.erase(std::remove_if(transitions.begin(), transitions.end(), [&](const Json& t) {
            return t["from"].asString() == animationControllerState || t["to"].asString() == animationControllerState;
        }), transitions.end());
        write(data); ImGui::EndChild(); return;
    }
    ImGui::Separator(); ImGui::TextUnformatted(Tr("Controller Parameters"));
    ImGui::InputText("##controllerParameterName", &animationControllerParameterName); AnimationRect("controller-parameter-name");
    Pick(Tr("Parameter Type"), animationControllerParameterType, {"number", "boolean", "trigger"});
    if (ImGui::Button(Tr("Add Parameter"))) {
        if (!data["parameters"].has(animationControllerParameterName)) {
            data["parameters"][animationControllerParameterName] = ObjectOf({{"type", animationControllerParameterType}, {"default", animationControllerParameterType == "number" ? Json(0) : Json(false)}});
            write(data);
        }
        ImGui::EndChild(); return;
    }
    AnimationRect("controller-add-parameter");
    for (const auto& name : parameters) {
        ImGui::PushID(name.c_str()); Json& parameter = data["parameters"][name];
        ImGui::Text("%s (%s)", name.c_str(), parameter["type"].asString().c_str());
        if (parameter["type"].asString() == "number") {
            double value = parameter["default"].asNumber();
            if (ImGui::InputDouble(Tr("Default Value"), &value, 0, 0, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) { parameter["default"] = value; write(data); }
        } else if (parameter["type"].asString() == "boolean") {
            bool value = parameter["default"].asBool();
            if (ImGui::Checkbox(Tr("Default Value"), &value)) { parameter["default"] = value; write(data); }
        }
        if (ImGui::Button(Tr("Remove Parameter"))) {
            data["parameters"].erase(name); write(data); ImGui::PopID(); ImGui::EndChild(); return;
        }
        ImGui::PopID();
    }
    ImGui::Separator(); ImGui::TextUnformatted(Tr("Controller Transitions"));
    for (size_t i = 0; i < data["transitions"].size(); ++i) {
        const Json& t = data["transitions"][i];
        const std::string label = std::to_string(i + 1) + ": " + t["from"].asString() + " > " + t["to"].asString();
        if (ImGui::Selectable(label.c_str(), animationControllerTransition == static_cast<int>(i))) animationControllerTransition = static_cast<int>(i);
        AnimationRect("controller-transition:" + std::to_string(i));
    }
    if (animationControllerTransition >= 0 && animationControllerTransition < static_cast<int>(data["transitions"].size())) {
        const int index = animationControllerTransition;
        Json& t = data["transitions"][index];
        std::string from = t["from"].asString(), to = t["to"].asString(); std::vector<std::string> origins = states; origins.push_back("*");
        if (Pick(Tr("From State"), from, origins)) { t["from"] = from; write(data); }
        if (Pick(Tr("To State"), to, states)) { t["to"] = to; write(data); }
        const Json& transitionValues = t;
        float blend = transitionValues["blend"].asFloat(), minimum = transitionValues["minTime"].asFloat();
        if (ImGui::InputFloat(Tr("Blend Seconds"), &blend, 0, 0, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue)) { t["blend"] = blend; write(data); }
        if (ImGui::InputFloat(Tr("Minimum State Seconds"), &minimum, 0, 0, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue)) { t["minTime"] = minimum; write(data); }
        for (size_t i = 0; i < t["conditions"].size(); ++i) {
            ImGui::PushID(static_cast<int>(i)); Json& condition = t["conditions"][i];
            std::string name = condition["parameter"].asString();
            if (Pick(Tr("Condition Parameter"), name, parameters)) { condition = Condition(data, name); write(data); }
            const std::string type = data["parameters"][name]["type"].asString();
            std::string op = condition["op"].asString();
            if (Pick(Tr("Condition Operator"), op, type == "trigger" ? std::vector<std::string>{"trigger"} : type == "boolean" ? std::vector<std::string>{"==", "!="} : std::vector<std::string>{"==", "!=", ">", ">=", "<", "<="})) { condition["op"] = op; write(data); }
            if (type == "number") {
                double value = condition["value"].asNumber();
                if (ImGui::InputDouble(Tr("Condition Value"), &value, 0, 0, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) { condition["value"] = value; write(data); }
            } else if (type == "boolean") {
                bool value = condition["value"].asBool();
                if (ImGui::Checkbox(Tr("Condition Value"), &value)) { condition["value"] = value; write(data); }
            }
            if (t["conditions"].size() > 1 && ImGui::Button(Tr("Remove Condition"))) {
                t["conditions"].items().erase(t["conditions"].items().begin() + static_cast<ptrdiff_t>(i)); write(data); ImGui::PopID(); ImGui::EndChild(); return;
            }
            ImGui::PopID();
        }
        if (!parameters.empty() && ImGui::Button(Tr("Add Condition"))) { t["conditions"].push(Condition(data, parameters[0])); write(data); }
        if (index > 0 && ImGui::Button(Tr("Transition Earlier"))) { std::swap(data["transitions"][index], data["transitions"][index - 1]); if (write(data)) --animationControllerTransition; }
        if (index + 1 < static_cast<int>(data["transitions"].size()) && ImGui::Button(Tr("Transition Later"))) { std::swap(data["transitions"][index], data["transitions"][index + 1]); if (write(data)) ++animationControllerTransition; }
        if (ImGui::Button(Tr("Remove Transition"))) { data["transitions"].items().erase(data["transitions"].items().begin() + index); write(data); animationControllerTransition = -1; }
    }
    ImGui::EndChild();
}
}  // namespace oe
