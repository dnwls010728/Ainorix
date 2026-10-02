#include "render/ShaderGraph.h"

#include <algorithm>
#include <cmath>

namespace oe {
namespace {
struct OpInfo { const char* name; ShaderOp op; int args; };
const OpInfo kOps[] = {
    {"constant",ShaderOp::Constant,0}, {"uniform",ShaderOp::Uniform,0}, {"uv",ShaderOp::UV,0},
    {"position",ShaderOp::Position,0}, {"normal",ShaderOp::Normal,0}, {"time",ShaderOp::Time,0},
    {"baseColor",ShaderOp::BaseColor,0}, {"texture",ShaderOp::Texture,1},
    {"add",ShaderOp::Add,2}, {"subtract",ShaderOp::Subtract,2}, {"multiply",ShaderOp::Multiply,2},
    {"divide",ShaderOp::Divide,2}, {"min",ShaderOp::Min,2}, {"max",ShaderOp::Max,2},
    {"sin",ShaderOp::Sin,1}, {"cos",ShaderOp::Cos,1}, {"floor",ShaderOp::Floor,1},
    {"fract",ShaderOp::Fract,1}, {"abs",ShaderOp::Abs,1}, {"clamp",ShaderOp::Clamp,3},
    {"mix",ShaderOp::Mix,3}, {"step",ShaderOp::Step,2}, {"dot",ShaderOp::Dot,2},
    {"normalize",ShaderOp::Normalize,1}, {"swizzle",ShaderOp::Swizzle,1}
};
bool Vector(const Json& j, Vec4& out) {
    auto finite = [](const Json& v) { return v.isNumber() && std::isfinite(v.asNumber()) && std::fabs(v.asNumber()) <= 65504; };
    if (finite(j)) { const float x = j.asFloat(); out = Vec4(x,x,x,x); return true; }
    if (!j.isArray() || j.size() != 4) return false;
    for (const Json& v : j.items()) if (!finite(v)) return false;
    out = Vec4(j[0].asFloat(),j[1].asFloat(),j[2].asFloat(),j[3].asFloat());
    return true;
}
bool Index(const Json& j, int count) {
    return j.isNumber() && j.asNumber() >= 0 && j.asNumber() < count && std::floor(j.asNumber()) == j.asNumber();
}
float Finite(float x) { return std::isnan(x) ? 0 : std::clamp(x, -65504.0f, 65504.0f); }
}  // namespace

bool CompileShaderGraph(const Json& json, ShaderGraph& out, std::string* error) {
    auto fail = [&](const std::string& message) { if (error) *error = message; return false; };
    if (!json.isObject()) return fail("shader must be an object");
    for (const auto& field : json.members())
        if (field.first != "format" && field.first != "nodes" && field.first != "uniforms" &&
            field.first != "color" && field.first != "emissive") return fail("unknown shader field '" + field.first + "'");
    if (json.has("format") && json["format"].asString() != "ownengine.shader") return fail("format must be ownengine.shader");
    const Json& nodes = json["nodes"];
    if (!nodes.isArray() || nodes.size() == 0 || nodes.size() > ShaderGraph::kMaxNodes) return fail("nodes must contain 1..32 instructions");
    ShaderGraph result;
    const Json& uniforms = json["uniforms"];
    if (!uniforms.isNull() && (!uniforms.isObject() || uniforms.size() > ShaderGraph::kMaxUniforms))
        return fail("uniforms must be an object with at most 8 named values");
    for (const auto& item : uniforms.members()) {
        if (item.first.empty()) return fail("uniform name must not be empty");
        Vec4 value;
        if (!Vector(item.second, value)) return fail("uniform '" + item.first + "' must be a finite scalar or four-vector");
        result.defaults[result.uniformNames.size()] = value;
        result.uniformNames.push_back(item.first);
    }
    for (size_t index = 0; index < nodes.size(); ++index) {
        const Json& node = nodes[index];
        const std::string location = "nodes[" + std::to_string(index) + "]";
        if (!node.isObject()) return fail(location + " must be an object");
        const OpInfo* info = nullptr;
        for (const OpInfo& op : kOps) if (node["op"].asString() == op.name) info = &op;
        if (!info) return fail(location + ".op is unknown");
        for (const auto& item : node.members()) {
            const bool value = item.first == "value" && (info->op == ShaderOp::Constant || info->op == ShaderOp::Swizzle);
            const bool name = item.first == "name" && info->op == ShaderOp::Uniform;
            if (item.first != "op" && item.first != "args" && !value && !name) return fail(location + ": unexpected field '" + item.first + "'");
        }
        const Json& args = node["args"];
        if ((info->args > 0 && !args.isArray()) || (!args.isNull() && (!args.isArray() || args.size() != static_cast<size_t>(info->args))))
            return fail(location + ".args requires " + std::to_string(info->args) + " prior node indices");
        ShaderInstruction instruction;
        instruction.op = info->op;
        for (int a = 0; a < info->args; ++a) {
            if (!Index(args[a], static_cast<int>(index))) return fail(location + ".args must reference earlier nodes (no cycles)");
            instruction.args[static_cast<size_t>(a)] = args[a].asInt();
        }
        if (info->op == ShaderOp::Constant || info->op == ShaderOp::Swizzle) {
            if (!Vector(node["value"], instruction.value)) return fail(location + ".value must be a finite scalar or four-vector");
            if (info->op == ShaderOp::Swizzle) {
                if (!node["value"].isArray()) return fail(location + ".value must contain four channel indices");
                for (const Json& v : node["value"].items()) if (!Index(v, 4)) return fail(location + ".value channels must be 0..3");
            }
        }
        if (info->op == ShaderOp::Uniform) {
            auto it = std::find(result.uniformNames.begin(), result.uniformNames.end(), node["name"].asString());
            if (it == result.uniformNames.end()) return fail(location + ".name is not a declared uniform");
            instruction.value.x = static_cast<float>(it - result.uniformNames.begin());
        }
        result.instructions.push_back(instruction);
    }
    if (!Index(json["color"], static_cast<int>(nodes.size()))) return fail("color must reference a node");
    result.color = json["color"].asInt();
    if (json.has("emissive")) {
        if (!Index(json["emissive"], static_cast<int>(nodes.size()))) return fail("emissive must reference a node");
        result.emissive = json["emissive"].asInt();
    }
    out = std::move(result);
    if (error) error->clear();
    return true;
}

bool ShaderUniforms(const ShaderGraph& graph, const Json& values,
                    std::array<Vec4, ShaderGraph::kMaxUniforms>& out, std::string* error) {
    auto fail = [&](const std::string& message) { if (error) *error = message; return false; };
    if (!values.isNull() && !values.isObject()) return fail("shader uniform overrides must be an object");
    auto result = graph.defaults;
    for (const auto& item : values.members()) {
        auto it = std::find(graph.uniformNames.begin(), graph.uniformNames.end(), item.first);
        if (it == graph.uniformNames.end()) return fail("unknown shader uniform '" + item.first + "'");
        if (!Vector(item.second, result[static_cast<size_t>(it - graph.uniformNames.begin())]))
            return fail("shader uniform '" + item.first + "' must be a finite scalar or four-vector");
    }
    out = result;
    if (error) error->clear();
    return true;
}

ShaderSurface EvaluateShaderGraph(const ShaderGraph& graph, const ShaderInputs& inputs,
                                 const std::array<Vec4, ShaderGraph::kMaxUniforms>& uniforms) {
    std::array<Vec4, ShaderGraph::kMaxNodes> registers{};
    for (size_t index = 0; index < graph.instructions.size(); ++index) {
        const ShaderInstruction& instruction = graph.instructions[index];
        const Vec4 a = registers[static_cast<size_t>(instruction.args[0])];
        const Vec4 b = registers[static_cast<size_t>(instruction.args[1])];
        const Vec4 c = registers[static_cast<size_t>(instruction.args[2])];
        Vec4 result;
        switch (instruction.op) {
            case ShaderOp::Constant: result = instruction.value; break;
            case ShaderOp::Uniform: result = uniforms[static_cast<size_t>(instruction.value.x)]; break;
            case ShaderOp::UV: result = inputs.uv; break;
            case ShaderOp::Position: result = inputs.position; break;
            case ShaderOp::Normal: result = inputs.normal; break;
            case ShaderOp::Time: result = Vec4(inputs.time,inputs.time,inputs.time,inputs.time); break;
            case ShaderOp::BaseColor: result = inputs.baseColor; break;
            case ShaderOp::Texture: result = inputs.texture ? inputs.texture(a.x,a.y) : Vec4(1,1,1,1); break;
            default: {
                float av[] = {a.x,a.y,a.z,a.w}, bv[] = {b.x,b.y,b.z,b.w}, cv[] = {c.x,c.y,c.z,c.w};
                float channels[] = {0,0,0,0};
                const float dot = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
                const float length = std::sqrt(a.x*a.x + a.y*a.y + a.z*a.z + a.w*a.w);
                const float swizzle[] = {instruction.value.x,instruction.value.y,instruction.value.z,instruction.value.w};
                for (int k = 0; k < 4; ++k) switch (instruction.op) {
                    case ShaderOp::Add: channels[k] = av[k]+bv[k]; break;
                    case ShaderOp::Subtract: channels[k] = av[k]-bv[k]; break;
                    case ShaderOp::Multiply: channels[k] = av[k]*bv[k]; break;
                    case ShaderOp::Divide: channels[k] = bv[k] == 0 ? 0 : av[k]/bv[k]; break;
                    case ShaderOp::Min: channels[k] = std::min(av[k],bv[k]); break;
                    case ShaderOp::Max: channels[k] = std::max(av[k],bv[k]); break;
                    case ShaderOp::Sin: channels[k] = std::sin(av[k]); break;
                    case ShaderOp::Cos: channels[k] = std::cos(av[k]); break;
                    case ShaderOp::Floor: channels[k] = std::floor(av[k]); break;
                    case ShaderOp::Fract: channels[k] = av[k]-std::floor(av[k]); break;
                    case ShaderOp::Abs: channels[k] = std::fabs(av[k]); break;
                    case ShaderOp::Clamp: channels[k] = std::min(std::max(av[k],bv[k]),cv[k]); break;
                    case ShaderOp::Mix: channels[k] = av[k]*(1-cv[k])+bv[k]*cv[k]; break;
                    case ShaderOp::Step: channels[k] = bv[k] < av[k] ? 0.0f : 1.0f; break;
                    case ShaderOp::Dot: channels[k] = dot; break;
                    case ShaderOp::Normalize: channels[k] = length > 1e-8f ? av[k]/length : 0; break;
                    case ShaderOp::Swizzle: channels[k] = av[static_cast<int>(swizzle[k])]; break;
                    default: break;
                }
                result = Vec4(channels[0],channels[1],channels[2],channels[3]);
                break;
            }
        }
        registers[index] = Vec4(Finite(result.x),Finite(result.y),Finite(result.z),Finite(result.w));
    }
    return {registers[static_cast<size_t>(graph.color)],
            graph.emissive < 0 ? Vec3() : registers[static_cast<size_t>(graph.emissive)].xyz()};
}

}  // namespace oe
