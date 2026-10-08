#include "render/GraphSource.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>

namespace oe {

namespace {

enum class Stage { Vertex, Fragment };

struct Writer {
    GraphDialect dialect;
    Stage stage;
    std::string out;

    bool Hlsl() const { return dialect == GraphDialect::Hlsl; }
    // Declarations are highp in GLSL ES fragment shaders, whose default float precision is mediump.
    std::string Type() const { return Hlsl() ? "float4" : "highp vec4"; }
    std::string Vec4() const { return Hlsl() ? "float4" : "vec4"; }
    std::string Zero3() const { return Hlsl() ? "0.0f.xxx" : "vec3(0.0)"; }
    std::string Splat(const std::string& scalar) const { return Hlsl() ? "((" + scalar + ").xxxx)" : "vec4(" + scalar + ")"; }

    static std::string Reg(int index) { return "oeg" + std::to_string(index); }

    // A float literal both languages read as float (GLSL ES has no implicit int to float).
    static std::string Number(float value) {
        char buffer[48];
        std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
        std::string text = buffer;
        if (text.find_first_of(".eE") == std::string::npos) text += ".0";
        return "(" + text + ")";
    }

    std::string Expression(const ShaderInstruction& ins) const {
        const std::string a = Reg(ins.args[0]), b = Reg(ins.args[1]), c = Reg(ins.args[2]);
        const bool vertex = stage == Stage::Vertex;
        static const char kChannels[] = "xyzw";
        switch (ins.op) {
            case ShaderOp::Constant:
                return Vec4() + "(" + Number(ins.value.x) + ", " + Number(ins.value.y) + ", " + Number(ins.value.z) + ", " + Number(ins.value.w) + ")";
            case ShaderOp::Uniform:
                return std::string(vertex ? "VGraphUniform(" : "GraphUniform(") + std::to_string(static_cast<int>(ins.value.x)) + ")";
            case ShaderOp::UV: return Vec4() + "(uv, 0.0, 0.0)";
            case ShaderOp::Position: return vertex ? Vec4() + "(wpos, 1.0)" : "GraphPosition()";
            case ShaderOp::Normal: return Vec4() + (vertex ? "(nrm, 0.0)" : "(normal, 0.0)");
            case ShaderOp::Time: return Splat(vertex ? "VGraphTime()" : "GraphTime()");
            case ShaderOp::BaseColor: return vertex ? "VGraphBaseColor()" : Vec4() + "(base, alpha)";
            case ShaderOp::Texture: return vertex ? Vec4() + "(1.0, 1.0, 1.0, 1.0)" : "GraphTexture(" + a + ".xy)";
            case ShaderOp::Add: return "(" + a + " + " + b + ")";
            case ShaderOp::Subtract: return "(" + a + " - " + b + ")";
            case ShaderOp::Multiply: return "(" + a + " * " + b + ")";
            case ShaderOp::Divide: {
                std::string text = Vec4() + "(";
                for (int k = 0; k < 4; ++k) {
                    const std::string ch = std::string(".") + kChannels[k];
                    text += std::string(k ? ", " : "") + "(" + b + ch + " == 0.0 ? 0.0 : " + a + ch + " / " + b + ch + ")";
                }
                return text + ")";
            }
            case ShaderOp::Min: return "min(" + a + ", " + b + ")";
            case ShaderOp::Max: return "max(" + a + ", " + b + ")";
            case ShaderOp::Sin: return "sin(" + a + ")";
            case ShaderOp::Cos: return "cos(" + a + ")";
            case ShaderOp::Floor: return "floor(" + a + ")";
            case ShaderOp::Fract: return std::string(Hlsl() ? "frac(" : "fract(") + a + ")";
            case ShaderOp::Abs: return "abs(" + a + ")";
            case ShaderOp::Clamp: return "min(max(" + a + ", " + b + "), " + c + ")";
            case ShaderOp::Mix: return "(" + a + " * (1.0 - " + c + ") + " + b + " * " + c + ")";
            case ShaderOp::Step: return "step(" + a + ", " + b + ")";
            case ShaderOp::Dot: return Splat("dot(" + a + ", " + b + ")");
            case ShaderOp::Normalize:
                return "(length(" + a + ") > 1e-8 ? " + a + " / length(" + a + ") : " + Vec4() + "(0.0, 0.0, 0.0, 0.0))";
            case ShaderOp::Swizzle: {
                std::string text = a + ".";
                for (float channel : {ins.value.x, ins.value.y, ins.value.z, ins.value.w}) text += kChannels[static_cast<int>(channel) & 3];
                return text;
            }
            case ShaderOp::ParticleAge: return Splat(vertex ? "VGraphParticle().x" : "GraphParticle().x");
            case ShaderOp::ParticleRandom: return Splat(vertex ? "VGraphParticle().y" : "GraphParticle().y");
            case ShaderOp::ParticleSpeed: return Splat(vertex ? "VGraphParticle().z" : "GraphParticle().z");
            case ShaderOp::ParticleCustom0: return Splat(vertex ? "VGraphCustom().x" : "GraphCustom().x");
            case ShaderOp::ParticleCustom1: return Splat(vertex ? "VGraphCustom().y" : "GraphCustom().y");
            case ShaderOp::ParticleCustom2: return Splat(vertex ? "VGraphCustom().z" : "GraphCustom().z");
            case ShaderOp::ParticleCustom3: return Splat(vertex ? "VGraphCustom().w" : "GraphCustom().w");
        }
        return Vec4() + "(0.0, 0.0, 0.0, 0.0)";
    }

