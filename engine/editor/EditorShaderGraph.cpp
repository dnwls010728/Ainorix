#include "editor/EditorShaderGraph.h"
#include <functional>
#include <cmath>
#include "render/ShaderGraph.h"

namespace oe {
namespace {
const char* kOutputs[] = {"color", "emissive", "normal", "offset"};

bool Dependencies(const Json& document, bool disconnected, std::vector<int>& order, std::string* error) {
    const Json& nodes = document["nodes"];
    auto fail = [&](const std::string& message) { if (error) *error = message; return false; };
    if (!nodes.isArray() || nodes.size() == 0 || nodes.size() > ShaderGraph::kMaxNodes)
        return fail("nodes must contain 1..48 instructions");
    std::vector<int> state(nodes.size(), 0);
    std::function<bool(int)> visit = [&](int index) {
        if (state[static_cast<size_t>(index)] == 1) return fail("shader connections must not form a cycle");
        if (state[static_cast<size_t>(index)] == 2) return true;
        state[static_cast<size_t>(index)] = 1;
        for (const Json& input : nodes[index]["args"].items()) {
            if (disconnected && input.isNumber() && input.asNumber() == -1) continue;
            if (!input.isNumber() || !std::isfinite(input.asNumber()) || input.asNumber() < 0 ||
                input.asNumber() >= static_cast<double>(nodes.size()) || std::floor(input.asNumber()) != input.asNumber())
                return fail("nodes[" + std::to_string(index) + "]: connect every input pin");
            int source = input.asInt();
            if (!visit(source)) return false;
        }
        state[static_cast<size_t>(index)] = 2;
        order.push_back(index);
        return true;
    };
    for (int index = 0; index < static_cast<int>(nodes.size()); ++index) if (!visit(index)) return false;
    return true;
}
}

const std::vector<std::string>& EditorShaderOperations() {
    static const std::vector<std::string> operations = {"constant", "uniform", "uv", "position", "normal", "time", "baseColor",
        "texture", "add", "subtract", "multiply", "divide", "min", "max", "sin", "cos", "floor", "fract", "abs",
        "clamp", "mix", "step", "dot", "normalize", "swizzle"};
    return operations;
}

int EditorShaderArity(const std::string& op) {
    for (const char* name : {"constant", "uniform", "uv", "position", "normal", "time", "baseColor"}) if (op == name) return 0;
    for (const char* name : {"texture", "sin", "cos", "floor", "fract", "abs", "normalize", "swizzle"}) if (op == name) return 1;
    for (const char* name : {"add", "subtract", "multiply", "divide", "min", "max", "step", "dot"}) if (op == name) return 2;
    if (op == "clamp" || op == "mix") return 3;
    return -1;
}

bool OrderEditorShaderGraph(const Json& document, Json& ordered, std::string* error) {
    std::vector<int> order;
    if (!Dependencies(document, false, order, error)) return false;
    std::vector<int> remap(order.size());
    for (size_t i = 0; i < order.size(); ++i) remap[static_cast<size_t>(order[i])] = static_cast<int>(i);
    Json result = document;
    result["nodes"] = Json::MakeArray();
    for (int index : order) {
        Json node = document["nodes"][index];
        for (Json& input : node["args"].items()) input = remap[static_cast<size_t>(input.asInt())];
        if (node["args"].isNull()) node.erase("args");
        result["nodes"].push(node);
    }
    for (const char* name : kOutputs) {
        if (!document.has(name)) continue;
        const Json& value = document[name];
        if (!value.isNumber() || !std::isfinite(value.asNumber()) || value.asNumber() < 0 ||
            value.asNumber() >= static_cast<double>(remap.size()) || std::floor(value.asNumber()) != value.asNumber()) {
            if (error) *error = std::string(name) + ": choose an output node";
            return false;
        }
        int index = value.asInt();
        result[name] = remap[static_cast<size_t>(index)];
    }
    ShaderGraph compiled;
    if (!CompileShaderGraph(result, compiled, error)) return false;
    ordered = result;
    if (error) error->clear();
    return true;
}

bool ConnectEditorShaderGraph(Json& document, int source, int target, int pin, std::string* error) {
    int count = static_cast<int>(document["nodes"].size());
    if (source < 0 || source >= count || target < 0 || target >= count || pin < 0 ||
        pin >= static_cast<int>(document["nodes"][target]["args"].size())) {
        if (error) *error = "invalid shader pin";
        return false;
    }
    Json candidate = document;
    candidate["nodes"][target]["args"][pin] = source;
    std::vector<int> order;
    if (!Dependencies(candidate, true, order, error)) return false;
    document = candidate;
    if (error) error->clear();
    return true;
}

void RemoveEditorShaderNode(Json& document, int index) {
    auto& nodes = document["nodes"].items();
    if (index < 0 || index >= static_cast<int>(nodes.size())) return;
    nodes.erase(nodes.begin() + index);
    auto remap = [index](Json& value) {
        int old = value.asInt(-1);
        if (old == index) value = -1;
        else if (old > index) value = old - 1;
    };
    for (Json& node : nodes) for (Json& input : node["args"].items()) remap(input);
    for (const char* name : kOutputs) {
        if (!document.has(name)) continue;
        remap(document[name]);
        if (std::string(name) != "color" && document[name].asInt(-1) == -1) document.erase(name);
    }
}
}  // namespace oe
