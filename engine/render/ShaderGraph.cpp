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
    {"normalize",ShaderOp::Normalize,1}, {"swizzle",ShaderOp::Swizzle,1},
    {"particleAge",ShaderOp::ParticleAge,0}, {"particleRandom",ShaderOp::ParticleRandom,0}, {"particleSpeed",ShaderOp::ParticleSpeed,0}, {"particleCustom0",ShaderOp::ParticleCustom0,0}, {"particleCustom1",ShaderOp::ParticleCustom1,0}, {"particleCustom2",ShaderOp::ParticleCustom2,0}, {"particleCustom3",ShaderOp::ParticleCustom3,0},
    {"pow",ShaderOp::Pow,2}, {"length",ShaderOp::Length,1}, {"smoothstep",ShaderOp::Smoothstep,3},
    {"noise",ShaderOp::Noise,1}, {"fbm",ShaderOp::Fbm,1},
    {"sceneColor",ShaderOp::SceneColor,1}, {"sceneBlur",ShaderOp::SceneBlur,2}, {"texel",ShaderOp::Texel,0},
    {"ambient",ShaderOp::Ambient,0}, {"lightVector",ShaderOp::LightVector,0}, {"lightColor",ShaderOp::LightColor,0},
    {"lightAttenuation",ShaderOp::LightAttenuation,0}, {"viewDirection",ShaderOp::ViewDirection,0},
    {"objectPosition",ShaderOp::ObjectPosition,0}
};
// Inputs that exist only on a surface (mesh, sprite, particle) or only on the display image.
bool SurfaceOnly(ShaderOp op) {
    switch (op) {
        case ShaderOp::Position: case ShaderOp::Normal: case ShaderOp::BaseColor: case ShaderOp::Texture: case ShaderOp::Ambient:
        case ShaderOp::LightVector: case ShaderOp::LightColor: case ShaderOp::LightAttenuation:
        case ShaderOp::ViewDirection: case ShaderOp::ObjectPosition:
        case ShaderOp::ParticleAge: case ShaderOp::ParticleRandom: case ShaderOp::ParticleSpeed:
        case ShaderOp::ParticleCustom0: case ShaderOp::ParticleCustom1: case ShaderOp::ParticleCustom2: case ShaderOp::ParticleCustom3:
            return true;
        default: return false;
    }
}
bool PostOnly(ShaderOp op) { return op == ShaderOp::SceneColor || op == ShaderOp::SceneBlur || op == ShaderOp::Texel; }
bool LightInput(ShaderOp op) { return op == ShaderOp::LightVector || op == ShaderOp::LightColor || op == ShaderOp::LightAttenuation; }
float Fract(float x) { return x - std::floor(x); }
// hash13 of the three.js prototype ("hash without sine"): a lattice point to 0..1.
float Hash13(float x, float y, float z) {
    x = Fract(x * 0.1031f); y = Fract(y * 0.1031f); z = Fract(z * 0.1031f);
    const float d = x * (z + 31.32f) + y * (y + 31.32f) + z * (x + 31.32f);
    x += d; y += d; z += d;
    return Fract((x + y) * z);
}
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

float ShaderNoise(float x, float y, float z) {
    const float ix = std::floor(x), iy = std::floor(y), iz = std::floor(z);
    float fx = x - ix, fy = y - iy, fz = z - iz;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    fz = fz * fz * (3 - 2 * fz);
    auto mix = [](float a, float b, float t) { return a + (b - a) * t; };
    auto plane = [&](float dz) {
        return mix(mix(Hash13(ix, iy, iz + dz), Hash13(ix + 1, iy, iz + dz), fx),
                   mix(Hash13(ix, iy + 1, iz + dz), Hash13(ix + 1, iy + 1, iz + dz), fx), fy);
    };
    return mix(plane(0), plane(1), fz);
}

float ShaderFbm(float x, float y, float z) {
    float amplitude = 0.5f, sum = 0;
    for (int octave = 0; octave < 4; ++octave) {
        sum += amplitude * ShaderNoise(x, y, z);
        x *= 2.03f; y *= 2.03f; z *= 2.03f;
        amplitude *= 0.5f;
    }
    return sum;
}