    static int ArgCount(ShaderOp op) {
        switch (op) {
            case ShaderOp::Texture: case ShaderOp::Sin: case ShaderOp::Cos: case ShaderOp::Floor: case ShaderOp::Fract:
            case ShaderOp::Abs: case ShaderOp::Normalize: case ShaderOp::Swizzle:
                return 1;
            case ShaderOp::Add: case ShaderOp::Subtract: case ShaderOp::Multiply: case ShaderOp::Divide: case ShaderOp::Min:
            case ShaderOp::Max: case ShaderOp::Step: case ShaderOp::Dot:
                return 2;
            case ShaderOp::Clamp: case ShaderOp::Mix:
                return 3;
            default:
                return 0;
        }
    }

    // Every node the outputs depend on, in order, each saturated like the interpreter's registers.
    void Nodes(const ShaderGraph& graph, std::initializer_list<int> outputs) {
        std::array<bool, ShaderGraph::kMaxNodes> needed{};
        int last = -1;
        for (int output : outputs) {
            if (output < 0 || output >= static_cast<int>(graph.instructions.size())) continue;
            needed[static_cast<size_t>(output)] = true;
            last = std::max(last, output);
        }
        for (int index = last; index >= 0; --index) {
            if (!needed[static_cast<size_t>(index)]) continue;
            const ShaderInstruction& ins = graph.instructions[static_cast<size_t>(index)];
            for (int k = 0; k < ArgCount(ins.op); ++k) needed[static_cast<size_t>(ins.args[static_cast<size_t>(k)])] = true;
        }
        for (int index = 0; index <= last; ++index) {
            if (!needed[static_cast<size_t>(index)]) continue;
            const std::string reg = Reg(index);
            out += "        " + Type() + " " + reg + " = " + Expression(graph.instructions[static_cast<size_t>(index)]) + ";\n";
            out += "        " + reg + " = GraphFinite(" + reg + ");\n";
        }
    }
};

}  // namespace

std::string GraphSurfaceBody(const ShaderGraph& graph, GraphDialect dialect) {
    Writer w{dialect, Stage::Fragment, {}};
    w.Nodes(graph, {graph.color, graph.emissive, graph.normal});
    const std::string color = Writer::Reg(graph.color);
    w.out += "        base = max(" + color + ".rgb, " + w.Zero3() + ");\n";
    w.out += "        alpha = clamp(" + color + ".a, 0.0, 1.0);\n";
    w.out += "        emission = " + (graph.emissive >= 0 ? Writer::Reg(graph.emissive) + ".rgb" : w.Zero3()) + ";\n";
    w.out += "        normal_out = " + (graph.normal >= 0 ? Writer::Reg(graph.normal) + ".xyz" : w.Zero3()) + ";\n";
    return w.out;
}

std::string GraphVertexBody(const ShaderGraph& graph, GraphDialect dialect) {
    Writer w{dialect, Stage::Vertex, {}};
    if (graph.offset < 0) return "        return " + w.Zero3() + ";\n";
    w.Nodes(graph, {graph.offset});
    w.out += "        return " + Writer::Reg(graph.offset) + ".xyz;\n";
    return w.out;
}

bool ReplaceFunctionBody(std::string& source, const std::string& name, const std::string& body) {
    for (size_t at = source.find(name + "("); at != std::string::npos; at = source.find(name + "(", at + 1)) {
        // A definition: preceded by its return type on the same line, its parameter list followed by `{`.
        if (at == 0 || !std::isspace(static_cast<unsigned char>(source[at - 1]))) continue;
        const size_t lineStart = source.rfind('\n', at);
        const std::string prefix = source.substr(lineStart == std::string::npos ? 0 : lineStart + 1, at - (lineStart == std::string::npos ? 0 : lineStart + 1));
        if (prefix.find_first_of("=+-*/(,;") != std::string::npos || prefix.find_first_not_of(" \t") == std::string::npos) continue;
        const size_t close = source.find(')', at);
        if (close == std::string::npos) return false;
        size_t open = close + 1;
        while (open < source.size() && std::isspace(static_cast<unsigned char>(source[open]))) ++open;
        if (open >= source.size() || source[open] != '{') continue;
        int depth = 0;
        for (size_t end = open; end < source.size(); ++end) {
            if (source[end] == '{') ++depth;
            else if (source[end] == '}' && --depth == 0) {
                source.replace(open + 1, end - open - 1, "\n" + body + "    ");
                return true;
            }
        }
        return false;
    }
    return false;
}

}  // namespace oe
