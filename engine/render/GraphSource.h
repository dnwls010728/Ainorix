#pragma once
#include <string>

#include "render/ShaderGraph.h"

namespace oe {

// Shader graphs compiled for the GPU: instead of interpreting a graph per vertex and pixel (the
// loops in Shaders.glsl), the GPU renderer gives each graph its own program. It takes the mesh
// shaders sokol-shdc generated from Shaders.glsl and replaces the bodies of SurfaceGraph and
// VertexOffset with the graph as straight-line code (one variable per node, the same finite
// semantics as the interpreter and ShaderGraph.cpp). Lighting stays in one place, Shaders.glsl.
enum class GraphDialect {
    Glsl,  // GLSL ES 3.00 (WebGL2, GLES3) and desktop GLSL
    Hlsl,  // HLSL 5 (D3D11)
};

// Body (without braces) of `void SurfaceGraph(uv, normal, inout base, inout alpha, out emission,
// out normal_out)` for this graph.
std::string GraphSurfaceBody(const ShaderGraph& graph, GraphDialect dialect);
// Body of `vec3 VertexOffset(wpos, nrm, uv)`: the instructions up to the `offset` output, or
// a zero offset for graphs without one.
std::string GraphVertexBody(const ShaderGraph& graph, GraphDialect dialect);
// Replaces the body of the function `name` defined in generated shader `source` (the first line
// that declares `name(` and is followed by a brace block). False when there is no such function.
bool ReplaceFunctionBody(std::string& source, const std::string& name, const std::string& body);

}  // namespace oe
