#pragma once
#include <array>
#include <functional>
#include <string>
#include <vector>

#include "core/Json.h"
#include "core/Math.h"

namespace oe {

// Stable instruction numbers are shared with the generated GPU shader evaluator.
enum class ShaderOp { Constant, Uniform, UV, Position, Normal, Time, BaseColor, Texture,
    Add, Subtract, Multiply, Divide, Min, Max, Sin, Cos, Floor, Fract, Abs, Clamp, Mix, Step, Dot, Normalize, Swizzle };

struct ShaderInstruction {
    ShaderOp op = ShaderOp::Constant;
    std::array<int, 3> args{};  // previously computed register indices
    Vec4 value;                // literal, uniform slot or swizzle channels
};

// A bounded, acyclic surface program; no filesystem, loops or platform source code.
struct ShaderGraph {
    static constexpr int kMaxNodes = 32, kMaxUniforms = 8;
    std::vector<ShaderInstruction> instructions;
    std::vector<std::string> uniformNames;
    std::array<Vec4, kMaxUniforms> defaults{};
    int color = 0, emissive = -1;  // color RGBA; optional emissive RGB
};

struct ShaderInputs {
    Vec4 uv{}, position{}, normal{}, baseColor{1, 1, 1, 1};
    float time = 0;  // fixed simulation time, never wall-clock time
    std::function<Vec4(float, float)> texture;  // base texture sample at graph-provided UV
};

struct ShaderSurface { Vec4 color; Vec3 emissive; };

// Compile JSON to ordered instructions. Failure leaves out unchanged and names the bad field.
bool CompileShaderGraph(const Json& json, ShaderGraph& out, std::string* error);
// Apply finite per-material uniform overrides; unspecified slots keep graph defaults.
bool ShaderUniforms(const ShaderGraph& graph, const Json& values,
                    std::array<Vec4, ShaderGraph::kMaxUniforms>& out, std::string* error);
// Reference evaluation: division by zero produces zero; values saturate to finite half-float range.
ShaderSurface EvaluateShaderGraph(const ShaderGraph& graph, const ShaderInputs& inputs,
                                 const std::array<Vec4, ShaderGraph::kMaxUniforms>& uniforms);

}  // namespace oe
