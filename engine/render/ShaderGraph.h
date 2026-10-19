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
    Add, Subtract, Multiply, Divide, Min, Max, Sin, Cos, Floor, Fract, Abs, Clamp, Mix, Step, Dot, Normalize, Swizzle, ParticleAge, ParticleRandom, ParticleSpeed, ParticleCustom0, ParticleCustom1, ParticleCustom2, ParticleCustom3,
    // Every stage: power, length, smoothstep and 3D value noise / four-octave fbm of xyz (ShaderNoise/ShaderFbm).
    Pow, Length, Smoothstep, Noise, Fbm,
    // Post stage only: the display image at a uv, its 5x5 gaussian blur, and (1/width, 1/height, width, height).
    SceneColor, SceneBlur, Texel,
    // Surface fragments: the scene's ambient light at the normal (rgb). Light stage (the `light` output) only:
    // the unit vector toward the light, its color (times intensity), and its attenuation (distance, spot cone, shadow).
    Ambient, LightVector, LightColor, LightAttenuation,
    // Surfaces, every stage: the unit vector from the surface toward the camera (an orthographic camera's axis), and
    // the position in the mesh's own space (xyz, w = 1; the rest pose of a skinned mesh, before `offset`).
    ViewDirection, ObjectPosition };

// What a graph runs on: a material's surface, or the camera's display image (PostProcess.shader).
enum class ShaderStage { Surface, Post };

struct ShaderInstruction {
    ShaderOp op = ShaderOp::Constant;
    std::array<int, 3> args{};  // previously computed register indices
    Vec4 value;                // literal, uniform slot or swizzle channels
};

// A bounded, acyclic surface program; no filesystem, loops or platform source code.
struct ShaderGraph {
    // kMaxNodes bounds every graph (register arrays). The GPU compiles each graph into its own program; until
    // that is ready, or when it fails, its interpreters run surface graphs of up to kMaxInterpretedSurfaceNodes
    // (mesh shaders) and post graphs of up to kMaxInterpretedPostNodes (composite_fs), and larger ones are skipped.
    static constexpr int kMaxNodes = 128, kMaxInterpretedSurfaceNodes = 48, kMaxInterpretedPostNodes = 64, kMaxUniforms = 8;
    ShaderStage stage = ShaderStage::Surface;
    // Post graphs: `"injection": "beforeToneMapping"` runs on the HDR scene color before bloom, exposure, tone mapping
    // and the sRGB encode, and its output (not clamped to 0..1) replaces it; false (the default
    // "afterToneMapping") runs last, on the display image.
    bool beforeToneMapping = false;
    std::vector<ShaderInstruction> instructions;
    std::vector<std::string> uniformNames;
    std::array<Vec4, kMaxUniforms> defaults{};
    int color = 0, emissive = -1;  // color RGBA; optional emissive RGB
    // Optional outputs: `normal` replaces the world-space lighting normal per fragment;
    // `offset` moves each vertex in world space (evaluated per vertex, see ShaderVertexOffset).
    int normal = -1, offset = -1;
    // Optional `light` output (surface graphs of lit materials): evaluated once per light the engine shades with
    // (directional, point, spot), with that light's lightVector/lightColor/lightAttenuation; the surface shows
    // `color` (its ambient part) plus the sum of `light` over the lights, in place of the PBR model.
    int light = -1;
    // Optional `maxOffset`: the largest distance (world units) `offset` moves a vertex, declared
    // by the author like a cull margin. Renderers grow shadow bounds by it instead of running the
    // vertex stage on the CPU; -1 = not declared (kDefaultOffsetMargin in RenderScene.h is used).
    float maxOffset = -1;
    // Optional `offsetBounds` {min: [x,y,z], max: [x,y,z]}: the range of `offset` per world axis, for
    // displacements that only go one way (a world bent down away from the camera moves vertices
    // down by up to hundreds of metres, never up): culling and shadow bounds grow by exactly that,
    // where `maxOffset` would grow them in every direction.
    bool hasOffsetBounds = false;
    Vec3 offsetMin, offsetMax;
    // Slots of the reserved uniforms `cameraPosition` (xyz, w = 1), `lightDirection`
    // (unit vector toward the first directional light, w = 0) and `playing` (1 while the game is
    // played, 0 while its scene is edited); -1 = not declared. The renderers fill them per frame
    // (BuildDrawList), replacing defaults and overrides.
    int cameraUniform = -1, lightUniform = -1, playingUniform = -1;
    // `screenSize` (width, height, 1/width, 1/height of the render target in pixels) and `pixelScale`:
    // the world size of one pixel at view distance d is d * pixelScale.x + pixelScale.y (perspective: x =
    // 2 tan(fov/2) / height, y = 0; orthographic: x = 0, y = 2 * orthoSize / height). -1 = not declared.
    int screenUniform = -1, pixelScaleUniform = -1;
    // Reads objectPosition: the GPU renderer draws it unbatched (batched geometry is in world space).
    bool objectSpace = false;
};

