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
    static constexpr int kMaxNodes = 48, kMaxUniforms = 8;
    std::vector<ShaderInstruction> instructions;
    std::vector<std::string> uniformNames;
    std::array<Vec4, kMaxUniforms> defaults{};
    int color = 0, emissive = -1;  // color RGBA; optional emissive RGB
    // Optional outputs: `normal` replaces the world-space lighting normal per fragment;
    // `offset` moves each vertex in world space (evaluated per vertex, see ShaderVertexOffset).
    int normal = -1, offset = -1;
    // Slots of the reserved uniforms `cameraPosition` (xyz, w = 1) and `lightDirection`
    // (unit vector toward the first directional light, w = 0); -1 = not declared. The
    // renderers fill them per frame (BuildDrawList), replacing defaults and overrides.
    int cameraUniform = -1, lightUniform = -1;
};

struct ShaderInputs {
    Vec4 uv{}, position{}, normal{}, baseColor{1, 1, 1, 1};
    float time = 0;  // fixed simulation time, never wall-clock time
    std::function<Vec4(float, float)> texture;  // base texture sample at graph-provided UV
};

struct ShaderSurface { Vec4 color; Vec3 emissive; Vec3 normal; };

// Compile JSON to ordered instructions. Failure leaves out unchanged and names the bad field.
bool CompileShaderGraph(const Json& json, ShaderGraph& out, std::string* error);
// Apply finite per-material uniform overrides; unspecified slots keep graph defaults.
bool ShaderUniforms(const ShaderGraph& graph, const Json& values,
                    std::array<Vec4, ShaderGraph::kMaxUniforms>& out, std::string* error);
// Same validation, but unspecified slots keep the values already in `inout` (a material's
// uniforms overridden per entity by MeshRenderer.shaderUniforms). Failure leaves inout unchanged.
bool OverrideShaderUniforms(const ShaderGraph& graph, const Json& values,
                            std::array<Vec4, ShaderGraph::kMaxUniforms>& inout, std::string* error);
// Reference evaluation: division by zero produces zero; values saturate to finite half-float range.
ShaderSurface EvaluateShaderGraph(const ShaderGraph& graph, const ShaderInputs& inputs,
                                 const std::array<Vec4, ShaderGraph::kMaxUniforms>& uniforms);

// Vertex stage of a graph with an `offset` output: world-space displacement of one vertex.
// Inputs are the undisplaced world position, the vertex normal, the transformed UV and the
// material's base color; texture nodes cannot feed `offset` (CompileShaderGraph rejects it).
Vec3 ShaderVertexOffset(const ShaderGraph& graph, const ShaderInputs& inputs,
                        const std::array<Vec4, ShaderGraph::kMaxUniforms>& uniforms);

}  // namespace oe