float SceneBlurWeight(int i, int j) { return std::exp(-static_cast<float>(i * i + j * j) * 0.35f); }

bool CompileShaderGraph(const Json& json, ShaderGraph& out, std::string* error) {
    auto fail = [&](const std::string& message) { if (error) *error = message; return false; };
    if (!json.isObject()) return fail("shader must be an object");
    for (const auto& field : json.members())
        if (field.first != "format" && field.first != "nodes" && field.first != "uniforms" && field.first != "stage" &&
            field.first != "color" && field.first != "emissive" && field.first != "normal" && field.first != "offset" && field.first != "light" &&
            field.first != "maxOffset" && field.first != "offsetBounds" && field.first != "injection") return fail("unknown shader field '" + field.first + "'");
    if (json.has("format") && json["format"].asString() != "ownengine.shader") return fail("format must be ownengine.shader");
    ShaderGraph result;
    if (json.has("stage")) {
        const std::string stage = json["stage"].asString();
        if (stage == "post") result.stage = ShaderStage::Post;
        else if (stage != "surface") return fail("stage must be \"surface\" (materials) or \"post\" (PostProcess.shader)");
    }
    const bool post = result.stage == ShaderStage::Post;
    if (json.has("injection")) {
        if (!post) return fail("injection is only allowed on a post graph (stage: post)");
        const std::string injection = json["injection"].asString();
        if (injection != "afterToneMapping" && injection != "beforeToneMapping")
            return fail("injection must be \"afterToneMapping\" or \"beforeToneMapping\"");
        result.beforeToneMapping = injection == "beforeToneMapping";
    }
    if (post)
        for (const char* output : {"emissive", "normal", "offset", "light", "maxOffset", "offsetBounds"})
            if (json.has(output)) return fail(std::string("a post graph has only a color output, not '") + output + "'");
    const int maxNodes = ShaderGraph::kMaxNodes;
    const Json& nodes = json["nodes"];
    if (!nodes.isArray() || nodes.size() == 0 || nodes.size() > static_cast<size_t>(maxNodes))
        return fail("nodes must contain 1.." + std::to_string(maxNodes) + " instructions");
    const Json& uniforms = json["uniforms"];
    if (!uniforms.isNull() && (!uniforms.isObject() || uniforms.size() > ShaderGraph::kMaxUniforms))
        return fail("uniforms must be an object with at most 8 named values");
    for (const auto& item : uniforms.members()) {
        if (item.first.empty()) return fail("uniform name must not be empty");
        Vec4 value;
        if (!Vector(item.second, value)) return fail("uniform '" + item.first + "' must be a finite scalar or four-vector");
        result.defaults[result.uniformNames.size()] = value;
        if (item.first == "cameraPosition") result.cameraUniform = static_cast<int>(result.uniformNames.size());
        if (item.first == "lightDirection") result.lightUniform = static_cast<int>(result.uniformNames.size());
        if (item.first == "playing") result.playingUniform = static_cast<int>(result.uniformNames.size());
        if (item.first == "screenSize") result.screenUniform = static_cast<int>(result.uniformNames.size());
        if (item.first == "pixelScale") result.pixelScaleUniform = static_cast<int>(result.uniformNames.size());
        result.uniformNames.push_back(item.first);
    }
    for (size_t index = 0; index < nodes.size(); ++index) {
        const Json& node = nodes[index];
        const std::string location = "nodes[" + std::to_string(index) + "]";
        if (!node.isObject()) return fail(location + " must be an object");
        const OpInfo* info = nullptr;
        for (const OpInfo& op : kOps) if (node["op"].asString() == op.name) info = &op;
        if (!info) return fail(location + ".op is unknown");
        if (post && SurfaceOnly(info->op)) return fail(location + ": '" + info->name + "' is a surface input; a post graph reads sceneColor/sceneBlur at uv");
        if (!post && PostOnly(info->op)) return fail(location + ": '" + info->name + "' needs \"stage\": \"post\" (PostProcess.shader)");
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
        result.objectSpace = result.objectSpace || info->op == ShaderOp::ObjectPosition;
        result.instructions.push_back(instruction);
    }
    if (!Index(json["color"], static_cast<int>(nodes.size()))) return fail("color must reference a node");
    result.color = json["color"].asInt();
    if (json.has("emissive")) {
        if (!Index(json["emissive"], static_cast<int>(nodes.size()))) return fail("emissive must reference a node");
        result.emissive = json["emissive"].asInt();
    }
    if (json.has("normal")) {
        if (!Index(json["normal"], static_cast<int>(nodes.size()))) return fail("normal must reference a node");
        result.normal = json["normal"].asInt();
    }
    if (json.has("offset")) {
        if (!Index(json["offset"], static_cast<int>(nodes.size()))) return fail("offset must reference a node");
        result.offset = json["offset"].asInt();
        // The vertex stage has no texture: reject graphs whose displacement reads one.
        std::array<bool, ShaderGraph::kMaxNodes> needed{};
        needed[static_cast<size_t>(result.offset)] = true;
        for (int index = result.offset; index >= 0; --index) {
            if (!needed[static_cast<size_t>(index)]) continue;
            const ShaderInstruction& instruction = result.instructions[static_cast<size_t>(index)];
            if (instruction.op == ShaderOp::Texture) return fail("offset must not depend on a texture node (nodes[" + std::to_string(index) + "])");
            if (instruction.op == ShaderOp::Ambient) return fail("offset must not depend on an ambient node (nodes[" + std::to_string(index) + "])");
            int count = 0;
            for (const OpInfo& op : kOps) if (op.op == instruction.op) count = op.args;
            for (int a = 0; a < count; ++a) needed[static_cast<size_t>(instruction.args[static_cast<size_t>(a)])] = true;
        }
    }
    if (json.has("light")) {
        if (!Index(json["light"], static_cast<int>(nodes.size()))) return fail("light must reference a node");
        result.light = json["light"].asInt();
    }
    // Light inputs exist only while one light is shaded: every other output must not depend on them.
    for (const auto& [name, output] : std::initializer_list<std::pair<const char*, int>>{
             {"color", result.color}, {"emissive", result.emissive}, {"normal", result.normal}, {"offset", result.offset}}) {
        if (output < 0) continue;
        std::array<bool, ShaderGraph::kMaxNodes> needed{};
        needed[static_cast<size_t>(output)] = true;
        for (int index = output; index >= 0; --index) {
            if (!needed[static_cast<size_t>(index)]) continue;
            const ShaderInstruction& instruction = result.instructions[static_cast<size_t>(index)];
            if (LightInput(instruction.op))
                return fail(std::string(name) + " must not depend on a light input (nodes[" + std::to_string(index) + "]); light inputs feed the light output");
            int count = 0;
            for (const OpInfo& op : kOps) if (op.op == instruction.op) count = op.args;
            for (int a = 0; a < count; ++a) needed[static_cast<size_t>(instruction.args[static_cast<size_t>(a)])] = true;
        }
    }
    if (json.has("maxOffset")) {
        const Json& limit = json["maxOffset"];
        if (result.offset < 0) return fail("maxOffset requires an offset output");
        if (!limit.isNumber() || !(limit.asNumber() >= 0 && limit.asNumber() <= 65504)) return fail("maxOffset must be a number in 0..65504");
        result.maxOffset = limit.asFloat();
    }
    if (json.has("offsetBounds")) {
        const Json& bounds = json["offsetBounds"];
        if (result.offset < 0) return fail("offsetBounds requires an offset output");
        if (json.has("maxOffset")) return fail("give either maxOffset or offsetBounds, not both");
        auto vec = [&](const char* key, Vec3& out) {
            const Json& v = bounds[key];
            if (!v.isArray() || v.size() != 3) return false;
            for (size_t i = 0; i < 3; ++i) if (!v[i].isNumber() || !(std::fabs(v[i].asNumber()) <= 65504)) return false;
            out = Vec3(v[0].asFloat(), v[1].asFloat(), v[2].asFloat());
            return true;
        };
        if (!bounds.isObject() || bounds.size() != 2 || !vec("min", result.offsetMin) || !vec("max", result.offsetMax) ||
            result.offsetMin.x > result.offsetMax.x || result.offsetMin.y > result.offsetMax.y || result.offsetMin.z > result.offsetMax.z)
            return fail("offsetBounds must be {min: [x,y,z], max: [x,y,z]} with min <= max per axis, each within 65504");
        result.hasOffsetBounds = true;
    }
    out = std::move(result);
    if (error) error->clear();
    return true;
}