struct ShaderInputs {
    Vec4 uv{}, position{}, normal{}, baseColor{1, 1, 1, 1};
    Vec4 custom{}; // particleCustom0..3, transported as attributes without uniform slots
    Vec4 particle{};  // normalized age, stable random, world speed, reserved
    float time = 0;  // fixed simulation time, never wall-clock time
    std::function<Vec4(float, float)> texture;  // base texture sample at graph-provided UV
    // Post stage: the injection point's image at a uv (bilinear, clamped), its 5x5 gaussian blur with samples
    // `spacing` pixels apart, and (1/width, 1/height, width, height) of that image.
    std::function<Vec4(float, float)> scene;
    std::function<Vec4(float, float, float)> sceneBlur;
    Vec4 texel{};
    Vec4 ambient{};  // ambient light at the normal (rgb), see ShaderOp::Ambient
    // Light stage: toward the light (xyz), its color (rgb), its attenuation.
    Vec4 lightVector{}, lightColor{};
    float lightAttenuation = 0;
    Vec4 viewDirection{}, objectPosition{};  // see ShaderOp::ViewDirection / ObjectPosition (ShaderViewDirection)
};

// The viewDirection input at `position`. `viewer` is the camera position (w = 0), or for an orthographic camera the
// unit direction toward it along its axis (w = 1); see ShaderViewer in RenderScene.h. Must match GraphViewToward.
Vec4 ShaderViewDirection(const Vec4& viewer, const Vec3& position);

// 3D value noise in 0..1 (smooth, cells of size 1) and four octaves of it (lacunarity 2.03, gain 0.5,
// about 0..0.94), the noise of the three.js prototype. Must match GraphNoise/GraphFbm in Shaders.glsl.
float ShaderNoise(float x, float y, float z);
float ShaderFbm(float x, float y, float z);
// Weight of the 5x5 post blur sample at integer offset (i, j): exp(-(i*i + j*j) * 0.35). Must match composite_fs.
float SceneBlurWeight(int i, int j);

struct ShaderSurface { Vec4 color; Vec3 emissive; Vec3 normal; };

// Compile JSON to ordered instructions. Failure leaves out unchanged and names the bad field.
// `"stage": "post"` makes a PostProcess graph: color output only, inputs uv/time/texel/sceneColor/
// sceneBlur (no surface inputs), up to kMaxNodes instructions.
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

// Light stage of a graph with a `light` output: what one light adds (rgb, not clamped), for the inputs' light.
Vec3 EvaluateShaderLight(const ShaderGraph& graph, const ShaderInputs& inputs,
                         const std::array<Vec4, ShaderGraph::kMaxUniforms>& uniforms);

// Vertex stage of a graph with an `offset` output: world-space displacement of one vertex.
// Inputs are the undisplaced world position, the vertex normal, the transformed UV and the
// material's base color; texture nodes cannot feed `offset` (CompileShaderGraph rejects it).
Vec3 ShaderVertexOffset(const ShaderGraph& graph, const ShaderInputs& inputs,
                        const std::array<Vec4, ShaderGraph::kMaxUniforms>& uniforms);

}  // namespace oe
