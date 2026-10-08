#include "editor/EditorInternal.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include "app/Engine.h"
#include "core/FileSystem.h"
#include "editor/EditorNodeCanvas.h"
#include "editor/EditorText.h"
#include "editor/EditorVfxGraph.h"

namespace oe {
namespace {
const ImU32 kWire = IM_COL32(112, 202, 242, 255);
std::string LayoutFile(const std::string& path) {
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : path) { hash ^= c; hash *= 1099511628211ULL; }
    std::ostringstream name; name << ".oe/vfx-layout/" << std::hex << std::setw(16) << std::setfill('0') << hash << ".json";
    return name.str();
}
bool BoundedPoint(const Json& point) {
    return point.isArray() && point.size() == 2 && point[0].isNumber() && point[1].isNumber() &&
        std::isfinite(point[0].asNumber()) && std::isfinite(point[1].asNumber()) &&
        std::fabs(point[0].asNumber()) <= 100000 && std::fabs(point[1].asNumber()) <= 100000;
}
}

void NativeEditor::Impl::EffectsGraphLayout(bool save) {
    if (effectsPath.empty()) return;
    const std::string path = engine.ResolvePath(LayoutFile(effectsPath));
    for (auto& entry : effectsGraphPositions) {
        entry.second.x = std::clamp(entry.second.x, -100000.0f, 100000.0f);
        entry.second.y = std::clamp(entry.second.y, -100000.0f, 100000.0f);
    }
    if (save) {
        Json layout = ObjectOf({{"version", 1}, {"asset", effectsPath}, {"zoom", effectsGraphZoom},
            {"pan", Json(Json::Array{effectsGraphPan.x, effectsGraphPan.y})}, {"positions", Json::MakeObject()},
            {"materials", Json::MakeArray()}, {"anchors", Json::MakeArray()}});
        for (const auto& position : effectsGraphPositions)
            layout["positions"][position.first] = Json(Json::Array{position.second.x, position.second.y});
        for (const std::string& material : effectsGraphMaterials) layout["materials"].push(material);
        for (const std::string& anchor : effectsGraphAnchors) layout["anchors"].push(anchor);
        std::string error;
        if (!WriteTextFileAtomic(path, layout.dump(2) + "\n", &error)) { Notify(error, true); effectsLayoutDirty = false; return; }
        effectsLayoutDirty = false; return;
    }
    effectsLayoutPath = effectsPath;
    std::string text, error;
    if (!FileExists(path)) return;
    if (!ReadTextFile(path, text)) { Notify(Tr("Cannot read node layout."), true); return; }
    if (text.size() > 2 * 1024 * 1024) { Notify(Tr("Invalid node layout; using automatic positions."), true); return; }
    const Json layout = Json::parse(text, &error);
    if (!error.empty() || layout["version"].asInt() != 1 || layout["asset"].asString() != effectsPath ||
        !layout["positions"].isObject() || layout["positions"].size() > 256 || !BoundedPoint(layout["pan"]) ||
        !layout["zoom"].isNumber() || !std::isfinite(layout["zoom"].asNumber()) ||
        layout["zoom"].asFloat() < 0.08f || layout["zoom"].asFloat() > 1.5f) {
        Notify(Tr("Invalid node layout; using automatic positions."), true); effectsGraphFit = true; return;
    }
    for (const auto& entry : layout["positions"].members()) if (entry.first.size() <= 4096 && BoundedPoint(entry.second))
        effectsGraphPositions[entry.first] = ImVec2(entry.second[0].asFloat(), entry.second[1].asFloat());
    effectsGraphPan = ImVec2(layout["pan"][0].asFloat(), layout["pan"][1].asFloat()); effectsGraphZoom = layout["zoom"].asFloat();
    for (const char* key : {"materials", "anchors"}) {
        auto& references = std::string(key) == "materials" ? effectsGraphMaterials : effectsGraphAnchors;
        if (layout[key].isArray() && layout[key].size() <= 32) for (const Json& reference : layout[key].items())
            if (reference.isString() && !reference.asString().empty() && reference.asString().size() <= 4096)
                references.push_back(reference.asString());
    }
}