bool ShaderUniforms(const ShaderGraph& graph, const Json& values,
                    std::array<Vec4, ShaderGraph::kMaxUniforms>& out, std::string* error) {
    out = graph.defaults;
    return OverrideShaderUniforms(graph, values, out, error);
}

bool OverrideShaderUniforms(const ShaderGraph& graph, const Json& values,
                            std::array<Vec4, ShaderGraph::kMaxUniforms>& inout, std::string* error) {
    auto fail = [&](const std::string& message) { if (error) *error = message; return false; };
    if (!values.isNull() && !values.isObject()) return fail("shader uniform overrides must be an object");
    auto result = inout;
    for (const auto& item : values.members()) {
        auto it = std::find(graph.uniformNames.begin(), graph.uniformNames.end(), item.first);
        if (it == graph.uniformNames.end()) return fail("unknown shader uniform '" + item.first + "'");
        if (!Vector(item.second, result[static_cast<size_t>(it - graph.uniformNames.begin())]))
            return fail("shader uniform '" + item.first + "' must be a finite scalar or four-vector");
    }
    inout = result;
    if (error) error->clear();
    return true;
}

namespace {
// Runs the first `count` instructions; later registers stay zero.
std::array<Vec4, ShaderGraph::kMaxNodes> RunGraph(const ShaderGraph& graph, const ShaderInputs& inputs,
                                                  const std::array<Vec4, ShaderGraph::kMaxUniforms>& uniforms, size_t count) {
    std::array<Vec4, ShaderGraph::kMaxNodes> registers{};
    for (size_t index = 0; index < count; ++index) {
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
            case ShaderOp::ParticleAge: result = Vec4(inputs.particle.x,inputs.particle.x,inputs.particle.x,inputs.particle.x); break;
            case ShaderOp::ParticleRandom: result = Vec4(inputs.particle.y,inputs.particle.y,inputs.particle.y,inputs.particle.y); break;
            case ShaderOp::ParticleSpeed: result = Vec4(inputs.particle.z,inputs.particle.z,inputs.particle.z,inputs.particle.z); break;
            case ShaderOp::ParticleCustom0: result = Vec4(inputs.custom.x,inputs.custom.x,inputs.custom.x,inputs.custom.x); break;
            case ShaderOp::ParticleCustom1: result = Vec4(inputs.custom.y,inputs.custom.y,inputs.custom.y,inputs.custom.y); break;
            case ShaderOp::ParticleCustom2: result = Vec4(inputs.custom.z,inputs.custom.z,inputs.custom.z,inputs.custom.z); break;
            case ShaderOp::ParticleCustom3: result = Vec4(inputs.custom.w,inputs.custom.w,inputs.custom.w,inputs.custom.w); break;
            case ShaderOp::Time: result = Vec4(inputs.time,inputs.time,inputs.time,inputs.time); break;
            case ShaderOp::BaseColor: result = inputs.baseColor; break;
            case ShaderOp::Texture: result = inputs.texture ? inputs.texture(a.x,a.y) : Vec4(1,1,1,1); break;
            case ShaderOp::SceneColor: result = inputs.scene ? inputs.scene(a.x,a.y) : Vec4(0,0,0,1); break;
            case ShaderOp::SceneBlur: result = inputs.sceneBlur ? inputs.sceneBlur(a.x,a.y,b.x) : Vec4(0,0,0,1); break;
            case ShaderOp::Texel: result = inputs.texel; break;
            case ShaderOp::Ambient: result = inputs.ambient; break;
            case ShaderOp::LightVector: result = inputs.lightVector; break;
            case ShaderOp::LightColor: result = inputs.lightColor; break;
            case ShaderOp::LightAttenuation: result = Vec4(inputs.lightAttenuation, inputs.lightAttenuation, inputs.lightAttenuation, inputs.lightAttenuation); break;
            case ShaderOp::ViewDirection: result = inputs.viewDirection; break;
            case ShaderOp::ObjectPosition: result = inputs.objectPosition; break;
            case ShaderOp::Noise: { const float n = ShaderNoise(a.x,a.y,a.z); result = Vec4(n,n,n,n); break; }
            case ShaderOp::Fbm: { const float n = ShaderFbm(a.x,a.y,a.z); result = Vec4(n,n,n,n); break; }
            case ShaderOp::Length: { const float n = std::sqrt(a.x*a.x + a.y*a.y + a.z*a.z + a.w*a.w); result = Vec4(n,n,n,n); break; }
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
                    // pow of a non-positive base is 0 (also for exponent 0), so both renderers agree.
                    case ShaderOp::Pow: channels[k] = av[k] <= 0 ? 0.0f : std::pow(av[k], bv[k]); break;
                    // smoothstep(edge0 a, edge1 b, x c); equal edges act as step(edge, x).
                    case ShaderOp::Smoothstep: {
                        if (bv[k] == av[k]) { channels[k] = cv[k] < av[k] ? 0.0f : 1.0f; break; }
                        const float t = std::min(std::max((cv[k]-av[k])/(bv[k]-av[k]), 0.0f), 1.0f);
                        channels[k] = t*t*(3-2*t);
                        break;
                    }
                    default: break;
                }
                result = Vec4(channels[0],channels[1],channels[2],channels[3]);
                break;
            }
        }
        registers[index] = Vec4(Finite(result.x),Finite(result.y),Finite(result.z),Finite(result.w));
    }
    return registers;
}
}  // namespace

