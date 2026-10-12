#include "editor/EditorIcons.h"
#include "editor/EditorInternal.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include "app/Engine.h"
#include "core/FileSystem.h"
#include "core/Log.h"
#include "editor/EditorNodeCanvas.h"
#include "editor/EditorText.h"
#include "editor/EditorVfxGraph.h"
#include "scene/VfxExpression.h"

namespace oe {
namespace {
// An emitter or trail node is a stack of contexts, as in a particle's life: how many are born, what they are
// born with, what happens to them while they live, how they are drawn. A context holds the engine's field
// groups ("modules"); a field is a row with a pin (what a parameter connects to) and its value.
struct GraphContext {
    const char* name;
    ImU32 color;
    std::vector<const char*> modules;
};
const std::vector<GraphContext>& GraphContexts() {
    static const std::vector<GraphContext> contexts = {
        {"Spawn", IM_COL32(64, 140, 96, 255), {"Emission", "Timing"}},
        {"Initialize", IM_COL32(64, 112, 176, 255), {"Shape", "Appearance"}},
        {"Update", IM_COL32(176, 130, 58, 255), {"Motion", "Curves", "Expressions"}},
        {"Output", IM_COL32(132, 98, 176, 255), {"Render"}},
    };
    return contexts;
}
// Fields that are shown whether or not the effect sets them: what nearly every effect tunes or wires.
bool PrimaryField(const std::string& name) {
    for (const char* primary : {"rate", "lifetime", "speed", "startSize", "startColor", "material", "width", "color", "baseAnchor", "tipAnchor"})
        if (name == primary) return true;
    return false;
}
const float kBlockWidth = 340, kReferenceWidth = 240, kRow = 26, kContextRow = 26, kModuleRow = 22;
struct GraphRow {
    enum Kind { Trigger, Context, Module, Field, Expression } kind;  // Expression: field = "<stage>:<channel>", shown = the channel
    float y, height;
    int context;
    std::string module, field;
    const Json* schema;  // the field's entry in vfx.schema
    int shown, total;    // Module: fields listed / fields it has
};
// A value as it is shown when the graph is too small for widgets: short numbers, no JSON punctuation.
std::string ValueText(const Json& value) {
    if (value.isNumber()) return Format("%.3g", value.asNumber());
    if (value.isString()) return value.asString();
    if (value.isBool()) return value.asBool() ? "on" : "off";
    if (value.isArray()) {
        bool numbers = !value.items().empty();
        for (const Json& item : value.items()) numbers = numbers && item.isNumber();
        if (!numbers) return value.items().empty() ? "-" : Format("%d ...", static_cast<int>(value.size()));
        std::string text;
        for (const Json& item : value.items()) text += (text.empty() ? "" : "  ") + Format("%.2g", item.asNumber());
        return text;
    }
    return value.isNull() ? "-" : "...";
}
ImU32 KindColor(const std::string& kind) {
    if (kind == "Emitter") return IM_COL32(176, 104, 52, 255);
    if (kind == "Trail") return IM_COL32(40, 122, 128, 255);
    if (kind == "Param") return IM_COL32(134, 100, 46, 255);
    if (kind == "Material") return IM_COL32(104, 76, 142, 255);
    if (kind == "Anchor") return IM_COL32(52, 112, 84, 255);
    return IM_COL32(150, 84, 72, 255);
}
ImU32 KindWire(const std::string& kind) {
    if (kind == "Emitter") return IM_COL32(244, 168, 110, 255);
    if (kind == "Param") return IM_COL32(238, 196, 110, 255);
    if (kind == "Material") return IM_COL32(196, 160, 240, 255);
    if (kind == "Anchor") return IM_COL32(120, 214, 160, 255);
    return IM_COL32(112, 202, 242, 255);
}
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
        // Positions are for the context-stack cards; a layout from before them is arranged again.
        layout["cards"] = 2; layout["folded"] = Json::MakeArray();
        for (const auto& flag : effectsGraphFlags) if (flag.second && layout["folded"].size() < 4096) layout["folded"].push(flag.first);
        layout["valuePan"] = Json(Json::Array{effectsValuePan.x,effectsValuePan.y}); layout["valueZoom"] = effectsValueZoom;
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
    effectsGraphFlags.clear();
    if (!FileExists(path)) { effectsGraphFit = true; return; }
    if (!ReadTextFile(path, text)) { Notify(Tr("Cannot read node layout."), true); return; }
    if (text.size() > 2 * 1024 * 1024) { Notify(Tr("Invalid node layout; using automatic positions."), true); return; }
    const Json layout = Json::parse(text, &error);
    if (!error.empty() || layout["version"].asInt() != 1 || layout["asset"].asString() != effectsPath ||
        !layout["positions"].isObject() || layout["positions"].size() > 4096 || !BoundedPoint(layout["pan"]) ||
        !layout["zoom"].isNumber() || !std::isfinite(layout["zoom"].asNumber()) ||
        layout["zoom"].asFloat() < 0.08f || layout["zoom"].asFloat() > 1.5f) {
        Notify(Tr("Invalid node layout; using automatic positions."), true); effectsGraphFit = true; return;
    }
    if (layout["cards"].asInt() == 2) {
        for (const auto& entry : layout["positions"].members()) if (entry.first.size() <= 4096 && BoundedPoint(entry.second))
            effectsGraphPositions[entry.first] = ImVec2(entry.second[0].asFloat(), entry.second[1].asFloat());
        effectsGraphPan = ImVec2(layout["pan"][0].asFloat(), layout["pan"][1].asFloat()); effectsGraphZoom = layout["zoom"].asFloat();
        if (layout["folded"].isArray() && layout["folded"].size() <= 4096) for (const Json& flag : layout["folded"].items())
            if (flag.isString() && flag.asString().size() <= 4096) effectsGraphFlags[flag.asString()] = 1;
    } else effectsGraphFit = true;
    if(BoundedPoint(layout["valuePan"]) && layout["valueZoom"].isNumber() && std::isfinite(layout["valueZoom"].asNumber()) &&
        layout["valueZoom"].asFloat()>=0.08f && layout["valueZoom"].asFloat()<=1.5f) {
        effectsValuePan=ImVec2(layout["valuePan"][0].asFloat(),layout["valuePan"][1].asFloat());effectsValueZoom=layout["valueZoom"].asFloat();
    }
    for (const char* key : {"materials", "anchors"}) {
        auto& references = std::string(key) == "materials" ? effectsGraphMaterials : effectsGraphAnchors;
        if (layout[key].isArray() && layout[key].size() <= 32) for (const Json& reference : layout[key].items())
            if (reference.isString() && !reference.asString().empty() && reference.asString().size() <= 4096)
                references.push_back(reference.asString());
    }
}

void NativeEditor::Impl::EffectsGraph() {
    if (effectsLayoutPath != effectsPath) EffectsGraphLayout(false);
    if (ImGui::Button(WithIcon(kIconLayoutGrid, Tr("Auto Arrange")).c_str())) { effectsGraphPositions.clear(); effectsGraphFit = true; effectsLayoutDirty = true; }
    EffectsRect("graph-arrange"); ImGui::SameLine();
    if (ImGui::Button(WithIcon(kIconSquareDashed, Tr("Fit Graph")).c_str())) effectsGraphFit = true; EffectsRect("graph-fit"); ImGui::SameLine();
    const bool trail = effectsBlock >= static_cast<int>(effectsDocument["emitters"].size());
    const Json& component = effectsSchema["components"][trail ? 1 : 0];
    // A parameter for the field that is selected in the graph (also: right-click a field).
    const std::string selectedField = trail && effectsGraphField == "rate" ? "width" : effectsGraphField;
    auto addParameter = [&](int block, const std::string& fieldName) {
        Json document = effectsDocument;
        const int emitters = static_cast<int>(document["emitters"].size());
        const bool onTrail = block >= emitters;
        const Json& entry = document[onTrail ? "trails" : "emitters"][onTrail ? block - emitters : block];
        Json value;
        for (const Json& field : effectsSchema["components"][onTrail ? 1 : 0]["fields"].items()) if (field["name"].asString() == fieldName) value = field["default"];
        const Json& values = entry[onTrail ? "trail" : "emitter"]; if (values.has(fieldName)) value = values[fieldName];
        const std::string name = UniqueEditorVfxName(document, "parameter");
        document["params"][name] = ObjectOf({{"default", value}, {"targets", Json(Json::Array{
            ObjectOf({{"to", entry["name"].asString() + "." + fieldName}})})}});
        EffectsReplace(document);
    };
    if (ImGui::Button(WithIcon(kIconPlus, Format(Tr("Parameter for %s"), HumanizeName(selectedField).c_str())).c_str())) addParameter(effectsBlock, selectedField);
    EffectsRect("graph-add-param");
    ImGui::SetItemTooltip("%s", Tr("A parameter is a named value scenes and scripts can change. Click a field in the graph to choose it, or right-click the field."));
    ImGui::SameLine();
    if (ImGui::Button(WithIcon(kIconPalette, Tr("Material Reference")).c_str())) ImGui::OpenPopup("##graphmaterials");
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
    if (ImGui::Button(WithIcon(kIconMapPin, Tr("Anchor Reference")).c_str())) ImGui::OpenPopup("##graphanchors");
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
    if (!effectsGraphHint.empty()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.6f, 0.45f, 1));
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted((std::string(kIconTriangleAlert) + " " + effectsGraphHint).c_str());
        ImGui::PopStyleColor();
    }
    (void)component;
    const float available = ImGui::GetContentRegionAvail().x;
    const float inspector = std::min(ImGui::GetFontSize() * 24.0f, available * 0.34f);
    ImGui::BeginChild("##effectgrapharea", ImVec2(std::max(160.0f, available - inspector - 8), 0));
    const EditorVfxGraph graph = BuildEditorVfxGraph(effectsDocument, effectsGraphMaterials, effectsGraphAnchors, effectsGraphField);
    // The value whose nodes are on the canvas (an expression row of an emitter was clicked).
    if (effectsBlock >= static_cast<int>(effectsDocument["emitters"].size())) effectsValueInGraph = false;
    const std::string valuePrefix = effectsValueInGraph ? EffectsValueSync() : std::string();
    ImVec2 valueSink, valueOrigin;
    bool valueVisible = false;
    const Json document = effectsDocument;
    const int emitterCount = static_cast<int>(document["emitters"].size());
    std::map<std::string, const EditorVfxNode*> nodes;
    for (const auto& node : graph.nodes) { nodes[node.id] = &node; if (!effectsGraphModules.count(node.id)) effectsGraphModules[node.id] = -1; }

    // What a block node lists: its contexts, their modules and the fields that matter (set by the effect,
    // driven by a parameter, or primary), each with its height. Drawing, wires and fitting all use it.
    auto valuesOf = [&](const EditorVfxNode& node) -> const Json& {
        const bool onTrail = node.block >= emitterCount;
        return document[onTrail ? "trails" : "emitters"][onTrail ? node.block - emitterCount : node.block][onTrail ? "trail" : "emitter"];
    };
    auto mappedField = [&](const EditorVfxNode& node, const std::string& field) {
        for (const auto& parameter : document["params"].members()) for (const Json& target : parameter.second["targets"].items())
            if (target["to"].asString() == node.name + "." + field) return true;
        return false;
    };
    auto rowsOf = [&](const EditorVfxNode& node, float* height) {
        std::vector<GraphRow> list;
        float y = EditorNodeCanvas::kHeader + 4;
        if (node.kind == "Emitter") { list.push_back({GraphRow::Trigger, y, kRow, -1, "", "trigger", nullptr, 0, 0}); y += kRow; }
        const Json& fields = effectsSchema["components"][node.kind == "Trail" ? 1 : 0]["fields"];
        const Json& values = valuesOf(node);
        const auto& contexts = GraphContexts();
        for (int c = 0; c < static_cast<int>(contexts.size()); ++c) {
            bool any = false;
            for (const char* module : contexts[static_cast<size_t>(c)].modules) for (const Json& field : fields.items()) any = any || field["module"].asString() == module;
            if (!any) continue;
            list.push_back({GraphRow::Context, y, kContextRow, c, "", "", nullptr, 0, 0}); y += kContextRow;
            if (effectsGraphFlags[node.id + "|ctx:" + std::to_string(c)]) continue;  // collapsed
            for (const char* module : contexts[static_cast<size_t>(c)].modules) {
                const bool all = effectsGraphFlags[node.id + "|all:" + module] != 0;
                std::vector<const Json*> listed;
                int total = 0;
                for (const Json& field : fields.items()) {
                    if (field["module"].asString() != module) continue;
                    ++total;
                    const std::string name = field["name"].asString();
                    if (name == "expressions") { --total; continue; }  // listed per value below
                    if (all || PrimaryField(name) || values.has(name) || mappedField(node, name)) listed.push_back(&field);
                }
                if (!total) continue;
                list.push_back({GraphRow::Module, y, kModuleRow, c, module, "", nullptr, static_cast<int>(listed.size()), total}); y += kModuleRow;
                for (const Json* field : listed) { list.push_back({GraphRow::Field, y, kRow, c, module, (*field)["name"].asString(), field, 0, 0}); y += kRow; }
            }
            if (node.kind == "Emitter" && (c == 1 || c == 2)) {
                // Values computed per particle: at birth (Initialize) or every step (Update). Those the effect
                // has, the one whose nodes are shown, or all of them.
                const char* stage = c == 1 ? "birth" : "update";
                const char* module = c == 1 ? "Birth Expressions" : "Update Expressions";
                const Json& authored = values["expressions"][stage];
                const bool all = effectsGraphFlags[node.id + "|all:" + module] != 0;
                std::vector<int> listed;
                for (int channel = 0; channel < static_cast<int>(kVfxChannelCount); ++channel) {
                    const bool shown = effectsValueInGraph && node.block == effectsBlock &&
                        effectsCurveKeys["expression:stage"] == c - 1 && effectsCurveKeys["expression:channel"] == channel;
                    if (all || shown || authored.has(VfxChannelNames()[static_cast<size_t>(channel)])) listed.push_back(channel);
                }
                list.push_back({GraphRow::Module, y, kModuleRow, c, module, "", nullptr, static_cast<int>(listed.size()), static_cast<int>(kVfxChannelCount)}); y += kModuleRow;
                for (int channel : listed) {
                    list.push_back({GraphRow::Expression, y, kRow, c, module, std::string(stage) + ":" + VfxChannelNames()[static_cast<size_t>(channel)], nullptr, channel, 0});
                    y += kRow;
                }
            }
            y += 4;
        }
        if (height) *height = y + 6;
        return list;
    };
    auto dimensions = [&](const EditorVfxNode& node) {
        if (node.block < 0) return ImVec2(kReferenceWidth, EditorNodeCanvas::kHeader + 44);
        float height = 0; rowsOf(node, &height);
        return ImVec2(kBlockWidth, height);
    };

    // New nodes: parameters and references in a column at the left, blocks side by side, the system at the right.
    {
        float blockX = kReferenceWidth + 110, referenceY = 0;
        for (const auto& node : graph.nodes) if (effectsGraphPositions.count(node.id) && node.block >= 0)
            blockX = std::max(blockX, effectsGraphPositions[node.id].x + kBlockWidth + 60);
        for (const auto& node : graph.nodes) if (effectsGraphPositions.count(node.id) && node.block < 0 && node.kind != "Output")
            referenceY = std::max(referenceY, effectsGraphPositions[node.id].y + dimensions(node).y + 22);
        for (const auto& node : graph.nodes) {
            if (effectsGraphPositions.count(node.id) || node.kind == "Output") continue;
            if (node.block >= 0) { effectsGraphPositions[node.id] = ImVec2(blockX, 0); blockX += kBlockWidth + 60; }
            else { effectsGraphPositions[node.id] = ImVec2(0, referenceY); referenceY += dimensions(node).y + 22; }
        }
        if (!effectsGraphPositions.count("output")) effectsGraphPositions["output"] = ImVec2(blockX, 0);
    }
    // Prune stale node positions, keeping layout bounded through arbitrarily long editing sessions.
    for (auto it = effectsGraphPositions.begin(); it != effectsGraphPositions.end(); ) {
        if (!nodes.count(it->first) && it->first.compare(0, 6, "value:") != 0) { effectsGraphModules.erase(it->first); it = effectsGraphPositions.erase(it); } else ++it;
    }
    {
        EditorNodeCanvas canvas("##effectcanvas", effectsGraphPan, effectsGraphZoom);
        effectsRects["graph-canvas"] = {canvas.origin.x, canvas.origin.y, canvas.size.x, canvas.size.y};
        if (effectsGraphFit && !graph.nodes.empty()) {
            ImVec2 low = effectsGraphPositions.begin()->second, high = low;
            for (const auto& node : graph.nodes) {
                const ImVec2 pos = effectsGraphPositions[node.id], end = pos + dimensions(node);
                low.x = std::min(low.x, pos.x); low.y = std::min(low.y, pos.y);
                high.x = std::max(high.x, end.x); high.y = std::max(high.y, end.y);
            }
            // With a value's nodes on the canvas, those too.
            if (effectsValueInGraph) for (const auto& entry : effectsGraphPositions) if (entry.first.compare(0, valuePrefix.size(), valuePrefix) == 0) {
                low.x = std::min(low.x, entry.second.x); low.y = std::min(low.y, entry.second.y);
                high.x = std::max(high.x, entry.second.x + 190); high.y = std::max(high.y, entry.second.y + 130);
            }
            canvas.Fit(low, high);
            // Emitters are tall: rather than shrink them until the values cannot be read, show the top at a
            // size that can and let the rest be reached by moving the view or folding contexts.
            if (effectsGraphZoom < 0.8f) {
                effectsGraphZoom = 0.8f;
                const float fitted = (high.x - low.x) * effectsGraphZoom;
                effectsGraphPan = ImVec2(std::max(24.0f, (canvas.size.x - fitted) * 0.5f), 24) - low * effectsGraphZoom;
            }
            effectsGraphFit = false; effectsLayoutDirty = true;
        }
        const ImVec2 beforePan = effectsGraphPan; const float beforeZoom = effectsGraphZoom;
        canvas.Navigate();
        effectsLayoutDirty = effectsLayoutDirty || beforePan.x != effectsGraphPan.x || beforePan.y != effectsGraphPan.y || beforeZoom != effectsGraphZoom;
        const float zoom = effectsGraphZoom;
        std::map<std::string, std::vector<GraphRow>> layouts;
        for (const auto& node : graph.nodes) if (node.block >= 0) layouts[node.id] = rowsOf(node, nullptr);
        auto output = [&](const EditorVfxNode& node) {
            return canvas.Point(effectsGraphPositions[node.id] + ImVec2(node.block >= 0 ? kBlockWidth : kReferenceWidth, EditorNodeCanvas::kHeader * 0.5f));
        };
        // Where a wire to a field ends: the field's row, or the header of its context while that is collapsed.
        auto input = [&](const EditorVfxNode& node, const std::string& field) {
            const std::vector<GraphRow>& list = layouts[node.id];
            std::string module;
            for (const Json& entry : effectsSchema["components"][node.kind == "Trail" ? 1 : 0]["fields"].items()) if (entry["name"].asString() == field) module = entry["module"].asString();
            float y = EditorNodeCanvas::kHeader * 0.5f;
            for (const GraphRow& row : list) {
                if ((row.kind == GraphRow::Field || row.kind == GraphRow::Trigger) && row.field == field) { y = row.y + row.height * 0.5f; break; }
                if (row.kind == GraphRow::Context) for (const char* name : GraphContexts()[static_cast<size_t>(row.context)].modules) if (module == name) y = row.y + row.height * 0.5f;
            }
            return canvas.Point(effectsGraphPositions[node.id] + ImVec2(0, y));
        };
        for (const auto& edge : graph.edges) if (nodes.count(edge.source) && nodes.count(edge.target)) {
            const float emphasis = effectsGraphSelected.empty() ? 1.0f : (edge.source == effectsGraphSelected || edge.target == effectsGraphSelected) ? 1.6f : 0.6f;
            canvas.Wire(output(*nodes[edge.source]), input(*nodes[edge.target], edge.field), KindWire(nodes[edge.source]->kind), emphasis);
        }
        if (nodes.count(effectsGraphLink)) canvas.Wire(output(*nodes[effectsGraphLink]), ImGui::GetIO().MousePos, IM_COL32(255, 213, 116, 255), 1.6f);
        const float text = ImGui::GetFontSize() * zoom;
        const bool widgets = text >= 9.5f;   // below that the values are shown as text
        const bool labels = text >= 6.5f;
        ImGui::PushFont(nullptr, text);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2) * zoom);
        ImDrawList* draw = canvas.draw;
        for (const auto& node : graph.nodes) {
            ImGui::PushID(node.id.c_str()); ImVec2& position = effectsGraphPositions[node.id];
            const ImVec2 p = canvas.Point(position), size = dimensions(node);
            const std::string title = node.block >= 0 || node.kind == "Param" ? node.name : node.kind == "Output" ? std::string(Tr("System")) : node.name.substr(node.name.find_last_of('/') == std::string::npos ? 0 : node.name.find_last_of('/') + 1);
            effectsLayoutDirty = canvas.Card(position, size, title.c_str(), KindColor(node.kind), effectsGraphSelected == node.id, true, Tr(node.kind.c_str())) || effectsLayoutDirty;
            EffectsRect("node:" + node.id);
            if (ImGui::IsItemClicked()) { effectsGraphSelected = node.id; if (node.block >= 0) effectsBlock = node.block; }
            if (node.kind == "Material" && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) OpenShader(node.name);
            if (node.kind != "Output" && node.kind != "Trail") {
                canvas.Pin("##output", output(node), true); EffectsRect("output:" + node.id);
                if (node.kind == "Emitter") ImGui::SetItemTooltip("%s", Tr("Drag to another emitter's Trigger: it then bursts where particles of this one die"));
                if (ImGui::IsItemActivated()) effectsGraphLink = node.id;
            }
            auto pinAt = [&](const std::string& field, ImVec2 at, bool connected) {
                ImGui::PushID(field.c_str());
                canvas.Pin("##input", at, connected); EffectsRect("input:" + node.id + ":" + field);
                const bool drop = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                    nodes.count(effectsGraphLink) && ImGui::IsMouseReleased(ImGuiMouseButton_Left);
                if (drop || ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    Json candidate; std::string hint;
                    if (PatchEditorVfxWire(effectsDocument, drop ? nodes[effectsGraphLink] : nullptr, node, field, candidate, hint)) {
                        EffectsReplace(candidate); hint = effectsError;
                    } else hint = Tr(hint.c_str());
                    effectsGraphHint = hint; effectsGraphErrorNode = hint.empty() ? "" : node.id;
                }
                ImGui::PopID();
            };
            if (node.block >= 0) {
                const std::vector<GraphRow>& list = layouts[node.id];
                const float labelWidth = kBlockWidth * 0.44f;
                for (const GraphRow& row : list) {
                    const ImVec2 at = p + ImVec2(0, row.y) * zoom, end = p + ImVec2(kBlockWidth, row.y + row.height) * zoom;
                    const float middle = (at.y + end.y - text) * 0.5f;
                    ImGui::PushID(static_cast<int>(&row - list.data()));
                    if (row.kind == GraphRow::Context) {
                        // A band in the context's color: click folds the context away.
                        const GraphContext& context = GraphContexts()[static_cast<size_t>(row.context)];
                        const std::string flag = node.id + "|ctx:" + std::to_string(row.context);
                        draw->AddRectFilled(at + ImVec2(1, 1), end - ImVec2(1, 1), (context.color & 0x00FFFFFF) | 0x66000000);
                        draw->AddRectFilled(at + ImVec2(1, 1), ImVec2(at.x + 5 * zoom, end.y - 1), context.color);
                        if (labels) draw->AddText(ImVec2(at.x + 14 * zoom, middle), IM_COL32(244, 246, 250, 255),
                            (std::string(effectsGraphFlags[flag] ? kIconChevronRight : kIconChevronDown) + " " + Tr(context.name)).c_str());
                        ImGui::SetCursorScreenPos(at + ImVec2(10, 0) * zoom);
                        if (ImGui::InvisibleButton("##context", ImVec2(kBlockWidth - 20, row.height) * zoom)) { effectsGraphFlags[flag] = !effectsGraphFlags[flag]; effectsLayoutDirty = true; }
                        EffectsRect("context:" + node.id + ":" + context.name);
                    } else if (row.kind == GraphRow::Module) {
                        // The group's name and how many of its fields are listed: click lists all of them (or only those in use).
                        const std::string flag = node.id + "|all:" + row.module;
                        const bool all = effectsGraphFlags[flag] != 0;
                        if (labels) {
                            draw->AddText(ImVec2(at.x + 16 * zoom, middle), IM_COL32(170, 182, 202, 255), Tr(row.module.c_str()));
                            const std::string count = all ? std::string(Tr("in use only")) : Format(Tr("%d of %d"), row.shown, row.total) + "  " + kIconPlus;
                            const float countWidth = ImGui::CalcTextSize(count.c_str()).x;
                            draw->AddText(ImVec2(end.x - countWidth - 12 * zoom, middle), IM_COL32(130, 142, 162, 255), count.c_str());
                        }
                        ImGui::SetCursorScreenPos(at + ImVec2(10, 0) * zoom);
                        if (ImGui::InvisibleButton("##module", ImVec2(kBlockWidth - 20, row.height) * zoom)) {
                            effectsGraphFlags[flag] = !all; effectsLayoutDirty = true;
                            effectsBlock = node.block; effectsGraphSelected = node.id;
                            const std::string shownModule = row.module.find("Expressions") != std::string::npos ? "Expressions" : row.module;
                            for (size_t i = 0; i < effectsSchema["modules"].size(); ++i) if (effectsSchema["modules"][i].asString() == shownModule) effectsModule = static_cast<int>(i);
                        }
                        EffectsRect("module:" + node.id + ":" + row.module);
                        ImGui::SetItemTooltip("%s", Tr(all ? "Lists only the fields this effect sets" : "Lists every field of this group"));
                    } else if (row.kind == GraphRow::Expression) {
                        const size_t split = row.field.find(':');
                        const std::string stageName = row.field.substr(0, split), channelName = row.field.substr(split + 1);
                        const int stageIndex = stageName == "update" ? 1 : 0;
                        const Json& source = valuesOf(node)["expressions"][stageName][channelName];
                        const bool authored = source.isString() && !source.asString().empty();
                        const bool active = effectsValueInGraph && effectsBlock == node.block &&
                            effectsCurveKeys["expression:stage"] == stageIndex && effectsCurveKeys["expression:channel"] == row.shown;
                        if (active) draw->AddRectFilled(at + ImVec2(1, 0), end - ImVec2(1, 0), IM_COL32(129, 224, 163, 44));
                        if (labels) {
                            draw->PushClipRect(at, ImVec2(at.x + labelWidth * zoom, end.y), true);
                            draw->AddText(ImVec2(at.x + 16 * zoom, middle), authored ? IM_COL32(170, 232, 190, 255) : IM_COL32(150, 160, 178, 255), HumanizeName(channelName).c_str());
                            draw->PopClipRect();
                            draw->PushClipRect(ImVec2(at.x + labelWidth * zoom, at.y), ImVec2(end.x - 10 * zoom, end.y), true);
                            draw->AddText(ImVec2(at.x + labelWidth * zoom, middle), authored ? IM_COL32(238, 204, 150, 255) : IM_COL32(120, 130, 148, 255),
                                authored ? source.asString().c_str() : Tr("click to add"));
                            draw->PopClipRect();
                        }
                        ImGui::SetCursorScreenPos(at + ImVec2(12, 0) * zoom);
                        if (ImGui::InvisibleButton("##expression", ImVec2(kBlockWidth - 24, row.height) * zoom)) {
                            // Shows this value's nodes on the canvas, ending in this row; again hides them.
                            if (active) effectsValueInGraph = false;
                            else {
                                effectsValueInGraph = true; effectsBlock = node.block; effectsGraphSelected = node.id; effectsValueSelected = -1;
                                effectsCurveKeys["expression:stage"] = stageIndex; effectsCurveKeys["expression:channel"] = row.shown;
                                for (size_t i = 0; i < effectsSchema["modules"].size(); ++i) if (effectsSchema["modules"][i].asString() == "Expressions") effectsModule = static_cast<int>(i);
                            }
                        }
                        EffectsRect("expression:" + node.id + ":" + row.field);
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                            ImGui::SetTooltip("%s%s%s", authored ? source.asString().c_str() : "", authored ? "\n" : "", Tr(active ? "Click to hide this value's nodes" : "Click to build this value from nodes on the canvas"));
                        if (ImGui::BeginPopupContextItem("##expressionmenu")) {
                            if (ImGui::MenuItem(Tr("Remove Expression"), nullptr, false, authored)) {
                                Json changed = effectsDocument;
                                changed["emitters"][node.block]["emitter"]["expressions"][stageName].erase(channelName);
                                if (active) effectsValueInGraph = false;
                                EffectsReplace(changed);
                            }
                            ImGui::EndPopup();
                        }
                        const ImVec2 pin(at.x, (at.y + end.y) * 0.5f);
                        canvas.Pin("##value", pin, authored); EffectsRect("input:" + node.id + ":" + row.field);
                        if (active) {
                            valueVisible = true; valueSink = pin; valueOrigin = position + ImVec2(-50, row.y - 40);
                            // The value's last node is dropped here; right-click leaves the value without one.
                            const bool drop = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && effectsValueLink >= 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Left);
                            if ((drop || ImGui::IsItemClicked(ImGuiMouseButton_Right)) && !effectsValueGraph.isNull()) {
                                Json candidate = effectsValueGraph; candidate["output"] = drop ? effectsValueLink : -1;
                                if (!EffectsValueApply(candidate)) effectsValueGraph = candidate;
                            }
                        }
                    } else {
                        const bool trigger = row.kind == GraphRow::Trigger;
                        bool connected = false;
                        for (const auto& edge : graph.edges) connected = connected || (edge.target == node.id && edge.field == row.field);
                        const bool chosen = !trigger && effectsBlock == node.block && effectsGraphField == row.field;
                        if (chosen) draw->AddRectFilled(at + ImVec2(1, 0), end - ImVec2(1, 0), IM_COL32(112, 202, 242, 36));
                        if (labels) {
                            draw->PushClipRect(at, ImVec2(at.x + (trigger ? kBlockWidth : labelWidth) * zoom, end.y), true);
                            draw->AddText(ImVec2(at.x + 16 * zoom, middle), connected ? IM_COL32(238, 204, 150, 255) : IM_COL32(214, 221, 232, 255),
                                trigger ? Tr("Trigger") : HumanizeName(row.field).c_str());
                            draw->PopClipRect();
                        }
                        // The label: click chooses the field (for a parameter), right-click offers what can be done with it.
                        ImGui::SetCursorScreenPos(at + ImVec2(12, 0) * zoom);
                        ImGui::InvisibleButton("##label", ImVec2((trigger ? kBlockWidth - 24 : labelWidth - 14), row.height) * zoom);
                        if (trigger) ImGui::SetItemTooltip("%s", Tr("Connect another emitter's output here: this emitter bursts where its particles die"));
                        else {
                            if (ImGui::IsItemClicked()) {
                                effectsBlock = node.block; effectsGraphField = row.field; effectsGraphSelected = node.id;
                                for (size_t i = 0; i < effectsSchema["modules"].size(); ++i) if (effectsSchema["modules"][i].asString() == row.module) effectsModule = static_cast<int>(i);
                            }
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && row.schema) ImGui::SetTooltip("%s\n%s", row.field.c_str(), (*row.schema)["doc"].asString().c_str());
                            if (ImGui::BeginPopupContextItem("##fieldmenu")) {
                                if (ImGui::MenuItem(WithIcon(kIconPlus, Tr("Make Parameter")).c_str(), nullptr, false, !connected)) addParameter(node.block, row.field);
                                if (ImGui::MenuItem(Tr("Reset to Default"), nullptr, false, !connected && valuesOf(node).has(row.field) && row.schema)) {
                                    Json patch = ObjectOf({{row.field.c_str(), (*row.schema)["default"]}});
                                    Call("vfx.set", ObjectOf({{"path", effectsPath}, {node.kind == "Trail" ? "trail" : "emitter", node.name}, {"values", patch}}));
                                }
                                ImGui::EndPopup();
                            }
                            const ImVec2 valueAt = at + ImVec2(labelWidth, 2) * zoom;
                            const float valueWidth = (kBlockWidth - labelWidth - 10) * zoom;
                            if (widgets && row.schema) {
                                ImGui::SetCursorScreenPos(valueAt);
                                ImGui::SetNextItemWidth(valueWidth);
                                ImGui::PushItemWidth(valueWidth);
                                EffectsFieldValue(node.block, *row.schema, "gfield:" + node.id + ":" + row.field);
                                ImGui::PopItemWidth();
                            } else if (labels && row.schema) {
                                // Far out: the value as text, so the node still says what the effect is.
                                const Json& values = valuesOf(node);
                                const Json& value = values.has(row.field) ? values[row.field] : (*row.schema)["default"];
                                const float side = (row.height - 10) * zoom;
                                if ((*row.schema)["type"].asString() == "color" && value.isArray() && value.size() >= 3)
                                    draw->AddRectFilled(ImVec2(valueAt.x, (at.y + end.y - side) * 0.5f), ImVec2(valueAt.x + side * 2, (at.y + end.y + side) * 0.5f),
                                        ImGui::ColorConvertFloat4ToU32(ImVec4(value[0].asFloat(), value[1].asFloat(), value[2].asFloat(), 1)), 2 * zoom);
                                else {
                                    draw->PushClipRect(ImVec2(valueAt.x, at.y), ImVec2(valueAt.x + valueWidth, end.y), true);
                                    draw->AddText(ImVec2(valueAt.x, middle), IM_COL32(238, 204, 150, 255), ValueText(value).c_str());
                                    draw->PopClipRect();
                                }
                            }
                        }
                        pinAt(row.field, ImVec2(at.x, (at.y + end.y) * 0.5f), connected);
                    }
                    ImGui::PopID();
                }
            } else if (node.kind == "Param" || node.kind == "Output") {
                ImGui::SetCursorScreenPos(p + ImVec2(12, EditorNodeCanvas::kHeader + 10) * zoom);
                ImGui::PushItemWidth((kReferenceWidth - 24) * zoom);
                if (!widgets) {
                    if (labels) draw->AddText(p + ImVec2(12, EditorNodeCanvas::kHeader + 12) * zoom, IM_COL32(238, 204, 150, 255),
                        (node.kind == "Param" ? ValueText(document["params"][node.name]["default"]) : std::to_string(document["budget"]["maxParticles"].asInt(10000))).c_str());
                } else if (node.kind == "Param") EffectsParameterValue(node.name, (kReferenceWidth - 24) * zoom);
                else {
                    int budget = effectsDocument["budget"]["maxParticles"].asInt(10000);
                    if (ImGui::DragInt("##graphbudget", &budget, 1, 0, 40000, Tr("%d particles at most"), ImGuiSliderFlags_AlwaysClamp)) {
                        Json changed = effectsDocument; changed["budget"]["maxParticles"] = budget;
                        EffectsReplace(changed, EffectsMerge("graphbudget"));
                    }
                    EffectsRect("graph-budget");
                }
                ImGui::PopItemWidth();
            } else {
                // A material or an anchor: its name, cut to the card; the whole of it in the tooltip.
                const std::string value = node.name;
                const char* tail = value.c_str() + value.size();
                while (tail > value.c_str() && ImGui::CalcTextSize(value.c_str(), tail).x > (kReferenceWidth - 24) * zoom) {
                    --tail; while (tail > value.c_str() && (static_cast<unsigned char>(*tail) & 0xc0) == 0x80) --tail;
                }
                if (labels) draw->AddText(p + ImVec2(12, EditorNodeCanvas::kHeader + 12) * zoom, IM_COL32(238, 204, 150, 255), value.c_str(), tail);
                ImGui::SetCursorScreenPos(p + ImVec2(12, EditorNodeCanvas::kHeader + 6) * zoom);
                ImGui::InvisibleButton("##description", ImVec2(kReferenceWidth - 24, 30) * zoom);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", node.kind == "Material" ? (value + "\n" + Tr("Double-click opens it in the Shader Graph")).c_str() : value.c_str());
                if (node.kind == "Material" && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) OpenShader(node.name);
            }
            if (effectsGraphErrorNode == node.id && !effectsGraphHint.empty()) {
                draw->AddRect(p, p + size * zoom, IM_COL32(255, 105, 90, 255), 6 * zoom, 2);
                if (ImGui::IsMouseHoveringRect(p, p + ImVec2(size.x, EditorNodeCanvas::kHeader) * zoom)) ImGui::SetTooltip("%s", effectsGraphHint.c_str());
            }
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
        ImGui::PopFont();
        if (effectsValueInGraph && valueVisible) {
            const int before = effectsValueSelected;
            EffectsValueCanvas(canvas, zoom, valuePrefix, valueOrigin, &valueSink);
            if (effectsValueSelected != before && effectsValueSelected >= 0) effectsGraphSelected.clear();
        }
        canvas.Footer(Tr("Drag output to pin: connect   Right-click pin: disconnect   Right-click field: more   Wheel: zoom"));
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Escape)) effectsGraphLink.clear();
    }
    ImGui::EndChild(); ImGui::SameLine();
    // Beside the graph: what the effect looks like, and every field of the block and group that is chosen.
    ImGui::BeginChild("##effectgraphinspector", ImVec2(0, 0), ImGuiChildFlags_Borders);
    const float side = std::min(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y * 0.5f);
    ImGui::BeginChild("##graphpreview", ImVec2(0, side + ImGui::GetFrameHeightWithSpacing() * 2 + 8)); if (effectsScene) EffectsPreview(true); ImGui::EndChild();
    ImGui::BeginChild("##graphproperties", ImVec2(0, 0));
    if (effectsValueInGraph) {
        // The value on the canvas: its node tools, the selected node, and the expression the nodes stand for.
        const int stage = effectsCurveKeys["expression:stage"], channel = effectsCurveKeys["expression:channel"];
        ImGui::SeparatorText(WithIcon(kIconBraces, std::string(Tr(stage ? "Update" : "Birth")) + " / " + HumanizeName(VfxChannelNames()[static_cast<size_t>(channel)])).c_str());
        EffectsValuePalette(valuePrefix, valueOrigin + ImVec2(-240, 70));
        if (!effectsValueError.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.6f, 0.45f, 1)); ImGui::TextWrapped("%s", effectsValueError.c_str()); ImGui::PopStyleColor(); }
        if (effectsValueGraph.isNull()) ImGui::TextWrapped("%s", Tr("Keep this expression in text view; conversion was refused."));
        EffectsValueProperties(valuePrefix);
    }
    EffectsInspector(); ImGui::EndChild();
    ImGui::EndChild();
    if (effectsLayoutDirty && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle)) EffectsGraphLayout(true);
}
}  // namespace oe
