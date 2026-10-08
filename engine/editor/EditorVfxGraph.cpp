#include "editor/EditorVfxGraph.h"
#include "scene/VfxExpression.h"
#include <algorithm>
#include <set>

namespace oe {
std::string UniqueEditorVfxName(const Json& document, const std::string& prefix) {
    for (int suffix = 0; ; ++suffix) {
        const std::string name = prefix + std::to_string(suffix); bool used = document["params"].has(name);
        for (const char* list : {"emitters", "trails"}) for (const Json& block : document[list].items())
            used = used || block["name"].asString() == name;
        if (!used) return name;
    }
}

EditorVfxGraph BuildEditorVfxGraph(const Json& document, const std::vector<std::string>& materials,
    const std::vector<std::string>& anchors, const std::string& field) {
    EditorVfxGraph graph;
    std::set<std::string> materialPaths(materials.begin(), materials.end()), anchorNames(anchors.begin(), anchors.end());
    int index = 0;
    for (const char* list : {"emitters", "trails"}) for (const Json& block : document[list].items()) {
        const bool trail = std::string(list) == "trails";
        const std::string name = block["name"].asString();
        const Json& values = block[trail ? "trail" : "emitter"];
        EditorVfxNode node{"block:" + name, trail ? "Trail" : "Emitter", name, index++, {"material"}};
        if (trail) { node.inputs.push_back("baseAnchor"); node.inputs.push_back("tipAnchor"); }
        else node.inputs.push_back("trigger");
        const std::string parameterField = trail && field == "rate" ? "width" : field;
        if (std::find(node.inputs.begin(), node.inputs.end(), parameterField) == node.inputs.end()) node.inputs.push_back(parameterField);
        for (const auto& param : document["params"].members()) for (const Json& target : param.second["targets"].items()) {
            const std::string to = target["to"].asString(), prefix = name + ".";
            if (to.compare(0, prefix.size(), prefix) != 0) continue;
            const std::string property = to.substr(prefix.size());
            if (std::find(node.inputs.begin(), node.inputs.end(), property) == node.inputs.end()) node.inputs.push_back(property);
            graph.edges.push_back({"param:" + param.first, node.id, property});
        }
        auto mapped = [&](const std::string& property) {
            for (const auto& parameter : document["params"].members()) for (const Json& target : parameter.second["targets"].items())
                if (target["to"].asString() == name + "." + property) return true;
            return false;
        };
        const std::string material = values["material"].asString();
        if (!material.empty()) {
            materialPaths.insert(material);
            if (!mapped("material")) graph.edges.push_back({"material:" + material, node.id, "material"});
        }
        if (!trail && !values["onDeath"].asString().empty() && !mapped("onDeath"))
            graph.edges.push_back({node.id, "block:" + values["onDeath"].asString(), "trigger"});
        if (trail) for (const char* anchor : {"baseAnchor", "tipAnchor"}) {
            const std::string reference = values[anchor].asString();
            if (!reference.empty()) {
                anchorNames.insert(reference);
                if (!mapped(anchor)) graph.edges.push_back({"anchor:" + reference, node.id, anchor});
            }
        }
        graph.nodes.push_back(node);
    }
    for (const auto& parameter : document["params"].members())
        graph.nodes.push_back({"param:" + parameter.first, "Param", parameter.first, -1, {}});
    for (const std::string& path : materialPaths) graph.nodes.push_back({"material:" + path, "Material", path, -1, {}});
    for (const std::string& name : anchorNames) graph.nodes.push_back({"anchor:" + name, "Anchor", name, -1, {}});
    graph.nodes.push_back({"output", "Output", "", -1, {}});
    return graph;
}
bool PatchEditorVfxWire(const Json& document, const EditorVfxNode* source, const EditorVfxNode& target,
    const std::string& field, Json& candidate, std::string& hint) {
    candidate = document;
    if (target.block < 0) { hint = "Connect references to an emitter or trail input."; return false; }
    const int count = static_cast<int>(document["emitters"].size());
    const bool trail = target.block >= count;
    Json& values = candidate[trail ? "trails" : "emitters"][trail ? target.block - count : target.block][trail ? "trail" : "emitter"];
    if (source && source->kind != "Param") {
        const std::string controlled = field == "trigger" && source->kind == "Emitter" ? source->name + ".onDeath" : target.name + "." + field;
        for (const auto& entry : document["params"].members()) for (const Json& mapping : entry.second["targets"].items())
            if (mapping["to"].asString() == controlled) { hint = "Disconnect the existing parameter target first."; return false; }
    }
    if (source && source->kind == "Param") {
        const std::string to = target.name + "." + field;
        for (const auto& entry : document["params"].members()) for (const Json& mapping : entry.second["targets"].items())
            if (mapping["to"].asString() == to) { hint = "Disconnect the existing parameter target first."; return false; }
        Json mapping = Json::MakeObject(); mapping["to"] = to;
        candidate["params"][source->name]["targets"].push(mapping); return true;
    }
    if (!source) {
        std::vector<std::string> empty;
        for (auto& entry : candidate["params"].members()) {
            auto& mappings = entry.second["targets"].items();
            const size_t before=mappings.size();
            mappings.erase(std::remove_if(mappings.begin(), mappings.end(), [&](const Json& mapping) {
                return mapping["to"].asString() == target.name + "." + field;
            }), mappings.end());
            if (before>0 && mappings.empty()) {
                Json defaults=Json::MakeObject();
                for (const auto& parameter : document["params"].members()) defaults[parameter.first]=parameter.second["default"];
                bool referenced=false;
                for (const Json& block : document["emitters"].items()) {
                    VfxExpressionSet compiled;VfxExpressionError error;
                    if (!CompileVfxExpressions(block["emitter"].has("expressions")?block["emitter"]["expressions"]:Json::MakeObject(),defaults,compiled,&error)) continue;
                    for (const auto* programs : {&compiled.birth,&compiled.update}) for (const auto& program : *programs)
                        for (const auto& instruction : program.code)
                            referenced=referenced || (instruction.op==VfxExprOp::Parameter && instruction.name==entry.first);
                }
                if (!referenced) empty.push_back(entry.first);
            }
        }
        for (const std::string& name : empty) candidate["params"].erase(name);
        if (field == "trigger") {
            for (Json& block : candidate["emitters"].items())
                if (block["emitter"]["onDeath"].asString() == target.name) block["emitter"].erase("onDeath");
        } else if (field == "material" || field == "baseAnchor" || field == "tipAnchor") values.erase(field);
        return true;
    }
    if (field == "material" && source->kind == "Material") { values[field] = source->name; return true; }
    if ((field == "baseAnchor" || field == "tipAnchor") && source->kind == "Anchor") { values[field] = source->name; return true; }
    if (field == "trigger" && target.kind == "Emitter" && source->kind == "Emitter") {
        candidate["emitters"][source->block]["emitter"]["onDeath"] = target.name; return true;
    }
    hint = "Use Material for material, Anchor for ribbon anchors, Emitter for trigger, or Param for a component field.";
    return false;
}
}  // namespace oe
