#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "core/Json.h"
#include "core/Math.h"

namespace oe {
enum class VfxValueType { Scalar, Vector };
enum class VfxExprOp { Constant, Input, Parameter, Component, Vector, Negate, Add, Subtract, Multiply, Divide,
    Less, LessEqual, Greater, GreaterEqual, Equal, NotEqual, Min, Max, Abs, Clamp, Lerp, Select,
    Sin, Cos, Pow, Exp, Sqrt, Floor, Fract, Dot, Length, Curve };
struct VfxExprInstruction {
    VfxExprOp op = VfxExprOp::Constant;
    VfxValueType type = VfxValueType::Scalar;
    Vec3 value;
    std::vector<int> args;
    std::string name;
};
struct VfxExpressionProgram { std::vector<VfxExprInstruction> code; VfxValueType type = VfxValueType::Scalar; };
struct VfxExpressionError {
    std::string code = "invalid_expression", message, hint;
};
// Bounded context, populated from captured birth inputs and fixed-step clocks. Vector inputs use emitter coordinates.
struct VfxExpressionInputs {
    float age = 0, lifetime = 1, emitterTime = 0, systemTime = 0, index = 0, count = 0, random = 0, speed = 0;
    float birthSize = 0, birthOpacity = 1;
    Vec3 birthPosition, birthVelocity, birthRotation, birthScale{1,1,1}, birthColor{1,1,1};
    const Json* params = nullptr;
};
enum class VfxChannel { Position, Velocity, Rotation, Scale, Color, Size, Opacity, Custom0, Custom1, Custom2, Custom3, Count };
constexpr size_t kVfxChannelCount = static_cast<size_t>(VfxChannel::Count);
struct VfxExpressionSet {
    static constexpr int kChannelInstructions = 64, kEmitterInstructions = 256, kEffectInstructions = 512;
    static constexpr int kStepInstructions = 1000000, kSourceBytes = 1024, kDepth = 32;
    Json source, parameterTypes;
    std::array<VfxExpressionProgram, kVfxChannelCount> birth, update;
    int birthCost = 0, updateCost = 0;
};
// Source is the only serialized representation. Failure preserves output and includes a line/column and repair hint.
bool CompileVfxExpression(const std::string& source, VfxValueType expected, const Json& params,
                          VfxExpressionProgram& out, VfxExpressionError* error);
bool CompileVfxExpressions(const Json& source, const Json& params, VfxExpressionSet& out, VfxExpressionError* error);
// Instruction results saturate to +/-65504; NaN and division by zero become zero. No libm transcendentals.
Vec3 EvaluateVfxExpression(const VfxExpressionProgram& program, const VfxExpressionInputs& inputs);
// Public authoring language, limits and reusable source snippets.
Json VfxExpressionSchema();
const std::array<const char*, kVfxChannelCount>& VfxChannelNames();
VfxValueType VfxChannelType(size_t channel);
Json VfxParameterTypes(const Json& params);
struct ParticleEmitter;
// Immutable programs are shared by snapshots; current parameters/clocks remain value-owned runtime inputs.
bool PrepareVfxExpressions(const ParticleEmitter& emitter, VfxExpressionError* error = nullptr);
// Conservative per-step reservation, including initial update evaluation and death births.
uint64_t VfxExpressionWork(const ParticleEmitter& emitter);
}  // namespace oe
