#include "scene/VfxExpression.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

#include "scene/Components.h"

namespace oe {
namespace {
// Flush subnormals explicitly: some native targets flush them while WASM preserves them.
float Finite(float v) { return v != v || std::abs(v) < std::numeric_limits<float>::min() ? 0 : Clamp(v, -65504, 65504); }
Vec3 Finite(Vec3 v) { return Vec3(Finite(v.x), Finite(v.y), Finite(v.z)); }
float Floor(float v) {
    const int i = static_cast<int>(Finite(v));
    return static_cast<float>(i - (v < static_cast<float>(i) ? 1 : 0));
}
float Sqrt(float v) {
    if (v <= 0) return 0;
    uint32_t bits; std::memcpy(&bits, &v, sizeof(bits));
    bits = (bits >> 1) + 0x1fc00000u;
    float x; std::memcpy(&x, &bits, sizeof(x));
    for (int i = 0; i < 6; ++i) x = (x + v / x) * 0.5f;
    return x;
}
// Fixed range reduction and polynomial evaluation; contraction is disabled by the engine build.
float Sin(float v) {
    const float x = v - Floor(v / 6.283185307179586f + 0.5f) * 6.283185307179586f;
    const float y = x * x;
    return x * (1 + y * (-0.1666666666667f + y * (0.0083333333333f + y *
        (-0.0001984126984f + y * (0.0000027557319f + y * -0.0000000250521f)))));
}
float Exp(float v) {
    if (v < -20) return 0;
    if (v > 11.1f) return 65504;
    const int n = static_cast<int>(Floor(v / 0.6931471805599453f + 0.5f));
    const float x = v - static_cast<float>(n) * 0.6931471805599453f;
    float result = 1 + x * (1 + x * (0.5f + x * (0.1666666666667f + x *
        (0.0416666666667f + x * (0.0083333333333f + x * (0.0013888888889f + x * 0.0001984126984f))))));
    for (int i = 0; i < std::abs(n); ++i) result *= n < 0 ? 0.5f : 2.0f;
    return Finite(result);
}
float Log(float v) {
    int exponent = 0;
    while (v > 1.5f) { v *= 0.5f; ++exponent; }
    while (v < 0.75f) { v *= 2; --exponent; }
    const float z = (v - 1) / (v + 1), z2 = z * z;
    float power = z, sum = 0;
    for (int i = 1; i <= 17; i += 2) { sum += power / static_cast<float>(i); power *= z2; }
    return 2 * sum + static_cast<float>(exponent) * 0.6931471805599453f;
}
float Pow(float x, float y) {
    if (y == 0) return 1;
    if (x == 0) return 0;
    if (x < 0 && Floor(y) != y) return 0;
    const float result = Exp(Finite(y * Log(std::abs(x))));
    return x < 0 && static_cast<int>(std::abs(y)) % 2 ? -result : result;
}
bool Identifier(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool Digit(char c) { return c >= '0' && c <= '9'; }
VfxValueType ValueType(const Json& value) {
    return value.isArray() && value.size() == 3 ? VfxValueType::Vector : VfxValueType::Scalar;
}
Vec3 JsonValue(const Json& value) {
    return value.isArray() && value.size() == 3 ? Vec3(value[0].asFloat(), value[1].asFloat(), value[2].asFloat()) :
        Vec3(value.asFloat(), value.asFloat(), value.asFloat());
}
bool Numeric(const Json& value) {
    if (value.isNumber()) return std::isfinite(value.asFloat()) && std::abs(value.asFloat()) <= 65504;
    if (!value.isArray() || value.size() != 3) return false;
    for (const Json& coordinate : value.items())
        if (!coordinate.isNumber() || !std::isfinite(coordinate.asFloat()) || std::abs(coordinate.asFloat()) > 65504) return false;
    return true;
}
class Parser {
public:
    Parser(const std::string& source, const Json& params) : source_(source), params_(params) {}
    VfxExpressionProgram Run(VfxValueType expected) {
        if (source_.empty() || source_.size() > VfxExpressionSet::kSourceBytes) Fail("source needs 1..1024 bytes");
        const int result = Expression(0);
        Space(); if (position_ != source_.size()) Fail("unexpected token");
        if (program_.code[static_cast<size_t>(result)].type != expected) Fail("channel expects " + TypeName(expected));
        program_.type = expected; return program_;
    }
    VfxExpressionError error;
private:
    const std::string& source_; const Json& params_;
    size_t position_ = 0; int depth_ = 0;
    VfxExpressionProgram program_;
    static std::string TypeName(VfxValueType type) { return type == VfxValueType::Scalar ? "scalar" : "vec3"; }
    void Space() { while (position_ < source_.size() && (source_[position_] == ' ' || source_[position_] == '\t' ||
        source_[position_] == '\n' || source_[position_] == '\r')) ++position_; }
    bool Take(const std::string& token) {
        Space(); if (source_.compare(position_, token.size(), token) != 0) return false;
        position_ += token.size(); return true;
    }
    [[noreturn]] void Fail(const std::string& message, bool budget = false) {
        int line = 1, column = 1;
        for (size_t i = 0; i < position_; ++i) if (source_[i] == '\n') { ++line; column = 1; } else ++column;
        error.code = budget ? "expression_budget" : "invalid_expression";
        error.message = "line " + std::to_string(line) + ", column " + std::to_string(column) + ": " + message;
        error.hint = budget ? "Reduce source/depth/instructions; see vfx.schema.expressions.limits." :
            "Use scalar/vec3 inputs and functions from vfx.schema.expressions. Vectors use vec3(x,y,z).";
        throw std::runtime_error(message);
    }
    int Emit(VfxExprOp op, VfxValueType type, std::vector<int> args = {}, Vec3 value = {}, std::string name = {}) {
        if (program_.code.size() >= VfxExpressionSet::kChannelInstructions) Fail("channel exceeds 64 instructions", true);
        VfxExprInstruction instruction; instruction.op = op; instruction.type = type;
        instruction.args = std::move(args); instruction.value = value; instruction.name = std::move(name);
        program_.code.push_back(std::move(instruction)); return static_cast<int>(program_.code.size()) - 1;
    }
    VfxValueType Type(int index) const { return program_.code[static_cast<size_t>(index)].type; }
    int Expression(int minimum) {
        if (++depth_ > VfxExpressionSet::kDepth) Fail("nesting exceeds 32", true);
        int left = Primary();
        for (;;) {
            Space(); int precedence = 0; size_t length = 0; VfxExprOp op = VfxExprOp::Add;
            for (const auto& candidate : Operators()) if (source_.compare(position_, candidate.text.size(), candidate.text) == 0) {
                precedence = candidate.precedence; length = candidate.text.size(); op = candidate.op; break;
            }
            if (length == 0 || precedence < minimum) break;
            position_ += length; const int right = Expression(precedence + 1);
            const bool comparison = precedence == 1;
            if (comparison && (Type(left) != VfxValueType::Scalar || Type(right) != VfxValueType::Scalar)) Fail("comparisons require scalars");
            left = Emit(op, comparison ? VfxValueType::Scalar : Combined(left, right), {left, right});
        }
        --depth_; return left;
    }
    struct Operator { std::string text; int precedence; VfxExprOp op; };
    static const std::vector<Operator>& Operators() {
        static const std::vector<Operator> ops = {{"<=",1,VfxExprOp::LessEqual},{">=",1,VfxExprOp::GreaterEqual},
            {"==",1,VfxExprOp::Equal},{"!=",1,VfxExprOp::NotEqual},{"<",1,VfxExprOp::Less},{">",1,VfxExprOp::Greater},
            {"+",2,VfxExprOp::Add},{"-",2,VfxExprOp::Subtract},{"*",3,VfxExprOp::Multiply},{"/",3,VfxExprOp::Divide}};
        return ops;
    }
    VfxValueType Combined(int a, int b) const {
        return Type(a) == VfxValueType::Vector || Type(b) == VfxValueType::Vector ? VfxValueType::Vector : VfxValueType::Scalar;
    }
    int Primary() {
        if (Take("-")) { const int value = Expression(4); return Emit(VfxExprOp::Negate, Type(value), {value}); }
        if (Take("+")) return Expression(4);
        if (Take("(")) { const int value = Expression(0); if (!Take(")")) Fail("expected ')'"); return value; }
        Space();
        if (position_ < source_.size() && (Digit(source_[position_]) || source_[position_] == '.')) {
            const size_t numberBegin = position_;
            float value = 0, place = 0.1f; bool decimal = false, digit = false;
            while (position_ < source_.size()) {
                const char c = source_[position_];
                if (c == '.' && !decimal) { decimal = true; ++position_; continue; }
                if (!Digit(c)) break;
                digit = true;
                if (decimal) { value += static_cast<float>(c - '0') * place; place *= 0.1f; }
                else value = value * 10 + static_cast<float>(c - '0');
                ++position_;
                if (!std::isfinite(value) || value > 65504) Fail("literal must be within +/-65504");
            }
            if (!digit) Fail("expected number");
            return Emit(VfxExprOp::Constant, VfxValueType::Scalar, {}, Vec3(value,value,value), source_.substr(numberBegin,position_-numberBegin));
        }
        const size_t begin = position_;
        while (position_ < source_.size() && (Identifier(source_[position_]) || Digit(source_[position_]) || source_[position_] == '.')) ++position_;
        const std::string name = source_.substr(begin, position_ - begin);
        if (name.empty()) Fail("expected expression");
        if (Take("(")) {
            std::vector<int> args;
            if (!Take(")")) for (;;) {
                args.push_back(Expression(0));
                if (Take(")")) break;
                if (!Take(",")) Fail("expected ',' or ')'");
            }
            return Function(name, args);
        }
        std::string input = name, component;
        const size_t dot = name.rfind('.');
        if (dot != std::string::npos && (name.compare(0,6,"param.") != 0 || dot > 5) && dot + 2 == name.size() && std::string("xyz").find(name.back()) != std::string::npos) {
            component = name.substr(dot + 1); input = name.substr(0, dot);
        }
        int value = -1;
        if (input.compare(0,6,"param.") == 0) {
            const std::string parameter = input.substr(6);
            if (!params_.has(parameter) || !Numeric(params_[parameter])) Fail("unknown or nonnumeric parameter '" + parameter + "'");
            value = Emit(VfxExprOp::Parameter, ValueType(params_[parameter]), {}, {}, parameter);
        } else {
            static const std::vector<std::string> scalars = {"age","lifetime","emitterTime","systemTime","index","count","random","speed","birthSize","birthOpacity"};
            static const std::vector<std::string> vectors = {"birthPosition","birthVelocity","birthRotation","birthScale","birthColor"};
            if (input == "pi") value = Emit(VfxExprOp::Constant,VfxValueType::Scalar,{},Vec3(kPi,kPi,kPi),"pi");
            else if (std::find(scalars.begin(),scalars.end(),input) != scalars.end()) value = Emit(VfxExprOp::Input,VfxValueType::Scalar,{}, {},input);
            else if (std::find(vectors.begin(),vectors.end(),input) != vectors.end()) value = Emit(VfxExprOp::Input,VfxValueType::Vector,{}, {},input);
            else Fail("unknown input '" + input + "'");
        }
        if (!component.empty()) {
            if (Type(value) != VfxValueType::Vector) Fail("component read requires vec3");
            value = Emit(VfxExprOp::Component,VfxValueType::Scalar,{value},Vec3(static_cast<float>(std::string("xyz").find(component)),0,0));
        }
        return value;
    }
    int Function(const std::string& name, const std::vector<int>& args) {
        struct FunctionInfo { const char* name; VfxExprOp op; size_t arity; };
        static const FunctionInfo functions[] = {{"vec3",VfxExprOp::Vector,3},{"min",VfxExprOp::Min,2},{"max",VfxExprOp::Max,2},
            {"abs",VfxExprOp::Abs,1},{"clamp",VfxExprOp::Clamp,3},{"lerp",VfxExprOp::Lerp,3},{"select",VfxExprOp::Select,3},
            {"sin",VfxExprOp::Sin,1},{"cos",VfxExprOp::Cos,1},{"pow",VfxExprOp::Pow,2},{"exp",VfxExprOp::Exp,1},
            {"sqrt",VfxExprOp::Sqrt,1},{"floor",VfxExprOp::Floor,1},{"fract",VfxExprOp::Fract,1},{"dot",VfxExprOp::Dot,2},
            {"length",VfxExprOp::Length,1},{"curve",VfxExprOp::Curve,0}};
        const FunctionInfo* function = nullptr;
        for (const auto& candidate : functions) if (name == candidate.name) function = &candidate;
        if (!function) Fail("unknown function '" + name + "'");
        if (name == "curve") {
            if (args.size() < 3 || args.size() > 33 || args.size() % 2 == 0 || Type(args[0]) != VfxValueType::Scalar)
                Fail("curve(t,t0,value0,...) needs 1..16 keys and scalar t");
            float previous = -1;
            for (size_t i = 1; i < args.size(); i += 2) {
                const auto& key = program_.code[static_cast<size_t>(args[i])];
                if (key.op != VfxExprOp::Constant || key.value.x < 0 || key.value.x > 1 || key.value.x < previous) Fail("curve ages must be ascending constant numbers in 0..1");
                previous = key.value.x;
                if (Type(args[i+1]) != Type(args[2])) Fail("curve key values must share a type");
            }
            return Emit(function->op, Type(args[2]), args);
        }
        if (args.size() != function->arity) Fail(name + " requires " + std::to_string(function->arity) + " arguments");
        if (name == "vec3") {
            for (int value : args) if (Type(value) != VfxValueType::Scalar) Fail("vec3 requires scalar coordinates");
            return Emit(function->op,VfxValueType::Vector,args);
        }
        if (name == "select" && Type(args[0]) != VfxValueType::Scalar) Fail("select condition must be scalar");
        if (name == "dot" || name == "length") {
            for (int value : args) if (Type(value) != VfxValueType::Vector) Fail(name + " requires vec3");
            return Emit(function->op,VfxValueType::Scalar,args);
        }
        VfxValueType type = VfxValueType::Scalar;
        for (size_t i = name == "select" ? 1u : 0u; i < args.size(); ++i) if (Type(args[i]) == VfxValueType::Vector) type = VfxValueType::Vector;
        return Emit(function->op,type,args);
    }
};
Vec3 Input(const std::string& name, const VfxExpressionInputs& input) {
    if (name == "birthPosition") return input.birthPosition;
    if (name == "birthVelocity") return input.birthVelocity;
    if (name == "birthRotation") return input.birthRotation;
    if (name == "birthScale") return input.birthScale;
    if (name == "birthColor") return input.birthColor;
    float value = 0;
    if (name == "age") value = input.age;
    else if (name == "lifetime") value = input.lifetime;
    else if (name == "emitterTime") value = input.emitterTime;
    else if (name == "systemTime") value = input.systemTime;
    else if (name == "index") value = input.index;
    else if (name == "count") value = input.count;
    else if (name == "random") value = input.random;
    else if (name == "speed") value = input.speed;
    else if (name == "birthSize") value = input.birthSize;
    else if (name == "birthOpacity") value = input.birthOpacity;
    return Vec3(value,value,value);
}
}  // namespace

const std::array<const char*, kVfxChannelCount>& VfxChannelNames() {
    static const std::array<const char*,kVfxChannelCount> names = {
        "position","velocity","rotation","scale","color","size","opacity","custom0","custom1","custom2","custom3"};
    return names;
}
VfxValueType VfxChannelType(size_t channel) { return channel < 5 ? VfxValueType::Vector : VfxValueType::Scalar; }
Json VfxParameterTypes(const Json& params) {
    Json types = Json::MakeObject();
    for (const auto& entry : params.members()) types[entry.first] = Numeric(entry.second) ?
        (ValueType(entry.second) == VfxValueType::Scalar ? "scalar" : "vector") : "unsupported";
    return types;
}
bool CompileVfxExpression(const std::string& source, VfxValueType expected, const Json& params,
                          VfxExpressionProgram& out, VfxExpressionError* error) {
    Parser parser(source, params);
    try { out = parser.Run(expected); return true; }
    catch (const std::runtime_error&) { if (error) *error = parser.error; return false; }
}
bool CompileVfxExpressions(const Json& source, const Json& params, VfxExpressionSet& out, VfxExpressionError* error) {
    VfxExpressionSet compiled; compiled.source = source; compiled.parameterTypes = VfxParameterTypes(params);
    auto fail = [&](const std::string& message, bool budget = false) {
        if (error) { error->code = budget ? "expression_budget" : "invalid_expression"; error->message = message;
            error->hint = "Use expressions:{birth:{channel:'text'},update:{channel:'text'}}; consult vfx.schema.expressions."; }
        return false;
    };
    if (!source.isObject()) return fail("expressions must be an object");
    for (const auto& stage : source.members()) {
        if ((stage.first != "birth" && stage.first != "update") || !stage.second.isObject()) return fail("expressions stage must be birth or update with an object");
        auto& programs = stage.first == "birth" ? compiled.birth : compiled.update;
        for (const auto& channel : stage.second.members()) {
            const auto& names = VfxChannelNames(); size_t index = names.size();
            for (size_t i = 0; i < names.size(); ++i) if (channel.first == names[i]) index = i;
            if (index == names.size() || !channel.second.isString()) return fail(stage.first + "." + channel.first + ": unknown channel or non-text source");
            VfxExpressionError detail;
            if (!CompileVfxExpression(channel.second.asString(),VfxChannelType(index),params,programs[index],&detail)) {
                detail.message = stage.first + "." + channel.first + ": " + detail.message; if (error) *error = detail; return false;
            }
            (stage.first == "birth" ? compiled.birthCost : compiled.updateCost) += static_cast<int>(programs[index].code.size());
        }
    }
    if (compiled.birthCost + compiled.updateCost > VfxExpressionSet::kEmitterInstructions) return fail("emitter exceeds 256 expression instructions",true);
    out = std::move(compiled); return true;
}
Vec3 EvaluateVfxExpression(const VfxExpressionProgram& program, const VfxExpressionInputs& inputs) {
    std::array<Vec3,VfxExpressionSet::kChannelInstructions> values{};
    for (size_t i = 0; i < program.code.size(); ++i) {
        const auto& instruction = program.code[i];
        const auto& args = instruction.args;
        Vec3 result, a = args.empty() ? Vec3() : values[static_cast<size_t>(args[0])];
        const Vec3 b = args.size() < 2 ? Vec3() : values[static_cast<size_t>(args[1])];
        const Vec3 c = args.size() < 3 ? Vec3() : values[static_cast<size_t>(args[2])];
        if (instruction.op == VfxExprOp::Constant) result = instruction.value;
        else if (instruction.op == VfxExprOp::Input) result = Input(instruction.name,inputs);
        else if (instruction.op == VfxExprOp::Parameter) result = inputs.params ? JsonValue((*inputs.params)[instruction.name]) : Vec3();
        else if (instruction.op == VfxExprOp::Vector) result = Vec3(a.x,b.x,c.x);
        else if (instruction.op == VfxExprOp::Component) {
            const float components[] = {a.x,a.y,a.z}; const float v = components[static_cast<int>(instruction.value.x)];
            result = Vec3(v,v,v);
        } else if (instruction.op == VfxExprOp::Dot || instruction.op == VfxExprOp::Length) {
            const float v = instruction.op == VfxExprOp::Dot ? Dot(a,b) : Sqrt(Dot(a,a)); result = Vec3(v,v,v);
        } else if (instruction.op == VfxExprOp::Select) result = a.x != 0 ? b : c;
        else if (instruction.op == VfxExprOp::Curve) {
            result = values[static_cast<size_t>(args.back())];
            if (a.x <= b.x) result = c;
            else for (size_t key = 3; key < args.size(); key += 2) {
                const float t = values[static_cast<size_t>(args[key])].x, before = values[static_cast<size_t>(args[key-2])].x;
                if (a.x <= t) { const float ratio = t == before ? 1 : (a.x-before)/(t-before);
                    result = values[static_cast<size_t>(args[key-1])] * (1-ratio) + values[static_cast<size_t>(args[key+1])] * ratio; break; }
            }
        } else {
            const float av[] = {a.x,a.y,a.z}, bv[] = {b.x,b.y,b.z}, cv[] = {c.x,c.y,c.z};
            float v[3]{};
            for (int axis = 0; axis < 3; ++axis) switch (instruction.op) {
                case VfxExprOp::Negate: v[axis]=-av[axis]; break;
                case VfxExprOp::Add: v[axis]=av[axis]+bv[axis]; break;
                case VfxExprOp::Subtract: v[axis]=av[axis]-bv[axis]; break;
                case VfxExprOp::Multiply: v[axis]=av[axis]*bv[axis]; break;
                case VfxExprOp::Divide: v[axis]=bv[axis]==0?0:av[axis]/bv[axis]; break;
                case VfxExprOp::Less: v[axis]=av[axis]<bv[axis]?1.0f:0.0f; break;
                case VfxExprOp::LessEqual: v[axis]=av[axis]<=bv[axis]?1.0f:0.0f; break;
                case VfxExprOp::Greater: v[axis]=av[axis]>bv[axis]?1.0f:0.0f; break;
                case VfxExprOp::GreaterEqual: v[axis]=av[axis]>=bv[axis]?1.0f:0.0f; break;
                case VfxExprOp::Equal: v[axis]=av[axis]==bv[axis]?1.0f:0.0f; break;
                case VfxExprOp::NotEqual: v[axis]=av[axis]!=bv[axis]?1.0f:0.0f; break;
                case VfxExprOp::Min: v[axis]=std::min(av[axis],bv[axis]); break;
                case VfxExprOp::Max: v[axis]=std::max(av[axis],bv[axis]); break;
                case VfxExprOp::Abs: v[axis]=std::abs(av[axis]); break;
                case VfxExprOp::Clamp: v[axis]=std::min(std::max(av[axis],bv[axis]),cv[axis]); break;
                case VfxExprOp::Lerp: v[axis]=av[axis]*(1-cv[axis])+bv[axis]*cv[axis]; break;
                case VfxExprOp::Sin: v[axis]=Sin(av[axis]); break;
                case VfxExprOp::Cos: v[axis]=Sin(av[axis]+kPi*0.5f); break;
                case VfxExprOp::Pow: v[axis]=Pow(av[axis],bv[axis]); break;
                case VfxExprOp::Exp: v[axis]=Exp(av[axis]); break;
                case VfxExprOp::Sqrt: v[axis]=Sqrt(av[axis]); break;
                case VfxExprOp::Floor: v[axis]=Floor(av[axis]); break;
                case VfxExprOp::Fract: v[axis]=av[axis]-Floor(av[axis]); break;
                default: break;
            }
            result = Vec3(v[0],v[1],v[2]);
        }
        values[i] = Finite(result);
    }
    return program.code.empty() ? Vec3() : values[program.code.size()-1];
}
bool PrepareVfxExpressions(const ParticleEmitter& emitter, VfxExpressionError* error) {
    if (emitter.expressions.isObject() && emitter.expressions.size() == 0) { emitter.expressionProgram.reset(); return true; }
    const Json types = VfxParameterTypes(emitter.expressionParams);
    if (!emitter.expressionProgram || emitter.expressionProgram->source != emitter.expressions || emitter.expressionProgram->parameterTypes != types) {
        auto program = std::make_shared<VfxExpressionSet>();
        if (!CompileVfxExpressions(emitter.expressions,emitter.expressionParams,*program,error)) return false;
        emitter.expressionProgram = std::move(program);
    }
    const int cost = 5 * (emitter.expressionProgram->updateCost + emitter.expressionProgram->birthCost);
    if (static_cast<int64_t>(std::max(0,emitter.maxParticles)) * cost > VfxExpressionSet::kStepInstructions) {
        if (error) { error->code="expression_budget";error->message="emitter exceeds 1000000 worst-case instructions per step";
            error->hint="Reduce maxParticles or expression instructions. Reservation includes five birth passes for death chains."; }
        return false;
    }
    return true;
}
uint64_t VfxExpressionWork(const ParticleEmitter& emitter) {
    if (!emitter.expressionProgram) return 0;
    return static_cast<uint64_t>(std::max(0,emitter.maxParticles))*5u*
        static_cast<uint64_t>(emitter.expressionProgram->birthCost+emitter.expressionProgram->updateCost);
}
Json VfxExpressionSchema() {
    Json out=Json::MakeObject();
    out["syntax"]="expressions:{birth:{position:'vec3(...)'},update:{position:'vec3(...)'}}; source text only";
    out["channels"]=Json::MakeArray(); for(const char* name:VfxChannelNames()) out["channels"].push(name);
    out["inputs"]="age (0..1), lifetime, emitterTime, systemTime, index, count, random, speed, birthPosition/Velocity/Rotation/Scale/Color, birthSize, birthOpacity, param.name, vector.x/y/z, pi";
    out["functions"]="vec3, min, max, abs, clamp, lerp, select, sin, cos, pow, exp, sqrt, floor, fract, dot, length, curve(t,t0,value0,...)";
    out["operators"]="+ - * / < <= > >= == !=; scalar/vector arithmetic broadcasts scalar; comparisons are scalar";
    out["finite"]="each instruction clamps +/-65504; NaN/division by zero/negative sqrt/negative fractional pow become zero";
    out["limits"]["sourceBytes"]=VfxExpressionSet::kSourceBytes;out["limits"]["depth"]=VfxExpressionSet::kDepth;
    out["limits"]["channelInstructions"]=VfxExpressionSet::kChannelInstructions;out["limits"]["emitterInstructions"]=VfxExpressionSet::kEmitterInstructions;
    out["limits"]["effectInstructions"]=VfxExpressionSet::kEffectInstructions;out["limits"]["stepInstructions"]=VfxExpressionSet::kStepInstructions;
    out["limits"]["requestInstructions"]=100000000;
    out["limits"]["reservation"]="sum(maxParticles*(5*(updateCost+birthCost))) <= stepInstructions; five birth passes conservatively cover death chains";
    out["snippets"]["orbit"]="vec3(cos(age*2*pi+index*2*pi/max(count,1)),0,sin(age*2*pi+index*2*pi/max(count,1)))";
    out["snippets"]["spiral"]="vec3(cos(age*4*pi+index*2*pi/max(count,1)),age*2,sin(age*4*pi+index*2*pi/max(count,1)))";
    out["snippets"]["pulse"]="1+sin(age*2*pi)*0.15";
    out["snippets"]["ease_out"]="1-pow(1-age,3)";
    out["snippets"]["rise_hold_fall"]="curve(age,0,0,0.2,1,0.8,1,1,0)";
    out["snippets"]["random_range"]="lerp(0.5,1.5,random)";
    return out;
}
}  // namespace oe