void NativeEditor::Impl::EffectsGraph() {
    if (effectsLayoutPath != effectsPath) EffectsGraphLayout(false);
    if (ImGui::Button(Tr("Auto Arrange"))) { effectsGraphPositions.clear(); effectsGraphFit = true; effectsLayoutDirty = true; }
    EffectsRect("graph-arrange"); ImGui::SameLine();
    if (ImGui::Button(Tr("Fit Graph"))) effectsGraphFit = true; EffectsRect("graph-fit"); ImGui::SameLine();
    if (ImGui::Button(Tr("Material Reference"))) ImGui::OpenPopup("##graphmaterials");
    if (ImGui::BeginPopup("##graphmaterials")) {
        for (const Json& asset : assets.items()) if (asset["kind"].asString() == "material") {
            const std::string path = asset["path"].asString();
            if (ImGui::Selectable(path.c_str()) && effectsGraphMaterials.size() < 32 && std::find(effectsGraphMaterials.begin(), effectsGraphMaterials.end(), path) == effectsGraphMaterials.end()) {
                effectsGraphMaterials.push_back(path); effectsLayoutDirty = true;
            }
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr("Anchor Reference"))) ImGui::OpenPopup("##graphanchors");
    if (ImGui::BeginPopup("##graphanchors")) {
        std::map<std::string, int> counts;
        for (const auto& entity : engine.GetScene().Entities()) ++counts[entity.second.name];
        for (const auto& entity : engine.GetScene().Entities()) {
            const std::string name = entity.second.name;
            if (name.empty() || counts[name] != 1 || !engine.GetScene().Get<Transform>(entity.first)) continue;
            if (ImGui::Selectable(name.c_str()) && effectsGraphAnchors.size() < 32 && std::find(effectsGraphAnchors.begin(), effectsGraphAnchors.end(), name) == effectsGraphAnchors.end()) {
                effectsGraphAnchors.push_back(name); effectsLayoutDirty = true;
            }
        }
        ImGui::EndPopup();
    }
    const bool trail = effectsBlock >= static_cast<int>(effectsDocument["emitters"].size());
    const Json& component = effectsSchema["components"][trail ? 1 : 0];
    ImGui::SetNextItemWidth(140);
    if (ImGui::BeginCombo(Tr("Target Field"), effectsGraphField.c_str())) {
        for (const Json& field : component["fields"].items()) {
            const std::string name = field["name"].asString();
            if (ImGui::Selectable(name.c_str(), name == effectsGraphField)) effectsGraphField = name;
        }
        ImGui::EndCombo();
    }
    EffectsRect("graph-field"); ImGui::SameLine();
    if (ImGui::Button(Tr("Add Parameter"))) {
        Json document = effectsDocument;
        const int emitters = static_cast<int>(document["emitters"].size());
        const Json& block = document[trail ? "trails" : "emitters"][trail ? effectsBlock - emitters : effectsBlock];
        const std::string fieldName = trail && effectsGraphField == "rate" ? "width" : effectsGraphField;
        Json value;
        for (const Json& field : component["fields"].items()) if (field["name"].asString() == fieldName) value = field["default"];
        const Json& values = block[trail ? "trail" : "emitter"]; if (values.has(fieldName)) value = values[fieldName];
        const std::string name = UniqueEditorVfxName(document, "parameter");
        document["params"][name] = ObjectOf({{"default", value}, {"targets", Json(Json::Array{
            ObjectOf({{"to", block["name"].asString() + "." + fieldName}})})}});
        EffectsReplace(document);
    }
    EffectsRect("graph-add-param");
    ImGui::TextWrapped("%s", Tr("Drag output to input. Right-click an input to disconnect; its last target removes the parameter. Middle mouse pans; wheel zooms."));
    if (!effectsGraphHint.empty()) ImGui::TextWrapped("%s", effectsGraphHint.c_str());
    const float available = ImGui::GetContentRegionAvail().x;
    const float inspector = std::min(320.0f, available * 0.32f);
    ImGui::BeginChild("##effectgrapharea", ImVec2(std::max(160.0f, available - inspector - 8), 0));
    const EditorVfxGraph graph = BuildEditorVfxGraph(effectsDocument, effectsGraphMaterials, effectsGraphAnchors, effectsGraphField);
    std::map<std::string, const EditorVfxNode*> nodes;
    int referenceRow = 0, blockIndex = 0;
    for (const auto& node : graph.nodes) {
        nodes[node.id] = &node;
        if (!effectsGraphModules.count(node.id)) effectsGraphModules[node.id] = -1;
        const int row = node.block >= 0 ? blockIndex++ : node.kind == "Output" ? 0 : referenceRow++;
        if (effectsGraphPositions.count(node.id)) continue;
        if (node.block >= 0) {
            effectsGraphPositions[node.id] = ImVec2(280.0f + static_cast<float>(row) * 280, 0);
        } else if (node.kind == "Output") effectsGraphPositions[node.id] = ImVec2(280.0f + static_cast<float>(effectsDocument["emitters"].size() + effectsDocument["trails"].size()) * 280, 0);
        else {
            ImVec2 pos(0, static_cast<float>(row) * 115);
            while (std::any_of(effectsGraphPositions.begin(), effectsGraphPositions.end(), [&](const auto& entry) {
                return std::fabs(entry.second.x - pos.x) < 240 && std::fabs(entry.second.y - pos.y) < 90;
            })) pos.y += 115;
            effectsGraphPositions[node.id] = pos;
        }
    }
    // Prune stale node positions, keeping layout bounded through arbitrarily long editing sessions.
    for (auto it = effectsGraphPositions.begin(); it != effectsGraphPositions.end(); ) {
        if (!nodes.count(it->first)) { effectsGraphModules.erase(it->first); it = effectsGraphPositions.erase(it); } else ++it;
    }
    {
        EditorNodeCanvas canvas("##effectcanvas", effectsGraphPan, effectsGraphZoom);
        effectsRects["graph-canvas"] = {canvas.origin.x, canvas.origin.y, canvas.size.x, canvas.size.y};
        auto dimensions = [&](const EditorVfxNode& node) { return ImVec2(240, node.block < 0 ? 90.0f : 80.0f + static_cast<float>(node.inputs.size()) * 23 + static_cast<float>(effectsSchema["modules"].size()) * 22 + (effectsGraphModules[node.id] >= 0 ? 180 : 0)); };
        if (effectsGraphFit && !graph.nodes.empty()) {
            ImVec2 low = effectsGraphPositions.begin()->second, high = low;
            for (const auto& node : graph.nodes) {
                const ImVec2 pos = effectsGraphPositions[node.id], end = pos + dimensions(node);
                low.x = std::min(low.x, pos.x); low.y = std::min(low.y, pos.y);
                high.x = std::max(high.x, end.x); high.y = std::max(high.y, end.y);
            }
            canvas.Fit(low, high); effectsGraphFit = false; effectsLayoutDirty = true;
        }
        const ImVec2 beforePan = effectsGraphPan; const float beforeZoom = effectsGraphZoom;
        canvas.Navigate();
        effectsLayoutDirty = effectsLayoutDirty || beforePan.x != effectsGraphPan.x || beforePan.y != effectsGraphPan.y || beforeZoom != effectsGraphZoom;
        auto output = [&](const std::string& id) { return canvas.Point(effectsGraphPositions[id] + ImVec2(240, 24)); };
        auto input = [&](const EditorVfxNode& node, const std::string& field) {
            auto pin = std::find(node.inputs.begin(), node.inputs.end(), field);
            return canvas.Point(effectsGraphPositions[node.id] + ImVec2(0, 54 + static_cast<float>(pin - node.inputs.begin()) * 23));
        };
        for (const auto& edge : graph.edges) if (nodes.count(edge.source) && nodes.count(edge.target))
            canvas.Wire(output(edge.source), input(*nodes[edge.target], edge.field), kWire);
        if (nodes.count(effectsGraphLink)) canvas.Wire(output(effectsGraphLink), ImGui::GetIO().MousePos, kWire);
        ImGui::PushFont(nullptr, ImGui::GetFontSize() * effectsGraphZoom);
        for (const auto& node : graph.nodes) {
            ImGui::PushID(node.id.c_str()); ImVec2& position = effectsGraphPositions[node.id];
            const ImVec2 p = canvas.Point(position);
            const std::string title = std::string(Tr(node.kind.c_str())) + (node.block >= 0 || node.kind == "Param" ? "  " + node.name : "");
            effectsLayoutDirty = canvas.Card(position, dimensions(node), title.c_str(), node.kind == "Output" ?
                IM_COL32(48, 91, 84, 255) : IM_COL32(52, 69, 93, 255), effectsGraphSelected == node.id, true) || effectsLayoutDirty;
            EffectsRect("node:" + node.id);
            if (ImGui::IsItemClicked()) { effectsGraphSelected = node.id; if (node.block >= 0) effectsBlock = node.block; }
            if (node.kind == "Material" && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) OpenShader(node.name);
            for (const std::string& field : node.inputs) {
                const ImVec2 pin = input(node, field); ImGui::PushID(field.c_str());
                bool connected = false;
                for (const auto& edge : graph.edges) connected = connected || (edge.target == node.id && edge.field == field);
                canvas.draw->AddText(pin + ImVec2(12, -7) * effectsGraphZoom, IM_COL32(205, 213, 224, 255), field.c_str());
                canvas.Pin("##input", pin, connected); EffectsRect("input:" + node.id + ":" + field);
                const bool drop = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                    nodes.count(effectsGraphLink) && ImGui::IsMouseReleased(ImGuiMouseButton_Left);
                if (drop || ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    Json candidate; std::string hint;
                    if (PatchEditorVfxWire(effectsDocument, drop ? nodes[effectsGraphLink] : nullptr, node, field, candidate, hint)) {
                        EffectsReplace(candidate); hint = effectsError;
                    } else hint = Tr(hint.c_str());
                    effectsGraphHint = hint; effectsGraphErrorNode = hint.empty() ? "" : node.id;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", field.c_str());
                ImGui::PopID();
            }
            if (node.kind != "Output" && node.kind != "Trail") {
                canvas.Pin("##output", output(node.id), true); EffectsRect("output:" + node.id);
                if (ImGui::IsItemActivated()) effectsGraphLink = node.id;
            }
            if (node.block >= 0) {
                int module = 0; float moduleY = 70 + static_cast<float>(node.inputs.size()) * 23;
                for (const Json& name : effectsSchema["modules"].items()) {
                    ImGui::SetCursorScreenPos(p + ImVec2(12, moduleY) * effectsGraphZoom);
                    const bool moduleSelected = effectsGraphModules[node.id] == module;
                    const std::string label = std::string(moduleSelected ? "v  " : ">  ") + Tr(name.asString().c_str());
                    if (ImGui::Selectable(label.c_str(), moduleSelected, 0, ImVec2(216, 20) * effectsGraphZoom)) {
                        effectsBlock = node.block; effectsModule = module; effectsGraphSelected = node.id;
                        effectsGraphModules[node.id] = moduleSelected ? -1 : module;
                    }
                    EffectsRect("module:" + node.id + ":" + name.asString()); moduleY += 22;
                    if (effectsGraphModules[node.id] == module) {
                        ImGui::SetCursorScreenPos(p + ImVec2(12, moduleY) * effectsGraphZoom);
                        ImGui::PushID(module);
                        ImGui::BeginChild("##modulefields", ImVec2(216, 180) * effectsGraphZoom, ImGuiChildFlags_Borders);
                        const int savedBlock = effectsBlock, savedModule = effectsModule;
                        effectsBlock = node.block; effectsModule = module; EffectsBlockFields();
                        effectsBlock = savedBlock; effectsModule = savedModule;
                        ImGui::EndChild(); ImGui::PopID(); moduleY += 180;
                    }
                    ++module;
                }
            } else if (node.kind == "Param" || node.kind == "Output") {
                ImGui::SetCursorScreenPos(p + ImVec2(12, 48) * effectsGraphZoom);
                ImGui::PushItemWidth(216 * effectsGraphZoom);
                if (node.kind == "Param") EffectsParameterValue(node.name, 216 * effectsGraphZoom);
                else {
                    int budget = effectsDocument["budget"]["maxParticles"].asInt(10000);
                    if (ImGui::DragInt("##graphbudget", &budget, 1, 0, 40000, "%d", ImGuiSliderFlags_AlwaysClamp)) {
                        Json document = effectsDocument; document["budget"]["maxParticles"] = budget;
                        EffectsReplace(document, EffectsMerge("graphbudget"));
                    }
                    EffectsRect("graph-budget");
                }
                ImGui::PopItemWidth();
            } else {
                std::string text = node.kind == "Output" ? std::to_string(effectsDocument["budget"]["maxParticles"].asInt(10000)) :
                    node.kind == "Param" ? effectsDocument["params"][node.name]["default"].dump() : node.name;
                const char* end = text.c_str() + text.size();
                while (end > text.c_str() && ImGui::CalcTextSize(text.c_str(), end).x > 216 * effectsGraphZoom) {
                    --end; while (end > text.c_str() && (static_cast<unsigned char>(*end) & 0xc0) == 0x80) --end;
                }
                canvas.draw->AddText(p + ImVec2(12, 54) * effectsGraphZoom, IM_COL32(229, 193, 140, 255), text.c_str(), end);
                ImGui::SetCursorScreenPos(p + ImVec2(12, 48) * effectsGraphZoom);
                ImGui::InvisibleButton("##description", ImVec2(216, 30) * effectsGraphZoom);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text.c_str());
                if (node.kind == "Material" && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) OpenShader(node.name);
            }
            if (effectsGraphErrorNode == node.id && !effectsGraphHint.empty()) {
                canvas.draw->AddRect(p, p + dimensions(node) * effectsGraphZoom, IM_COL32(255, 105, 90, 255), 5, 2);
                if (ImGui::IsMouseHoveringRect(p, p + dimensions(node) * effectsGraphZoom)) ImGui::SetTooltip("%s", effectsGraphHint.c_str());
            }
            ImGui::PopID();
        }
        ImGui::PopFont();
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Escape)) effectsGraphLink.clear();
    }
    ImGui::EndChild(); ImGui::SameLine();
    ImGui::BeginChild("##effectgraphinspector", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::BeginChild("##graphproperties", ImVec2(0, std::max(140.0f, ImGui::GetContentRegionAvail().y * 0.45f)));
    EffectsInspector(); ImGui::EndChild();
    ImGui::BeginChild("##graphpreview", ImVec2(0, 0)); if (effectsScene) EffectsPreview(true); ImGui::EndChild();
    ImGui::EndChild();
    if (effectsLayoutDirty && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle)) EffectsGraphLayout(true);
}
}  // namespace oe