ShaderSurface EvaluateShaderGraph(const ShaderGraph& graph, const ShaderInputs& inputs,
                                 const std::array<Vec4, ShaderGraph::kMaxUniforms>& uniforms) {
    const auto registers = RunGraph(graph, inputs, uniforms, graph.instructions.size());
    return {registers[static_cast<size_t>(graph.color)],
            graph.emissive < 0 ? Vec3() : registers[static_cast<size_t>(graph.emissive)].xyz(),
            graph.normal < 0 ? Vec3() : registers[static_cast<size_t>(graph.normal)].xyz()};
}

Vec3 EvaluateShaderLight(const ShaderGraph& graph, const ShaderInputs& inputs,
                         const std::array<Vec4, ShaderGraph::kMaxUniforms>& uniforms) {
    if (graph.light < 0) return Vec3();
    const Vec4 value = RunGraph(graph, inputs, uniforms, static_cast<size_t>(graph.light) + 1)[static_cast<size_t>(graph.light)];
    return Vec3(std::max(value.x, 0.0f), std::max(value.y, 0.0f), std::max(value.z, 0.0f));
}

Vec4 ShaderViewDirection(const Vec4& viewer, const Vec3& position) {
    if (viewer.w > 0.5f) return Vec4(viewer.x, viewer.y, viewer.z, 0);
    const Vec3 d = viewer.xyz() - position;
    const float length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    return length > 1e-8f ? Vec4(d.x / length, d.y / length, d.z / length, 0) : Vec4();
}

Vec3 ShaderVertexOffset(const ShaderGraph& graph, const ShaderInputs& inputs,
                        const std::array<Vec4, ShaderGraph::kMaxUniforms>& uniforms) {
    if (graph.offset < 0) return Vec3();
    return RunGraph(graph, inputs, uniforms, static_cast<size_t>(graph.offset) + 1)[static_cast<size_t>(graph.offset)].xyz();
}

}  // namespace oe
